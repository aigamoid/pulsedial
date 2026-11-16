/*
 * Spotify リモコンの見た目と音の解析 (実機と Mac のシミュレータで共通)
 *
 * Analyzer: マイクの音 (44.1kHz) から
 *   - スペアナ 32 帯 (45Hz〜14kHz を対数で分ける。低いほうは FFT の 1 本ずつ。高い音ほど弱いので 1 オクターブ 3dB 持ち上げる)
 *   - キック (低音の打ち込み)・アクセント (全体の立ち上がり)・にぎやかさ
 *   - 静か (部屋で音楽が鳴っていない) かどうか
 *   Spotify は曲の拍やテンポを新しいアプリに出さないので、部屋で鳴っている音そのものを聞く
 * 画面は GlassUi (Spotify 公式風 + リキッドグラス風)。文字の描き方 (RasterFn) と Mask はここで決める
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <mutex>

namespace spviz {

constexpr int kSize       = 466;
constexpr int kSampleRate = 44100;
constexpr int kHop        = 512;   // 1 回に読む量 (11.6ms)
constexpr int kFft        = 1024;  // 1 本 43Hz (2048 にすると、PSRAM で重く Wi-Fi の邪魔になった)
constexpr int kBands      = 32;
constexpr int kArt        = 216;   // ジャケットを持つ大きさ (画面では直径 200 前後)

using AllocFn = void* (*)(size_t bytes);
using FreeFn  = void (*)(void* p);

// 8 ビットの濃さだけの絵 (文字など)
struct Mask {
    int w      = 0;
    int h      = 0;
    uint8_t* a = nullptr;
};

// 文字を、字体の元の大きさで白黒 (または灰色) に描く。out->a は alloc で取る
// 実機は M5GFX、Mac は CoreText と DejaVu で描く
enum RasterFont : uint8_t {
    kFontJp40     = 0,  // 曲名・アーティスト・案内 (実機は lgfxJapanGothicP_40)
    kFontDigits72 = 1,  // 時計の数字 (DejaVu72)
    kFontJp40Bold = 2,  // 太字 (同じ字体を 1px ずらして重ねる)
};
using RasterFn = bool (*)(void* ctx, const char* utf8, int font, Mask* out, AllocFn alloc);

// ---------------------------------------------------------------- 音の解析
struct Frame {
    float band[kBands] = {};  // 0..1 (前に受け取ってからの最大)
    float energy       = 0;   // にぎやかさ 0..1
    int kicks          = 0;   // 前に受け取ってからのキックの数
    int hits           = 0;   // 前に受け取ってからのアクセントの数
    bool quiet         = true;
    float tempo        = 0;   // マイクで推定したテンポ (BPM)。確かでなければ 0
    float beatAgo      = 0;   // 最後の拍から何秒たったか (tempo が 0 でないとき)
};

class Analyzer {
public:
    ~Analyzer();
    void begin();
    // kHop 個ずつ渡す (マイクのタスクから)
    void process(const int16_t* hop);
    // 描く側から
    void take(Frame& out);
    void resetTempo() { _resetTempo.store(true); }  // 曲が変わった: テンポを聞き直す
    // 確かめる用 (ログ)
    float ceilingDb() const { return _ceil; }
    float floorDb() const;

private:
    void fft();
    float* _cbuf = nullptr;  // 実機: esp-dsp の FFT の入れ物 (複素数 kFft 個) と回転の表
    float* _wtab = nullptr;

    float _x[kFft];
    float _re[kFft], _im[kFft];
    float _win[kFft];
    float _cos[kFft / 2], _sin[kFft / 2];
    uint16_t _rev[kFft];
    int _edge[kBands + 1];
    float _tilt[kBands];
    float _floor[kBands];
    float _bandCeil[kBands];
    float _smooth[kBands];
    static constexpr int kBlockHops = 172;  // 2 秒
    static constexpr int kMinBlocks = 30;   // 60 秒
    float _blockMin[kBands];
    float _mins[kMinBlocks][kBands];
    int _minPos   = 0;
    int _minCount = 0;
    float _prevV[kBands];
    float _ceil  = -60;
    int _hops    = 0;
    float _hpIn  = 0;
    float _hpOut = 0;
    // キック (50〜250Hz の立ち上がり)
    float _lowHist[4];
    int _lowPos      = 0;
    float _lowFloor  = -90;
    float _onHist[86];
    float _fluxHist[86];
    int _histPos     = 0;
    int _histCount   = 0;
    int _sinceKick   = 1000;
    int _sinceHit    = 1000;
    float _energy    = 0;
    float _activity  = 0;
    int _quietHops   = 1000;
    // テンポの推定: 立ち上がりの強さを 6 秒ためて、0.5 秒ごとに自己相関で拍の間隔を探す
    static constexpr int kOss = 512;
    float _oss[kOss];
    float _ossLin[kOss];  // 推定の作業用 (マイクのタスクのスタックに置かない)
    float _ac[2 * 74 + 2];
    float _score[74 + 2];
    int _ossPos = 0, _ossCount = 0;
    int _sinceEst      = 0;
    int _hopsSinceEst  = 0;
    float _tempo       = 0;  // 確かなときだけ
    float _periodHops  = 0;
    float _phaseHops   = 0;  // 推定したとき、最後の拍から何ホップか
    float _cand[4]     = {};
    int _candN         = 0;
    int _lostHops      = 0;
    void estimateTempo();
    // 描く側と分け合う
    std::mutex _m;
    float _maxBand[kBands];
    int _kicks = 0, _hits = 0;
    float _sharedEnergy = 0;
    bool _sharedQuiet   = true;
    float _sharedTempo  = 0;
    std::atomic<bool> _resetTempo{false};
    float _sharedAgo    = 0;
};

}  // namespace spviz

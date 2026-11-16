/*
 * Spotify リモコンの画面 (実機と Mac のシミュレータで共通)。Spotify 公式の再生画面の雰囲気 + リキッドグラス風
 *
 * 見た目 (丸い 466x466):
 *   - 背景: ジャケットを大きくぼかした色。上は明るめ、下と外周は暗く
 *   - ジャケット: 角丸 220px、影つき。まわりの背景が音に合わせてぼんやり光る (ジャケットのあざやかな色)
 *   - 曲名 (白・太字) とアーティスト (灰)。左に緑の「再生中バー」(マイクの音で動く)、右に ♥ (緑のチェック / + の丸)
 *   - 細い再生バーと時間、すりガラスのカプセル (前・再生・次)、上に再生している機器の名前 (緑)
 *   - 20 秒さわらないと「静かな画面」になる (さわると戻る): ジャケットを画面いっぱいに出し、外周に虹彩のような
 *     光の筋 (音に合わせて伸び縮みする虹色)。上に小さく曲名とアーティスト、下に小さく再生バーと ♥
 *
 * 描き方 (ESP32-S3 で軽くするため、なるべく前もって作っておく):
 *   - 止まっている部分 (背景・ジャケット・文字・ガラス) は「下地」として、変わったときに 1 回だけ作る
 *   - 背景の光は 7 段階の絵を前もって作り (下地を作り直したあと 1 コマに 1 段ずつ)、毎回は写すだけ
 *   - 毎コマは、動く部分 (再生中バー・再生バー・♥・押したボタン・流れる曲名) だけ写し直して描く
 *   - 描いた行の範囲を返すので、そこだけパネルへ送る
 * 焼き付き対策: 画面全体が半径 14px の円を描くように、1.5 秒に 1 歩ずつ漂い続ける (作り置きの絵をずらして写すだけ。
 *   ずれた分は、ふちの色を延ばして埋める)。さわらないと明るいボタンを消し、静かな画面が 1 分続いたら小さい文字も半分の明るさに。
 *   静かな画面ではアプリ側が全体の明るさを 85% にする。長く止めたときの時計の画面もアプリ側が選ぶ
 */
#pragma once

#include "PixelArt.h"
#include "SpotifyViz.h"

#include <stddef.h>
#include <stdint.h>

namespace spviz {

enum class Mode : uint8_t { Message, Idle, Player };
enum class Flash : uint8_t { Play, Pause, Next, Prev, Like, Unlike, VolUp, VolDown };
// 静かな画面のビジュアライザー: Iris = 虹色のぼかした光 (派手)、Wave = ふちをうねる細い白 (明るいジャケットは黒) の線 (エモ)
enum class Viz : uint8_t { Iris, Wave };

class GlassUi {
public:
    // fastAlloc: 毎コマ読む小さな表を置く (内蔵メモリがよい。nullptr なら alloc)
    bool begin(AllocFn alloc, FreeFn free, RasterFn raster, void* rasterCtx, AllocFn fastAlloc = nullptr);
    void end();

    void showMessage(const char* line1, const char* line2, const char* line3);
    // 案内の画面 (接続中・Wi-Fi 設定・Spotify 設定など): うっすら光る LED の粒の背景 (緑の帯が流れる) に、
    // ドット絵の絵柄と白い文字 (ふちどりなし)。big = 1 行目を大きく (PIN)。QR の中身は setNoticeQr で
    enum class Notice : uint8_t { Logo, Wifi, Key, Qr };
    void showNotice(Notice icon, const char* line1, const char* line2, const char* line3, bool big = false);
    void setNoticeQr(const uint8_t* modules, int n);  // n x n の粒 (1 = 黒)
    void showIdle(const char* hint);
    void showPlayer();
    Mode mode() const { return _mode; }

    void setClock(int hour, int minute);
    void setTrack(const char* title, const char* artist);
    void setDevice(const char* name);
    // ジャケット (RGB565、ふつうのバイト順)。nullptr = なし
    void setArt(const uint16_t* rgb565, int w, int h);
    void setPlayback(bool playing, uint32_t posMs, uint32_t durMs, bool liked, bool canLike);
    // 曲のテンポ (BPM) と激しさ (0..1)。bpm > 0 なら、マイクの代わりに拍で動かす (拍の位置は再生位置から計算)
    void setBeat(float bpm, float energy, float offsetMs = 0);  // offsetMs: 曲の位置がこの値のとき拍
    // 覚えていたドット絵を LED 風に出す (本物のジャケットが setArt で届いたら、32 → 64 → 本物と細かくしていく)
    void setArtPixel(const PixelArt& p);
    // マイクで推定したテンポ (Spotify 側のテンポがない曲のとき使う)。毎コマ、解析の結果を渡す
    void setMicBeat(float bpm, float beatAgoSec, float energy);
    void flash(Flash f);
    // 音量 0..100 (-1 = 分からない)。音量のエフェクトのゲージに使う
    void setVolume(int v) { _volume = v; }
    // さわったとき (消えていたボタンを出す)
    void wake();
    // すぐ静かな画面にする (画面の空いているところをタップしたとき)
    void enterAmbient();
    // 静かな画面になるまでの秒 (ふつうは 20 秒。シミュレータで短くする用)
    void setAmbientAfter(float s) { _ambientAfter = s; }
    // 2 つのコアで半分ずつ描くための手伝い: fn(ctx, job, arg) は job(arg, 0) をもう片方のコアで、job(arg, 1) をここで走らせ、
    // 両方終わるまで待つ。なければ順に走らせる (Mac のシミュレーター)
    using ParJob = void (*)(void* arg, int part);
    using ParFn  = void (*)(void* ctx, ParJob job, void* arg);
    void setParallel(ParFn fn, void* ctx)
    {
        _par    = fn;
        _parCtx = ctx;
    }
    // 曲送りのエフェクト中 (このあいだはジャケットを替えない: 全体の描き直しでコマが落ちて重く見えた)
    bool skipFxActive() const;
    // 曲送りの見せ方 (見比べ用): 0 = 光の筋、1 = エフェクトなし、2 = 波形の線の一部として光る
    void setSkipStyle(int s) { _skipStyle = s; }
    // 曲名を下地に描かず、文字の欄だけでふわっと出す (曲が替わっても画面全体を描き直さない)
    void setTitleLite(bool b)
    {
        _titleLite = b;
        _baseDirty = true;
    }
    // このコマでしたこと (見比べ用。シミュレーターで本体の重さを見積もる): 1 下地を作り直した 2 全体 4 切り替え中 8 光の筋 16 彗星
    uint32_t frameFlags() const { return _frameFlags; }
    void setViz(Viz v)
    {
        if (v != _viz) {
            _viz       = v;
            _baseDirty = true;
        }
    }
    Viz viz() const { return _viz; }
    // ボタンを消した静かな画面か (このときのタップは、ボタンを出すだけにする)
    bool ambient() const { return _ambient; }
    // キックの強さ 0..1 (静かな画面で再生中のときだけ)。アプリが画面の明るさを一瞬上げるのに使う
    float pulse() const { return (_mode == Mode::Player && _ambient && _playing) ? _kickPunch * (_viz == Viz::Iris ? 1.0f : 0.25f) : 0.0f; }
    // 押した場所のボタン: 0 = なし、1 = 前、2 = 再生・停止、3 = 次、4 = ♥ (画面の座標)
    int hit(int x, int y) const;

    // パネルへ送る四角 (画面の座標)
    struct Dirty {
        int16_t x, y, w, h;
    };
    static constexpr int kMaxDirty = 72;
    // 1 コマ進めて、変わったところを fb (RGB565 のバイト入れ替え、1 行 stride 画素、x は xoff から) に描く。
    // パネルへ送る四角を out に入れ、数を *n に返す (何も変わらなければ 0)
    void frame(float dt, const Frame& in, uint16_t* fb, int stride, int xoff, Dirty* out, int* n);

    // 確かめる用 (ログ): 前に読んでからの、下地を作った回数と時間 (us)
    void takeStats(int* rebuilds, uint32_t* rebuildUs, uint32_t* drawUs);
    // 確かめる用: 部分ごとの時間の合計 (us)。0 = 線の形、1 = 波形、2 = 全体の描き直しと動く部分、3 = エフェクト
    void takeParts(uint32_t* out4);
    using ClockFn = int64_t (*)();
    void setClockFn(ClockFn f) { _clockFn = f; }

private:
    static constexpr int kGlowLevels = 5;
    static constexpr int kSpokes     = 64;  // 虹彩の筋の数 (太くぼかして、となりとにじませる)

    struct Text {
        Mask m;
    };
    struct Spring {
        float x = 0, v = 0;
        void step(float target, float k, float c, float dt);  // 細かく刻んで計算する (こま数が低くても発散しない)
    };
    struct Rect {
        int x0, y0, x1, y1;  // [x0, x1) x [y0, y1)
    };
    // 動く部分。kZA* は静かな画面のもの
    enum Zone : uint8_t {
        kZEq = 0, kZLike, kZProg, kZTitle, kZArtist, kZCap,
        kZAProg, kZALike, kZATitle, kZAArtist, kZADev, kZABpm,
        kZones
    };
    enum Fx : uint8_t { kFxNone = 0, kFxPlay, kFxPause, kFxNext, kFxPrev, kFxVolUp, kFxVolDown, kFxLike, kFxUnlike };

    void freeText(Text& t);
    bool makeText(Text& t, const char* s, int font, float scale);
    void rebuildBg();
    void rebuildBase();
    void makeGlowLevel(int level);
    bool zoneOn(int z) const;
    Rect zoneRect(int z) const;  // 画面の座標
    void drawZone(int z, uint16_t* fb, int stride, int xoff);
    void compose(int y, int x0, int x1, uint16_t* fbRow);  // 画面の 1 行の [x0, x1) を、下地 (光の段階) からずらして写す
    // shadow > 0 なら、先に文字のふち 1px に薄い影を描く (明るいジャケットの上でも読める。大きな黒いぼかしは使わない)
    void blitText(uint16_t* dst, int dstride, const Text& t, int cx, int cy, int align, int r, int g, int b, float a,
                  const Rect& clip, float scroll = 0, int window = 0, float shadow = 0, bool text = true);  // text = false なら影だけ
    void drawTimes(uint16_t* f, int stride, const Rect& clip);
    void drawTimesAt(uint16_t* f, int stride, const Rect& clip, int y, int xl, int xr, float alpha);
    void drawFx(uint16_t* f, int stride);
    void stroke(uint16_t* f, int stride, float ax, float ay, float bx, float by, float w, float alpha);
    float fxDur() const;
    void drawLike(uint16_t* f, int stride, const Rect& r, float lx, float ly, float scale);
    void drawBar(uint16_t* f, int stride, const Rect& r, float cx, float by, float half, float thick, float knob);
    void drawIris(uint16_t* f, int stride);
    void drawWave(uint16_t* f, int stride, bool all);  // all = false なら、前のコマとも今のコマとも線から遠い画素はさわらない
    uint32_t nowUs() const { return _clockFn ? static_cast<uint32_t>(_clockFn()) : 0; }

    AllocFn _alloc   = nullptr;
    FreeFn _free     = nullptr;
    RasterFn _raster = nullptr;
    void* _rasterCtx = nullptr;
    ClockFn _clockFn = nullptr;

    // 画像 (下地は画面の真ん中に置いた形で持ち、写すときに _ox, _oy だけずらす)
    uint16_t* _bg      = nullptr;  // 背景 (ぼかしたジャケット)。kSize x kSize、RGB565 バイト入れ替え
    uint16_t* _base    = nullptr;  // 下地 (背景 + 止まっている部分)
    uint16_t* _old     = nullptr;  // 曲が変わったときの、切り替え前の画面 (0.5 秒でふわっと切り替える)
    float _fadeT       = 1;        // 0..1 (1 = 切り替え済み)
    bool _fadeReq      = false;    // 次に下地を作り直したら、ふわっと切り替える
    bool _waveFromFb   = false;    // 波形の下の絵を、画面バッファから取る (切り替え中)
    uint8_t* _shade    = nullptr;  // 背景の明るさ (上は明るく、下と外周は暗く)。前もって作る
    uint16_t* _glow[kGlowLevels] = {};  // 光の段階ごとの、光る範囲の絵 (0 は下地そのもの)
    uint8_t* _glowMask = nullptr;  // 光る強さ (ジャケットのまわり)
    uint8_t _glowReady = 1;        // 作り終わった段階 (ビット)
    int _glowShown     = 0;        // 画面に出ている段階
    float _glowVal     = 0;
    float _kickPunch   = 0;
    uint16_t* _art     = nullptr;  // ジャケット 220x220 (ふつうの RGB565)
    uint16_t* _artFull = nullptr;  // ジャケット 466x466 (静かな画面用)
    // 虹彩: ふちの帯の画素の表。画面の位置・左の筋の番号・左の筋からの光の受け方 (_irisPos)、
    // 右の筋からの受け方 (_irisG1)、ふちからの距離 (_irisT)。最初に一度だけ作る
    uint32_t* _irisPos = nullptr;
    uint8_t* _irisG1   = nullptr;
    uint8_t* _irisT    = nullptr;
    int _irisN         = 0;
    float _irisLen[kSpokes]     = {};
    float _irisLvl[kBands]      = {};  // 帯ごとの強さ (上がるときはすぐ、下がるときは少しゆっくり)
    uint8_t _irisBand[kSpokes]  = {};  // 筋 → 帯 (左右が低音、上下が高音)
    float _irisMax[kSpokes]     = {};  // 筋のいちばん長いとき (横は長く、上下は短め)
    float _irisHue = 0, _irisHueNow = 0;
    uint8_t _irisJit[kSpokes]   = {};
    float _ambientAfter         = 20;
    // 波形の線: ふちの帯の画素の表 (画面の位置・角度 0..1023) と真ん中からの距離 (1/8px)。最初に一度だけ作る
    static constexpr int kWaveAngles = 1024;
    static constexpr int kWaveHarm   = 9;  // うねりの成分 (2〜10 番)
    uint32_t* _wavePos = nullptr;
    uint16_t* _waveR   = nullptr;
    int _waveN         = 0;
    float _waveAmp[kWaveHarm]   = {};
    float _wavePhase[kWaveHarm] = {};
    float _waveCur[kWaveAngles] = {};  // いまの線の半径
    float _waveEcho1[kWaveAngles] = {};  // 少し遅れてついてくる残像
    float _waveEcho2[kWaveAngles] = {};
    float _sinTab[kWaveAngles]  = {};
    bool _waveDark = false;  // 明るいジャケットでは黒い線
    Viz _viz       = Viz::Iris;
    ParFn _par     = nullptr;
    void* _parCtx  = nullptr;
    void split(ParJob job, void* arg)
    {
        if (_par) {
            _par(_parCtx, job, arg);
        } else {
            job(arg, 0);
            job(arg, 1);
        }
    }
    struct WaveCtx;
    void waveSpan(const WaveCtx& c, int i0, int i1);
    // エフェクト (再生・停止・曲送り・音量) と、操作のあと 7 秒だけ出す詳細 (機器名・時間・BPM)
    uint8_t _fx       = kFxNone;
    float _fxT        = 10;
    bool _fxWas       = false;
    // エフェクトを描いた四角 (前のコマの分は、次のコマで下地に戻す)
    static constexpr int kMaxFx = 24;
    Rect _fxPrev[kMaxFx];
    int _fxPrevN    = 0;
    bool _cometWas  = false;
    int _skipStyle     = 0;
    bool _titleLite    = false;
    float _titleInPrev = 1.0f;
    uint32_t _frameFlags = 0;
    float _cometPrev = -1;       // 前のコマの彗星の頭 (角度の目盛り)。今の頭まで光の筋でつなぐ (コマが落ちてもなめらかに)
    Rect _cometBox{0, 0, 0, 0};  // 筋を描いた四角
    float cometHead(float u) const;  // 進み具合 u (0..1) → 頭の角度の目盛り (すぐ速く、最後にゆっくり)
    int fxRects(Rect* out) const;  // 今のコマのエフェクトが描く場所 (画面の座標)
    int _volume       = -1;
    float _detailT    = 0;  // 詳細を出す残り秒
    float _detailA    = 0;  // 詳細の濃さ 0..1
    float _detailAWas = -1;
    bool _basePaused  = false;  // 下地を止めた状態 (暗い) で作ったか
    Text _bpmText;
    int _bpmShown     = -1;
    uint8_t _lineLut[256];  // 下の明るさ → 波形の線の色 (明るい所は黒、暗い所は白)
    // 毎コマ読む小さな表 (内蔵メモリ): 線と残像 2 本の半径 (1/8px、角度 1024 ごと) と、距離 → 濃さ
    int16_t* _w8      = nullptr;  // [3][kWaveAngles]
    int16_t* _w8p     = nullptr;  // 前のコマの _w8 (変わらない画素を飛ばすため)
    Rect _waveRects[16];          // 波形の帯を囲む四角 (送る範囲。画面に固定)
    int _nWaveRects   = 0;
    uint8_t* _lutLine = nullptr;  // [3][256] 線 (光りつき)・残像 1・残像 2
    uint32_t _parts[4] = {};
    // 拍
    float _bpm        = 0;
    float _micBpm     = 0;
    float _beatOffsetMs = 0;
    bool _bpmTextChanged = false;
    // 案内の画面
    Notice _notice  = Notice::Logo;
    bool _noticeBig = false;
    uint8_t _qrMods[57 * 57];
    int _qrN        = 0;
    float _noticeT  = 0;
    uint8_t _fieldCellX[kSize];
    uint8_t _fieldQX[kSize];
    uint8_t _fieldA[32 * 32];
    bool _fieldReady = false;
    int _holeType    = 0;  // 背景の粒を消す所: 0 なし、1 丸、2 角の丸い四角
    float _holeCx = 0, _holeCy = 0, _holeW = 0, _holeH = 0, _holeSoft = 1;
    void drawNoticeBase();
    void noticeFrame(uint16_t* f, int stride, float dt);
    // LED 風のドット絵 (grid x grid の粒。0 なら本物のジャケット)
    uint8_t _ledRGB[64 * 64 * 3];
    int _ledGrid      = 0;
    int _refineStage  = 0;  // (使わない: 32 → 64 と細かくするのは重かった)
    // ドット絵 → 本物: 粒がにじんでぼやけた絵 (16 マスの色をなめらかにつなぐ) を通って、本物がはっきり出る
    bool _blurFade = false;
    uint8_t _blurRGB[kPixGrid * kPixGrid * 3];
    int16_t _blurX0[kSize];
    uint8_t _blurXT[kSize];
    float _fadeDur    = 0.5f;
    float _titleIn    = 1.0f;  // 曲が替わってからの新しい曲名の濃さ (切り替え中に、前の曲名と重ならないように)
    int _ledLutGrid   = -1;
    uint8_t _ledCellX[kSize];
    uint8_t _ledQX[kSize];
    uint8_t _ledA[32 * 32];
    void prepLed();
    float _micAgo     = 0;
    float _micEnergy  = 0.6f;
    double _micBeatF  = 0;  // マイクの拍の時計 (自分で進め、聞いた拍の位置に少しずつ合わせる)
    bool _beatOn      = false;
    void updateBpmText();
    float _beatEnergy = 0.6f;
    int64_t _lastBeat = -1;
    float _beatPh     = 1;  // いまの拍の中の位置 0..1
    bool _downbeat    = false;
    uint32_t _beatSeed = 1;
    bool _hasArt       = false;
    float _small[48 * 48 * 3];     // ぼかしたジャケット (背景の元)
    bool _hasSmall     = false;
    uint8_t _accent[3] = {40, 120, 255};  // 光の色 (ジャケットのあざやかな色。なければ差し色の青)
    uint8_t _bx0[kSize];                 // 背景を広げるときの、横の位置の表
    uint8_t _btx[kSize];

    // 文字
    Text _title, _artist, _device, _clock, _hint, _msg[3];
    Text _titleS, _artistS;  // 静かな画面の小さい曲名とアーティスト
    Text _digits[12];  // 0-9 : -  (時間の表示用)
    char _titleStr[160] = {}, _artistStr[160] = {}, _deviceStr[64] = {}, _hintStr[96] = {}, _msgStr[3][96] = {};
    int _hour = -2, _minute = -2;

    // 状態
    Mode _mode        = Mode::Message;
    bool _ambient     = false;  // ボタンを消した静かな画面
    float _idleT      = 0;      // さわってからの秒
    bool _playing     = false;
    bool _liked       = false;
    bool _canLike     = false;
    uint32_t _posMs = 0, _durMs = 0;
    float _forceT     = 0;  // 押した直後は、返事を待たずに再生・停止の印を変える
    float _likeForceT = 0;  // ♥ も同じ
    bool _lastLiked = false, _lastCan = false;
    float _t        = 0;
    int _ox = 0, _oy = 0;  // 下地をずらす量 (画面 = 下地 + ずれ)。= ゆっくりの円 (_dx, _dy) + キックの揺れ
    int _dx = 0, _dy = 0;
    int _shakeX = 0, _shakeY = 0;
    float _shakeT  = 0;
    uint32_t _rng  = 2463534242u;
    int _driftStep  = 0;
    float _ambientT   = 0;     // 静かな画面が続いている秒
    float _uiDim      = 1.0f;  // 静かな画面の小さい文字・再生バー・♥ の明るさ (1 分で半分に)
    float _zoneAlpha  = 1.0f;
    float _driftT   = 0;

    bool _bgDirty   = true;
    bool _baseDirty = true;
    bool _fullSend  = true;

    // 動く部分
    bool _zoneWas[kZones] = {};
    int _progKey          = -1;
    bool _capPlaying      = false;
    float _eq[4]          = {};
    float _eqV[4]         = {};
    Spring _likePop;
    float _likeBurst  = 1;          // 0..1 (♥ を付けた瞬間の輪)
    float _press[3]   = {1, 1, 1};  // カプセルのボタンを押したときの光 (0..1、1 = 消えた)
    float _titleScroll = 0, _artistScroll = 0;
    float _scrollT     = 0;
    bool _titleMarquee = false, _artistMarquee = false;
    bool _titleSMarquee = false, _artistSMarquee = false;

    // 計測
    int _rebuilds       = 0;
    uint32_t _rebuildUs = 0;
    uint32_t _drawUs    = 0;
};

}  // namespace spviz

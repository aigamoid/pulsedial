/*
 * 曲ごとのテンポと、アルバムごとのドット絵を、本体のフラッシュ (/spiflash/spotify) に覚える (電源を切っても残る)
 * - テンポ: Spotify の曲 ID ごとに BPM・拍の位置 (曲の頭から何 ms ずれた所が拍か)・どこで知ったか。2048 曲
 * - ドット絵: ジャケットの画像 ID ごとに 16x16・16 色。1024 アルバム
 * - 決まった大きさの枠に入れ、変わった枠だけ書き直す (フラッシュをいたわる)。書くのは flush() (ジャケットの係から)
 * - 枠ごとに確かめ用の値 (sum) を持つ。書いている途中で電源が切れて壊れた枠は、読むときに捨てる
 * - SPOTIFY_MEM_DUMP を定義してビルドすると、開いたときに中身をログに出す (tools/show_memory.py 用。ふだんは出さない)
 * 中身は聴いた曲の記録なので、本体の外には出さない
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "PixelArt.h"

namespace spotify {

enum TempoSource : uint8_t { kTempoNone = 0, kTempoRecco = 1, kTempoMic = 2 };

struct TempoInfo {
    float bpm      = 0;  // 0 = 分からない
    float offsetMs = 0;  // 拍の位置: 曲の位置がこの値 (と、その拍の間隔ずつ) のとき拍
    float energy   = 0.6f;  // 曲の激しさ 0..1 (ReccoBeats)
    uint8_t source = kTempoNone;
    bool reccoChecked = false;  // ReccoBeats にもう聞いた (なかった場合も)
};

class TrackMemory {
public:
    bool begin();  // ファイルを読む (なければ作る)
    void end();
    bool findTempo(const char* trackId, TempoInfo* out);
    void putTempo(const char* trackId, const TempoInfo& t);
    bool findPix(const char* artUrl, spviz::PixelArt* out);
    bool hasPix(const char* artUrl);
    void putPix(const char* artUrl, const spviz::PixelArt& p);
    void flush();  // 変わった枠をフラッシュへ

private:
    struct TempoRec {
        uint64_t key;
        uint16_t bpm10;
        uint16_t offMs;
        uint8_t source;
        uint8_t flags;  // bit0: ReccoBeats に聞いた
        uint16_t energy1k;
        uint32_t stamp;
        uint32_t sum;  // 確かめ用 (0 = 古い形式。読めるが、次に書くときに付ける)
    };
    struct PixRec {
        uint64_t key;
        uint32_t stamp;
        uint32_t sum;  // 同上
        uint16_t pal[spviz::kPixColors];
        uint8_t idx4[spviz::kPixGrid * spviz::kPixGrid / 2];  // 4 ビットずつ
    };
    static constexpr int kTempoSlots = 2048;
    static constexpr int kPixSlots   = 1024;
    static constexpr int kMaxDirty   = 32;
    bool load(const char* path, void* data, size_t bytes);
    void writeSlot(const char* path, size_t headerBytes, size_t slotBytes, int slot, const void* data);
    void writeAll(bool tempo);  // 全部の枠を書く (書く印の表があふれたとき・古い形式を直すとき)
    int pickSlot(uint64_t key, bool tempo);

    SemaphoreHandle_t _m = nullptr;
    TempoRec* _tempo     = nullptr;  // PSRAM
    PixRec* _pix         = nullptr;
    uint32_t _stamp      = 1;
    int16_t _dirtyTempo[kMaxDirty];
    int16_t _dirtyPix[kMaxDirty];
    int _nDirtyTempo = 0, _nDirtyPix = 0;
    bool _allTempo = false, _allPix = false;  // 表があふれた: 次の flush で全部書く
    std::atomic<bool> _ok{false};
};

}  // namespace spotify

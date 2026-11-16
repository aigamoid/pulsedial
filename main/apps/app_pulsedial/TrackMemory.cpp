/*
 * 曲ごとのテンポと、アルバムごとのドット絵を覚える。説明は TrackMemory.h
 */
#include "TrackMemory.h"

#include <esp_heap_caps.h>
#include <mooncake_log.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>

namespace spotify {

namespace {

constexpr const char* kTag       = "spotify-mem";
constexpr const char* kDir       = "/spiflash/spotify";
constexpr const char* kTempoPath = "/spiflash/spotify/tempo.bin";
constexpr const char* kPixPath   = "/spiflash/spotify/pix.bin";
constexpr size_t kHeader         = 16;
constexpr uint32_t kTempoMagic   = 0x4D545053;  // "SPTM"
constexpr uint32_t kPixMagic     = 0x58505053;  // "SPPX"
constexpr uint32_t kTempoVersion = 2;  // 2: マイクの取りこぼしで数 % 速く覚えた値を捨てる
constexpr uint32_t kPixVersion   = 1;

// 64 ビットの印 (FNV-1a)。0 は「空き」に使うので避ける
uint64_t keyOf(const char* s)
{
    uint64_t h = 1469598103934665603ULL;
    for (; s && *s; ++s) h = (h ^ static_cast<uint8_t>(*s)) * 1099511628211ULL;
    return h ? h : 1;
}

// ジャケットの URL は、最後の「/」のあと (画像の ID) だけを見る (配る先のサーバーが変わっても同じ絵)
const char* artId(const char* url)
{
    const char* p = url ? strrchr(url, '/') : nullptr;
    return p ? p + 1 : (url ? url : "");
}

// 枠の確かめ用の値 (FNV-1a 32 ビット)。sum の欄は 0 として計算する。0 は「古い形式」に使うので避ける
template <typename R>
uint32_t sumOf(const R& r)
{
    R c   = r;
    c.sum = 0;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&c);
    uint32_t h       = 2166136261u;
    for (size_t i = 0; i < sizeof(R); ++i) h = (h ^ b[i]) * 16777619u;
    return h ? h : 1;
}

// 読んだ枠を確かめる: 壊れた枠は空きにする。古い形式 (sum = 0) があれば true (全部書き直して sum を付ける)
template <typename R>
bool checkSlots(R* recs, int n, int* bad)
{
    bool legacy = false;
    for (int i = 0; i < n; ++i) {
        if (!recs[i].key) continue;
        if (recs[i].sum == 0) {
            legacy = true;
        } else if (recs[i].sum != sumOf(recs[i])) {
            memset(&recs[i], 0, sizeof(R));
            ++*bad;
        }
    }
    return legacy;
}

}  // namespace

bool TrackMemory::load(const char* path, void* data, size_t bytes)
{
    const uint32_t magic   = path == kTempoPath ? kTempoMagic : kPixMagic;
    const uint32_t version = path == kTempoPath ? kTempoVersion : kPixVersion;
    FILE* f              = fopen(path, "rb");
    if (f) {
        uint32_t head[4] = {};
        const bool ok    = fread(head, 1, kHeader, f) == kHeader && head[0] == magic && head[1] == version &&
                        fread(data, 1, bytes, f) == bytes;
        fclose(f);
        if (ok) return true;
        mclog::tagWarn(kTag, "{}: wrong file, making a new one", path);
    }
    // 新しく作る (見出し + 空の枠)
    memset(data, 0, bytes);
    f = fopen(path, "wb");
    if (!f) {
        mclog::tagWarn(kTag, "{}: cannot create", path);
        return false;
    }
    const uint32_t head[4] = {magic, version, 0, 0};
    bool ok                = fwrite(head, 1, kHeader, f) == kHeader;
    for (size_t off = 0; ok && off < bytes; off += 4096) {
        const size_t n = std::min<size_t>(4096, bytes - off);
        ok             = fwrite(static_cast<const uint8_t*>(data) + off, 1, n, f) == n;
    }
    fclose(f);
    return ok;
}

bool TrackMemory::begin()
{
    if (_ok) return true;
    _m     = xSemaphoreCreateMutex();
    _tempo = static_cast<TempoRec*>(heap_caps_malloc(sizeof(TempoRec) * kTempoSlots, MALLOC_CAP_SPIRAM));
    _pix   = static_cast<PixRec*>(heap_caps_malloc(sizeof(PixRec) * kPixSlots, MALLOC_CAP_SPIRAM));
    if (!_m || !_tempo || !_pix) {
        end();
        return false;
    }
    mkdir(kDir, 0775);
    const bool t = load(kTempoPath, _tempo, sizeof(TempoRec) * kTempoSlots);
    const bool p = load(kPixPath, _pix, sizeof(PixRec) * kPixSlots);
    int bad = 0;
    _allTempo = checkSlots(_tempo, kTempoSlots, &bad);
    _allPix   = checkSlots(_pix, kPixSlots, &bad);
    if (bad) mclog::tagWarn(kTag, "dropped {} broken slots", bad);
    int nt = 0, np = 0;
    for (int i = 0; i < kTempoSlots; ++i) {
        if (!_tempo[i].key) continue;
        ++nt;
        _stamp = std::max(_stamp, _tempo[i].stamp + 1);
    }
    for (int i = 0; i < kPixSlots; ++i) {
        if (!_pix[i].key) continue;
        ++np;
        _stamp = std::max(_stamp, _pix[i].stamp + 1);
    }
    mclog::tagInfo(kTag, "remembered {} tempos, {} pixel arts{}", nt, np, (t && p) ? "" : " (file problem)");
#ifdef SPOTIFY_MEM_DUMP
    // 中身の一覧 (確かめる用。曲名は持っていないので、Mac で曲 ID の印と突き合わせる)。聴いた曲の記録なので、ふだんは出さない
    for (int i = 0; i < kTempoSlots; ++i) {
        const TempoRec& r = _tempo[i];
        if (!r.key) continue;
        mclog::tagInfo(kTag, "dump tempo {:016x} bpm {:.1f} beat {}ms src {} recco {}", static_cast<unsigned long long>(r.key),
                       r.bpm10 / 10.0f, r.offMs, r.source, r.flags & 1);
    }
    for (int i = 0; i < kPixSlots; ++i) {  // ドット絵: 16 色の表 (RGB565) と、マスごとの色番号 (4 ビット) をそのまま 16 進で
        const PixRec& r = _pix[i];
        if (!r.key) continue;
        char hex[2 * (sizeof(r.pal) + sizeof(r.idx4)) + 1];
        const uint8_t* b = reinterpret_cast<const uint8_t*>(r.pal);
        int n            = 0;
        for (size_t k = 0; k < sizeof(r.pal); ++k) n += snprintf(hex + n, 3, "%02x", b[k]);
        for (size_t k = 0; k < sizeof(r.idx4); ++k) n += snprintf(hex + n, 3, "%02x", r.idx4[k]);
        mclog::tagInfo(kTag, "dump pix {:016x} {}", static_cast<unsigned long long>(r.key), hex);
    }
#endif
    _ok = true;
    return true;
}

void TrackMemory::end()
{
    if (_ok) flush();
    _ok = false;
    heap_caps_free(_tempo);
    heap_caps_free(_pix);
    _tempo = nullptr;
    _pix   = nullptr;
    if (_m) vSemaphoreDelete(_m);
    _m           = nullptr;
    _nDirtyTempo = _nDirtyPix = 0;
    _allTempo = _allPix = false;
}

// 同じ印の枠、なければ空き、なければいちばん長く使っていない枠
int TrackMemory::pickSlot(uint64_t key, bool tempo)
{
    const int n = tempo ? kTempoSlots : kPixSlots;
    int empty = -1, oldest = 0;
    uint32_t oldestStamp = UINT32_MAX;
    for (int i = 0; i < n; ++i) {
        const uint64_t k  = tempo ? _tempo[i].key : _pix[i].key;
        const uint32_t st = tempo ? _tempo[i].stamp : _pix[i].stamp;
        if (k == key) return i;
        if (!k && empty < 0) empty = i;
        if (k && st < oldestStamp) {
            oldestStamp = st;
            oldest      = i;
        }
    }
    return empty >= 0 ? empty : oldest;
}

bool TrackMemory::findTempo(const char* trackId, TempoInfo* out)
{
    if (!_ok || !trackId || !*trackId) return false;
    const uint64_t key = keyOf(trackId);
    bool found         = false;
    xSemaphoreTake(_m, portMAX_DELAY);
    for (int i = 0; i < kTempoSlots; ++i) {
        if (_tempo[i].key != key) continue;
        TempoRec& r       = _tempo[i];
        out->bpm          = r.bpm10 / 10.0f;
        out->offsetMs     = r.offMs;
        out->energy       = r.energy1k ? r.energy1k / 1000.0f : 0.6f;
        out->source       = r.source;
        out->reccoChecked = r.flags & 1;
        r.stamp           = _stamp++;  // 使った (覚えるのは次に書くとき)
        found             = true;
        break;
    }
    xSemaphoreGive(_m);
    return found;
}

void TrackMemory::putTempo(const char* trackId, const TempoInfo& t)
{
    if (!_ok || !trackId || !*trackId) return;
    const uint64_t key = keyOf(trackId);
    xSemaphoreTake(_m, portMAX_DELAY);
    const int s   = pickSlot(key, true);
    TempoRec& r   = _tempo[s];
    r             = TempoRec{};
    r.key         = key;
    r.bpm10       = static_cast<uint16_t>(std::clamp(std::lround(t.bpm * 10.0f), 0L, 65535L));
    r.offMs       = static_cast<uint16_t>(std::clamp(std::lround(t.offsetMs), 0L, 65535L));
    r.source      = t.source;
    r.energy1k    = static_cast<uint16_t>(std::clamp(std::lround(t.energy * 1000.0f), 1L, 1000L));
    r.flags       = t.reccoChecked ? 1 : 0;
    r.stamp       = _stamp++;
    bool listed   = false;
    for (int i = 0; i < _nDirtyTempo; ++i) listed |= _dirtyTempo[i] == s;
    if (!listed) {
        if (_nDirtyTempo < kMaxDirty) {
            _dirtyTempo[_nDirtyTempo++] = static_cast<int16_t>(s);
        } else {
            _allTempo = true;  // 表があふれた: 次は全部書く
        }
    }
    xSemaphoreGive(_m);
}

bool TrackMemory::findPix(const char* artUrl, spviz::PixelArt* out)
{
    if (!_ok || !artUrl || !*artUrl) return false;
    const uint64_t key = keyOf(artId(artUrl));
    bool found         = false;
    xSemaphoreTake(_m, portMAX_DELAY);
    for (int i = 0; i < kPixSlots; ++i) {
        if (_pix[i].key != key) continue;
        PixRec& r = _pix[i];
        memcpy(out->pal, r.pal, sizeof(out->pal));
        for (int k = 0; k < spviz::kPixGrid * spviz::kPixGrid; ++k) out->idx[k] = (r.idx4[k >> 1] >> ((k & 1) * 4)) & 15;
        r.stamp = _stamp++;
        found   = true;
        break;
    }
    xSemaphoreGive(_m);
    return found;
}

bool TrackMemory::hasPix(const char* artUrl)
{
    if (!_ok || !artUrl || !*artUrl) return false;
    const uint64_t key = keyOf(artId(artUrl));
    bool found         = false;
    xSemaphoreTake(_m, portMAX_DELAY);
    for (int i = 0; i < kPixSlots && !found; ++i) found = _pix[i].key == key;
    xSemaphoreGive(_m);
    return found;
}

void TrackMemory::putPix(const char* artUrl, const spviz::PixelArt& p)
{
    if (!_ok || !artUrl || !*artUrl) return;
    const uint64_t key = keyOf(artId(artUrl));
    xSemaphoreTake(_m, portMAX_DELAY);
    const int s = pickSlot(key, false);
    PixRec& r   = _pix[s];
    r           = PixRec{};
    r.key       = key;
    r.stamp     = _stamp++;
    memcpy(r.pal, p.pal, sizeof(r.pal));
    for (int k = 0; k < spviz::kPixGrid * spviz::kPixGrid; ++k) r.idx4[k >> 1] |= (p.idx[k] & 15) << ((k & 1) * 4);
    bool listed = false;
    for (int i = 0; i < _nDirtyPix; ++i) listed |= _dirtyPix[i] == s;
    if (!listed) {
        if (_nDirtyPix < kMaxDirty) {
            _dirtyPix[_nDirtyPix++] = static_cast<int16_t>(s);
        } else {
            _allPix = true;
        }
    }
    xSemaphoreGive(_m);
}

void TrackMemory::writeSlot(const char* path, size_t headerBytes, size_t slotBytes, int slot, const void* data)
{
    FILE* f = fopen(path, "r+b");
    if (!f) return;
    if (fseek(f, static_cast<long>(headerBytes + slotBytes * slot), SEEK_SET) == 0) fwrite(data, 1, slotBytes, f);
    fclose(f);
}

// 全部の枠を 1 つずつ写して、頭から順に書く (ロックは 1 枠を写す間だけ)
void TrackMemory::writeAll(bool tempo)
{
    FILE* f = fopen(tempo ? kTempoPath : kPixPath, "r+b");
    if (!f) return;
    bool ok = fseek(f, static_cast<long>(kHeader), SEEK_SET) == 0;
    for (int i = 0; ok && i < (tempo ? kTempoSlots : kPixSlots); ++i) {
        if (tempo) {
            xSemaphoreTake(_m, portMAX_DELAY);
            TempoRec t = _tempo[i];
            xSemaphoreGive(_m);
            if (t.key) t.sum = sumOf(t);
            ok = fwrite(&t, 1, sizeof(t), f) == sizeof(t);
        } else {
            xSemaphoreTake(_m, portMAX_DELAY);
            PixRec p = _pix[i];
            xSemaphoreGive(_m);
            if (p.key) p.sum = sumOf(p);
            ok = fwrite(&p, 1, sizeof(p), f) == sizeof(p);
        }
    }
    fclose(f);
    if (!ok) mclog::tagWarn(kTag, "write all ({}) failed", tempo ? "tempo" : "pix");
}

void TrackMemory::flush()
{
    if (!_ok) return;
    // 表があふれた・古い形式だった: 全部書く (そのときの印の表は要らなくなる)
    for (int pass = 0; pass < 2; ++pass) {
        const bool tempo = pass == 0;
        xSemaphoreTake(_m, portMAX_DELAY);
        bool& all = tempo ? _allTempo : _allPix;
        const bool doAll = all;
        if (doAll) {
            all = false;
            (tempo ? _nDirtyTempo : _nDirtyPix) = 0;
        }
        xSemaphoreGive(_m);
        if (doAll) writeAll(tempo);
    }
    // 1 枠ずつ写して書く (写しは小さく。スタックに大きな配列を置かない)
    while (true) {
        TempoRec t;
        PixRec p;
        int slot  = -1;
        bool isPx = false;
        xSemaphoreTake(_m, portMAX_DELAY);
        if (_nDirtyTempo > 0) {
            slot = _dirtyTempo[--_nDirtyTempo];
            t    = _tempo[slot];
        } else if (_nDirtyPix > 0) {
            slot = _dirtyPix[--_nDirtyPix];
            p    = _pix[slot];
            isPx = true;
        }
        xSemaphoreGive(_m);
        if (slot < 0) break;
        if (isPx) {
            p.sum = sumOf(p);
            writeSlot(kPixPath, kHeader, sizeof(PixRec), slot, &p);
        } else {
            t.sum = sumOf(t);
            writeSlot(kTempoPath, kHeader, sizeof(TempoRec), slot, &t);
        }
    }
}

}  // namespace spotify

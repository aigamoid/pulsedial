/*
 * ジャケットの JPEG を読む。説明は ArtJpeg.h
 */
#include "ArtJpeg.h"

#include <esp_heap_caps.h>
#include <string.h>

#include <algorithm>

#include "esp32s3/rom/tjpgd.h"

namespace spotify {

namespace {

// チップの TJpgDec を番地で呼ぶ (esp_rom/esp32s3/ld/esp32s3.rom.ld の jd_prepare / jd_decomp)。
// 名前で呼ぶと、同じ名前の LVGL 版 (中の作りがちがう) につながって落ちた
using RomPrepare = JRESULT (*)(JDEC*, UINT (*)(JDEC*, BYTE*, UINT), void*, UINT, void*);
using RomDecomp  = JRESULT (*)(JDEC*, UINT (*)(JDEC*, void*, JRECT*), BYTE);
const RomPrepare romPrepare = reinterpret_cast<RomPrepare>(0x40000858);
const RomDecomp romDecomp   = reinterpret_cast<RomDecomp>(0x40000864);

constexpr int kRing = 24;  // 縮めている途中の行 (1 ブロックの行 = 元の 16 行ぶん + 余り)

struct Acc {
    uint16_t r, g, b, n;
};

struct Io {
    const uint8_t* src;
    size_t len, pos;
    uint16_t* out;
    int sw, sh, ow, oh;   // 元 (縮めて読んだあと) と、できあがり
    uint16_t* dxOf;       // 元の x → できあがりの x
    Acc* acc;             // [kRing][ow]
    int done;             // できあがりの行: ここまで書いた
    CancelFn cancel;
    void* cancelCtx;
};

UINT input(JDEC* jd, BYTE* buf, UINT n)
{
    auto* io          = static_cast<Io*>(jd->device);
    const size_t left = io->len - io->pos;
    if (n > left) n = static_cast<UINT>(left);
    if (buf) memcpy(buf, io->src + io->pos, n);
    io->pos += n;
    return n;
}

inline int dyOf(const Io* io, int sy)
{
    return static_cast<int>(static_cast<int64_t>(sy) * io->oh / io->sh);
}

// できあがりの行 dy を、平均して書き出す
void flushRow(Io* io, int dy)
{
    Acc* a        = io->acc + (dy % kRing) * io->ow;
    uint16_t* row = io->out + dy * io->ow;
    for (int x = 0; x < io->ow; ++x) {
        const int n = std::max<int>(1, a[x].n);
        row[x]      = static_cast<uint16_t>(((a[x].r / n) >> 3) << 11 | ((a[x].g / n) >> 2) << 5 | ((a[x].b / n) >> 3));
        a[x]        = Acc{0, 0, 0, 0};
    }
}

// ROM の TJpgDec は RGB888 で 1 ブロックずつ (左から右、上から下) 返す
UINT output(JDEC* jd, void* bitmap, JRECT* r)
{
    auto* io      = static_cast<Io*>(jd->device);
    const BYTE* p = static_cast<const BYTE*>(bitmap);
    const int bw  = r->right - r->left + 1;
    for (int y = r->top; y <= r->bottom; ++y) {
        if (y >= io->sh) {
            p += bw * 3;
            continue;
        }
        Acc* a = io->acc + (dyOf(io, y) % kRing) * io->ow;
        for (int x = r->left; x <= r->right; ++x, p += 3) {
            if (x >= io->sw) continue;
            Acc& e = a[io->dxOf[x]];
            e.r += p[0];
            e.g += p[1];
            e.b += p[2];
            ++e.n;
        }
    }
    // 1 ブロックの行が終わったら: もう元の行が来ない、できあがりの行を書き出す
    if (r->right + 1 >= io->sw) {
        const int lastDone = r->bottom + 1 >= io->sh ? io->oh - 1 : dyOf(io, r->bottom + 1) - 1;
        while (io->done < lastDone) flushRow(io, ++io->done);
        if (io->cancel && io->cancel(io->cancelCtx)) return 0;  // 途中でやめる
    }
    return 1;
}

}  // namespace

bool decodeJpegRom(const uint8_t* jpg, size_t len, uint16_t* out, int maxSide, int* w, int* h, CancelFn cancel, void* cancelCtx)
{
    constexpr size_t kWork = 4096;
    // 作業用は PSRAM に (内蔵メモリは画面を送る DMA と Wi-Fi に残す。曲送りの瞬間に足りなくなって落ちた)
    void* work = heap_caps_malloc(kWork, MALLOC_CAP_SPIRAM);
    if (!work) work = heap_caps_malloc(kWork, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!work) return false;
    Io io{jpg, len, 0, out, 0, 0, 0, 0, nullptr, nullptr, -1, cancel, cancelCtx};
    JDEC jd;
    bool ok = false;
    if (romPrepare(&jd, input, work, kWork, &io) == JDR_OK) {
        // とても大きい絵 (画面の 3 倍より上) だけ、読むときに 1/2・1/4 にしておく (TJpgDec の縮小は間引きなので、ふだんは使わない)
        BYTE scale = 0;
        while (scale < 3 && (static_cast<int>(std::max(jd.width, jd.height)) >> scale) > 3 * maxSide) ++scale;
        io.sw          = static_cast<int>(jd.width) >> scale;
        io.sh          = static_cast<int>(jd.height) >> scale;
        const float k  = std::min(1.0f, static_cast<float>(maxSide) / std::max(io.sw, io.sh));
        io.ow          = std::max(1, static_cast<int>(io.sw * k + 0.5f));
        io.oh          = std::max(1, static_cast<int>(io.sh * k + 0.5f));
        io.dxOf        = static_cast<uint16_t*>(heap_caps_malloc(sizeof(uint16_t) * io.sw, MALLOC_CAP_SPIRAM));
        io.acc         = static_cast<Acc*>(heap_caps_calloc(kRing * io.ow, sizeof(Acc), MALLOC_CAP_SPIRAM));
        if (io.sw > 0 && io.sh > 0 && io.dxOf && io.acc) {
            for (int x = 0; x < io.sw; ++x) io.dxOf[x] = static_cast<uint16_t>(static_cast<int64_t>(x) * io.ow / io.sw);
            ok = romDecomp(&jd, output, scale) == JDR_OK;
            while (ok && io.done < io.oh - 1) flushRow(&io, ++io.done);
            *w = io.ow;
            *h = io.oh;
        }
        heap_caps_free(io.dxOf);
        heap_caps_free(io.acc);
    }
    heap_caps_free(work);
    return ok;
}

}  // namespace spotify

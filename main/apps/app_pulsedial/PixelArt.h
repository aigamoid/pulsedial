/*
 * ジャケットのドット絵 (16x16 マス・16 色)。フラッシュに覚えておき、曲が変わったらまずこれを LED 風に出す
 * (本物のジャケットは裏で取り、届いたら 32 → 64 → 本物と細かくしていく)。実機と Mac のシミュレーターで共通
 */
#pragma once

#include <stdint.h>
#include <string.h>

#include <algorithm>

namespace spviz {

constexpr int kPixGrid   = 16;
constexpr int kPixColors = 16;

struct PixelArt {
    uint16_t pal[kPixColors];          // RGB565 (ふつうのバイト順)
    uint8_t idx[kPixGrid * kPixGrid];  // マスごとの色番号
};

// マスごとの平均色 (grid x grid、RGB 各 1 バイト) を、RGB565 の絵 (ふつうのバイト順) から
inline void cellAverages(const uint16_t* img, int w, int h, int grid, uint8_t* rgbOut)
{
    for (int cy = 0; cy < grid; ++cy) {
        const int y0 = cy * h / grid, y1 = std::max(y0 + 1, (cy + 1) * h / grid);
        for (int cx = 0; cx < grid; ++cx) {
            const int x0 = cx * w / grid, x1 = std::max(x0 + 1, (cx + 1) * w / grid);
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int y = y0; y < y1; ++y) {
                const uint16_t* row = img + y * w;
                for (int x = x0; x < x1; ++x) {
                    const uint16_t c = row[x];
                    r += (c >> 11) & 31;
                    g += (c >> 5) & 63;
                    b += c & 31;
                    ++n;
                }
            }
            n            = std::max<uint32_t>(1, n);
            uint8_t* o   = rgbOut + (cy * grid + cx) * 3;
            o[0]         = static_cast<uint8_t>(r * 255 / (31 * n));
            o[1]         = static_cast<uint8_t>(g * 255 / (63 * n));
            o[2]         = static_cast<uint8_t>(b * 255 / (31 * n));
        }
    }
}

// 16x16 の平均色を、似た色どうしでまとめて 16 色にする (k-means。初めの色は明るさの順に等間隔で選ぶので、毎回同じ結果)
inline void makePixelArt(const uint16_t* img, int w, int h, PixelArt* out)
{
    constexpr int N = kPixGrid * kPixGrid, K = kPixColors;
    uint8_t px[N * 3];
    cellAverages(img, w, h, kPixGrid, px);
    int order[N];
    for (int i = 0; i < N; ++i) order[i] = i;
    auto lum = [&](int i) { return px[i * 3] * 3 + px[i * 3 + 1] * 6 + px[i * 3 + 2]; };
    std::sort(order, order + N, [&](int a, int b) { return lum(a) < lum(b); });
    float c[K][3];
    for (int k = 0; k < K; ++k) {
        const int i = order[(2 * k + 1) * N / (2 * K)];
        for (int j = 0; j < 3; ++j) c[k][j] = px[i * 3 + j];
    }
    for (int it = 0; it < 10; ++it) {
        float sum[K][3] = {};
        int cnt[K]      = {};
        for (int i = 0; i < N; ++i) {
            int best = 0;
            float bd = 1e30f;
            for (int k = 0; k < K; ++k) {
                const float dr = px[i * 3] - c[k][0], dg = px[i * 3 + 1] - c[k][1], db = px[i * 3 + 2] - c[k][2];
                const float d = 3 * dr * dr + 6 * dg * dg + db * db;
                if (d < bd) {
                    bd   = d;
                    best = k;
                }
            }
            out->idx[i] = static_cast<uint8_t>(best);
            for (int j = 0; j < 3; ++j) sum[best][j] += px[i * 3 + j];
            ++cnt[best];
        }
        for (int k = 0; k < K; ++k) {
            if (!cnt[k]) continue;
            for (int j = 0; j < 3; ++j) c[k][j] = sum[k][j] / cnt[k];
        }
    }
    for (int k = 0; k < K; ++k) {
        const int r = static_cast<int>(c[k][0] + 0.5f), g = static_cast<int>(c[k][1] + 0.5f), b = static_cast<int>(c[k][2] + 0.5f);
        out->pal[k] = static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
}

}  // namespace spviz

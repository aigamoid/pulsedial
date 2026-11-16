/*
 * Spotify リモコンの画面。説明は GlassUi.h
 */
#include "GlassUi.h"

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif
#include <stdlib.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

namespace spviz {

namespace {

// ---------------------------------------------------------------- 置き場所 (画面の真ん中からの px)
constexpr int kArtS     = 220;  // ジャケット
constexpr int kArtHalf  = kArtS / 2;
constexpr int kArtTop   = -176;
constexpr float kArtR   = 14;   // 角の丸み
constexpr int kTitleY   = 70;
constexpr int kArtistY  = 97;
constexpr int kWin      = 232;  // 曲名を見せる幅 (はみ出したら流す)
constexpr int kSideX    = 128;  // 左に再生中バー、右に ♥
constexpr int kSideY    = 82;
constexpr int kProgY    = 121;
constexpr int kProgHalf = 118;
constexpr int kTimeY    = 138;
constexpr int kCapY     = 177;  // カプセル
constexpr float kCapHW  = 98;
constexpr float kCapHH  = 27;
constexpr int kBtnX     = 64;   // 前・次のボタンの位置
constexpr float kPlayR  = 21;   // 再生ボタンの丸
constexpr int kDevY     = -197;
constexpr int kC        = kSize / 2;  // 下地の真ん中
constexpr float kTextShadow = 0.55f;   // 静かな画面の文字のふちの影の濃さ

// 静かな画面: 上に小さく曲名とアーティスト、下に小さく再生バーと ♥。外周に虹彩
constexpr int kATitleY    = -126;  // 虹彩の光にかからないよう、少し内側
constexpr int kAArtistY   = -107;
constexpr int kAWin       = 200;
constexpr int kAProgY     = 124;
constexpr int kABpmY      = 106;  // BPM (詳細のときだけ): 再生バーのすぐ上
constexpr int kAProgHalf  = 72;
constexpr int kALikeY     = 144;
constexpr float kIrisR0   = 150.0f;  // 筋がいちばん伸びたときの内側
constexpr float kIrisR1   = 233.5f;  // 筋の根もと (画面のふち)
constexpr float kIrisSigma = 0.30f;  // 筋のぼかし (筋どうしの間を 1 として)
constexpr float kWaveR      = 204.0f;  // 波形の線のふつうの半径
constexpr float kWaveBand   = 18.0f;   // 線が動ける幅 (片側)。内側の文字 (漂って 14px ずれる) にかからないように

// 背景の光 (ジャケットのまわり)。下地の座標での範囲
constexpr int kGlowX0 = kC - 176, kGlowX1 = kC + 176;
constexpr int kGlowY0 = 0, kGlowY1 = kC + 110;
constexpr int kGlowW = kGlowX1 - kGlowX0, kGlowH = kGlowY1 - kGlowY0;
constexpr float kGlowMax = 0.62f;  // いちばん明るい段階の強さ

// ---------------------------------------------------------------- 色
constexpr int kGray[3]  = {179, 179, 179};  // Spotify の灰 (#B3B3B3)
constexpr int kGreen[3] = {40, 120, 255};   // 差し色 (名前は昔の緑のまま。アイコンの青にそろえた)

constexpr uint8_t kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float smooth(float e0, float e1, float x)
{
    const float t = clampf((x - e0) / (e1 - e0), 0, 1);
    return t * t * (3 - 2 * t);
}

inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// RGB565 (バイト入れ替え) と RGB
inline uint16_t pack(int r, int g, int b)
{
    r = clampi(r, 0, 255);
    g = clampi(g, 0, 255);
    b = clampi(b, 0, 255);
    const uint16_t c = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return static_cast<uint16_t>((c >> 8) | (c << 8));
}

inline void unpack(uint16_t s, int& r, int& g, int& b)
{
    const uint16_t c = static_cast<uint16_t>((s >> 8) | (s << 8));
    r = (c >> 8) & 0xF8;
    r |= r >> 5;
    g = (c >> 3) & 0xFC;
    g |= g >> 6;
    b = (c << 3) & 0xF8;
    b |= b >> 5;
}

// a は 0..256
inline void blendPx(uint16_t* p, int r, int g, int b, int a)
{
    if (a <= 0) return;
    if (a >= 256) {
        *p = pack(r, g, b);
        return;
    }
    int pr, pg, pb;
    unpack(*p, pr, pg, pb);
    *p = pack(pr + (((r - pr) * a) >> 8), pg + (((g - pg) * a) >> 8), pb + (((b - pb) * a) >> 8));
}

inline void addPx(uint16_t* p, int r, int g, int b)
{
    int pr, pg, pb;
    unpack(*p, pr, pg, pb);
    *p = pack(pr + r, pg + g, pb + b);
}

// 速い平方根 (逆数の平方根の近似 + ニュートン法 2 回)
inline float fastSqrt(float x)
{
    if (x <= 0) return 0;
    union {
        float f;
        uint32_t i;
    } u{x};
    u.i     = 0x5f3759dfu - (u.i >> 1);
    float y = u.f;
    y       = y * (1.5f - 0.5f * x * y * y);
    y       = y * (1.5f - 0.5f * x * y * y);
    return x * y;
}

// 角の丸い四角の距離 (中が負)
inline float sdRoundRect(float px, float py, float hw, float hh, float r)
{
    const float qx = fabsf(px) - hw + r, qy = fabsf(py) - hh + r;
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return fastSqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

// 線分までの距離
inline float segDist(float px, float py, float ax, float ay, float bx, float by)
{
    const float vx = bx - ax, vy = by - ay;
    const float t  = clampf(((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy), 0, 1);
    const float dx = px - ax - vx * t, dy = py - ay - vy * t;
    return fastSqrt(dx * dx + dy * dy);
}

}  // namespace

// ============================================================================ ばね

void GlassUi::Spring::step(float target, float k, float c, float dt)
{
    const int n   = std::max(1, static_cast<int>(ceilf(dt * 120.0f)));
    const float h = dt / n;
    for (int i = 0; i < n; ++i) {
        v += (k * (target - x) - c * v) * h;
        x += v * h;
    }
}

// ============================================================================ 始める・終わる

bool GlassUi::begin(AllocFn alloc, FreeFn free, RasterFn raster, void* rasterCtx, AllocFn fastAlloc)
{
    if (!fastAlloc) fastAlloc = alloc;
    _w8      = static_cast<int16_t*>(fastAlloc(sizeof(int16_t) * 3 * kWaveAngles));
    _w8p     = static_cast<int16_t*>(fastAlloc(sizeof(int16_t) * 3 * kWaveAngles));
    _lutLine = static_cast<uint8_t*>(fastAlloc(3 * 256));
    if (!_w8 || !_w8p || !_lutLine) return false;
    for (int d = 0; d < 256; ++d) {  // d は 1/8px
        const float dp = d / 8.0f;
        _lutLine[d]       = static_cast<uint8_t>(255 * std::max(clampf(1.6f - dp, 0, 1), 0.22f * clampf(1.0f - dp / 7.0f, 0, 1)));
        _lutLine[256 + d] = static_cast<uint8_t>(255 * 0.38f * clampf(1.2f - dp, 0, 1));
        _lutLine[512 + d] = static_cast<uint8_t>(255 * 0.18f * clampf(1.2f - dp, 0, 1));
    }
    for (int i = 0; i < 3 * kWaveAngles; ++i) _w8[i] = _w8p[i] = static_cast<int16_t>(204 * 8);
    _alloc     = alloc;
    _free      = free;
    _raster    = raster;
    _rasterCtx = rasterCtx;
    const size_t px = static_cast<size_t>(kSize) * kSize;
    _bg             = static_cast<uint16_t*>(_alloc(px * 2));
    _base           = static_cast<uint16_t*>(_alloc(px * 2));
    _shade          = static_cast<uint8_t*>(_alloc(px));
    _art            = static_cast<uint16_t*>(_alloc(kArtS * kArtS * 2));
    _glowMask       = static_cast<uint8_t*>(_alloc(static_cast<size_t>(kGlowW) * kGlowH));
    _artFull        = static_cast<uint16_t*>(_alloc(px * 2));
    _old            = static_cast<uint16_t*>(_alloc(px * 2));
    for (int i = 1; i < kGlowLevels; ++i) _glow[i] = static_cast<uint16_t*>(_alloc(static_cast<size_t>(kGlowW) * kGlowH * 2));
    if (!_bg || !_base || !_shade || !_art || !_glowMask || !_artFull || !_old) return false;
    for (int i = 1; i < kGlowLevels; ++i) {
        if (!_glow[i]) return false;
    }
    static const char* kDigits[12] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ":", "-"};
    for (int i = 0; i < 12; ++i) makeText(_digits[i], kDigits[i], kFontJp40, 0.36f);

    // 背景の明るさ: 上は明るく・下は暗く、外周も暗く (一度だけ作る)
    for (int y = 0; y < kSize; ++y) {
        const float vert = 0.62f - 0.40f * y / kSize;
        for (int x = 0; x < kSize; ++x) {
            const float dx = x + 0.5f - kC, dy = y + 0.5f - kC;
            const float d  = fastSqrt(dx * dx + dy * dy) / kC;
            _shade[y * kSize + x] = static_cast<uint8_t>(clampf(255.0f * vert * (1.0f - 0.5f * smooth(0.55f, 1.0f, d)), 0, 255));
        }
    }
    // 背景を 48 から広げるときの横の位置
    for (int x = 0; x < kSize; ++x) {
        const float fx = clampf((x + 0.5f) * 48.0f / kSize - 0.5f, 0, 46.999f);
        _bx0[x]        = static_cast<uint8_t>(fx);
        _btx[x]        = static_cast<uint8_t>((fx - _bx0[x]) * 255);
    }
    // 光る強さ: ジャケットのふちから外へ 80px でなくなる。範囲のふちに向けても弱める
    const float acx = kC, acy = kC + kArtTop + kArtHalf;
    for (int y = 0; y < kGlowH; ++y) {
        for (int x = 0; x < kGlowW; ++x) {
            const float px = kGlowX0 + x + 0.5f, py = kGlowY0 + y + 0.5f;
            const float d  = sdRoundRect(px - acx, py - acy, kArtHalf, kArtHalf, kArtR);
            float m        = 0;
            if (d > 0) {
                m = 1.0f - smooth(0.0f, 80.0f, d);
                m = m * fastSqrt(m);
                m *= 1.0f - smooth(kGlowY1 - 40.0f, kGlowY1, py);
                m *= 1.0f - smooth(kGlowW / 2.0f - 30.0f, kGlowW / 2.0f, fabsf(px - kC));
            }
            _glowMask[y * kGlowW + x] = static_cast<uint8_t>(clampf(m * 255.0f, 0, 255));
        }
    }
    // 筋 → 帯: 左右 (3 時と 9 時) が低音、上下が高音。左右対称
    for (int sp = 0; sp < kSpokes; ++sp) {
        float phi = (sp + 0.5f) * 6.2831853f / kSpokes;  // 上が 0
        if (phi > 3.14159265f) phi = 6.2831853f - phi;
        const float d  = fabsf(phi - 1.5707963f) / 1.5707963f;  // 0 = 横、1 = 上下
        _irisBand[sp] = static_cast<uint8_t>(std::min(kBands - 1, 1 + static_cast<int>(d * (kBands - 2))));
        _irisMax[sp]  = 82.0f - 52.0f * d * d;  // 横は長く、上下 (文字のあるところ) は短め
    }
    // 虹彩の表: ふちの帯の画素ぜんぶ。左右 2 本の筋から受ける光 (ガウスのぼかし) を前もって入れておく
    for (int pass = 0; pass < 2; ++pass) {
        int n = 0;
        for (int y = 0; y < kSize; ++y) {
            const float dy = y + 0.5f - kC;
            for (int x = 0; x < kSize; ++x) {
                const float dx = x + 0.5f - kC;
                const float r  = fastSqrt(dx * dx + dy * dy);
                if (r < kIrisR0 - 1 || r > kIrisR1 + 0.5f) continue;
                if (pass == 1) {
                    float a = atan2f(dx, -dy) / 6.2831853f;  // 上が 0、右回り
                    if (a < 0) a += 1;
                    const float u  = a * kSpokes - 0.5f;  // 筋の真ん中が整数
                    const int s0   = (static_cast<int>(floorf(u)) + kSpokes) % kSpokes;
                    const float f  = u - floorf(u);
                    const float g0 = expf(-f * f / (2 * kIrisSigma * kIrisSigma));
                    const float g1 = expf(-(1 - f) * (1 - f) / (2 * kIrisSigma * kIrisSigma));
                    _irisPos[n] = static_cast<uint32_t>(y * kSize + x) | (static_cast<uint32_t>(s0) << 18) |
                                  (static_cast<uint32_t>(g0 * 255) << 24);
                    _irisG1[n]  = static_cast<uint8_t>(g1 * 255);
                    _irisT[n]   = static_cast<uint8_t>(clampf((kIrisR1 - r) * 2, 0, 255));  // 0.5px 刻み
                }
                ++n;
            }
        }
        if (pass == 0) {
            _irisN   = n;
            _irisPos = static_cast<uint32_t*>(_alloc(sizeof(uint32_t) * n));
            _irisG1  = static_cast<uint8_t*>(_alloc(n));
            _irisT   = static_cast<uint8_t*>(_alloc(n));
            if (!_irisPos || !_irisG1 || !_irisT) return false;
        }
    }
    // 波形の表: 半径 kWaveR ± kWaveBand の帯の画素 (角度と距離)
    for (int pass = 0; pass < 2; ++pass) {
        int n = 0;
        for (int y = 0; y < kSize; ++y) {
            const float dy = y + 0.5f - kC;
            for (int x = 0; x < kSize; ++x) {
                const float dx = x + 0.5f - kC;
                const float r  = fastSqrt(dx * dx + dy * dy);
                if (fabsf(r - kWaveR) > kWaveBand) continue;
                if (pass == 1) {
                    float a = atan2f(dx, -dy) / 6.2831853f;
                    if (a < 0) a += 1;
                    // x (9 ビット)・y (9 ビット)・角度 (10 ビット)
                    _wavePos[n] = static_cast<uint32_t>(x) | (static_cast<uint32_t>(y) << 9) |
                                  (static_cast<uint32_t>(static_cast<int>(a * kWaveAngles) & (kWaveAngles - 1)) << 18);
                    _waveR[n]   = static_cast<uint16_t>(r * 8);
                }
                ++n;
            }
        }
        if (pass == 0) {
            _waveN   = n;
            _wavePos = static_cast<uint32_t*>(_alloc(sizeof(uint32_t) * n));
            _waveR   = static_cast<uint16_t*>(_alloc(sizeof(uint16_t) * n));
            if (!_wavePos || !_waveR) return false;
        }
    }
    {
        constexpr int kSlabs = 7;
        int bx0[kSlabs][2], bx1[kSlabs][2], by0[kSlabs][2], by1[kSlabs][2];
        for (int k = 0; k < kSlabs; ++k) {
            for (int side = 0; side < 2; ++side) {
                bx0[k][side] = by0[k][side] = 9999;
                bx1[k][side] = by1[k][side] = -1;
            }
        }
        for (int i = 0; i < _waveN; ++i) {
            const int x = _wavePos[i] & 511, y = (_wavePos[i] >> 9) & 511;
            const int k = std::min(kSlabs - 1, y * kSlabs / kSize), side = x < kC ? 0 : 1;
            bx0[k][side] = std::min(bx0[k][side], x);
            bx1[k][side] = std::max(bx1[k][side], x + 1);
            by0[k][side] = std::min(by0[k][side], y);
            by1[k][side] = std::max(by1[k][side], y + 1);
        }
        _nWaveRects = 0;
        for (int k = 0; k < kSlabs; ++k) {
            if (bx1[k][0] >= 0 && bx1[k][1] >= 0 && bx1[k][0] >= bx0[k][1] - 4) {  // 左右がつながる段は 1 つに
                _waveRects[_nWaveRects++] = {std::min(bx0[k][0], bx0[k][1]), std::min(by0[k][0], by0[k][1]),
                                             std::max(bx1[k][0], bx1[k][1]), std::max(by1[k][0], by1[k][1])};
                continue;
            }
            for (int side = 0; side < 2; ++side) {
                if (bx1[k][side] >= 0) _waveRects[_nWaveRects++] = {bx0[k][side], by0[k][side], bx1[k][side], by1[k][side]};
            }
        }
    }
    for (int i = 0; i < kWaveAngles; ++i) {
        _sinTab[i]    = sinf(6.2831853f * i / kWaveAngles);
        _waveCur[i]   = kWaveR;
        _waveEcho1[i] = kWaveR;
        _waveEcho2[i] = kWaveR;
    }
    for (int k = 0; k < kWaveHarm; ++k) _wavePhase[k] = k * 1.7f;
    // 線の色: ふだんは白。白っぽいところ (明るさ 160〜215 で切り替え) だけ黒 (オレンジやピンクの上は白のまま)
    for (int l = 0; l < 256; ++l) _lineLut[l] = static_cast<uint8_t>(255 * (1.0f - smooth(160.0f, 215.0f, static_cast<float>(l))));
    _likePop.x = 1;
    _mode      = Mode::Message;
    _bgDirty = _baseDirty = _fullSend = true;
    return true;
}

void GlassUi::end()
{
    for (Text* t : {&_title, &_artist, &_device, &_clock, &_hint, &_msg[0], &_msg[1], &_msg[2]}) freeText(*t);
    for (auto& d : _digits) freeText(d);
    freeText(_titleS);
    freeText(_artistS);
    if (_irisPos) _free(_irisPos);
    if (_irisT) _free(_irisT);
    if (_irisG1) _free(_irisG1);
    if (_wavePos) _free(_wavePos);
    if (_waveR) _free(_waveR);
    _wavePos = nullptr;
    _waveR   = nullptr;
    _irisPos = nullptr;
    _irisT   = nullptr;
    _irisG1  = nullptr;
    for (uint16_t** p : {&_bg, &_base, &_art, &_artFull, &_old}) {
        if (*p) _free(*p);
        *p = nullptr;
    }
    for (auto& g : _glow) {
        if (g) _free(g);
        g = nullptr;
    }
    if (_shade) _free(_shade);
    if (_w8) _free(_w8);
    if (_w8p) _free(_w8p);
    _w8p = nullptr;
    if (_lutLine) _free(_lutLine);
    _w8      = nullptr;
    _lutLine = nullptr;
    if (_glowMask) _free(_glowMask);
    _shade    = nullptr;
    _glowMask = nullptr;
}

void GlassUi::freeText(Text& t)
{
    if (t.m.a) _free(t.m.a);
    t = Text{};
}

// 文字を描いて縮める (面積で平均するので、ふちがなめらかになる)
bool GlassUi::makeText(Text& t, const char* s, int font, float f)
{
    freeText(t);
    if (!s || !*s || !_raster) return false;
    Mask raw;
    if (!_raster(_rasterCtx, s, font, &raw, _alloc) || !raw.a || raw.w <= 0 || raw.h <= 0) return false;
    const int w = std::max(1, static_cast<int>(ceilf(raw.w * f))), h = std::max(1, static_cast<int>(ceilf(raw.h * f)));
    float* tmp  = static_cast<float*>(_alloc(sizeof(float) * w * raw.h));
    t.m.a       = static_cast<uint8_t*>(_alloc(static_cast<size_t>(w) * h));
    if (!tmp || !t.m.a) {
        if (tmp) _free(tmp);
        _free(raw.a);
        freeText(t);
        return false;
    }
    t.m.w = w;
    t.m.h = h;
    for (int x = 0; x < w; ++x) {
        const float s0 = x / f, s1 = (x + 1) / f;
        for (int y = 0; y < raw.h; ++y) {
            float acc = 0;
            for (int sx = static_cast<int>(s0); sx < std::min(raw.w, static_cast<int>(ceilf(s1))); ++sx) {
                const float wgt = std::min(s1, sx + 1.0f) - std::max(s0, static_cast<float>(sx));
                if (wgt > 0) acc += wgt * raw.a[y * raw.w + sx];
            }
            tmp[y * w + x] = acc * f;
        }
    }
    for (int y = 0; y < h; ++y) {
        const float s0 = y / f, s1 = (y + 1) / f;
        for (int x = 0; x < w; ++x) {
            float acc = 0;
            for (int sy = static_cast<int>(s0); sy < std::min(raw.h, static_cast<int>(ceilf(s1))); ++sy) {
                const float wgt = std::min(s1, sy + 1.0f) - std::max(s0, static_cast<float>(sy));
                if (wgt > 0) acc += wgt * tmp[sy * w + x];
            }
            t.m.a[y * w + x] = static_cast<uint8_t>(clampf(acc * f + 0.5f, 0, 255));
        }
    }
    _free(tmp);
    _free(raw.a);
    return true;
}

// ============================================================================ 状態

namespace {
constexpr int kNoticeN       = 21;    // 案内の絵柄の粒の数 (21 x 21)
constexpr float kNoticeCell  = 9.0f;  // 1 粒の大きさ (px)
constexpr float kNoticeTop   = 70.0f;
constexpr float kNoticeQrTop = 40.0f;
constexpr int kFieldN        = 33;    // 背景の粒 (33 x 33)
constexpr uint8_t kNGreen[3] = {40, 120, 255};  // 案内の画面の色 (名前は昔の緑のまま。アイコンの青にそろえた)
constexpr uint8_t kNWhite[3] = {240, 245, 242};

// LED の粒の明るさ: 粒の中の位置 (中心からの u, v。0..0.5)。すき間と角の丸みで、ふちは 1px なめらかに
inline float ledAlpha(float u, float v, float cell, float gap = 0.09f, float rad = 0.27f)
{
    const float h  = 0.5f - gap;
    const float dx = std::max(u - (h - rad), 0.0f), dy = std::max(v - (h - rad), 0.0f);
    const float d  = sqrtf(dx * dx + dy * dy) - rad;
    return clampf(0.5f - d * cell, 0, 1);
}

// アプリの印 (ランチャーのアイコンと同じ絵柄): 白い波形の輪 + 外の紺の輪 + 青い ▷。
// 1 粒を 8 x 8 に分け、線がどれだけかかるかで色を決める (1 回だけ作る)。元の絵は SpotifyRemote/tools/make_icon_original.py の B
const uint8_t* logoCells()
{
    static uint8_t* rgb = nullptr;  // PSRAM に (内蔵メモリは Wi-Fi の設定ページに残す)
    if (rgb) return rgb;
#ifdef ESP_PLATFORM
    rgb = static_cast<uint8_t*>(heap_caps_malloc(kNoticeN * kNoticeN * 3, MALLOC_CAP_SPIRAM));
#else
    rgb = static_cast<uint8_t*>(malloc(kNoticeN * kNoticeN * 3));
#endif
    if (!rgb) {
        static uint8_t fallback[kNoticeN * kNoticeN * 3];
        rgb = fallback;
    }
    const float n = kNoticeN, c = n * 0.5f, u = n / 31.0f;  // アイコン (31 粒) の 1 粒 = u
    const float white[3] = {240, 248, 255}, navy[3] = {22, 44, 96}, blue[3] = {40, 120, 255};
    for (int i = 0; i < kNoticeN; ++i) {
        for (int j = 0; j < kNoticeN; ++j) {
            float acc[3] = {0, 0, 0};
            for (int sy = 0; sy < 8; ++sy) {
                for (int sx = 0; sx < 8; ++sx) {
                    const float x = j + (sx + 0.5f) / 8, y = i + (sy + 0.5f) / 8;
                    const float dx = x - c, dy = y - c, r = sqrtf(dx * dx + dy * dy), ang = atan2f(dy, dx);
                    const float rw = 10.6f * u + 1.3f * u * sinf(ang * 5 + 0.4f) * cosf(ang * 3);  // 白い波形
                    const float rn = 12.6f * u + 0.6f * u * sinf(ang * 7 + 1.3f) * cosf(ang * 3);  // 外の紺
                    const bool tri = x >= c - 3.2f * u && fabsf(dy) <= 4.6f * u * (c + 4.8f * u - x) / (8.0f * u);
                    const float* col = nullptr;
                    if (fabsf(r - rw) < 0.55f * u) {
                        col = white;
                    } else if (fabsf(r - rn) < 0.5f * u) {
                        col = navy;
                    } else if (tri) {
                        col = blue;
                    }
                    if (col) {
                        for (int k = 0; k < 3; ++k) acc[k] += col[k];
                    }
                }
            }
            uint8_t* o = rgb + (i * kNoticeN + j) * 3;
            for (int k = 0; k < 3; ++k) o[k] = static_cast<uint8_t>(std::min(255.0f, acc[k] / 64 * 1.35f));  // 縁の粒も光るように少し明るく
        }
    }
    return rgb;
}

// Wi-Fi の扇: 下の点から上へ 3 本の弧 (白)
void wifiCells(uint8_t* rgb)
{
    const float n = kNoticeN, c = (n - 1) / 2, by = n * 0.80f;
    for (int i = 0; i < kNoticeN; ++i) {
        for (int j = 0; j < kNoticeN; ++j) {
            const float dx = j - c, dy = i - by, r = sqrtf(dx * dx + dy * dy), a = atan2f(dx, -dy);
            bool on = r < 0.9f;
            if (fabsf(a) < 0.80f) {
                for (float R : {n * 0.25f, n * 0.47f, n * 0.69f}) on |= fabsf(r - R) < 0.62f;
            }
            uint8_t* o = rgb + (i * kNoticeN + j) * 3;
            if (on) {
                memcpy(o, kNWhite, 3);
            } else {
                o[0] = o[1] = o[2] = 0;
            }
        }
    }
}

// 鍵 (緑)
void keyCells(uint8_t* rgb)
{
    const float n = kNoticeN, c = (n - 1) / 2;
    for (int i = 0; i < kNoticeN; ++i) {
        for (int j = 0; j < kNoticeN; ++j) {
            const float dx = j - c, dy = i - c;
            bool on = fabsf(sqrtf((dx + n * 0.18f) * (dx + n * 0.18f) + dy * dy) - n * 0.16f) < 0.8f;
            on |= dx > -n * 0.02f && dx < n * 0.38f && fabsf(dy) < 0.8f;
            on |= (fabsf(dx - n * 0.28f) < 0.6f || fabsf(dx - n * 0.18f) < 0.6f) && dy > 0 && dy < n * 0.12f;
            uint8_t* o = rgb + (i * kNoticeN + j) * 3;
            if (on) {
                memcpy(o, kNGreen, 3);
            } else {
                o[0] = o[1] = o[2] = 0;
            }
        }
    }
}

// 粒の絵を LED 風に下地へ (明るいほうを残す)
void drawLed(uint16_t* dst, const uint8_t* rgb, int gw, int gh, float x0, float y0, float cell)
{
    const int ya = std::max(0, static_cast<int>(y0)), yb = std::min(kSize, static_cast<int>(y0 + gh * cell) + 1);
    const int xa = std::max(0, static_cast<int>(x0)), xb = std::min(kSize, static_cast<int>(x0 + gw * cell) + 1);
    for (int y = ya; y < yb; ++y) {
        const float fi = (y + 0.5f - y0) / cell;
        const int i    = static_cast<int>(fi);
        if (i < 0 || i >= gh) continue;
        const float v = fabsf(fi - i - 0.5f);
        for (int x = xa; x < xb; ++x) {
            const float fj = (x + 0.5f - x0) / cell;
            const int j    = static_cast<int>(fj);
            if (j < 0 || j >= gw) continue;
            const uint8_t* c = rgb + (i * gw + j) * 3;
            if (c[0] < 4 && c[1] < 4 && c[2] < 4) continue;
            const float a = ledAlpha(fabsf(fj - j - 0.5f), v, cell);
            if (a <= 0.01f) continue;
            int r, g, b;
            unpack(dst[y * kSize + x], r, g, b);
            dst[y * kSize + x] = pack(std::max(r, static_cast<int>(c[0] * a)), std::max(g, static_cast<int>(c[1] * a)),
                                      std::max(b, static_cast<int>(c[2] * a)));
        }
    }
}
}  // namespace

void GlassUi::showNotice(Notice icon, const char* l1, const char* l2, const char* l3, bool big)
{
    const char* lines[3] = {l1, l2, l3};
    bool changed         = _mode != Mode::Message || icon != _notice || big != _noticeBig;
    const bool remake    = big != _noticeBig;
    _notice              = icon;
    _noticeBig           = big;
    for (int i = 0; i < 3; ++i) {
        const char* s = lines[i] ? lines[i] : "";
        if (!remake && strcmp(s, _msgStr[i]) == 0 && _msg[i].m.a) continue;
        strncpy(_msgStr[i], s, sizeof(_msgStr[i]) - 1);
        _msgStr[i][sizeof(_msgStr[i]) - 1] = 0;
        if (i == 0) {
            makeText(_msg[0], _msgStr[0], kFontJp40Bold, big ? 1.0f : 0.7f);
        } else {
            makeText(_msg[i], _msgStr[i], kFontJp40, (big || i == 2) ? 0.45f : 0.5f);
        }
        changed = true;
    }
    if (changed) {
        _mode      = Mode::Message;
        _baseDirty = true;
    }
}

void GlassUi::setNoticeQr(const uint8_t* modules, int n)
{
    if (!modules || n <= 0 || n > 57) return;
    if (n == _qrN && memcmp(modules, _qrMods, n * n) == 0) return;
    memcpy(_qrMods, modules, n * n);
    _qrN = n;
    if (_mode == Mode::Message && _notice == Notice::Qr) _baseDirty = true;
}

// 案内の下地: 黒に、ドット絵の絵柄 (QR は白い板に黒い丸い粒) と白い文字。背景の粒は毎コマ noticeFrame で重ねる
void GlassUi::drawNoticeBase()
{
    memset(_base, 0, static_cast<size_t>(kSize) * kSize * 2);
    const Rect all{0, 0, kSize, kSize};
    const int cx = kC;
    const float iconW = kNoticeN * kNoticeCell, ix0 = (kSize - iconW) / 2;
    float ty[3]       = {323, 355, 381};
    if (_notice == Notice::Qr && _qrN > 0) {
        // QR: 白い板 (すき間なし) に、黒い模様を 1 粒ずつ丸い黒い粒で (LED の粒のすき間があると、近くで写すと読めなかった)。
        // まわりに 3 粒の余白。明るさは少し落とす
        const int qn      = _qrN + 6;
        const float cell  = std::min(6.2f, 217.0f / qn);
        const float W     = qn * cell, x0 = (kSize - W) / 2, y0 = kNoticeQrTop;
        const float R     = cell * 1.5f;
        for (int y = static_cast<int>(y0); y < std::min(kSize, static_cast<int>(y0 + W) + 1); ++y) {
            for (int x = static_cast<int>(x0); x < std::min(kSize, static_cast<int>(x0 + W) + 1); ++x) {
                const float ox = std::max(fabsf(x + 0.5f - (x0 + W / 2)) - (W / 2 - R), 0.0f);
                const float oy = std::max(fabsf(y + 0.5f - (y0 + W / 2)) - (W / 2 - R), 0.0f);
                const float plate = clampf(R - sqrtf(ox * ox + oy * oy) + 0.5f, 0, 1);
                if (plate <= 0) continue;
                const float fi = (y + 0.5f - y0) / cell, fj = (x + 0.5f - x0) / cell;
                const int i = static_cast<int>(fi) - 3, j = static_cast<int>(fj) - 3;
                float dark = 0;
                if (i >= 0 && i < _qrN && j >= 0 && j < _qrN && _qrMods[i * _qrN + j]) {
                    dark = ledAlpha(fabsf(fj - static_cast<int>(fj) - 0.5f), fabsf(fi - static_cast<int>(fi) - 0.5f), cell, 0.06f, 0.30f);
                }
                const int l = static_cast<int>(225 * plate * (1 - dark));
                _base[y * kSize + x] = pack(l, l, l);
            }
        }
        _holeType = 2;
        _holeCx = kSize / 2.0f, _holeCy = y0 + W / 2, _holeW = W + 26, _holeH = W + 26, _holeSoft = 8;
        ty[0] = 291, ty[1] = 328, ty[2] = 356;
    } else {
        uint8_t rgb[kNoticeN * kNoticeN * 3];
        if (_notice == Notice::Wifi) {
            wifiCells(rgb);
        } else if (_notice == Notice::Key) {
            keyCells(rgb);
        } else {
            memcpy(rgb, logoCells(), sizeof(rgb));
        }
        drawLed(_base, rgb, kNoticeN, kNoticeN, ix0, kNoticeTop, kNoticeCell);
        if (_notice == Notice::Key) {
            _holeType = 2;
            _holeCx = kSize / 2.0f, _holeCy = kNoticeTop + iconW / 2, _holeW = iconW * 0.8f, _holeH = iconW * 0.3f, _holeSoft = 26;
        } else {
            _holeType = 1;
            _holeCx = kSize / 2.0f, _holeCy = kNoticeTop + iconW / 2, _holeW = iconW * 0.44f, _holeSoft = 22;
        }
        if (_noticeBig) ty[0] = 324, ty[1] = 365, ty[2] = 391;
    }
    blitText(_base, kSize, _msg[0], cx, static_cast<int>(ty[0]), 0, 255, 255, 255, 1.0f, all);
    blitText(_base, kSize, _msg[1], cx, static_cast<int>(ty[1]), 0, 255, 255, 255, 0.85f, all);
    blitText(_base, kSize, _msg[2], cx, static_cast<int>(ty[2]), 0, 255, 255, 255, 0.6f, all);
}

// 案内の 1 コマ: 背景の粒 (33 x 33、うっすら白 + 斜めに流れる緑の帯) を絵柄の下だけ消して敷き、下地と明るいほうを取る。3 つの粒も
void GlassUi::noticeFrame(uint16_t* f, int stride, float dt)
{
    _noticeT += dt;
    if (!_fieldReady) {
        const float cell = static_cast<float>(kSize) / kFieldN;
        for (int x = 0; x < kSize; ++x) {
            const float fx = (x + 0.5f) / cell;
            const int i    = std::min(kFieldN - 1, static_cast<int>(fx));
            _fieldCellX[x] = static_cast<uint8_t>(i);
            _fieldQX[x]    = static_cast<uint8_t>(std::min(31, static_cast<int>((fx - i) * 32)));
        }
        for (int qy = 0; qy < 32; ++qy) {
            for (int qx = 0; qx < 32; ++qx) {
                _fieldA[qy * 32 + qx] = static_cast<uint8_t>(ledAlpha(fabsf((qx + 0.5f) / 32 - 0.5f), fabsf((qy + 0.5f) / 32 - 0.5f), cell) * 255);
            }
        }
        _fieldReady = true;
    }
    // 粒ごとの色 (このコマ)
    const float pos = fmodf(_noticeT / 2.6f, 1.0f) * (2 * kFieldN + 12) - 6;
    for (int i = 0; i < kFieldN; ++i) {
        for (int j = 0; j < kFieldN; ++j) {
            const float t = (i + j - pos) / 3.2f;
            const float s = expf(-t * t) * 0.28f;
            uint8_t* o    = _ledRGB + (i * kFieldN + j) * 3;  // ドット絵用の入れ物を借りる (案内の画面では使わない)
            o[0]          = static_cast<uint8_t>(12 + kNGreen[0] * s);
            o[1]          = static_cast<uint8_t>(12 + kNGreen[1] * s);
            o[2]          = static_cast<uint8_t>(12 + kNGreen[2] * s);
        }
    }
    struct Ctx {
        GlassUi* ui;
        uint16_t* f;
        int stride;
    } c{this, f, stride};
    split(
        [](void* arg, int part) {
            const auto* c = static_cast<const Ctx*>(arg);
            GlassUi* ui   = c->ui;
            for (int y = part ? kSize / 2 : 0; y < (part ? kSize : kSize / 2); ++y) {
                const int ci   = std::min(kFieldN - 1, y * kFieldN / kSize);
                const int qy   = std::min(31, (y * kFieldN * 32 / kSize) & 31);
                uint16_t* row  = c->f + y * c->stride;
                const uint16_t* b = ui->_base + y * kSize;
                const float dy = y + 0.5f - ui->_holeCy;
                for (int x = 0; x < kSize; ++x) {
                    const uint8_t* col = ui->_ledRGB + (ci * kFieldN + ui->_fieldCellX[x]) * 3;
                    int a              = ui->_fieldA[qy * 32 + ui->_fieldQX[x]];
                    if (ui->_holeType == 1) {  // 丸く消す
                        const float dx = x + 0.5f - ui->_holeCx;
                        const float d  = sqrtf(dx * dx + dy * dy) - ui->_holeW;
                        if (d < ui->_holeSoft) a = static_cast<int>(a * clampf(d / ui->_holeSoft, 0, 1));
                    } else if (ui->_holeType == 2) {  // 角の丸い四角に消す
                        const float ox = std::max(fabsf(x + 0.5f - ui->_holeCx) - ui->_holeW / 2, 0.0f);
                        const float oy = std::max(fabsf(dy) - ui->_holeH / 2, 0.0f);
                        if (ox < ui->_holeSoft && oy < ui->_holeSoft) a = static_cast<int>(a * clampf(sqrtf(ox * ox + oy * oy) / ui->_holeSoft, 0, 1));
                    }
                    int r, g, bb;
                    unpack(b[x], r, g, bb);
                    row[x] = pack(std::max(r, col[0] * a >> 8), std::max(g, col[1] * a >> 8), std::max(bb, col[2] * a >> 8));
                }
            }
        },
        &c);
    // 3 つの粒 (順に光る)。QR の画面では出さない
    if (_notice != Notice::Qr) {
        const float cell = 12.0f, x0 = (kSize - 5 * cell) / 2, y0 = 276.0f;
        const int on     = static_cast<int>(_noticeT / 0.35f) % 3;
        for (int k = 0; k < 3; ++k) {
            const float lv = k == on ? 1.0f : 0.28f;
            const float bx = x0 + 2 * k * cell;
            for (int y = static_cast<int>(y0); y < static_cast<int>(y0 + cell) + 1; ++y) {
                for (int x = static_cast<int>(bx); x < static_cast<int>(bx + cell) + 1; ++x) {
                    const float a = ledAlpha(fabsf((x + 0.5f - bx) / cell - 0.5f), fabsf((y + 0.5f - y0) / cell - 0.5f), cell) * lv;
                    if (a <= 0.01f) continue;
                    uint16_t* p = &f[y * stride + x];
                    int r, g, b;
                    unpack(*p, r, g, b);
                    *p = pack(std::max(r, static_cast<int>(kNWhite[0] * a)), std::max(g, static_cast<int>(kNWhite[1] * a)),
                              std::max(b, static_cast<int>(kNWhite[2] * a)));
                }
            }
        }
    }
}

void GlassUi::showMessage(const char* l1, const char* l2, const char* l3)
{
    showNotice(Notice::Logo, l1, l2, l3, false);
}

void GlassUi::showIdle(const char* hint)
{
    if (!hint) hint = "";
    bool changed = _mode != Mode::Idle;
    if (strcmp(hint, _hintStr) != 0) {
        strncpy(_hintStr, hint, sizeof(_hintStr) - 1);
        _hintStr[sizeof(_hintStr) - 1] = 0;
        makeText(_hint, _hintStr, kFontJp40, 0.42f);
        changed = true;
    }
    if (changed) {
        _mode      = Mode::Idle;
        _baseDirty = true;
    }
}

void GlassUi::showPlayer()
{
    if (_mode == Mode::Player) return;
    _mode      = Mode::Player;
    _ambient   = true;  // 操作の画面はなくし、いつもジャケットいっぱいの画面
    _idleT     = 0;
    _baseDirty = true;
}

void GlassUi::setClock(int hour, int minute)
{
    if (hour == _hour && minute == _minute) return;
    _hour   = hour;
    _minute = minute;
    char buf[24];
    if (hour < 0) {
        snprintf(buf, sizeof(buf), "--:--");
    } else {
        snprintf(buf, sizeof(buf), "%d:%02d", hour, minute);
    }
    makeText(_clock, buf, kFontDigits72, 1.0f);
    if (_mode == Mode::Idle) _baseDirty = true;
}

void GlassUi::setTrack(const char* title, const char* artist)
{
    if (!title) title = "";
    if (!artist) artist = "";
    if (strcmp(title, _titleStr) == 0 && strcmp(artist, _artistStr) == 0) return;
    strncpy(_titleStr, title, sizeof(_titleStr) - 1);
    _titleStr[sizeof(_titleStr) - 1] = 0;
    strncpy(_artistStr, artist, sizeof(_artistStr) - 1);
    _artistStr[sizeof(_artistStr) - 1] = 0;
    makeText(_title, _titleStr, kFontJp40Bold, 0.62f);
    makeText(_artist, _artistStr, kFontJp40, 0.47f);
    makeText(_titleS, _titleStr, kFontJp40Bold, 0.42f);
    makeText(_artistS, _artistStr, kFontJp40, 0.36f);
    _ambientT       = 0;
    _uiDim          = 1.0f;
    _titleMarquee   = _title.m.w > kWin;
    _artistMarquee  = _artist.m.w > kWin;
    _titleSMarquee  = _titleS.m.w > kAWin;
    _artistSMarquee = _artistS.m.w > kAWin;
    const bool lite = _titleLite && _mode == Mode::Player && _ambient;  // 文字の欄だけで出す (画面全体は描き直さない)
    if (_mode == Mode::Player && !lite) _fadeReq = true;
    _fadeDur       = 0.5f;
    _titleIn       = 0;
    _scrollT       = 0;
    _titleScroll = _artistScroll = 0;
    if (_mode == Mode::Player && !lite) _baseDirty = true;
}

void GlassUi::setDevice(const char* name)
{
    if (!name) name = "";
    if (strcmp(name, _deviceStr) == 0) return;
    strncpy(_deviceStr, name, sizeof(_deviceStr) - 1);
    _deviceStr[sizeof(_deviceStr) - 1] = 0;
    makeText(_device, _deviceStr, kFontJp40Bold, 0.36f);
    if (_mode == Mode::Player && !_ambient) _baseDirty = true;
}

void GlassUi::setArt(const uint16_t* rgb565, int w, int h)
{
    _hasArt = rgb565 && w > 0 && h > 0;
    if (_hasArt) {
        // 220px のジャケット (面積で平均)
        const float fx = static_cast<float>(w) / kArtS, fy = static_cast<float>(h) / kArtS;
        for (int y = 0; y < kArtS; ++y) {
            const int y0 = static_cast<int>(y * fy), y1 = std::max(y0 + 1, static_cast<int>((y + 1) * fy));
            for (int x = 0; x < kArtS; ++x) {
                const int x0 = static_cast<int>(x * fx), x1 = std::max(x0 + 1, static_cast<int>((x + 1) * fx));
                int r = 0, g = 0, b = 0, n = 0;
                for (int sy = y0; sy < std::min(h, y1); ++sy) {
                    for (int sx = x0; sx < std::min(w, x1); ++sx) {
                        const uint16_t c = rgb565[sy * w + sx];
                        r += (c >> 11) & 31;
                        g += (c >> 5) & 63;
                        b += c & 31;
                        ++n;
                    }
                }
                n                = std::max(1, n);
                _art[y * kArtS + x] = static_cast<uint16_t>(((r / n) << 11) | ((g / n) << 5) | (b / n));
            }
        }
        // 静かな画面用の 466px (面積で平均)。上半分・下半分を 2 つのコアで
        {
            struct ArtCtx {
                GlassUi* ui;
                const uint16_t* src;
                int w, h;
            } ac{this, rgb565, w, h};
            split(
                [](void* arg, int part) {
                    const auto* c = static_cast<const ArtCtx*>(arg);
                    const int w = c->w, h = c->h;
                    const float gx = static_cast<float>(w) / kSize, gy = static_cast<float>(h) / kSize;
                    for (int y = part ? kSize / 2 : 0; y < (part ? kSize : kSize / 2); ++y) {
                        const int y0 = static_cast<int>(y * gy), y1 = std::max(y0 + 1, static_cast<int>((y + 1) * gy));
                        for (int x = 0; x < kSize; ++x) {
                            const int x0 = static_cast<int>(x * gx), x1 = std::max(x0 + 1, static_cast<int>((x + 1) * gx));
                            int r = 0, g = 0, b = 0, n = 0;
                            for (int sy = y0; sy < std::min(h, y1); ++sy) {
                                for (int sx = x0; sx < std::min(w, x1); ++sx) {
                                    const uint16_t px = c->src[sy * w + sx];
                                    r += (px >> 11) & 31;
                                    g += (px >> 5) & 63;
                                    b += px & 31;
                                    ++n;
                                }
                            }
                            n = std::max(1, n);
                            c->ui->_artFull[y * kSize + x] = static_cast<uint16_t>(((r / n) << 11) | ((g / n) << 5) | (b / n));
                        }
                    }
                },
                &ac);
        }
        // 背景の元: 48x48 に縮めて、ぼかす
        constexpr int S = 48;
        for (int y = 0; y < S; ++y) {
            for (int x = 0; x < S; ++x) {
                float r = 0, g = 0, b = 0;
                int n   = 0;
                for (int sy = y * h / S; sy < (y + 1) * h / S; ++sy) {
                    for (int sx = x * w / S; sx < (x + 1) * w / S; ++sx) {
                        const uint16_t c = rgb565[sy * w + sx];
                        r += ((c >> 11) & 31) * (255.0f / 31);
                        g += ((c >> 5) & 63) * (255.0f / 63);
                        b += (c & 31) * (255.0f / 31);
                        ++n;
                    }
                }
                n                        = std::max(1, n);
                _small[(y * S + x) * 3]     = r / n;
                _small[(y * S + x) * 3 + 1] = g / n;
                _small[(y * S + x) * 3 + 2] = b / n;
            }
        }
        // 作業用 (約 28KB) は PSRAM に取る (描くタスクのスタックは小さいので、そこには置かない)
        float* tmp = static_cast<float*>(_alloc(sizeof(float) * S * S * 3));
        for (int pass = 0; tmp && pass < 4; ++pass) {  // 箱のぼかし (横・縦) を 4 回
            for (int y = 0; y < S; ++y) {
                for (int x = 0; x < S; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        float acc = 0;
                        for (int k = -3; k <= 3; ++k) acc += _small[(y * S + clampi(x + k, 0, S - 1)) * 3 + c];
                        tmp[(y * S + x) * 3 + c] = acc / 7;
                    }
                }
            }
            for (int y = 0; y < S; ++y) {
                for (int x = 0; x < S; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        float acc = 0;
                        for (int k = -3; k <= 3; ++k) acc += tmp[(clampi(y + k, 0, S - 1) * S + x) * 3 + c];
                        _small[(y * S + x) * 3 + c] = acc / 7;
                    }
                }
            }
        }
        if (tmp) _free(tmp);
        // ふちの光の色: いちばんあざやかで明るいところ
        float best = -1;
        for (int i = 0; i < S * S; ++i) {
            const float r = _small[i * 3], g = _small[i * 3 + 1], b = _small[i * 3 + 2];
            const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
            const float score = (mx - mn) * mx;
            if (score > best && mx > 1) {
                best       = score;
                const float k = 255.0f / mx;
                _accent[0] = static_cast<uint8_t>(clampf(r * k, 0, 255));
                _accent[1] = static_cast<uint8_t>(clampf(g * k, 0, 255));
                _accent[2] = static_cast<uint8_t>(clampf(b * k, 0, 255));
            }
        }
        if (best < 20 * 255) {  // 色がほとんどない: 差し色の青
            _accent[0] = kGreen[0];
            _accent[1] = kGreen[1];
            _accent[2] = kGreen[2];
        }
        _hasSmall = true;
    } else {
        _hasSmall  = false;
        _accent[0] = kGreen[0];
        _accent[1] = kGreen[1];
        _accent[2] = kGreen[2];
    }
    _bgDirty   = true;
    _baseDirty = true;
    if (_mode == Mode::Player) _fadeReq = true;
    _fadeDur = 0.5f;
    _blurFade = false;
    if (_hasArt && _ledGrid == kPixGrid && _mode == Mode::Player) {  // ドット絵 → (ぼやけて) → 本物 を 1 回で
        memcpy(_blurRGB, _ledRGB, sizeof(_blurRGB));
        for (int x = 0; x < kSize; ++x) {  // 16 マスの色をなめらかにつなぐための、x ごとの左の粒と割合
            const float f = clampf((x + 0.5f) * kPixGrid / kSize - 0.5f, 0, kPixGrid - 1.001f);
            _blurX0[x]    = static_cast<int16_t>(f);
            _blurXT[x]    = static_cast<uint8_t>((f - static_cast<int>(f)) * 255);
        }
        _blurFade = true;
        _fadeDur  = 0.7f;
    }
    _ledGrid     = 0;
    _refineStage = 0;
}

void GlassUi::setArtPixel(const PixelArt& p)
{
    for (int i = 0; i < kPixGrid * kPixGrid; ++i) {
        const uint16_t c = p.pal[p.idx[i] & (kPixColors - 1)];
        _ledRGB[i * 3]     = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31);
        _ledRGB[i * 3 + 1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
        _ledRGB[i * 3 + 2] = static_cast<uint8_t>((c & 31) * 255 / 31);
    }
    _ledGrid     = kPixGrid;
    _refineStage = 0;
    _baseDirty   = true;
    if (_mode == Mode::Player) _fadeReq = true;
    _fadeDur = 0.35f;
}

// LED の粒: 1 粒の中の位置 (32 段) ごとの明るさの表と、x ごとの粒の番号。粒の間は暗いすき間 (幅 16%、角の丸み 28%)
void GlassUi::prepLed()
{
    if (_ledLutGrid == _ledGrid || _ledGrid <= 0) return;
    _ledLutGrid   = _ledGrid;
    const float c = static_cast<float>(kSize) / _ledGrid;  // 1 粒の大きさ (px)
    for (int x = 0; x < kSize; ++x) {
        const float f  = (x + 0.5f) / c;
        const int i    = std::min(_ledGrid - 1, static_cast<int>(f));
        _ledCellX[x]   = static_cast<uint8_t>(i);
        _ledQX[x]      = static_cast<uint8_t>(std::min(31, static_cast<int>((f - i) * 32)));
    }
    constexpr float gap = 0.16f, rad = 0.28f;
    const float h       = 0.5f - gap / 2;
    for (int qy = 0; qy < 32; ++qy) {
        for (int qx = 0; qx < 32; ++qx) {
            const float u = fabsf((qx + 0.5f) / 32 - 0.5f), v = fabsf((qy + 0.5f) / 32 - 0.5f);
            const float dx = std::max(u - (h - rad), 0.0f), dy = std::max(v - (h - rad), 0.0f);
            const float d  = sqrtf(dx * dx + dy * dy) - rad;
            _ledA[qy * 32 + qx] = static_cast<uint8_t>(clampf(0.5f - d * c, 0, 1) * 255);
        }
    }
}

void GlassUi::setPlayback(bool playing, uint32_t posMs, uint32_t durMs, bool liked, bool canLike)
{
    if (_forceT <= 0) _playing = playing;
    _posMs   = posMs;
    _durMs   = durMs;
    _canLike = canLike;
    if (_likeForceT <= 0) _liked = liked;
}

void GlassUi::setBeat(float bpm, float energy, float offsetMs)
{
    _bpm          = (bpm >= 40 && bpm <= 260) ? bpm : 0.0f;
    _beatEnergy   = clampf(energy, 0, 1);
    _beatOffsetMs = offsetMs;
    updateBpmText();
}

void GlassUi::setMicBeat(float bpm, float beatAgoSec, float energy)
{
    const float was = _micBpm;
    _micBpm         = (bpm >= 40 && bpm <= 260) ? bpm : 0.0f;
    _micAgo         = beatAgoSec;
    _micEnergy      = clampf(energy * 1.2f, 0, 1);
    if (was <= 0 && _micBpm > 0) _micBeatF = beatAgoSec * _micBpm / 60.0f;  // 聞こえた拍から始める
    updateBpmText();
}

void GlassUi::updateBpmText()
{
    const int shown = static_cast<int>(lroundf(_bpm > 0 ? _bpm : _micBpm));
    if (shown != _bpmShown) {
        _bpmShown       = shown;
        _bpmTextChanged = true;
        char buf[24];
        snprintf(buf, sizeof(buf), "%d BPM", shown);
        if (shown > 0) {
            makeText(_bpmText, buf, kFontJp40, 0.34f);
        } else {
            freeText(_bpmText);
        }
    }
}

float GlassUi::cometHead(float u) const
{
    const float e = 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u);  // すぐ速く、最後にゆっくり止まる
    return (_fx == kFxNext ? e : 1.0f - e) * kWaveAngles;
}

bool GlassUi::skipFxActive() const
{
    return (_fx == kFxNext || _fx == kFxPrev) && _fxT < fxDur();
}

float GlassUi::fxDur() const
{
    switch (_fx) {
        case kFxPlay:
        case kFxPause: return 0.9f;
        case kFxNext:
        case kFxPrev: return 0.85f;  // 短く、きびきび (1.3 秒だと重く見えた)
        case kFxVolUp:
        case kFxVolDown: return 1.6f;
        case kFxLike:
        case kFxUnlike: return 1.0f;
        default: return 0;
    }
}

// 細い白い線 (まわりに暗いにじみ。どんな絵の上でも見える)
void GlassUi::stroke(uint16_t* f, int stride, float ax, float ay, float bx, float by, float w, float alpha)
{
    if (alpha <= 0.01f) return;
    const int x0 = std::max(0, static_cast<int>(std::min(ax, bx) - w - 6)), x1 = std::min(kSize - 1, static_cast<int>(std::max(ax, bx) + w + 6));
    const int y0 = std::max(0, static_cast<int>(std::min(ay, by) - w - 6)), y1 = std::min(kSize - 1, static_cast<int>(std::max(ay, by) + w + 6));
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float d = segDist(x + 0.5f, y + 0.5f, ax, ay, bx, by);
            uint16_t* p   = &f[y * stride + x];
            const float sh = 0.5f * clampf(1.0f - (d - w / 2) / 5.0f, 0, 1) * alpha;
            if (sh > 0.01f) blendPx(p, 0, 0, 0, static_cast<int>(sh * 256));
            const float c = clampf(w / 2 + 0.5f - d, 0, 1) * alpha;
            if (c > 0.01f) blendPx(p, 255, 255, 255, static_cast<int>(c * 256));
        }
    }
}

// 再生・停止・曲送り・音量のエフェクト (画面の真ん中の細い線の印、輪、音量のゲージ)。彗星と波形のふくらみは drawWave で
int GlassUi::fxRects(Rect* out) const
{
    int n     = 0;
    auto push = [&](float x0, float y0, float x1, float y1) {
        if (n >= kMaxFx) return;
        const Rect r{std::max(0, static_cast<int>(floorf(x0))), std::max(0, static_cast<int>(floorf(y0))),
                     std::min(kSize, static_cast<int>(ceilf(x1)) + 1), std::min(kSize, static_cast<int>(ceilf(y1)) + 1)};
        if (r.x1 > r.x0 && r.y1 > r.y0) out[n++] = r;
    };
    // 輪: 横の帯に切って、左右の弧を囲む (大きな輪でも画面全体にならない)
    auto ring = [&](float cx, float cy, float rho, float pad) {
        const float ro = rho + pad, ri = std::max(0.0f, rho - pad);
        constexpr int kBands = 8;
        for (int b = 0; b < kBands; ++b) {
            const float y0 = cy - ro + 2 * ro * b / kBands, y1 = cy - ro + 2 * ro * (b + 1) / kBands;
            const float dmin = (y0 <= cy && y1 >= cy) ? 0.0f : std::min(fabsf(y0 - cy), fabsf(y1 - cy));
            const float dmax = std::max(fabsf(y0 - cy), fabsf(y1 - cy));
            const float wo   = fastSqrt(std::max(0.0f, ro * ro - dmin * dmin));
            const float wi   = dmax < ri ? fastSqrt(ri * ri - dmax * dmax) : 0.0f;
            if (wi > 4) {
                push(cx - wo - 1, y0 - 1, cx - wi + 1, y1 + 1);
                push(cx + wi - 1, y0 - 1, cx + wo + 1, y1 + 1);
            } else {
                push(cx - wo - 1, y0 - 1, cx + wo + 1, y1 + 1);
            }
        }
    };
    const float t = _fxT, T = fxDur();
    if (_fx == kFxNone || t >= T) return 0;
    const float cx = kC + _ox + 0.5f, cy = kC + _oy + 0.5f;
    const float u = t / T, ease = 1.0f - (1.0f - u) * (1.0f - u);
    if (_fx == kFxPlay || _fx == kFxPause) {
        ring(cx, cy, _fx == kFxPlay ? 22.0f + (kWaveR - 22.0f) * ease : kWaveR - (kWaveR - 22.0f) * ease, 6);
        push(cx - 36, cy - 36, cx + 36, cy + 36);
    }
    if (_fx == kFxLike || _fx == kFxUnlike) {
        ring(cx, cy, _fx == kFxLike ? 22.0f + (kWaveR - 30.0f) * ease : (kWaveR - 30.0f) - (kWaveR - 52.0f) * ease, 7);
        if (_fx == kFxLike) push(cx - 34, cy - 30, cx + 34, cy + 30);
    }
    if (_fx == kFxNext || _fx == kFxPrev) {
        if (_skipStyle == 0 && _cometBox.x1 > _cometBox.x0) push(_cometBox.x0 - 2, _cometBox.y0 - 2, _cometBox.x1 + 2, _cometBox.y1 + 2);  // 光の筋 (drawFx が描いた所)
        push(cx - 84, cy - 26, cx + 84, cy + 26);
    }
    if ((_fx == kFxVolUp || _fx == kFxVolDown) && _volume >= 0) {
        const float R = kWaveR - 26.0f;
        push(cx + 0.64f * R - 8, cy - 0.77f * R - 8, cx + R + 8, cy + 0.77f * R + 8);
        const int dh = _digits[0].m.h;
        push(cx + R - 30 - 4 * _digits[0].m.w - 4, cy - dh / 2.0f - 3, cx + R - 30 + 4, cy + dh / 2.0f + 4);
    }
    return n;
}

void GlassUi::drawFx(uint16_t* f, int stride)
{
    const float t = _fxT, T = fxDur();
    if (_fx == kFxNone || t >= T) return;
    const float cx = kC + _ox + 0.5f, cy = kC + _oy + 0.5f;
    const float u  = t / T;
    const float ease = 1.0f - (1.0f - u) * (1.0f - u);
    const float a  = clampf(t / 0.08f, 0, 1) * clampf((T - t) / (T * 0.45f), 0, 1);  // すっと出て、ゆっくり消える
    // 再生: 中心から細い輪が広がって波形へ。停止: 波形から中心へ吸いこまれる
    if (_fx == kFxPlay || _fx == kFxPause) {
        const float rho = _fx == kFxPlay ? 22.0f + (kWaveR - 22.0f) * ease : kWaveR - (kWaveR - 22.0f) * ease;
        const float ra  = (1.0f - u) * (1.0f - u);
        for (int y = std::max(0, static_cast<int>(cy - rho - 4)); y < std::min(kSize, static_cast<int>(cy + rho + 5)); ++y) {
            const float dy = y + 0.5f - cy;
            if (fabsf(dy) > rho + 4) continue;
            const float h1 = fastSqrt(std::max(0.0f, (rho + 4) * (rho + 4) - dy * dy));
            const float h0 = fabsf(dy) < rho - 4 ? fastSqrt((rho - 4) * (rho - 4) - dy * dy) : 0.0f;
            const int sp[4] = {std::max(0, static_cast<int>(cx - h1)), std::min(kSize, static_cast<int>(cx - h0) + 1),
                               std::max(0, static_cast<int>(cx + h0)), std::min(kSize, static_cast<int>(cx + h1) + 1)};
            for (int k = 0; k < 4; k += 2) {
                for (int x = sp[k]; x < sp[k + 1]; ++x) {
                    const float dx = x + 0.5f - cx;
                    const float d  = fabsf(fastSqrt(dx * dx + dy * dy) - rho);
                    const float c  = (clampf(1.6f - d, 0, 1) + 0.25f * clampf(1.0f - d / 4.0f, 0, 1)) * ra;
                    if (c > 0.01f) blendPx(&f[y * stride + x], 255, 255, 255, static_cast<int>(std::min(1.0f, c) * 256));
                }
            }
        }
        // 真ん中の印 (細い線)
        const float sc = 36.0f * (0.85f + 0.3f * ease);
        if (_fx == kFxPlay) {
            const float p0x = cx - 0.34f * sc, p0y = cy - 0.48f * sc, p1x = cx - 0.34f * sc, p1y = cy + 0.48f * sc, p2x = cx + 0.52f * sc, p2y = cy;
            stroke(f, stride, p0x, p0y, p1x, p1y, 2.9f, a);
            stroke(f, stride, p1x, p1y, p2x, p2y, 2.9f, a);
            stroke(f, stride, p2x, p2y, p0x, p0y, 2.9f, a);
        } else {
            stroke(f, stride, cx - 0.2f * sc, cy - 0.46f * sc, cx - 0.2f * sc, cy + 0.46f * sc, 2.9f, a);
            stroke(f, stride, cx + 0.2f * sc, cy - 0.46f * sc, cx + 0.2f * sc, cy + 0.46f * sc, 2.9f, a);
        }
    }
    // ♥: 付けたら緑の輪が中心から広がり、細い線の緑のチェックが浮かぶ。外したら灰の輪が中心へ吸いこまれる
    if (_fx == kFxLike || _fx == kFxUnlike) {
        const bool like = _fx == kFxLike;
        const float rho = like ? 22.0f + (kWaveR - 30.0f) * ease : (kWaveR - 30.0f) - (kWaveR - 52.0f) * ease;
        const float ra  = (1.0f - u) * (1.0f - u);
        const int col[3] = {like ? kGreen[0] : 200, like ? kGreen[1] : 200, like ? kGreen[2] : 200};
        for (int y = std::max(0, static_cast<int>(cy - rho - 4)); y < std::min(kSize, static_cast<int>(cy + rho + 5)); ++y) {
            const float dy = y + 0.5f - cy;
            if (fabsf(dy) > rho + 4) continue;
            const float h1 = fastSqrt(std::max(0.0f, (rho + 4) * (rho + 4) - dy * dy));
            const float h0 = fabsf(dy) < rho - 4 ? fastSqrt((rho - 4) * (rho - 4) - dy * dy) : 0.0f;
            const int sp[4] = {std::max(0, static_cast<int>(cx - h1)), std::min(kSize, static_cast<int>(cx - h0) + 1),
                               std::max(0, static_cast<int>(cx + h0)), std::min(kSize, static_cast<int>(cx + h1) + 1)};
            for (int k = 0; k < 4; k += 2) {
                for (int x = sp[k]; x < sp[k + 1]; ++x) {
                    const float dx = x + 0.5f - cx;
                    const float d  = fabsf(fastSqrt(dx * dx + dy * dy) - rho);
                    const float c  = (clampf(1.8f - d, 0, 1) + 0.3f * clampf(1.0f - d / 5.0f, 0, 1)) * ra;
                    if (c > 0.01f) blendPx(&f[y * stride + x], col[0], col[1], col[2], static_cast<int>(std::min(1.0f, c) * 256));
                }
            }
        }
        if (like) {  // 細い線の緑のチェック (影は暗く)
            const float sc = 34.0f * (0.85f + 0.3f * ease);
            const float pts[3][2] = {{cx - 0.42f * sc, cy + 0.02f * sc}, {cx - 0.12f * sc, cy + 0.32f * sc}, {cx + 0.46f * sc, cy - 0.3f * sc}};
            for (int k = 0; k < 2; ++k) {
                const float ax = pts[k][0], ay = pts[k][1], bx = pts[k + 1][0], by = pts[k + 1][1];
                const int x0 = std::max(0, static_cast<int>(std::min(ax, bx) - 9)), x1 = std::min(kSize - 1, static_cast<int>(std::max(ax, bx) + 9));
                const int y0 = std::max(0, static_cast<int>(std::min(ay, by) - 9)), y1 = std::min(kSize - 1, static_cast<int>(std::max(ay, by) + 9));
                for (int y = y0; y <= y1; ++y) {
                    for (int x = x0; x <= x1; ++x) {
                        const float d  = segDist(x + 0.5f, y + 0.5f, ax, ay, bx, by);
                        uint16_t* p    = &f[y * stride + x];
                        const float sh = 0.5f * clampf(1.0f - (d - 1.6f) / 5.0f, 0, 1) * a;
                        if (sh > 0.01f) blendPx(p, 0, 0, 0, static_cast<int>(sh * 256));
                        const float cc = clampf(2.0f - d, 0, 1) * a;
                        if (cc > 0.01f) blendPx(p, kGreen[0], kGreen[1], kGreen[2], static_cast<int>(cc * 256));
                    }
                }
            }
        }
    }
    // 曲送り: 彗星の先頭に光る白い玉 (線の上を走る)。前のコマの頭から今の頭まで、線をたどる光の筋でつなぐ
    // (コマが落ちても 1 コマに大きく飛んで見えない)
    if ((_fx == kFxNext || _fx == kFxPrev) && _skipStyle == 0) {
        _frameFlags |= 8;
        const float uu  = t / T;
        const float cur = cometHead(uu);
        float prev      = _cometPrev >= 0 ? _cometPrev : cur;
        if (fabsf(cur - prev) > kWaveAngles * 0.45f) prev = cur;  // 1 周の継ぎ目
        const float ha = clampf(uu / 0.06f, 0, 1) * clampf((1.0f - uu) / 0.25f, 0, 1);
        constexpr int kSeg = 8;
        float px[kSeg + 1], py[kSeg + 1];
        float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
        for (int k = 0; k <= kSeg; ++k) {  // k = 0 が前のコマ、kSeg が今の頭
            const float ai  = prev + (cur - prev) * k / kSeg;
            const int idx   = static_cast<int>(ai) & (kWaveAngles - 1);
            const float ang = ai * (6.2831853f / kWaveAngles);
            const float rr  = _waveCur[idx];
            px[k]           = kC + sinf(ang) * rr;  // 波形は画面に固定 (ずれは入れない)
            py[k]           = kC - cosf(ang) * rr;
            bx0 = std::min(bx0, px[k]), by0 = std::min(by0, py[k]), bx1 = std::max(bx1, px[k]), by1 = std::max(by1, py[k]);
        }
        const int x0 = std::max(0, static_cast<int>(bx0 - 14)), x1 = std::min(kSize - 1, static_cast<int>(bx1 + 14));
        const int y0 = std::max(0, static_cast<int>(by0 - 14)), y1 = std::min(kSize - 1, static_cast<int>(by1 + 14));
        _cometBox    = {x0, y0, x1 + 1, y1 + 1};
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                // いちばん近い所と、そこが頭からどれだけ後ろか (後ろほど細く薄い)
                float best = 1e9f, back = 1;
                for (int k = 0; k < kSeg; ++k) {
                    const float d = segDist(x + 0.5f, y + 0.5f, px[k], py[k], px[k + 1], py[k + 1]);
                    if (d < best) {
                        best = d;
                        back = 1.0f - static_cast<float>(k + 1) / kSeg;
                    }
                }
                const float w   = 5.0f - 2.5f * back;  // 頭は太く、後ろは細く
                const float lv  = ha * (1.0f - 0.55f * back);
                uint16_t* p     = &f[y * stride + x];
                const float sh  = 0.4f * clampf(1.0f - (best - w) / 8.0f, 0, 1) * lv;
                if (sh > 0.01f) blendPx(p, 0, 0, 0, static_cast<int>(sh * 256));
                const float c = (clampf(w - best, 0, 1) + 0.5f * clampf(1.0f - best / (w + 7.0f), 0, 1)) * lv;
                if (c > 0.01f) blendPx(p, 255, 255, 255, static_cast<int>(std::min(1.0f, c) * 256));
            }
        }
        _cometPrev = cur;
    } else {
        _cometPrev = -1;
    }
    // 曲送り: » / « がすべって消える
    if (_fx == kFxNext || _fx == kFxPrev) {
        const float dir = _fx == kFxNext ? 1.0f : -1.0f;
        const float ox  = dir * 40.0f * ease;
        for (int k = 0; k < 2; ++k) {
            const float bx = cx + ox + dir * (k * 16.0f - 8.0f);
            stroke(f, stride, bx - dir * 8, cy - 15, bx + dir * 8, cy, 2.9f, a);
            stroke(f, stride, bx + dir * 8, cy, bx - dir * 8, cy + 15, 2.9f, a);
        }
    }
    // 音量: 右側に細い弧のゲージと数字
    if ((_fx == kFxVolUp || _fx == kFxVolDown) && _volume >= 0) {
        const float R = kWaveR - 26.0f, a0 = 0.70f, a1 = 2.44f;  // 上から右回りの角度 (40°〜140°)
        const float fill = a0 + (a1 - a0) * clampf(_volume / 100.0f, 0, 1);
        for (int y = std::max(0, static_cast<int>(cy - R - 6)); y < std::min(kSize, static_cast<int>(cy + R + 7)); ++y) {
            const float dy = y + 0.5f - cy;
            for (int x = static_cast<int>(cx); x < std::min(kSize, static_cast<int>(cx + R + 7)); ++x) {
                const float dx = x + 0.5f - cx;
                const float d  = fabsf(fastSqrt(dx * dx + dy * dy) - R);
                if (d > 5) continue;
                const float ang = atan2f(dx, -dy);
                if (ang < a0 - 0.02f || ang > a1 + 0.02f) continue;
                const bool on   = ang <= fill;
                const float w   = on ? 1.9f : 1.2f;
                const float sh  = 0.4f * clampf(1.0f - (d - w) / 3.0f, 0, 1) * a;  // 暗いふちどり (明るい絵の上でも見える)
                uint16_t* p     = &f[y * stride + x];
                if (sh > 0.01f) blendPx(p, 0, 0, 0, static_cast<int>(sh * 256));
                const float c = clampf(w + 0.5f - d, 0, 1) * a * (on ? 1.0f : 0.45f);
                if (c > 0.01f) blendPx(p, 255, 255, 255, static_cast<int>(c * 256));
            }
        }
        // 数字 (ゲージの内側)
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", _volume);
        int w = 0;
        for (const char* p = buf; *p; ++p) w += _digits[*p - '0'].m.w - 1;
        int pos = static_cast<int>(cx + R - 30 - w);
        const Rect all{0, 0, kSize, kSize};
        for (const char* p = buf; *p; ++p) {
            blitText(f, stride, _digits[*p - '0'], pos + 1, static_cast<int>(cy) + 1, -1, 0, 0, 0, a * 0.6f, all);
            blitText(f, stride, _digits[*p - '0'], pos, static_cast<int>(cy), -1, 255, 255, 255, a, all);
            pos += _digits[*p - '0'].m.w - 1;
        }
    }
}

void GlassUi::wake()
{
    _idleT    = 0;
    _detailT  = 7.0f;  // 操作したら 7 秒だけ詳細を出す
    _ambientT = 0;
    if (_uiDim < 1.0f) {  // 半分の明るさにしていた小さい文字を戻す
        _uiDim     = 1.0f;
        _baseDirty = true;
    }
}

void GlassUi::flash(Flash f)
{
    wake();
    switch (f) {
        case Flash::VolUp:
        case Flash::VolDown:
            _fx  = f == Flash::VolUp ? kFxVolUp : kFxVolDown;
            _fxT = 0;
            return;
        default: break;
    }
    if (f == Flash::Next || f == Flash::Prev) _cometPrev = -1;  // 光の筋は、この曲送りの頭から
    const bool noSkipFx = (f == Flash::Next || f == Flash::Prev) && _skipStyle == 1;
    if (!noSkipFx && (f == Flash::Play || f == Flash::Pause || f == Flash::Next || f == Flash::Prev)) {
        _fx  = f == Flash::Play ? kFxPlay : (f == Flash::Pause ? kFxPause : (f == Flash::Next ? kFxNext : kFxPrev));
        _fxT = 0;
    }
    switch (f) {
        case Flash::Play:
        case Flash::Pause:
            _playing  = f == Flash::Play;
            _forceT   = 2.5f;
            _press[1] = 0;
            break;
        case Flash::Prev: _press[0] = 0; break;
        case Flash::Next: _press[2] = 0; break;
        case Flash::Like:
        case Flash::Unlike:
            _fx  = f == Flash::Like ? kFxLike : kFxUnlike;
            _fxT = 0;
            _liked      = f == Flash::Like;
            _likeForceT = 3.0f;
            _likePop.x  = f == Flash::Like ? 0.3f : 0.7f;
            _likePop.v  = 0;
            if (f == Flash::Like) _likeBurst = 0;
            break;
        default: break;
    }
}

int GlassUi::hit(int x, int y) const
{
    if (_mode != Mode::Player || _ambient) return 0;
    const int cx = kC + _ox, cy = kC + _oy;
    const int dy = y - (cy + kCapY);
    if (abs(dy) <= static_cast<int>(kCapHH) + 8 && abs(x - cx) <= static_cast<int>(kCapHW) + 8) {
        const int dx = x - cx;
        return dx < -kBtnX / 2 ? 1 : (dx > kBtnX / 2 ? 3 : 2);
    }
    if (_canLike && abs(x - (cx + kSideX)) <= 24 && abs(y - (cy + kSideY)) <= 24) return 4;
    return 0;
}

void GlassUi::takeParts(uint32_t* out4)
{
    for (int i = 0; i < 4; ++i) {
        out4[i]   = _parts[i];
        _parts[i] = 0;
    }
}

void GlassUi::takeStats(int* rebuilds, uint32_t* rebuildUs, uint32_t* drawUs)
{
    *rebuilds  = _rebuilds;
    *rebuildUs = _rebuildUs;
    *drawUs    = _drawUs;
    _rebuilds  = 0;
    _rebuildUs = _drawUs = 0;
}

// ============================================================================ 下地

// 背景: ぼかしたジャケット (48x48) を広げて、少しあざやかに。明るさは前もって作った表で (整数だけで計算する)
void GlassUi::rebuildBg()
{
    constexpr int S = 48;
    int row[S][3];
    for (int y = 0; y < kSize; ++y) {
        const float fy = clampf((y + 0.5f) * S / kSize - 0.5f, 0, S - 1.001f);
        const int y0   = static_cast<int>(fy);
        const float ty = fy - y0;
        for (int i = 0; i < S; ++i) {
            if (_hasSmall) {
                float c[3];
                for (int k = 0; k < 3; ++k) {
                    const float a = _small[(y0 * S + i) * 3 + k], b = _small[((y0 + 1) * S + i) * 3 + k];
                    c[k]          = a + (b - a) * ty;
                }
                const float lum = 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2];
                for (int k = 0; k < 3; ++k) row[i][k] = static_cast<int>(clampf(lum + (c[k] - lum) * 1.3f, 0, 255));
            } else {  // ジャケットがない: Spotify の暗い灰
                row[i][0] = row[i][1] = 64;
                row[i][2]             = 70;
            }
        }
        const uint8_t* bay = kBayer[y & 3];
        const uint8_t* sh  = _shade + y * kSize;
        uint16_t* out      = _bg + y * kSize;
        for (int x = 0; x < kSize; ++x) {
            const int i = _bx0[x], t = _btx[x];
            const int* a = row[i];
            const int* b = row[i + 1];
            const int s  = sh[x];
            const int d  = bay[x & 3];
            out[x]       = pack(((a[0] + (((b[0] - a[0]) * t) >> 8)) * s >> 8) + (d >> 1),
                                ((a[1] + (((b[1] - a[1]) * t) >> 8)) * s >> 8) + (d >> 2),
                                ((a[2] + (((b[2] - a[2]) * t) >> 8)) * s >> 8) + (d >> 1));
        }
    }
}

// すりガラス (リキッドグラス風): 中は背景を少し明るく・ふち近くは外側を曲げて見せる。上のふちは光る。下に影
static void glassShape(uint16_t* base, const uint16_t* bg, float gx, float gy, float hw, float hh, float r)
{
    const int x0 = std::max(0, static_cast<int>(gx - hw - 16)), x1 = std::min(kSize, static_cast<int>(gx + hw + 17));
    const int y0 = std::max(0, static_cast<int>(gy - hh - 16)), y1 = std::min(kSize, static_cast<int>(gy + hh + 22));
    const float Lx = -0.35f, Ly = -0.94f;  // 光は左上から
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float px = x + 0.5f - gx, py = y + 0.5f - gy;
            const float d  = sdRoundRect(px, py, hw, hh, r);
            uint16_t* dst  = &base[y * kSize + x];
            if (d > 0.5f) {  // 外: やわらかい影
                const float ds = sdRoundRect(px, py - 6, hw, hh, r);
                const float sa = 0.32f * (1.0f - smooth(-2.0f, 14.0f, ds));
                if (sa > 0.004f) blendPx(dst, 0, 0, 0, static_cast<int>(sa * 256));
                continue;
            }
            const float cov   = clampf(0.5f - d, 0, 1);
            const float depth = -d;
            // ふちの向き (外向き)
            float nx = sdRoundRect(px + 1, py, hw, hh, r) - sdRoundRect(px - 1, py, hw, hh, r);
            float ny = sdRoundRect(px, py + 1, hw, hh, r) - sdRoundRect(px, py - 1, hw, hh, r);
            const float nl = fastSqrt(nx * nx + ny * ny) + 1e-6f;
            nx /= nl;
            ny /= nl;
            // ふち近くは外側を曲げて見せる (レンズ)
            const float bend = depth < 14 ? (1 - depth / 14) * (1 - depth / 14) * 11.0f : 0.0f;
            const int sx = clampi(static_cast<int>(x + nx * bend), 0, kSize - 1), sy = clampi(static_cast<int>(y + ny * bend), 0, kSize - 1);
            int R, G, B;
            unpack(bg[sy * kSize + sx], R, G, B);
            float fr = R * 1.12f + 24, fg = G * 1.12f + 24, fb = B * 1.12f + 26;
            // 上半分につやを少し
            const float gloss = 14.0f * (1.0f - smooth(-hh, hh * 0.3f, py));
            fr += gloss;
            fg += gloss;
            fb += gloss;
            // ふちの光
            const float rim  = 1.0f - smooth(0.0f, 3.2f, depth);
            const float spec = std::max(0.0f, nx * Lx + ny * Ly);
            const float back = std::max(0.0f, -(nx * Lx + ny * Ly));
            const float hl   = 255.0f * rim * (0.16f + 0.62f * spec * spec + 0.10f * back);
            fr += hl;
            fg += hl;
            fb += hl;
            blendPx(dst, static_cast<int>(fr), static_cast<int>(fg), static_cast<int>(fb), static_cast<int>(cov * 256));
        }
    }
}

void GlassUi::rebuildBase()
{
    const uint32_t t0 = nowUs();
    if (!(_mode == Mode::Player && _ambient)) memcpy(_base, _bg, static_cast<size_t>(kSize) * kSize * 2);
    const Rect all{0, 0, kSize, kSize};
    const int cx = kC, cy = kC;

    _basePaused = !_playing;
    if (_mode == Mode::Player && _ambient) {
        // 静かな画面: ジャケットを画面いっぱいに、そのまま見せる。
        // (上下を帯で暗くすると丸い画面のふちが黒い枠に、文字の後ろだけぼかして暗くするとシミに見えた)
        // 文字は、ふち 1px の薄い影で読めるようにする
        // 上半分・下半分を 2 つのコアで。波形のときはふちを暗くしないので、1 画素ずつの距離の計算はしない
        prepLed();
        struct BaseCtx {
            GlassUi* ui;
            float rim, pause;
            int cx, cy;
        } bc{this, _viz == Viz::Wave ? 0.0f : 0.62f, _playing ? 1.0f : 0.55f, cx, cy};
        split(
            [](void* arg, int part) {
                const auto* c  = static_cast<const BaseCtx*>(arg);
                GlassUi* ui    = c->ui;
                const int kFix = static_cast<int>(256 * c->pause);
                for (int y = part ? kSize / 2 : 0; y < (part ? kSize : kSize / 2); ++y) {
                    const float dy     = y + 0.5f - c->cy;
                    const uint8_t* bay = kBayer[y & 3];
                    const uint16_t* art = ui->_artFull + y * kSize;
                    uint16_t* out       = ui->_base + y * kSize;
                    for (int x = 0; x < kSize; ++x) {
                        int k = kFix;
                        if (c->rim > 0) {
                            const float dx = x + 0.5f - c->cx;
                            const float r  = fastSqrt(dx * dx + dy * dy);
                            k = static_cast<int>(256 * (1.0f - c->rim * smooth(160.0f, 232.0f, r)) * c->pause);
                        }
                        int R = 40, G = 40, B = 44;
                        if (ui->_ledGrid > 0) {  // LED 風のドット絵: 粒の色 x 粒の形 (すき間は 18% の暗さ)
                            const int g        = ui->_ledGrid;
                            const int cyy      = std::min(g - 1, y * g / kSize);
                            const int qy       = std::min(31, (y * g * 32 / kSize) & 31);
                            const uint8_t* rgb = ui->_ledRGB + (cyy * g + ui->_ledCellX[x]) * 3;
                            const int f        = 46 + ui->_ledA[qy * 32 + ui->_ledQX[x]] * 210 / 255;
                            R                  = rgb[0] * f >> 8;
                            G                  = rgb[1] * f >> 8;
                            B                  = rgb[2] * f >> 8;
                        } else if (ui->_hasArt) {
                            const uint16_t a = art[x];
                            R                = ((a >> 11) & 31) * 255 / 31;
                            G                = ((a >> 5) & 63) * 255 / 63;
                            B                = (a & 31) * 255 / 31;
                        }
                        const int d = bay[x & 3];
                        out[x]      = pack(((R * k) >> 8) + (d >> 1), ((G * k) >> 8) + (d >> 2), ((B * k) >> 8) + (d >> 1));
                    }
                }
            },
            &bc);
        // 波形の線の色: 線が通るあたりが明るいジャケットは黒、ほかは白
        {
            long sum = 0;
            int cnt  = 0;
            for (int i = 0; i < _waveN; i += 7) {
                if (fabsf(_waveR[i] / 8.0f - kWaveR) > 12) continue;
                int r, g, b;
                unpack(_base[((_wavePos[i] >> 9) & 511) * kSize + (_wavePos[i] & 511)], r, g, b);  // 実際に線が通るところ
                sum += (r * 3 + g * 6 + b) / 10;
                ++cnt;
            }
            _waveDark = cnt > 0 && sum / cnt > 140;  // 明るければ黒い線、暗ければ白い線
        }
        // 曲名とアーティストは影なしで下地に (影は、操作のあと詳細が出ている間だけ、上から足す)
        if (!_titleSMarquee && !_titleLite) blitText(_base, kSize, _titleS, cx, cy + kATitleY, 0, 255, 255, 255, _uiDim, all);
        if (!_artistSMarquee && !_titleLite) blitText(_base, kSize, _artistS, cx, cy + kAArtistY, 0, 210, 210, 210, _uiDim, all);
        // 小さい再生バーの溝
        const float by = cy + kAProgY;
        for (int y = static_cast<int>(by) - 3; y <= static_cast<int>(by) + 3; ++y) {
            for (int x = cx - kAProgHalf - 3; x <= cx + kAProgHalf + 3; ++x) {
                const float c = clampf(2.0f - segDist(x + 0.5f, y + 0.5f, cx - kAProgHalf, by, cx + kAProgHalf, by), 0, 1);
                if (c > 0) blendPx(&_base[y * kSize + x], 255, 255, 255, static_cast<int>(c * 0.3f * 256));
            }
        }
    } else if (_mode == Mode::Player) {
        const float acx = static_cast<float>(cx), acy = static_cast<float>(cy + kArtTop + kArtHalf);
        // ジャケットの影と、角丸のジャケット
        const int x0 = std::max(0, cx - kArtHalf - 26), x1 = std::min(kSize, cx + kArtHalf + 27);
        const int y0 = std::max(0, cy + kArtTop - 14), y1 = std::min(kSize, cy + kArtTop + kArtS + 36);
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const float px = x + 0.5f - acx, py = y + 0.5f - acy;
                uint16_t* dst  = &_base[y * kSize + x];
                const float ds = sdRoundRect(px, py - 10, kArtHalf, kArtHalf, kArtR + 6);
                const float sa = 0.55f * (1.0f - smooth(-14.0f, 20.0f, ds));
                const float d  = sdRoundRect(px, py, kArtHalf, kArtHalf, kArtR);
                if (sa > 0.004f) blendPx(dst, 0, 0, 0, static_cast<int>(sa * 256));
                if (d > 0.5f) continue;
                const float cov = clampf(0.5f - d, 0, 1);
                int R = 40, G = 40, B = 44;
                if (_hasArt) {
                    const int ax = clampi(static_cast<int>(px + kArtHalf), 0, kArtS - 1), ay = clampi(static_cast<int>(py + kArtHalf), 0, kArtS - 1);
                    const uint16_t c = _art[ay * kArtS + ax];
                    R                = ((c >> 11) & 31) * 255 / 31;
                    G                = ((c >> 5) & 63) * 255 / 63;
                    B                = (c & 31) * 255 / 31;
                } else {  // ジャケットがないときは、灰の四角に薄い丸
                    const float rr = fastSqrt(px * px + py * py);
                    const int k    = rr < 40 ? 70 : (rr < 44 ? 90 : 52);
                    R = G = k;
                    B     = k + 4;
                }
                blendPx(dst, R, G, B, static_cast<int>(cov * 256));
            }
        }
        // 曲名・アーティスト (流さないときは下地に入れる)
        if (!_titleMarquee) blitText(_base, kSize, _title, cx, cy + kTitleY, 0, 255, 255, 255, 1.0f, all);
        if (!_artistMarquee) blitText(_base, kSize, _artist, cx, cy + kArtistY, 0, kGray[0], kGray[1], kGray[2], 1.0f, all);
        {
            // 再生している機器 (緑)
            blitText(_base, kSize, _device, cx, cy + kDevY, 0, kGreen[0], kGreen[1], kGreen[2], 1.0f, all);
            // 再生バーの溝
            const float by = cy + kProgY;
            for (int y = static_cast<int>(by) - 3; y <= static_cast<int>(by) + 3; ++y) {
                for (int x = cx - kProgHalf - 3; x <= cx + kProgHalf + 3; ++x) {
                    const float dd = segDist(x + 0.5f, y + 0.5f, cx - kProgHalf, by, cx + kProgHalf, by);
                    const float c  = clampf(2.5f - dd, 0, 1);
                    if (c > 0) blendPx(&_base[y * kSize + x], 255, 255, 255, static_cast<int>(c * 0.26f * 256));
                }
            }
            // すりガラスのカプセル (ボタンの印は、動く部分として描く)
            glassShape(_base, _bg, static_cast<float>(cx), static_cast<float>(cy + kCapY), kCapHW, kCapHH, kCapHH);
        }
    } else if (_mode == Mode::Idle) {
        // 時計: 背景を暗くして、大きな時刻と小さな案内
        for (int i = 0; i < kSize * kSize; ++i) {
            int r, g, b;
            unpack(_base[i], r, g, b);
            _base[i] = pack(r * 9 / 16, g * 9 / 16, b * 9 / 16);
        }
        blitText(_base, kSize, _clock, cx, cy - 14, 0, 245, 245, 245, 0.92f, all);
        blitText(_base, kSize, _hint, cx, cy + 70, 0, kGray[0], kGray[1], kGray[2], 0.85f, all);
    } else {
        drawNoticeBase();  // 案内: LED の粒のドット絵と白い文字 (背景の粒は毎コマ)
    }
    _glowReady = 1;  // 光の段階は作り直し (1 コマに 1 段ずつ)
    ++_rebuilds;
    _rebuildUs += nowUs() - t0;
}

// 光の段階 level の絵: 下地に、光る強さ x 段階の分だけ光の色を足す
void GlassUi::makeGlowLevel(int level)
{
    const int k = static_cast<int>(256.0f * kGlowMax * level / (kGlowLevels - 1));
    uint16_t* out = _glow[level];
    for (int y = 0; y < kGlowH; ++y) {
        const uint16_t* src = _base + (kGlowY0 + y) * kSize + kGlowX0;
        const uint8_t* m    = _glowMask + y * kGlowW;
        uint16_t* o         = out + y * kGlowW;
        for (int x = 0; x < kGlowW; ++x) {
            const int a = (m[x] * k) >> 8;
            if (a < 2) {
                o[x] = src[x];
                continue;
            }
            int r, g, b;
            unpack(src[x], r, g, b);
            o[x] = pack(r + ((_accent[0] * a) >> 8), g + ((_accent[1] * a) >> 8), b + ((_accent[2] * a) >> 8));
        }
    }
    _glowReady |= static_cast<uint8_t>(1u << level);
}

// ============================================================================ 文字

// 文字の型を重ねる。align: 0 = 真ん中、-1 = 左はし、1 = 右はし。window > 0 なら、その幅の中で scroll だけ流す (両はしはぼかす)
void GlassUi::blitText(uint16_t* dst, int dstride, const Text& t, int cx, int cy, int align, int r, int g, int b, float a,
                       const Rect& clip, float scroll, int window, float shadow, bool text)
{
    if (!t.m.a || a <= 0.01f) return;
    const int top = cy - t.m.h / 2;
    const bool marquee = window > 0 && t.m.w > window;
    const int winL = cx - window / 2, winR = cx + window / 2;
    int starts[2];
    int n = 0;
    if (marquee) {
        const int period = t.m.w + 60;
        const int off    = static_cast<int>(fmodf(scroll, static_cast<float>(period)));
        starts[n++]      = winL - off;
        starts[n++]      = winL - off + period;
    } else {
        starts[n++] = align == 0 ? cx - t.m.w / 2 : (align < 0 ? cx : cx - t.m.w);
    }
    const int ya = std::max({top, clip.y0, 0}), yb = std::min({top + t.m.h, clip.y1, kSize});
    const int ai = static_cast<int>(a * 256);
    for (int s = 0; s < n; ++s) {
        const int x0 = starts[s];
        int xa = std::max({x0, clip.x0, 0}), xb = std::min({x0 + t.m.w, clip.x1, kSize});
        if (marquee) {
            xa = std::max(xa, winL);
            xb = std::min(xb, winR);
        }
        if (shadow > 0.01f) {  // ふち 1px の影: まわり 8 画素の文字の濃さ (少し下寄り) を、黒で薄く重ねる (文字の上にはかけない)
            const int si = static_cast<int>(shadow * a * 256);
            auto at = [&](int x, int y) -> int {
                const int mx = x - x0, my = y - top;
                return (mx < 0 || my < 0 || mx >= t.m.w || my >= t.m.h) ? 0 : t.m.a[my * t.m.w + mx];
            };
            const int sya = std::max({top - 1, clip.y0, 0}), syb = std::min({top + t.m.h + 2, clip.y1, kSize});
            int sxa = std::max({x0 - 1, clip.x0, 0}), sxb = std::min({x0 + t.m.w + 1, clip.x1, kSize});
            if (marquee) {
                sxa = std::max(sxa, winL);
                sxb = std::min(sxb, winR);
            }
            for (int y = sya; y < syb; ++y) {
                uint16_t* row = dst + y * dstride;
                for (int x = sxa; x < sxb; ++x) {
                    int v = std::max({at(x, y - 1), at(x - 1, y - 1), at(x + 1, y - 1), at(x, y - 2)});  // 下寄り
                    v     = std::max(v, std::max({at(x - 1, y), at(x + 1, y), at(x, y + 1)}) * 3 / 4);
                    if (!v) continue;
                    int k = ((v * si) >> 8) * (255 - at(x, y)) / 255;
                    if (marquee) k = k * clampi(std::min(x - winL, winR - 1 - x) * 256 / 16, 0, 256) >> 8;
                    blendPx(&row[x], 0, 0, 0, k);
                }
            }
        }
        if (!text) continue;
        for (int y = ya; y < yb; ++y) {
            const uint8_t* m = t.m.a + (y - top) * t.m.w - x0;
            uint16_t* row    = dst + y * dstride;
            for (int x = xa; x < xb; ++x) {
                const int v = m[x];
                if (!v) continue;
                int k = (v * ai) >> 8;
                if (marquee) k = k * clampi(std::min(x - winL, winR - 1 - x) * 256 / 16, 0, 256) >> 8;
                blendPx(&row[x], r, g, b, k + (k >> 7));
            }
        }
    }
}

void GlassUi::drawTimes(uint16_t* f, int stride, const Rect& clip)
{
    const int cx = kC + _ox, cy = kC + _oy;
    auto put = [&](uint32_t ms, int x, int align) {
        const uint32_t s = ms / 1000;
        char buf[16];
        snprintf(buf, sizeof(buf), "%lu:%02lu", static_cast<unsigned long>(s / 60), static_cast<unsigned long>(s % 60));
        int w = 0;
        for (const char* p = buf; *p; ++p) {
            const int i = *p == ':' ? 10 : (*p == '-' ? 11 : *p - '0');
            w += _digits[i].m.w - 1;
        }
        int pos = align < 0 ? x : x - w;
        for (const char* p = buf; *p; ++p) {
            const int i = *p == ':' ? 10 : (*p == '-' ? 11 : *p - '0');
            blitText(f, stride, _digits[i], pos, cy + kTimeY, -1, kGray[0], kGray[1], kGray[2], 1.0f, clip);
            pos += _digits[i].m.w - 1;
        }
    };
    put(_posMs, cx - kProgHalf, -1);
    put(_durMs, cx + kProgHalf, 1);
}

// 経過時間を xl の左 (右寄せ)、曲の長さを xr の右 (左寄せ) に
void GlassUi::drawTimesAt(uint16_t* f, int stride, const Rect& clip, int y, int xl, int xr, float alpha)
{
    if (alpha <= 0.01f) return;
    auto put = [&](uint32_t ms, int x, bool rightAlign) {
        const uint32_t s = ms / 1000;
        char buf[16];
        snprintf(buf, sizeof(buf), "%lu:%02lu", static_cast<unsigned long>(s / 60), static_cast<unsigned long>(s % 60));
        int w = 0;
        for (const char* p = buf; *p; ++p) w += _digits[*p == ':' ? 10 : *p - '0'].m.w - 1;
        int pos = rightAlign ? x - w : x;
        for (const char* p = buf; *p; ++p) {
            const Text& g = _digits[*p == ':' ? 10 : *p - '0'];
            blitText(f, stride, g, pos + 1, y + 1, -1, 0, 0, 0, alpha * 0.7f, clip);  // 影 (明るい絵の上でも読める)
            blitText(f, stride, g, pos, y, -1, 235, 235, 235, alpha, clip);
            pos += g.m.w - 1;
        }
    };
    put(_posMs, xl, true);
    put(_durMs, xr, false);
}

// ============================================================================ 動く部分

// ♥: 付いていれば緑の丸に黒のチェック、なければ灰の丸に + (scale で大きさを変える)
void GlassUi::drawLike(uint16_t* f, int stride, const Rect& r, float lx, float ly, float scale)
{
    if (!_canLike) return;
    const float s = std::max(0.2f, _likePop.x) * scale;
    const float R = 10.5f * s;
    for (int y = r.y0; y < r.y1; ++y) {
        for (int x = r.x0; x < r.x1; ++x) {
            const float px = x + 0.5f - lx, py = y + 0.5f - ly;
            const float d  = fastSqrt(px * px + py * py);
            uint16_t* dst  = &f[y * stride + x];
            // ♥ を付けた瞬間の輪 (緑の点が広がる)
            if (_likeBurst < 1) {
                const float br = R + 4 * scale + 13 * scale * _likeBurst;
                const float a  = (1 - _likeBurst) * clampf(1.6f - fabsf(d - br), 0, 1);
                if (a > 0) {
                    const float ang = atan2f(py, px) * 8.0f / 6.2831853f;
                    const float dot = clampf(1.0f - fabsf(ang - roundf(ang)) * 3.0f, 0, 1);
                    if (dot > 0) blendPx(dst, kGreen[0], kGreen[1], kGreen[2], static_cast<int>(a * dot * _zoneAlpha * 256));
                }
            }
            if (d > R + 5) continue;
            // 暗いふちどり (明るい絵の上でも見える)
            const float sh = 0.45f * clampf(1.0f - (d - R) / 4.0f, 0, 1) * _zoneAlpha;
            if (sh > 0.01f) blendPx(dst, 0, 0, 0, static_cast<int>(sh * 256));
            if (d > R + 1) continue;
            if (_liked) {
                const float cov = clampf(R + 0.5f - d, 0, 1);
                blendPx(dst, kGreen[0], kGreen[1], kGreen[2], static_cast<int>(cov * _zoneAlpha * 256));
                const float k = std::min(segDist(px, py, -4.5f * s, 0.3f * s, -1.2f * s, 3.6f * s),
                                         segDist(px, py, -1.2f * s, 3.6f * s, 5.0f * s, -3.4f * s));
                const float c = clampf(1.6f * s + 0.5f - k, 0, 1);
                if (c > 0) blendPx(dst, 0, 0, 0, static_cast<int>(c * _zoneAlpha * 256));
            } else {
                const float ring = clampf(1.1f + 0.5f - fabsf(d - (R - 1.1f)), 0, 1);
                const float plus = std::min(segDist(px, py, -4.6f * s, 0, 4.6f * s, 0), segDist(px, py, 0, -4.6f * s, 0, 4.6f * s));
                const float c    = std::max(ring, clampf(1.0f + 0.5f - plus, 0, 1));
                if (c > 0) blendPx(dst, 235, 235, 235, static_cast<int>(c * _zoneAlpha * 256));
            }
        }
    }
}

// 再生バー: 白い線と丸いつまみ
void GlassUi::drawBar(uint16_t* f, int stride, const Rect& r, float cx, float by, float half, float thick, float knob)
{
    const float frac = _durMs ? clampf(static_cast<float>(_posMs) / _durMs, 0, 1) : 0;
    const float fx   = cx - half + 2.0f * half * frac;
    for (int y = std::max(r.y0, static_cast<int>(by - knob) - 1); y <= std::min(r.y1 - 1, static_cast<int>(by + knob) + 1); ++y) {
        for (int x = r.x0; x < r.x1; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            float c        = 0;
            if (px < fx + 3) c = clampf(thick - segDist(px, py, cx - half, by, fx, by), 0, 1);
            const float kd = fastSqrt((px - fx) * (px - fx) + (py - by) * (py - by));
            c              = std::max(c, clampf(knob + 0.5f - kd, 0, 1));
            if (c > 0) blendPx(&f[y * stride + x], 255, 255, 255, static_cast<int>(c * _zoneAlpha * 256));
        }
    }
}

// 虹彩: ふちから内へ伸びる虹色の光。中身は筋 (バー) だが、太くぼかして、となりとにじませ、先はふわっと消す。
// 表に並べたふちの帯の画素だけ、下地 (ずらしたもの) に色を足して描き直す。光は画面に固定 (下地だけがずれる)
void GlassUi::drawIris(uint16_t* f, int stride)
{
    float col[kSpokes][3];
    float inv[kSpokes];
    float len[kSpokes];
    for (int s = 0; s < kSpokes; ++s) {
        const float hue = s * (1.0f / kSpokes) + _t * 0.03f + _irisHueNow;  // 虹色がゆっくり回る (キックで少し跳ぶ)
        const float hh  = (hue - floorf(hue)) * 6.0f;
        const int i     = static_cast<int>(hh);
        const float fr  = hh - i;
        const float sat = 0.85f, q = 1 - sat * fr, t = 1 - sat * (1 - fr), p = 1 - sat;
        float r, g, b;
        switch (i) {
            case 0: r = 1, g = t, b = p; break;
            case 1: r = q, g = 1, b = p; break;
            case 2: r = p, g = 1, b = t; break;
            case 3: r = p, g = q, b = 1; break;
            case 4: r = t, g = p, b = 1; break;
            default: r = 1, g = p, b = q; break;
        }
        const float k = (0.75f + 0.25f * _irisJit[s] / 255.0f) * (1.6f + 1.0f * _kickPunch) / 255.0f;  // g の 255 を打ち消す
        col[s][0]     = r * 255 * k;
        col[s][1]     = g * 255 * k;
        col[s][2]     = b * 255 * k;
        len[s]        = std::max(1.0f, _irisLen[s] * 2);  // 0.5px 刻み
        inv[s]        = 1.0f / len[s];
    }
    for (int i = 0; i < _irisN; ++i) {
        const uint32_t e = _irisPos[i];
        const int p      = e & 0x3FFFF;
        const int y = p / kSize, x = p - y * kSize;
        const int s0 = (e >> 18) & 63, s1 = (s0 + 1) & (kSpokes - 1);
        const int bx = x - _ox, by = y - _oy;
        const uint16_t base = _base[clampi(by, 0, kSize - 1) * kSize + clampi(bx, 0, kSize - 1)];
        const float t = _irisT[i];
        // 根もとから先まで明るく、最後だけふわっと消える (1 - (t/L)^2)
        float f0 = t * inv[s0], f1 = t * inv[s1];
        f0       = f0 < 1 ? 1.0f - f0 * f0 : 0.0f;
        f1       = f1 < 1 ? 1.0f - f1 * f1 : 0.0f;
        const float a0 = (e >> 24) * f0, a1 = _irisG1[i] * f1;
        uint16_t* dst  = &f[y * stride + x];
        if (a0 + a1 < 3.0f) {
            *dst = base;
            continue;
        }
        int R, G, B;
        unpack(base, R, G, B);
        *dst = pack(R + static_cast<int>(col[s0][0] * a0 + col[s1][0] * a1), G + static_cast<int>(col[s0][1] * a0 + col[s1][1] * a1),
                    B + static_cast<int>(col[s0][2] * a0 + col[s1][2] * a1));
    }
}

// 波形 (エモ): ふちをうねる細い線と、少し遅れてついてくる残像 2 本。線は白 (明るいジャケットは黒) で、まわりがほんのり光る
struct GlassUi::WaveCtx {
    GlassUi* ui;
    int style;
    uint16_t* f;
    int stride;
    bool all, comet;
    int head, tail, dirNext, boff;
    float cometA;
};

void GlassUi::drawWave(uint16_t* f, int stride, bool all)
{
#ifdef GLASS_NOSKIP
    all = true;  // Mac のシミュレーターで、飛ばす描き方と結果が同じか比べる用
#endif
    WaveCtx c{this, _skipStyle, f, stride, all, false, 0, 1, _fx == kFxNext, _oy * kSize + _ox, 0};
    // 曲送りの彗星: 線の上を白い光が 1 周走る (前の曲は逆回り)。尾は後ろへ
    c.comet = (_fx == kFxNext || _fx == kFxPrev) && _fxT < fxDur();
    if (c.comet) {
        const float u = _fxT / fxDur();
        c.head        = static_cast<int>(cometHead(u)) & (kWaveAngles - 1);
        c.tail        = 400;
        c.cometA      = clampf(u / 0.08f, 0, 1) * clampf((1.0f - u) / 0.25f, 0, 1);
    }
    // 画素の表を前半・後半に分けて、2 つのコアで同時に描く (画素は重ならない)
    split(
        [](void* a, int part) {
            const auto* c = static_cast<const WaveCtx*>(a);
            const int mid = c->ui->_waveN / 2;
            c->ui->waveSpan(*c, part ? mid : 0, part ? c->ui->_waveN : mid);
        },
        &c);
    memcpy(_w8p, _w8, sizeof(int16_t) * 3 * kWaveAngles);
}

void GlassUi::waveSpan(const WaveCtx& cx, int i0, int i1)
{
    uint16_t* f        = cx.f;
    const int stride   = cx.stride;
    const bool all     = cx.all;
    const bool comet   = cx.comet;
    const int head = cx.head, tail = cx.tail, dirNext = cx.dirNext, boff = cx.boff;
    const float cometA = cx.cometA;
    const int16_t* p8  = _w8p;  // 前のコマの線
    const int16_t* p18 = _w8p + kWaveAngles;
    const int16_t* p28 = _w8p + 2 * kWaveAngles;
    const int16_t* c8   = _w8;
    const int16_t* e18  = _w8 + kWaveAngles;
    const int16_t* e28  = _w8 + 2 * kWaveAngles;
    const uint8_t* lutL = _lutLine;
    const uint8_t* lut1 = _lutLine + 256;
    const uint8_t* lut2 = _lutLine + 512;
    for (int i = i0; i < i1; ++i) {
        const uint32_t e = _wavePos[i];
        const int x = e & 511, y = (e >> 9) & 511;
        const int ai = (e >> 18) & (kWaveAngles - 1);
        const int r8 = _waveR[i];
        const int d0 = abs(r8 - c8[ai]), d1 = abs(r8 - e18[ai]), d2 = abs(r8 - e28[ai]);
        // 前のコマでも今のコマでも線・光・残像から遠い画素は、さわらない (画面はもう下地のまま)
        if (!all && d0 >= 56 && d1 >= 10 && d2 >= 10 && abs(r8 - p8[ai]) >= 56 && abs(r8 - p18[ai]) >= 10 &&
            abs(r8 - p28[ai]) >= 10) {
            continue;
        }
        uint16_t* dst = &f[y * stride + x];
        // 下の絵: ふだんは下地から (前のコマの線を消す)。曲の切り替え中は、混ぜたあとの画面そのもの
        uint16_t base;
        if (_waveFromFb) {
            base = *dst;
        } else {
            const int bx = x - _ox, by = y - _oy;
            base = (static_cast<unsigned>(bx) < static_cast<unsigned>(kSize) && static_cast<unsigned>(by) < static_cast<unsigned>(kSize))
                       ? _base[y * kSize + x - boff]
                       : _base[clampi(by, 0, kSize - 1) * kSize + clampi(bx, 0, kSize - 1)];
        }
        int boostI = 0, headDiff = 9999;
        if (comet) {
            const int diff = (dirNext ? head - ai : ai - head) & (kWaveAngles - 1);
            headDiff       = diff;
            if (diff < tail) {
                const float k = 1.0f - static_cast<float>(diff) / tail;
                boostI        = static_cast<int>(k * k * cometA * 256);
            }
        }
        // 線・光・残像のどれからも遠い画素は、写すだけ (ほとんどの画素はここ)
        if (d0 >= 56 && d1 >= 10 && d2 >= 10 && boostI == 0) {
            *dst = base;
            continue;
        }
        *dst = base;
        if (boostI > 2) {
            // 彗星: 暗いふちどり (明るい絵の上でも見える) のあとに、太い白い光
            const float boost = boostI / 256.0f, dp = d0 / 8.0f;
            float core = 3.8f, glow = 9.0f;
            if (cx.style == 2) {  // 頭のほうを太く明るく (玉に見える)。波形と一緒に描くので、別に描く手間がない
                const float hk = clampf(1.0f - headDiff / 26.0f, 0, 1);
                core += 2.4f * hk;
                glow += 4.0f * hk;
            }
            const float sh = boost * 0.45f * clampf(1.0f - (dp - (core - 0.8f)) / 8.0f, 0, 1);
            if (sh > 0.01f) blendPx(dst, 0, 0, 0, static_cast<int>(sh * 256));
            const float w = boost * std::max(clampf(core - dp, 0, 1), 0.6f * clampf(1.0f - dp / glow, 0, 1));
            if (w > 0.01f) blendPx(dst, 255, 255, 255, static_cast<int>(std::min(1.0f, w) * 256));
        }
        int a = lutL[std::min(d0, 255)];
        a     = std::max(a, static_cast<int>(lut1[std::min(d1, 255)]));
        a     = std::max(a, static_cast<int>(lut2[std::min(d2, 255)]));
        if (a < 3) continue;
        // 線の色は場所ごとに: 下が明るいところは黒、暗いところは白
        int R, G, B;
        unpack(base, R, G, B);
        const int c = _lineLut[(R * 3 + G * 6 + B) / 10];
        blendPx(dst, c, c, c, a + (a >> 7));
    }
}

void GlassUi::enterAmbient()
{
    if (_mode != Mode::Player || _ambient) return;
    _ambient   = true;
    _baseDirty = true;
}

bool GlassUi::zoneOn(int z) const
{
    if (_mode != Mode::Player) return false;
    if (_ambient) {
        return z == kZAProg || z == kZALike || z == kZATitle || z == kZAArtist || z == kZADev || z == kZABpm;
    }
    switch (z) {
        case kZTitle: return _titleMarquee;
        case kZArtist: return _artistMarquee;
        case kZEq:
        case kZLike:
        case kZProg:
        case kZCap: return true;
        default: return false;
    }
}

GlassUi::Rect GlassUi::zoneRect(int z) const
{
    const int cx = kC + _ox, cy = kC + _oy;
    Rect r{};
    switch (z) {
        case kZEq: r = {cx - kSideX - 12, cy + kSideY - 12, cx - kSideX + 10, cy + kSideY + 10}; break;
        case kZLike: r = {cx + kSideX - 22, cy + kSideY - 22, cx + kSideX + 23, cy + kSideY + 23}; break;
        case kZProg: r = {cx - kProgHalf - 8, cy + kProgY - 8, cx + kProgHalf + 9, cy + kTimeY + 9}; break;
        case kZTitle: r = {cx - kWin / 2 - 1, cy + kTitleY - 15, cx + kWin / 2 + 1, cy + kTitleY + 15}; break;
        case kZAProg: r = {cx - kAProgHalf - 6, cy + kAProgY - 7, cx + kAProgHalf + 7, cy + kAProgY + 8}; break;
        case kZADev: r = {cx - 112, cy - 158, cx + 112, cy - 138}; break;  // 機器名と BPM
        case kZABpm: r = {cx - 64, cy + kABpmY - 10, cx + 64, cy + kABpmY + 9}; break;  // 再生バー (cy+117〜) にかけない
        case kZALike: r = {cx - 72, cy + kALikeY - 13, cx + 73, cy + kALikeY + 17}; break;  // ♥ と左右の時間。上の再生バーにかけない
        case kZATitle: r = {cx - kAWin / 2 - 1, cy + kATitleY - 11, cx + kAWin / 2 + 1, cy + kATitleY + 11}; break;
        case kZAArtist: r = {cx - kAWin / 2 - 1, cy + kAArtistY - 9, cx + kAWin / 2 + 1, cy + kAArtistY + 9}; break;
        case kZArtist: r = {cx - kWin / 2 - 1, cy + kArtistY - 11, cx + kWin / 2 + 1, cy + kArtistY + 11}; break;
        case kZCap:
            r = {cx - static_cast<int>(kCapHW) - 2, cy + kCapY - static_cast<int>(kCapHH) - 2, cx + static_cast<int>(kCapHW) + 3,
                 cy + kCapY + static_cast<int>(kCapHH) + 3};
            break;
        default: break;
    }
    r.x0 = clampi(r.x0, 0, kSize);
    r.x1 = clampi(r.x1, 0, kSize);
    r.y0 = clampi(r.y0, 0, kSize);
    r.y1 = clampi(r.y1, 0, kSize);
    return r;
}

// 画面の y 行の [x0, x1) を、下地からずらして写す。光る範囲は、いま出している光の段階の絵から
void GlassUi::compose(int y, int x0, int x1, uint16_t* fbRow)
{
    // 下地の外 (ずらした分) は、いちばん近いふちの色を延ばして埋める (黒く欠けないように)
    const int by = clampi(y - _oy, 0, kSize - 1);
    int bx0 = x0 - _ox, bx1 = x1 - _ox;
    if (bx0 < 0) {
        const int n        = std::min(-bx0, x1 - x0);
        const uint16_t edge = _base[by * kSize];
        for (int i = 0; i < n; ++i) fbRow[x0 + i] = edge;
        bx0 = 0;
    }
    if (bx1 > kSize) {
        const int n        = std::min(bx1 - kSize, x1 - x0);
        const uint16_t edge = _base[by * kSize + kSize - 1];
        for (int i = 0; i < n; ++i) fbRow[x1 - n + i] = edge;
        bx1 = kSize;
    }
    if (bx0 >= bx1) return;
    const uint16_t* base = _base + by * kSize;
    uint16_t* dst        = fbRow + _ox;  // dst[bx] が画面の (bx + _ox)
    const bool glow      = _glowShown > 0 && by >= kGlowY0 && by < kGlowY1;
    if (!glow) {
        memcpy(dst + bx0, base + bx0, (bx1 - bx0) * 2);
        return;
    }
    const uint16_t* g = _glow[_glowShown] + (by - kGlowY0) * kGlowW - kGlowX0;  // g[bx]
    const int a = std::min(bx1, kGlowX0), b = std::max(bx0, kGlowX1);
    if (bx0 < a) memcpy(dst + bx0, base + bx0, (a - bx0) * 2);
    const int g0 = std::max(bx0, kGlowX0), g1 = std::min(bx1, kGlowX1);
    if (g0 < g1) memcpy(dst + g0, g + g0, (g1 - g0) * 2);
    if (b < bx1) memcpy(dst + b, base + b, (bx1 - b) * 2);
}

void GlassUi::drawZone(int z, uint16_t* fb, int stride, int xoff)
{
    const Rect r = zoneRect(z);
    uint16_t* f  = fb + xoff;  // f[y * stride + x] が画面の (x, y)
    for (int y = r.y0; y < r.y1; ++y) compose(y, r.x0, r.x1, f + y * stride);
    const int _cx = kC + _ox, _cy = kC + _oy;  // 画面の真ん中 (ずれを入れた位置)
    switch (z) {
        case kZEq: {  // 再生中バー (緑の 4 本)
            const int bx  = _cx - kSideX - 9;
            const int bot = _cy + kSideY + 8;
            const int* col = _playing ? kGreen : kGray;
            for (int i = 0; i < 4; ++i) {
                const float h  = 3.0f + 14.0f * clampf(_eq[i], 0, 1);
                const float x0 = bx + i * 5.0f, top = bot - h;
                for (int y = std::max(r.y0, static_cast<int>(top) - 1); y < std::min(r.y1, bot); ++y) {
                    const float cy = clampf(y + 1.0f - top, 0, 1);  // 上のふちをなめらかに
                    for (int x = static_cast<int>(x0); x < static_cast<int>(x0) + 3; ++x) {
                        if (x < r.x0 || x >= r.x1) continue;
                        blendPx(&f[y * stride + x], col[0], col[1], col[2], static_cast<int>(cy * 256));
                    }
                }
            }
            break;
        }
        case kZLike: drawLike(f, stride, r, _cx + kSideX + 0.5f, _cy + kSideY + 0.5f, 1.0f); break;
        case kZALike:
            drawTimesAt(f, stride, r, _cy + kALikeY, _cx - 20, _cx + 20, _detailA);  // 詳細: ♥ の左に経過、右に長さ
            // ⊕ も ♥ 済みの ✓ も、触っていないときは出さない。操作のあと詳細と一緒に出る
            _zoneAlpha = _uiDim * _detailA;
            if (_zoneAlpha > 0.01f) drawLike(f, stride, r, _cx + 0.5f, _cy + kALikeY + 0.5f, 0.72f);
            _zoneAlpha = 1.0f;
            break;
        case kZProg:  // 再生バー (白) と時間
            drawBar(f, stride, r, static_cast<float>(_cx), _cy + kProgY + 0.0f, kProgHalf, 2.5f, 6.0f);
            drawTimes(f, stride, r);
            break;
        case kZAProg:
            _zoneAlpha = _uiDim;
            drawBar(f, stride, r, static_cast<float>(_cx), _cy + kAProgY + 0.0f, kAProgHalf, 2.0f, 4.5f);
            _zoneAlpha = 1.0f;
            break;
        case kZADev:  // 詳細: 機器名 (真ん中そろえ)
            blitText(f, stride, _device, _cx, _cy - 148, 0, kGreen[0], kGreen[1], kGreen[2], _detailA, r, 0, 0, kTextShadow);
            break;
        case kZABpm:  // 詳細: BPM は再生バーのすぐ上 (触っていないときは出さない)
            if (_bpmText.m.a) {
                blitText(f, stride, _bpmText, _cx, _cy + kABpmY, 0, kGray[0], kGray[1], kGray[2], _detailA, r, 0, 0, kTextShadow);
            }
            break;
        case kZATitle: {  // 影は詳細が出ている間だけ (ふだんはふちどりなし)。流れない文字は下地にあるので影だけ (titleLite なら文字も)
            const float ta = _titleLite ? _uiDim * _titleIn * _titleIn * (3 - 2 * _titleIn) : _uiDim;
            blitText(f, stride, _titleS, _cx, _cy + kATitleY, 0, 255, 255, 255, ta, r, _titleScroll, kAWin, kTextShadow * _detailA,
                     _titleSMarquee || _titleLite);
            break;
        }
        case kZAArtist: {
            const float ta = _titleLite ? _uiDim * _titleIn * _titleIn * (3 - 2 * _titleIn) : _uiDim;
            blitText(f, stride, _artistS, _cx, _cy + kAArtistY, 0, 210, 210, 210, ta, r, _artistScroll, kAWin, kTextShadow * _detailA,
                     _artistSMarquee || _titleLite);
            break;
        }
        case kZTitle:
            blitText(f, stride, _title, _cx, _cy + kTitleY, 0, 255, 255, 255, 1.0f, r, _titleScroll, kWin);
            break;
        case kZArtist:
            blitText(f, stride, _artist, _cx, _cy + kArtistY, 0, kGray[0], kGray[1], kGray[2], 1.0f, r, _artistScroll, kWin);
            break;
        case kZCap: {  // カプセルのボタン: 前・再生 (白い丸)・次。押すと光が広がる
            const float cy = _cy + kCapY + 0.5f;
            const float bxs[3] = {_cx - kBtnX + 0.5f, _cx + 0.5f, _cx + kBtnX + 0.5f};
            for (int y = r.y0; y < r.y1; ++y) {
                for (int x = r.x0; x < r.x1; ++x) {
                    const float px = x + 0.5f, py = y + 0.5f;
                    const float inCap = clampf(0.5f - sdRoundRect(px - _cx - 0.5f, py - cy, kCapHW, kCapHH, kCapHH), 0, 1);
                    if (inCap <= 0) continue;
                    uint16_t* dst = &f[y * stride + x];
                    for (int i = 0; i < 3; ++i) {
                        const float dx = px - bxs[i], dy = py - cy;
                        const float d  = fastSqrt(dx * dx + dy * dy);
                        // 押したときの光
                        if (_press[i] < 1) {
                            const float rr = 12 + 26 * _press[i];
                            const float a  = (1 - _press[i]) * 0.35f * clampf(rr - d, 0, 1) * inCap;
                            if (a > 0) addPx(dst, static_cast<int>(255 * a), static_cast<int>(255 * a), static_cast<int>(255 * a));
                        }
                        if (i == 1) {  // 再生ボタン: 白い丸に黒の印
                            const float pr = kPlayR * (1.0f - 0.08f * (1 - _press[1]) * (_press[1] < 1));
                            if (d > pr + 1) continue;
                            blendPx(dst, 255, 255, 255, static_cast<int>(clampf(pr + 0.5f - d, 0, 1) * 256));
                            float cov = 0;
                            for (int q = 0; q < 4; ++q) {
                                const float u = (dx + (q & 1 ? 0.25f : -0.25f)) / pr, v = (dy + (q & 2 ? 0.25f : -0.25f)) / pr;
                                bool in;
                                if (_capPlaying) {
                                    in = fabsf(v) <= 0.4f && fabsf(u) >= 0.1f && fabsf(u) <= 0.33f;
                                } else {
                                    in = u >= -0.26f && fabsf(v) <= 0.42f * (0.46f - u) / 0.72f;
                                }
                                if (in) cov += 0.25f;
                            }
                            if (cov > 0) blendPx(dst, 18, 18, 18, static_cast<int>(cov * 256));
                        } else {  // 前・次: 白い三角と棒
                            if (fabsf(dx) > 12 || fabsf(dy) > 10) continue;
                            float cov = 0;
                            for (int q = 0; q < 4; ++q) {
                                float u = (dx + (q & 1 ? 0.25f : -0.25f)) / 10.0f, v = (dy + (q & 2 ? 0.25f : -0.25f)) / 10.0f;
                                if (i == 0) u = -u;
                                const bool tri = u >= -0.7f && u <= 0.5f && fabsf(v) <= 0.8f * (0.5f - u) / 1.2f;
                                const bool bar = u >= 0.55f && u <= 0.8f && fabsf(v) <= 0.8f;
                                if (tri || bar) cov += 0.25f;
                            }
                            if (cov > 0) blendPx(dst, 255, 255, 255, static_cast<int>(cov * 0.95f * 256));
                        }
                    }
                }
            }
            break;
        }
        default: break;
    }
}

// ============================================================================ 1 コマ

void GlassUi::frame(float dt, const Frame& micIn, uint16_t* fb, int stride, int xoff, Dirty* out, int* nOut)
{
    _frameFlags = 0;
    int nd      = 0;
    auto add    = [&](const Rect& r) {
        if (r.x1 <= r.x0 || r.y1 <= r.y0) return;
        if (nd < kMaxDirty) {
            out[nd++] = {static_cast<int16_t>(r.x0), static_cast<int16_t>(r.y0), static_cast<int16_t>(r.x1 - r.x0),
                         static_cast<int16_t>(r.y1 - r.y0)};
        } else {  // 入りきらなければ、最後のものを広げる
            Dirty& d   = out[kMaxDirty - 1];
            const int x0 = std::min<int>(d.x, r.x0), y0 = std::min<int>(d.y, r.y0);
            const int x1 = std::max<int>(d.x + d.w, r.x1), y1 = std::max<int>(d.y + d.h, r.y1);
            d          = {static_cast<int16_t>(x0), static_cast<int16_t>(y0), static_cast<int16_t>(x1 - x0), static_cast<int16_t>(y1 - y0)};
        }
    };
    const uint32_t t0 = nowUs();
    uint32_t tp = t0;
    auto part   = [&](int k) {
        const uint32_t n = nowUs();
        _parts[k] += n - tp;
        tp = n;
    };
    // 拍で動かす: テンポが分かっていれば、マイクの代わりに拍から「音」を作る (拍の頭で強く、すぐ弱まる。小節の頭は特に強く)
    Frame beatIn;
    const Frame* inp = &micIn;
    // Spotify 側のテンポがあれば再生位置から (0 秒 = 1 拍目)。なければマイクで聞いたテンポで、自分の時計を聞いた拍に寄せる
    _beatOn = _bpm > 0 || _micBpm > 0;
    if (_bpm <= 0 && _micBpm > 0) {
        const float cdt = clampf(dt, 0, 0.2f);
        if (_playing) _micBeatF += cdt * _micBpm / 60.0f;
        const float target = _micAgo * _micBpm / 60.0f;  // 最後の拍から何拍たったか
        float err          = target - static_cast<float>(_micBeatF - floor(_micBeatF));
        err -= floorf(err + 0.5f);  // -0.5..0.5 に
        _micBeatF += err * std::min(1.0f, cdt * 1.5f);
    }
    if (_beatOn) {
        const float beatEnergy = _bpm > 0 ? _beatEnergy : _micEnergy;
        const double beatF     = _bpm > 0 ? (static_cast<double>(_posMs) - _beatOffsetMs) / 1000.0 * _bpm / 60.0 : _micBeatF;
        const int64_t idx      = static_cast<int64_t>(floor(beatF));
        _beatPh            = static_cast<float>(beatF - idx);
        if (_playing && idx != _lastBeat) {
            beatIn.kicks = 1;
            _downbeat    = (idx & 3) == 0;
            _beatSeed    = static_cast<uint32_t>(idx) * 2654435761u + 12345u;
        }
        _lastBeat        = idx;
        const float env  = _playing ? expf(-_beatPh * 5.0f) * (_downbeat ? 1.0f : 0.8f) : 0.0f;
        const float half = _beatPh * 2 - floorf(_beatPh * 2);
        const float env8 = _playing ? expf(-half * 7.0f) * 0.6f : 0.0f;
        const float k    = 0.45f + 0.75f * beatEnergy;
        uint32_t r       = _beatSeed;
        for (int b = 0; b < kBands; ++b) {
            r                 = r * 1664525u + 1013904223u;
            const float p     = 0.45f + 0.55f * ((r >> 8) & 255) / 255.0f;  // 拍ごとに形を変える
            float v;
            if (b < 7) {
                v = env * (0.75f + 0.25f * p);
            } else if (b < 20) {
                v = env * 0.65f * p + env8 * 0.25f;
            } else {
                v = env8 * 0.8f * p + env * 0.3f;
            }
            beatIn.band[b] = clampf(v * k, 0, 1);
        }
        beatIn.energy = clampf((0.25f + 0.6f * env) * k, 0, 1);
        beatIn.quiet  = !_playing;
        inp           = &beatIn;
    }
    const Frame& in = *inp;
    dt                = clampf(dt, 0.001f, 0.2f);
    _t += dt;
    _forceT     = std::max(0.0f, _forceT - dt);
    _likeForceT = std::max(0.0f, _likeForceT - dt);
    bool full   = _fullSend;
    _fullSend   = false;

    // エフェクトと詳細
    _fxT += dt;
    _detailT = std::max(0.0f, _detailT - dt);
    const float detailTarget = _detailT > 0 ? 1.0f : 0.0f;
    _detailA += (detailTarget - _detailA) * std::min(1.0f, dt * 6.0f);  // ふわっと出て、ふわっと消える
    if (fabsf(_detailA - detailTarget) < 0.01f) _detailA = detailTarget;
    // 止めているときはジャケットを暗く (下地を作り直す)
    if (_mode == Mode::Player && _ambient && _basePaused == _playing) _baseDirty = true;
    // 20 秒さわらないと、ボタンを消した静かな画面に
    _idleT += dt;
    if (_mode == Mode::Player && !_ambient && _idleT > _ambientAfter) {
        _ambient   = true;
        _baseDirty = true;
    }

    // 焼き付き対策: 画面全体が半径 14px の円を描くように、1.5 秒に 1 歩ずつ漂い続ける (約 1 分半で 1 周)
    _driftT += dt;
    if (_driftT >= 1.5f) {
        _driftT = 0;
        ++_driftStep;
        const float a = _driftStep * (6.2831853f / 64.0f);
        _dx           = static_cast<int>(lroundf(14.0f * cosf(a)));
        _dy           = static_cast<int>(lroundf(14.0f * sinf(a)));
    }
    // 静かな画面が 1 分続いたら、小さい曲名・再生バー・♥ を半分の明るさに (曲が変わるか、さわると戻る)
    if (_mode == Mode::Player && _ambient) {
        _ambientT += dt;
        if (_ambientT > 60.0f && _uiDim > 0.6f) {
            _uiDim     = 0.5f;
            _baseDirty = true;
        }
    } else {
        _ambientT = 0;
        _uiDim    = 1.0f;
    }
    // 静かな画面で強いキック: ジャケットがガクッと揺れる (虹彩は画面に固定なので目立つ)
    _shakeT = std::max(0.0f, _shakeT - dt);
    if (_mode == Mode::Player && _ambient && _viz == Viz::Iris && _playing && in.kicks > 0 && in.energy > 0.25f && _shakeT <= 0) {
        _rng        = _rng * 1103515245u + 12345u;
        _shakeX     = static_cast<int>((_rng >> 16) % 9) - 4;
        _shakeY     = static_cast<int>((_rng >> 20) % 7) - 3;
        _shakeT     = 0.09f;
    }
    if (_shakeT <= 0) _shakeX = _shakeY = 0;
    {
        const int nx = _dx + _shakeX, ny = _dy + _shakeY;
        if (nx != _ox || ny != _oy) {
            _ox  = nx;
            _oy  = ny;
            full = true;
        }
    }

    // 再生中バー: マイクの音で動く。静か (イヤホンなど) でも再生中なら、それらしく揺らす。止めたら低く止まる
    static const int kGroups[5] = {1, 6, 13, 21, 31};
    for (int i = 0; i < 4; ++i) {
        float target;
        if (!_playing) {
            target = 0.12f + 0.06f * (i % 2);
        } else if (in.quiet) {
            target = 0.35f + 0.3f * sinf(_t * (5.1f + i * 1.7f) + i * 1.3f);
        } else {
            float m = 0;
            for (int b = kGroups[i]; b < kGroups[i + 1]; ++b) m = std::max(m, in.band[b]);
            target = 0.1f + 0.9f * m;
        }
        const int n   = std::max(1, static_cast<int>(ceilf(dt * 120.0f)));
        const float h = dt / n;
        for (int k = 0; k < n; ++k) {
            _eqV[i] += (500.0f * (target - _eq[i]) - 26.0f * _eqV[i]) * h;
            _eq[i] += _eqV[i] * h;
        }
        _eq[i] = clampf(_eq[i], 0, 1.1f);
    }
    _likePop.step(1, 260, 14, dt);
    _likeBurst = std::min(1.0f, _likeBurst + dt / 0.6f);
    for (float& p : _press) p = std::min(1.0f, p + dt / 0.45f);
    _scrollT += dt;
    const float sc = std::max(0.0f, _scrollT - 2.5f);
    _titleScroll   = sc * 30.0f;
    _artistScroll  = sc * 26.0f;

    // 背景の光: にぎやかさとキックで明るく (再生中だけ)。段階は 1 コマに 1 つずつ、1 秒に 15 回まで動かす
    const bool player = _mode == Mode::Player;
    if (player && _playing) {
        if (in.kicks > 0) _kickPunch = std::max(_kickPunch, 0.55f + 0.45f * in.energy);
    }
    _kickPunch *= expf(-dt * 4.0f);
    const float target = (player && _playing && !in.quiet) ? clampf(std::max(0.85f * in.energy, _kickPunch), 0, 1) : 0.0f;
    if (target > _glowVal) {
        _glowVal = target;  // 上がるときはすぐ
    } else {
        _glowVal += (target - _glowVal) * std::min(1.0f, dt * 7.0f);  // 下がるときは 0.15 秒ほどで
    }
    // 虹彩の筋: 帯ごとの強さ (上がるときはすぐ、下がるときは 0.15 秒ほどで)
    for (int b = 0; b < kBands; ++b) {
        const float v = (_playing && !in.quiet) ? in.band[b] : 0.0f;
        _irisLvl[b]   = v > _irisLvl[b] ? v : _irisLvl[b] + (v - _irisLvl[b]) * std::min(1.0f, dt * 7.0f);
    }
    // 波形の線: 低い音ほど大きくゆっくりうねり、高い音は細かく小さく。成分ごとに少しずつ回る (上がるときは速め、下がるときはゆっくり)
    {
        static const int kHarmBand[kWaveHarm + 1] = {1, 3, 5, 8, 11, 15, 19, 23, 27, 31};
        for (int k = 0; k < kWaveHarm; ++k) {
            float m = 0;
            if (_playing && !in.quiet) {
                for (int b = kHarmBand[k]; b < kHarmBand[k + 1]; ++b) m = std::max(m, in.band[b]);
            }
            float target = (13.0f - 1.1f * k) * m + 0.8f;  // px
            // 拍の手がかりがなく、マイクも静か (イヤホンなど): 低い成分だけ、ゆっくり呼吸するようにゆらす
            if (_playing && in.quiet && !_beatOn && k < 4) target += (3.2f - 0.6f * k) * (0.5f + 0.5f * sinf(_t * 1.3f + k * 1.1f));
            _waveAmp[k] += (target - _waveAmp[k]) * std::min(1.0f, dt * (target > _waveAmp[k] ? 14.0f : 3.0f));
            _wavePhase[k] += dt * (0.25f + 0.11f * k) * (k & 1 ? -1.0f : 1.0f);
        }
        // 拍で動かすときは、拍の頭で弾いた弦のように震えて収まる (1 拍の間に数回ゆれる。高い成分ほど細かく)。形は拍ごとに変わる
        float amp[kWaveHarm];
        float ph[kWaveHarm];
        uint32_t rr = _beatSeed;
        for (int k = 0; k < kWaveHarm; ++k) {
            rr    = rr * 1664525u + 1013904223u;
            ph[k] = _wavePhase[k];
            if (_beatOn) {
                const float env  = _playing ? expf(-_beatPh * 4.0f) * (_downbeat ? 1.35f : 1.0f) : 0.0f;
                const float wob  = cosf(6.2831853f * (1.5f + 0.45f * k) * _beatPh);
                const float size = (12.0f - 1.0f * k) * (0.45f + 0.75f * (_bpm > 0 ? _beatEnergy : _micEnergy));
                amp[k]           = size * env * wob + 1.2f;
                ph[k] += ((rr >> 10) & 1023) * (6.2831853f / 1024.0f);
            } else {
                amp[k] = _waveAmp[k];
            }
        }
        float pulse = 5.0f * _kickPunch;
        if ((_fx == kFxVolUp || _fx == kFxVolDown) && _fxT < 0.6f) pulse += (_fx == kFxVolUp ? 8.0f : -8.0f) * sinf(3.14159f * _fxT / 0.6f);
        int phIdx[kWaveHarm];
        for (int k = 0; k < kWaveHarm; ++k) phIdx[k] = static_cast<int>(ph[k] * (kWaveAngles / 6.2831853f)) & (kWaveAngles - 1);
        const float k1 = std::min(1.0f, dt * 5.0f), k2 = std::min(1.0f, dt * 4.0f);
        for (int i = 0; i < kWaveAngles; ++i) {
            float r = kWaveR + pulse;
            for (int k = 0; k < kWaveHarm; ++k) r += amp[k] * _sinTab[(i * (k + 2) + phIdx[k]) & (kWaveAngles - 1)];
            r            = clampf(r, kWaveR - kWaveBand + 3, kWaveR + kWaveBand - 3);
            _waveCur[i]  = r;
            _waveEcho1[i] += (r - _waveEcho1[i]) * k1;
            _waveEcho2[i] += (_waveEcho1[i] - _waveEcho2[i]) * k2;
            _w8[i]                   = static_cast<int16_t>(r * 8);
            _w8[kWaveAngles + i]     = static_cast<int16_t>(_waveEcho1[i] * 8);
            _w8[2 * kWaveAngles + i] = static_cast<int16_t>(_waveEcho2[i] * 8);
        }
    }
    if (in.kicks > 0 && _playing) _irisHue += 0.07f;  // キックのたびに虹色が回る
    _irisHueNow += (_irisHue - _irisHueNow) * std::min(1.0f, dt * 6.0f);
    for (int sp = 0; sp < kSpokes && _viz == Viz::Iris; ++sp) {
        const float rest = 3.0f + 2.0f * (0.5f + 0.5f * sinf(_t * 1.3f + sp * 0.21f));  // 音がないときは、ゆっくり息をする
        const float jit  = 0.7f + 0.3f * _irisJit[sp] / 255.0f;
        const float lvl  = powf(_irisLvl[_irisBand[sp]], 0.8f);
        _irisLen[sp]     = std::min(_irisMax[sp], rest + (_irisMax[sp] - 6.0f) * lvl * jit + 18.0f * _kickPunch * _irisMax[sp] / 82.0f);
    }

    part(0);  // ここまで: 線の形・拍・動きの計算
    // 下地
    if (_bgDirty && !(player && _ambient)) {  // 静かな画面では使わない (ジャケットいっぱいの下地だけ)
        rebuildBg();
        _bgDirty = false;
    }
    if (_baseDirty) {
        if (_fadeReq && !_fullSend) {  // 今の画面を、切り替えの元として取っておく
            for (int y = 0; y < kSize; ++y) memcpy(_old + y * kSize, fb + y * stride + xoff, kSize * 2);
            _fadeT = 0;
        }
        _fadeReq = false;
        rebuildBase();
        _baseDirty = false;
        _frameFlags |= 1;
        _glowShown = 0;
        full       = true;
    }
    if (_mode == Mode::Message) {  // 案内の画面: 背景の粒が流れるので、毎コマ全体を描いて送る
        noticeFrame(fb + xoff, stride, dt);
        add({0, 0, kSize, kSize});
        *nOut = nd;
        _drawUs += nowUs() - t0;
        return;
    }
    // 光の段階の絵を、まだなら 1 コマに 1 つ作る (よく使う弱いほうから)
    if (player && !_ambient) {
        for (int l = 1; l < kGlowLevels; ++l) {
            if (!(_glowReady & (1u << l))) {
                makeGlowLevel(l);
                break;
            }
        }
    }
    int want = (player && !_ambient) ? static_cast<int>(lroundf(_glowVal * (kGlowLevels - 1))) : 0;
    while (want > 0 && !(_glowReady & (1u << want))) --want;  // できている段階まで
    if (_ambient) want = 0;
    bool glowChanged = false;
    if (want != _glowShown) {  // 段階は飛ばしてよい (前もって作ってあるので、写すだけ)
        _glowShown  = want;
        glowChanged = true;
    }

    const bool fxOn = player && _ambient && _fx != kFxNone && _fxT < fxDur();
    // エフェクト中も全部は描き直さない: 前のコマでエフェクトを描いた四角だけ下地に戻し、今のコマの分を描いて送る
    _titleInPrev      = _titleIn;
    _titleIn          = std::min(1.0f, _titleIn + dt / 0.5f);
    const bool fading = _fadeT < 1;
    if (fading) full = true;
#ifdef GLASS_FULLREF
    full = true;  // Mac のシミュレーターで、毎コマ全部描き直したお手本と比べる用
#endif
    _fxWas = fxOn;
    bool active[kZones] = {};
    if (player) {
        const bool capPlaying = _playing;
        active[kZEq]     = true;
        active[kZLike]   = fabsf(_likePop.x - 1) > 0.004f || fabsf(_likePop.v) > 0.02f || _likeBurst < 1 || _liked != _lastLiked ||
                         _canLike != _lastCan;
        const int key    = static_cast<int>(_durMs ? 2.0f * kProgHalf * _posMs / _durMs : 0) * 100000 + static_cast<int>(_posMs / 1000);
        active[kZProg]   = key != _progKey;
        _progKey         = key;
        active[kZTitle]  = _titleMarquee;
        active[kZArtist] = _artistMarquee;
        active[kZCap]    = capPlaying != _capPlaying || _press[0] < 1 || _press[1] < 1 || _press[2] < 1;
        active[kZAProg]  = active[kZProg];
        active[kZALike]  = active[kZLike];
        const bool detailChanged = _detailA != _detailAWas;
        const bool titleAnim     = _titleLite && _titleInPrev < 1.0f;  // ふわっと出ているあいだ (出きったコマも)
        active[kZATitle]         = _titleSMarquee || detailChanged || titleAnim;  // 詳細が出る・消えるあいだは、文字の影が変わる
        active[kZAArtist]        = _artistSMarquee || detailChanged || titleAnim;
        _detailAWas              = _detailA;
        active[kZADev]           = detailChanged;
        active[kZABpm]           = detailChanged || (_bpmTextChanged && _detailA > 0);
        _bpmTextChanged          = false;
        if (detailChanged) active[kZALike] = true;
        if (_detailA > 0 && active[kZAProg]) active[kZALike] = true;  // 時間が進んだら
        _capPlaying      = capPlaying;
        _lastLiked       = _liked;
        _lastCan         = _canLike;
    }

    uint16_t* f = fb + xoff;
    if (full) _frameFlags |= 2;
    if (fading) _frameFlags |= 4;
    if (fxOn && (_fx == kFxNext || _fx == kFxPrev)) _frameFlags |= 16;
    if (full) {
        // 全部の行を下地から写し、曲の切り替え中は前の画面と混ぜる (上半分・下半分を 2 つのコアで)
        struct FullCtx {
            GlassUi* ui;
            uint16_t* f;
            int stride, a;
            bool fading;
            int a1, a2;  // ぼやけを通すとき: 前の画面 → ぼやけ (a1)、→ 本物 (a2)
            bool blur;
        } fc{this, f, stride, 0, fading, 0, 0, false};
        if (fading) {  // 曲が変わった: 前の画面から新しい画面へ、ふわっと
            _fadeT        = std::min(1.0f, _fadeT + dt / std::max(0.05f, _fadeDur));
            const float e = _fadeT * _fadeT * (3 - 2 * _fadeT);
            fc.a          = static_cast<int>(e * 256);
            if (_blurFade) {
                fc.blur = true;
                fc.a1   = static_cast<int>(smooth(0.0f, 0.55f, _fadeT) * 256);
                fc.a2   = static_cast<int>(smooth(0.35f, 1.0f, _fadeT) * 256);
            }
        }
        split(
            [](void* arg, int part) {
                const auto* c = static_cast<const FullCtx*>(arg);
                const int y0 = part ? kSize / 2 : 0, y1 = part ? kSize : kSize / 2;
                for (int y = y0; y < y1; ++y) {
                    uint16_t* row = c->f + y * c->stride;
                    c->ui->compose(y, 0, kSize, row);
                    if (!c->fading) continue;
                    const uint16_t* o = c->ui->_old + y * kSize;
                    const int a       = c->a;
                    if (c->blur) {  // ドット絵の画面 → 16 マスをなめらかにつないだぼやけ → 本物
                        GlassUi* ui    = c->ui;
                        const float fy = clampf((y - ui->_oy + 0.5f) * kPixGrid / kSize - 0.5f, 0, kPixGrid - 1.001f);
                        const int y0   = static_cast<int>(fy);
                        const int ty   = static_cast<int>((fy - y0) * 256);
                        const uint8_t* r0 = ui->_blurRGB + y0 * kPixGrid * 3;
                        const uint8_t* r1 = r0 + kPixGrid * 3;
                        for (int x = 0; x < kSize; ++x) {
                            const int bx = clampi(x - ui->_ox, 0, kSize - 1);
                            const int x0 = ui->_blurX0[bx], tx = ui->_blurXT[bx];
                            int bc[3];
                            for (int k = 0; k < 3; ++k) {
                                const int top = r0[x0 * 3 + k] * 256 + (r0[(x0 + 1) * 3 + k] - r0[x0 * 3 + k]) * tx;
                                const int bot = r1[x0 * 3 + k] * 256 + (r1[(x0 + 1) * 3 + k] - r1[x0 * 3 + k]) * tx;
                                bc[k]         = (top + (((bot - top) * ty) >> 8)) >> 8;
                            }
                            int r0c, g0c, b0c, r1c, g1c, b1c;
                            unpack(o[x], r0c, g0c, b0c);
                            unpack(row[x], r1c, g1c, b1c);
                            const int pr = r0c + (((bc[0] - r0c) * c->a1) >> 8), pg = g0c + (((bc[1] - g0c) * c->a1) >> 8),
                                      pb = b0c + (((bc[2] - b0c) * c->a1) >> 8);
                            row[x] = pack(pr + (((r1c - pr) * c->a2) >> 8), pg + (((g1c - pg) * c->a2) >> 8), pb + (((b1c - pb) * c->a2) >> 8));
                        }
                        continue;
                    }
                    for (int x = 0; x < kSize; ++x) {
                        if (o[x] == row[x]) continue;
                        int r0, g0, b0, r1, g1, b1;
                        unpack(o[x], r0, g0, b0);
                        unpack(row[x], r1, g1, b1);
                        row[x] = pack(r0 + (((r1 - r0) * a) >> 8), g0 + (((g1 - g0) * a) >> 8), b0 + (((b1 - b0) * a) >> 8));
                    }
                }
            },
            &fc);
        if (fading) {
            if (_fadeT >= 1) {
                _fullSend = true;  // 切り替えが終わったら、動く部分も入れて描き直す
                _blurFade = false;
            }
        }
        for (int z = 0; z < kZones; ++z) {
            if (zoneOn(z) && !fading) drawZone(z, fb, stride, xoff);
        }
        if (fading && player && _ambient) {  // 切り替え中も、流れる曲名とアーティストは文字だけ重ねる (欄の下地は塗らない)
            const int ccx = kC + _ox, ccy = kC + _oy;
            const float ta = _uiDim * _titleIn * _titleIn * (3 - 2 * _titleIn);  // 曲が替わった直後は、ふわっと濃く
            if (_titleSMarquee || _titleLite) {
                blitText(f, stride, _titleS, ccx, ccy + kATitleY, 0, 255, 255, 255, ta, zoneRect(kZATitle), _titleScroll, kAWin);
            }
            if (_artistSMarquee || _titleLite) {
                blitText(f, stride, _artistS, ccx, ccy + kAArtistY, 0, 210, 210, 210, ta, zoneRect(kZAArtist), _artistScroll, kAWin);
            }
        }
        add({0, 0, kSize, kSize});
    } else {
        for (int k = 0; k < _fxPrevN; ++k) {  // 前のコマのエフェクトを消す (下地に戻す)。重なる文字の欄は描き直す
            const Rect& r = _fxPrev[k];
            for (int y = r.y0; y < r.y1; ++y) compose(y, r.x0, r.x1, f + y * stride);
            add(r);
            for (int z = 0; z < kZones; ++z) {
                const Rect zr = zoneRect(z);
                if (zr.y0 < r.y1 && zr.y1 > r.y0 && zr.x0 < r.x1 && zr.x1 > r.x0) active[z] = true;
            }
        }
        if (glowChanged) {  // 光る範囲を写し直す (その中の動く部分も描き直す)
            const int gy0 = std::max(0, kGlowY0 + _oy), gy1 = std::min(kSize, kGlowY1 + _oy);
            const int gx0 = std::max(0, kGlowX0 + _ox), gx1 = std::min(kSize, kGlowX1 + _ox);
            for (int y = gy0; y < gy1; ++y) compose(y, gx0, gx1, f + y * stride);
            add({gx0, gy0, gx1, gy1});
            for (int z = 0; z < kZones; ++z) {
                const Rect r = zoneRect(z);
                if (r.y0 < gy1 && r.y1 > gy0 && r.x0 < gx1 && r.x1 > gx0) active[z] = true;
            }
        }
        for (int z = 0; z < kZones; ++z) {
            if (!zoneOn(z) || !(active[z] || _zoneWas[z])) continue;
            drawZone(z, fb, stride, xoff);
            add(zoneRect(z));
        }
    }
    for (int z = 0; z < kZones; ++z) _zoneWas[z] = active[z] && zoneOn(z);
    part(2);  // 全体の描き直しと動く部分
    // 静かな画面: 虹彩は毎コマ描き直す (画面全体の行を送る)
    if (player && _ambient) {
        if (_viz == Viz::Wave) {
            // 彗星 (曲送り) は線から少し離れた所も光るので、走っている間と消えたコマは帯を全部描く
            const bool comet = fxOn && (_fx == kFxNext || _fx == kFxPrev);
            _waveFromFb      = fading;
            drawWave(f, stride, full || comet || _cometWas);
            _waveFromFb = false;
            _cometWas   = comet;
            part(1);
        } else {
            drawIris(f, stride);
        }
        if (fxOn) drawFx(f, stride);
        _fxPrevN = fxOn ? fxRects(_fxPrev) : 0;
        if (!full) {
            for (int k = 0; k < _fxPrevN; ++k) add(_fxPrev[k]);
        }
        part(3);
        if (!full) {  // 波形の帯だけ送る (帯を囲むいくつかの四角)
            if (_viz == Viz::Wave) {
                for (int k = 0; k < _nWaveRects; ++k) add(_waveRects[k]);
            } else {
                add({0, 0, kSize, kSize});
            }
        }
    }

    *nOut = nd;
    _drawUs += nowUs() - t0;
}

}  // namespace spviz

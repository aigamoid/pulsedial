/*
 * Spotify リモコンの音の解析。説明は SpotifyViz.h
 */
#include "SpotifyViz.h"

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include "dsps_fft2r.h"
#define SPVIZ_DSP 1  // 実機は ESP32-S3 向けに速くした esp-dsp の FFT (手書きの FFT は 1 回 4ms 以上かかり、マイクの音を取りこぼした)
#endif

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

namespace spviz {

namespace {

constexpr float kTau = 6.28318531f;

inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

// ============================================================================ Analyzer

void Analyzer::begin()
{
    for (int n = 0; n < kFft; ++n) {
        _win[n] = 0.5f - 0.5f * cosf(kTau * n / (kFft - 1));
        _x[n]   = 0;
    }
    for (int k = 0; k < kFft / 2; ++k) {
        _cos[k] = cosf(kTau * k / kFft);
        _sin[k] = -sinf(kTau * k / kFft);
    }
    int bits = 0;
    while ((1 << bits) < kFft) ++bits;
    for (int n = 0; n < kFft; ++n) {
        int r = 0;
        for (int b = 0; b < bits; ++b) r |= ((n >> b) & 1) << (bits - 1 - b);
        _rev[n] = static_cast<uint16_t>(r);
    }
    // 帯の区切り (対数。低いほうは 1 本ずつ)
    const float binHz = static_cast<float>(kSampleRate) / kFft;
    int prev          = 0;
    for (int b = 0; b <= kBands; ++b) {
        const float f = 45.0f * powf(14000.0f / 45.0f, static_cast<float>(b) / kBands);
        int e         = static_cast<int>(lroundf(f / binHz));
        if (b > 0 && e <= prev) e = prev + 1;
        _edge[b] = e;
        prev     = e;
    }
    for (int b = 0; b < kBands; ++b) {
        const float fc = sqrtf(static_cast<float>(_edge[b]) * static_cast<float>(_edge[b + 1])) * binHz;
        _tilt[b]       = 3.0f * log2f(fc / 1000.0f);
        _floor[b]      = -90;
        _bandCeil[b]   = -60;
        _smooth[b]     = 0;
        _blockMin[b]   = 1e9f;
        _prevV[b]      = 0;
        _maxBand[b]    = 0;
    }
    for (float& v : _lowHist) v = -120;
    for (int i = 0; i < 86; ++i) _onHist[i] = _fluxHist[i] = 0;
    for (float& o : _oss) o = 0;
    _ossPos = _ossCount = _sinceEst = _hopsSinceEst = _candN = _lostHops = 0;
    _tempo = _periodHops = _phaseHops = 0;
    _lowPos = _histPos = _histCount = 0;
    _ceil = -60;
    _lowFloor = -90;
    _hpIn = _hpOut = 0;
    _hops          = 0;
    _minPos = _minCount = 0;
    _sinceKick = _sinceHit = 1000;
    _energy = _activity = 0;
    _quietHops          = 1000;
    _kicks = _hits = 0;
    _sharedEnergy  = 0;
    _sharedQuiet   = true;
}

float Analyzer::floorDb() const
{
    float s = 0;
    for (int b = 0; b < kBands; ++b) s += _floor[b];
    return s / kBands;
}

Analyzer::~Analyzer()
{
#ifdef SPVIZ_DSP
    heap_caps_free(_cbuf);
    heap_caps_free(_wtab);
#endif
}

void Analyzer::fft()
{
#ifdef SPVIZ_DSP
    if (!_cbuf) {  // 初めて: 入れ物と回転の表 (esp-dsp と同じ作り方: 作って、半分の長さでビット反転)
        _cbuf = static_cast<float*>(heap_caps_aligned_alloc(16, sizeof(float) * 2 * kFft, MALLOC_CAP_SPIRAM));
        _wtab = static_cast<float*>(heap_caps_aligned_alloc(16, sizeof(float) * kFft, MALLOC_CAP_SPIRAM));
        if (_cbuf && _wtab) {
            dsps_gen_w_r2_fc32(_wtab, kFft);
            dsps_bit_rev_fc32_ansi(_wtab, kFft >> 1);
        }
    }
    if (_cbuf && _wtab) {
        for (int n = 0; n < kFft; ++n) {
            _cbuf[2 * n]     = _x[n] * _win[n];
            _cbuf[2 * n + 1] = 0.0f;
        }
        dsps_fft2r_fc32_aes3_(_cbuf, kFft, _wtab);
        // 結果はビット反転の順に並ぶ: k 番目は _rev[k] の所 (並べ直さずに読む)
        const float norm = 1.0f / ((kFft / 4.0f) * (kFft / 4.0f));
        for (int k = 0; k < kFft / 2; ++k) {
            const float re = _cbuf[2 * _rev[k]], im = _cbuf[2 * _rev[k] + 1];
            _re[k]         = (re * re + im * im) * norm;
        }
        return;
    }
#endif
    for (int n = 0; n < kFft; ++n) {
        _re[_rev[n]] = _x[n] * _win[n];
        _im[_rev[n]] = 0.0f;
    }
    for (int len = 2; len <= kFft; len <<= 1) {
        const int half = len >> 1;
        const int step = kFft / len;
        for (int i = 0; i < kFft; i += len) {
            for (int j = 0; j < half; ++j) {
                const float wr = _cos[j * step];
                const float wi = _sin[j * step];
                const int a    = i + j;
                const int b    = a + half;
                const float xr = _re[b] * wr - _im[b] * wi;
                const float xi = _re[b] * wi + _im[b] * wr;
                _re[b]         = _re[a] - xr;
                _im[b]         = _im[a] - xi;
                _re[a] += xr;
                _im[a] += xi;
            }
        }
    }
    // 強さ (満振幅の正弦波が 0dB)
    const float norm = 1.0f / ((kFft / 4.0f) * (kFft / 4.0f));
    for (int k = 0; k < kFft / 2; ++k) _re[k] = (_re[k] * _re[k] + _im[k] * _im[k]) * norm;
}

void Analyzer::process(const int16_t* hop)
{
    memmove(_x, _x + kHop, sizeof(float) * (kFft - kHop));
    float* dst = _x + (kFft - kHop);
    for (int n = 0; n < kHop; ++n) {
        // 40Hz 以下 (直流やゴトゴト) を落とす
        const float in = hop[n] * (1.0f / 32768.0f);
        _hpOut         = 0.994f * (_hpOut + in - _hpIn);
        _hpIn          = in;
        dst[n]         = _hpOut;
    }
    fft();

    constexpr float dt = static_cast<float>(kHop) / kSampleRate;
    float db[kBands];
    float mx = -200, above = 0;
    for (int b = 0; b < kBands; ++b) {
        float p = 0;
        for (int k = _edge[b]; k < _edge[b + 1]; ++k) p += _re[k];
        db[b] = 10.0f * log10f(p + 1e-12f) + _tilt[b];
        mx    = std::max(mx, db[b]);
        // 雑音の高さは、ならした強さ (約 75ms) で測る (1 こまずつだと揺れが大きく、低く見積もってしまう)
        // 下がるときは早く、上がるときはゆっくり (音楽が続いても追いつかないように)
        _smooth[b]      = _smooth[b] * 0.85f + p * 0.15f;
        const float dbs = 10.0f * log10f(_smooth[b] + 1e-12f) + _tilt[b];
        // 雑音の高さ = 最近 60 秒でいちばん小さかった値 (2 秒ごとの最小を 30 個とっておく)。
        // 下がるのはすぐ、上がるのは古い記録が消えたときだけなので、曲が続いても音楽の高さまで上がりにくい
        if (_hops >= kFft / kHop) {  // 解析の窓に音がたまってから
            _blockMin[b] = std::min(_blockMin[b], dbs);
            float f      = _blockMin[b];
            for (int i = 0; i < _minCount; ++i) f = std::min(f, _mins[i][b]);
            _floor[b] = f;
        }
        above += std::max(0.0f, dbs - _floor[b]);
        // 帯ごとの最近の大きさ: すぐ上がって、1 秒に 8dB ずつ下がる
        _bandCeil[b] = db[b] > _bandCeil[b] ? db[b] : _bandCeil[b] - 8.0f * dt;
    }
    above /= kBands;
    // 全体でいちばん大きい帯の高さ: すぐ上がって、1 秒に 3dB ずつ下がる
    _ceil = mx > _ceil ? mx : _ceil - 3.0f * dt;
    // 鳴っているか (帯の平均で、雑音よりどれだけ大きいか)
    const float act = clampf((above - 6.0f) / 8.0f, 0, 1);
    _activity += (act - _activity) * (act > _activity ? 0.3f : 0.02f);

    // 0..1 にする: 上は「その帯の最近の大きさ」と「全体の大きさ」の間 (どの帯もよく動き、弱い帯は控えめ)
    float v[kBands];
    float sum = 0, flux = 0;
    for (int b = 0; b < kBands; ++b) {
        const float top  = 0.5f * _bandCeil[b] + 0.5f * _ceil;
        const float lo   = std::max(_floor[b] + 6.0f, top - 20.0f);
        const float span = std::max(8.0f, top - lo);
        float x          = clampf((db[b] - lo) / span, 0, 1);
        x                = powf(x, 0.8f) * _activity;
        v[b]             = x;
        sum += x;
        if (b >= 6) flux += std::max(0.0f, x - _prevV[b]);
        _prevV[b] = x;
    }
    ++_hops;
    if (_hops % kBlockHops == 0) {
        for (int b = 0; b < kBands; ++b) {
            _mins[_minPos][b] = _blockMin[b];
            _blockMin[b]      = 1e9f;
        }
        _minPos   = (_minPos + 1) % kMinBlocks;
        _minCount = std::min(kMinBlocks, _minCount + 1);
    }
    const float e = std::min(1.0f, sum / kBands * 2.5f);
    _energy += (e - _energy) * (e > _energy ? 0.4f : 0.04f);

    // キック: 50〜250Hz の強さが、直前より急に上がったとき (スマホのスピーカーは低音が出ないので、少し上まで見る)
    const float binHz = static_cast<float>(kSampleRate) / kFft;
    const int k0 = static_cast<int>(50.0f / binHz), k1 = static_cast<int>(250.0f / binHz);
    float lowP = 0;
    for (int k = k0; k < k1; ++k) lowP += _re[k];
    const float lowDb = 10.0f * log10f(lowP + 1e-12f);
    float prevAvg     = 0;
    for (float h : _lowHist) prevAvg += h;
    prevAvg *= 0.25f;
    const float on               = std::max(0.0f, lowDb - prevAvg);
    _lowHist[_lowPos++ & 3]      = lowDb;
    if (lowDb < _lowFloor) {
        _lowFloor += (lowDb - _lowFloor) * 0.3f;
    } else {
        _lowFloor += 0.5f * dt;
    }

    // 最近 1 秒の平均とばらつきから、しきい値を決める
    float mOn = 0, mF = 0, sOn = 0, sF = 0;
    const int n = std::max(1, _histCount);
    for (int i = 0; i < _histCount; ++i) {
        mOn += _onHist[i];
        mF += _fluxHist[i];
    }
    mOn /= n;
    mF /= n;
    for (int i = 0; i < _histCount; ++i) {
        sOn += (_onHist[i] - mOn) * (_onHist[i] - mOn);
        sF += (_fluxHist[i] - mF) * (_fluxHist[i] - mF);
    }
    sOn = sqrtf(sOn / n);
    sF  = sqrtf(sF / n);
    _onHist[_histPos]   = on;
    _fluxHist[_histPos] = flux;
    _histPos            = (_histPos + 1) % 86;
    _histCount          = std::min(86, _histCount + 1);

    ++_sinceKick;
    ++_sinceHit;
    int kick = 0, hit = 0;
    if (_activity > 0.25f && on > std::max(3.0f, mOn + 1.6f * sOn) && _sinceKick > 20 && lowDb > _lowFloor + 9.0f) {
        kick       = 1;
        _sinceKick = 0;
    }
    if (_activity > 0.25f && flux > std::max(1.0f, mF + 1.8f * sF) && _sinceHit > 10) {
        hit       = 1;
        _sinceHit = 0;
    }
    _quietHops = _activity < 0.12f ? _quietHops + 1 : 0;

    if (_resetTempo.exchange(false)) {  // 曲が変わった: ためた分を捨てて聞き直す
        _ossCount = _candN = 0;
        _tempo    = 0;
    }
    // テンポ: 立ち上がりの強さ (高めの帯の増え方 + 低音の立ち上がり) をためる
    _oss[_ossPos] = flux + 0.05f * on;
    _ossPos       = (_ossPos + 1) % kOss;
    _ossCount     = std::min(kOss, _ossCount + 1);
    ++_hopsSinceEst;
    if (++_sinceEst >= 43) {  // 0.5 秒ごと
        _sinceEst = 0;
        estimateTempo();
    }
    float ago = 0;
    if (_tempo > 0 && _periodHops > 1) {
        // 立ち上がりは解析の窓のぶん遅れて見えるので、0.07 秒早めに (シミュレーターで 0.05〜0.06 秒。本体はマイクと画面のぶんを足す)
        const float lagHops = 0.07f * kSampleRate / kHop;
        ago = fmodf(_phaseHops + _hopsSinceEst + lagHops, _periodHops) * kHop / kSampleRate;
    }

    std::lock_guard<std::mutex> lock(_m);
    for (int b = 0; b < kBands; ++b) _maxBand[b] = std::max(_maxBand[b], v[b]);
    _kicks += kick;
    _hits += hit;
    _sharedEnergy = _energy;
    _sharedQuiet  = _quietHops > 130;
    _sharedTempo  = _tempo;
    _sharedAgo    = ago;
}

// 拍の間隔を探す (70〜180 BPM)。倍・半分の取りちがえを減らすため、2 倍の間隔の重なりも足し、120 BPM あたりを少しひいきする。
// 4 回の推定のうち 3 回が 4% 以内でそろったら確かとみなす
void Analyzer::estimateTempo()
{
    constexpr float hopRate = static_cast<float>(kSampleRate) / kHop;  // 86.1 回/秒
    if (_quietHops > 130 || _activity < 0.2f) {  // 静か: しばらく続いたら忘れる
        _lostHops += 43;
        if (_lostHops > 300) _tempo = 0;
        _candN = 0;
        return;
    }
    _lostHops = 0;
    if (_ossCount < 344) return;  // 4 秒たまってから
    const int n = _ossCount;
    float* x = _ossLin;
    float mean = 0;
    for (int i = 0; i < n; ++i) {
        x[i] = _oss[(_ossPos - n + i + kOss) % kOss];
        mean += x[i];
    }
    mean /= n;
    for (int i = 0; i < n; ++i) x[i] -= mean;
    constexpr int kLmin = 28, kLmax = 74;  // 184〜70 BPM
    float* ac = _ac;
    for (int L = 0; L <= 2 * kLmax + 1; ++L) {
        if (L != 0 && L < kLmin) {
            ac[L] = 0;
            continue;
        }
        float acc = 0;
        for (int i = L; i < n; ++i) acc += x[i] * x[i - L];
        ac[L] = acc / (n - L);
    }
    if (ac[0] <= 1e-9f) return;
    float* score = _score;
    for (int L = 0; L <= kLmax + 1; ++L) score[L] = 0;
    int best = -1;
    float bestS = 0;
    for (int L = kLmin; L <= kLmax; ++L) {
        const float bpm = 60.0f * hopRate / L;
        const float o   = log2f(bpm / 120.0f);
        const float w   = expf(-0.5f * o * o / (1.1f * 1.1f));
        score[L]        = (ac[L] + 0.5f * ac[2 * L]) * w;
        if (score[L] > bestS) {
            bestS = score[L];
            best  = L;
        }
    }
    if (best < 0) return;
    const float conf = ac[best] / ac[0];
    float Lf         = static_cast<float>(best);
    if (best > kLmin && best < kLmax) {  // 山の頂上を、となりの値から細かく
        const float a = score[best - 1], b = score[best], c = score[best + 1];
        const float d = a - 2 * b + c;
        if (d < 0) Lf += 0.5f * (a - c) / d;
    }
    const float bpm = 60.0f * hopRate / Lf;
    if (conf < 0.12f) {
        _candN = 0;
        return;
    }
    // そろい具合を見る
    if (_candN == 4) {
        for (int i = 0; i < 3; ++i) _cand[i] = _cand[i + 1];
        _candN = 3;
    }
    _cand[_candN++] = bpm;
    int agree       = 0;
    for (int i = 0; i < _candN; ++i) agree += fabsf(_cand[i] - bpm) < bpm * 0.04f;
    if (_tempo > 0 && fabsf(bpm - _tempo) < _tempo * 0.04f) {
        _tempo += (bpm - _tempo) * 0.2f;  // 同じテンポ: ゆっくり寄せる
    } else if (agree >= 3) {
        _tempo = bpm;  // 新しいテンポ
    } else if (_tempo <= 0) {
        return;
    }
    // 拍の位置: 推定した間隔ごとに立ち上がりを足して、いちばん強くなる位置 (最後の拍から何ホップ前か)
    _periodHops = 60.0f * hopRate / _tempo;
    float bestPh = 0, bestSum = -1e30f;
    for (int ph = 0; ph < static_cast<int>(_periodHops); ++ph) {
        float sum = 0;
        for (int k = 0; k < 8; ++k) {
            const float pos = (n - 1) - ph - k * _periodHops;
            if (pos < 1) break;
            const int i0 = static_cast<int>(pos);
            const float t = pos - i0;
            sum += x[i0] * (1 - t) + x[i0 + 1 < n ? i0 + 1 : i0] * t;
        }
        if (sum > bestSum) {
            bestSum = sum;
            bestPh  = static_cast<float>(ph);
        }
    }
    _phaseHops    = bestPh;
    _hopsSinceEst = 0;
}

void Analyzer::take(Frame& out)
{
    std::lock_guard<std::mutex> lock(_m);
    for (int b = 0; b < kBands; ++b) {
        out.band[b]  = _maxBand[b];
        _maxBand[b]  = 0;
    }
    out.kicks  = _kicks;
    out.hits   = _hits;
    out.energy = _sharedEnergy;
    out.quiet  = _sharedQuiet;
    out.tempo   = _sharedTempo;
    out.beatAgo = _sharedAgo;
    _kicks = _hits = 0;
}

}  // namespace spviz

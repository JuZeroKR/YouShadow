#include "prosody.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

#include "json.hpp"
#include "scoring.h"

// 억양 · 리듬 · 강세 분석. 외부 라이브러리 없이 평범한 반복문만 쓴다 (10 초 클립 분석 ≪ 200 ms).
// 프레임 k 는 샘플 [160k, 160k+587) 을 본다. 587 = 상관 창 320 + 최대 지연 267.
// 프레임 중심 시각 = (160k + 293) / 16 ms ≈ k*10 + 18.3 ms. UI 는 k*10 + ~18 ms 로 시각에 붙이면 된다.
namespace prosody {
namespace {

// ---- 상수: 알고리즘 매개변수는 모두 여기에 ----
constexpr int kCorrWin = 320;                      // NCCF 상관 창 (20 ms)
constexpr int kLagMin = 40;                        // 400 Hz
constexpr int kLagMax = 267;                       // ≈ 60 Hz
constexpr int kFrameLen = kCorrWin + kLagMax;      // 587 샘플
constexpr float kFrameCenter = 293.f;              // 프레임 중심 (샘플, 프레임 시작 기준)
constexpr float kHpHz = 60.f;                      // 음높이 경로 고역 통과 (럼블 · 베이스 BGM)
constexpr float kLpHz = 1000.f;                    // 음높이 경로 저역 통과 (마찰음 잡음)
constexpr float kNccfVoiced = 0.60f;               // 주기성 문턱
constexpr float kVoicedAboveFloorDb = 10.f;        // 에너지 문턱 (바닥 + 10 dB)
constexpr float kLagBias = 0.03f;                  // 긴 지연을 조금 불리하게 (옥타브 아래로 떨어지는 것 방지, Praat 의 octave cost 와 비슷한 크기)
constexpr int kMedianHalf = 2;                     // 5 프레임 중앙값 필터
constexpr int kOctaveNbr = 5;                      // 옥타브 보정 때 보는 이웃 (±프레임)
constexpr float kOctaveHi = 1.5f, kOctaveLo = 0.67f, kOctaveTol = 0.15f;
constexpr int kMinIsland = 3;                      // 이보다 짧은 유성음 섬은 무성음으로
constexpr float kLoudOffsetDb = 10.f;              // loud 0 점 = 바닥 + 10 dB
constexpr float kMinDynamicsDb = 6.f;              // (95p − 5p − 10) 이 이보다 작으면 강세 비교 불가
constexpr int kWordPadMs = 50;                     // inWord 앞뒤 여유
constexpr int kSpeechPadMs = 100;                  // 말소리 구간 앞뒤 여유 (단어 있을 때)
constexpr int kSpeechPadFrames = 10;               // 말소리 구간 앞뒤 여유 (에너지로 잡을 때)
constexpr int kMinSpeechFrames = 30;               // 300 ms 미만이면 비교 생략
// DTW
constexpr float kDtwBandFrac = 0.25f;              // Sakoe-Chiba 띠 (정규화 위치 차)
constexpr int kDtwBandMin = 20;                    // 띠 최소 반폭 (프레임)
constexpr float kDtwStepPenalty = 0.1f;            // (1,0) · (0,1) 걸음 추가 비용
constexpr float kDtwPitchClampSt = 12.f, kDtwPitchScaleSt = 6.f;
constexpr float kSpanRatioMax = 2.5f, kSpanRatioMin = 0.4f;
constexpr int kMinVoicedCells = 30;                // 억양 비교에 필요한 유성음 셀 수
constexpr int kMinStressCells = 30;                // DTW 로 강세를 낼 때 필요한 셀 수
constexpr int kMinWordPitchCells = 5;              // 단어별 음높이 차이에 필요한 셀 수
// 점수
constexpr float kSpeedTolLog2 = 0.15f, kSpeedRangeLog2 = 0.85f;
constexpr float kShapeTol = 0.15f, kShapeRange = 0.60f;
constexpr float kRhythmShapeW = 0.6f, kRhythmSpeedW = 0.4f;
constexpr float kPitchDistTolSt = 1.0f, kPitchDistRangeSt = 5.0f;
constexpr float kPitchCorrFull = 0.9f;
constexpr float kIntonDistW = 0.65f, kIntonShapeW = 0.35f;
constexpr float kStressTol = 0.08f, kStressRange = 0.32f;
// 힌트
constexpr float kHintSpeedSlow = 1.25f, kHintSpeedFast = 0.8f;
constexpr float kHintDurLog2 = 0.5f;
constexpr int kHintMinWordMs = 80;                 // 길이 힌트를 낼 최소 단어 길이 (양쪽 모두)
constexpr float kHintLoud = 0.20f;
constexpr float kHintPitchSt = 3.f;
constexpr int kMaxHints = 4;

constexpr float kPi = 3.14159265358979f;
constexpr float kInf = 1e30f;
const float kNaN = std::numeric_limits<float>::quiet_NaN();

float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

// 프레임 k 의 중심 시각 (ms)
float frameCenterMs(int k) { return ((float)kHop * k + kFrameCenter) / (kRate / 1000.f); }

// 중심 시각이 [startMs, endMs] 에 드는 프레임 범위 [lo, hi]. 없으면 false
bool frameRange(float startMs, float endMs, int frames, int* lo, int* hi) {
    if (frames <= 0) return false;
    const float sps = kRate / 1000.f;  // 샘플/ms
    int a = (int)std::ceil((startMs * sps - kFrameCenter) / kHop);
    int b = (int)std::floor((endMs * sps - kFrameCenter) / kHop);
    a = std::max(a, 0);
    b = std::min(b, frames - 1);
    if (a > b) return false;
    *lo = a;
    *hi = b;
    return true;
}

int nearestFrame(float ms, int frames) {
    int k = (int)std::lround((ms * (kRate / 1000.f) - kFrameCenter) / kHop);
    return std::max(0, std::min(frames - 1, k));
}

// p ∈ [0,1] 분위수 (가장 가까운 순위)
float percentile(std::vector<float> v, float p) {
    if (v.empty()) return 0.f;
    size_t idx = (size_t)std::lround(p * (float)(v.size() - 1));
    idx = std::min(idx, v.size() - 1);
    std::nth_element(v.begin(), v.begin() + idx, v.end());
    return v[idx];
}

float median(std::vector<float> v) {
    if (v.empty()) return 0.f;
    const size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + m, v.end());
    float hi = v[m];
    if (v.size() % 2 == 1) return hi;
    float lo = *std::max_element(v.begin(), v.begin() + m);
    return 0.5f * (lo + hi);
}

// RBJ cookbook 2차 필터 (transposed direct form II)
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;
    float process(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

Biquad makeBiquad(float fc, float fs, bool highpass) {
    const float q = 0.70710678f;  // Butterworth
    const float w0 = 2.f * kPi * fc / fs, c = std::cos(w0), s = std::sin(w0), alpha = s / (2.f * q);
    const float a0 = 1.f + alpha;
    Biquad f;
    if (highpass) {
        f.b0 = (1.f + c) * 0.5f / a0;
        f.b1 = -(1.f + c) / a0;
        f.b2 = (1.f + c) * 0.5f / a0;
    } else {
        f.b0 = (1.f - c) * 0.5f / a0;
        f.b1 = (1.f - c) / a0;
        f.b2 = (1.f - c) * 0.5f / a0;
    }
    f.a1 = -2.f * c / a0;
    f.a2 = (1.f - alpha) / a0;
    return f;
}

// 유성음 연속 구간 [a, b) 들을 돌며 fn(a, b) 호출
template <class Fn>
void forEachRun(const std::vector<uint8_t>& mask, Fn fn) {
    const int n = (int)mask.size();
    int k = 0;
    while (k < n) {
        if (!mask[k]) { ++k; continue; }
        int a = k;
        while (k < n && mask[k]) ++k;
        fn(a, k);
    }
}

// 단어 구간 안 프레임의 크기 (상위 절반 평균: 단어 속 짧은 쉼이나 효과음 한 칸에 흔들리지 않게). 프레임이 없으면 가장 가까운 프레임
float meanLoud(const Track& t, int startMs, int endMs) {
    int lo, hi;
    if (!frameRange((float)startMs, (float)endMs, t.frames(), &lo, &hi)) {
        if (t.frames() == 0) return 0.f;
        return t.loud[nearestFrame(0.5f * (startMs + endMs), t.frames())];
    }
    std::vector<float> v(t.loud.begin() + lo, t.loud.begin() + hi + 1);
    std::sort(v.begin(), v.end());
    const size_t from = v.size() / 2;
    double s = 0;
    for (size_t i = from; i < v.size(); ++i) s += v[i];
    return (float)(s / (double)(v.size() - from));
}

bool voicedSt(const Track& t, int k) { return t.voiced[k] && !std::isnan(t.semitone[k]); }

// 말소리 구간끼리 DTW. path 는 (orig 프레임, user 프레임) 전역 인덱스, 시간 순
bool dtwAlign(const Track& o, const Track& u, std::vector<std::pair<int, int>>* path) {
    const int oS = o.speechStart, uS = u.speechStart;
    const int nO = o.speechEnd - oS, nU = u.speechEnd - uS;
    if (nO <= 0 || nU <= 0) return false;
    const float hw = std::max(kDtwBandFrac * (float)nU, (float)kDtwBandMin);

    // 띠 안의 셀만 저장한다 (행마다 [lo, lo+len))
    std::vector<int> lo(nO), base(nO + 1, 0);
    for (int i = 0; i < nO; ++i) {
        const float jc = (float)i * nU / (float)nO;
        int jl = std::max(0, (int)std::floor(jc - hw));
        int jh = std::min(nU - 1, (int)std::ceil(jc + hw));
        lo[i] = jl;
        base[i + 1] = base[i] + (jh - jl + 1);
    }
    std::vector<float> D((size_t)base[nO], kInf);
    auto at = [&](int i, int j) -> float {
        if (i < 0 || j < 0 || i >= nO) return kInf;
        const int jl = lo[i], len = base[i + 1] - base[i];
        if (j < jl || j >= jl + len) return kInf;
        return D[(size_t)base[i] + (j - jl)];
    };
    auto cost = [&](int i, int j) -> float {
        const int oi = oS + i, uj = uS + j;
        const bool vo = voicedSt(o, oi), vu = voicedSt(u, uj);
        float pitch;
        if (vo && vu) pitch = std::min(std::fabs(o.semitone[oi] - u.semitone[uj]), kDtwPitchClampSt) / kDtwPitchScaleSt;
        else pitch = (vo != vu) ? 1.f : 0.f;
        return pitch + std::fabs(o.loud[oi] - u.loud[uj]);
    };

    for (int i = 0; i < nO; ++i) {
        const int jl = lo[i], len = base[i + 1] - base[i];
        for (int j = jl; j < jl + len; ++j) {
            const float c = cost(i, j);
            float best;
            if (i == 0 && j == 0) best = 0.f;
            else best = std::min({at(i - 1, j - 1), at(i - 1, j) + kDtwStepPenalty, at(i, j - 1) + kDtwStepPenalty});
            D[(size_t)base[i] + (j - jl)] = best >= kInf ? kInf : best + c;
        }
    }
    if (at(nO - 1, nU - 1) >= kInf) return false;

    // 역추적 (같은 값이면 대각선 우선)
    path->clear();
    int i = nO - 1, j = nU - 1;
    path->push_back({oS + i, uS + j});
    while (i > 0 || j > 0) {
        const float d = at(i - 1, j - 1), up = at(i - 1, j) + kDtwStepPenalty, left = at(i, j - 1) + kDtwStepPenalty;
        if (d <= up && d <= left) { --i; --j; }
        else if (up <= left) --i;
        else --j;
        path->push_back({oS + i, uS + j});
    }
    std::reverse(path->begin(), path->end());
    return true;
}

float pearson(double n, double sx, double sy, double sxx, double syy, double sxy) {
    if (n < 2) return 0.f;
    const double mx = sx / n, my = sy / n;
    const double vx = sxx / n - mx * mx, vy = syy / n - my * my, cov = sxy / n - mx * my;
    if (vx <= 1e-9 || vy <= 1e-9) return 0.f;
    const double r = cov / std::sqrt(vx * vy);
    return (float)std::max(-1.0, std::min(1.0, r));
}

std::string fmt(const char* f, double v) {
    char buf[128];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

}  // namespace

Track analyze(const std::vector<float>& pcm, const std::vector<Word>& words) {
    Track t;
    const int n = (int)pcm.size();
    const int frames = n >= kFrameLen ? (n - kFrameLen) / kHop + 1 : 0;
    t.semitone.assign(frames, kNaN);
    t.hz.assign(frames, 0.f);
    t.loud.assign(frames, 0.f);
    t.voiced.assign(frames, 0);
    t.inWord.assign(frames, 0);
    t.hasDynamics = false;
    if (frames == 0) return t;

    // ---- 1. 크기: 프레임 평균(DC) 을 빼고 앞 320 샘플 RMS ----
    std::vector<float> frame(kFrameLen);
    std::vector<float> rmsDb(frames);
    for (int k = 0; k < frames; ++k) {
        const float* src = pcm.data() + (size_t)k * kHop;
        double mean = 0;
        for (int i = 0; i < kFrameLen; ++i) mean += src[i];
        mean /= kFrameLen;
        double e = 0;
        for (int i = 0; i < kCorrWin; ++i) {
            const double v = src[i] - mean;
            e += v * v;
        }
        rmsDb[k] = 20.f * std::log10((float)std::sqrt(e / kCorrWin) + 1e-6f);
    }
    const float floorDb = percentile(rmsDb, 0.05f);
    const float voicedDb = floorDb + kVoicedAboveFloorDb;
    // loud 는 단어 구간을 안 뒤(5 단계) 에 채운다: 효과음 · 배경음 피크가 아니라 말소리의 큰 쪽을 1 로 잡기 위해

    // ---- 2. 음높이 경로: 60 Hz 고역 + 1 kHz 저역 ----
    std::vector<float> filt(n);
    {
        Biquad hp = makeBiquad(kHpHz, (float)kRate, true), lp = makeBiquad(kLpHz, (float)kRate, false);
        for (int i = 0; i < n; ++i) filt[i] = lp.process(hp.process(pcm[i]));
    }

    // ---- 3. NCCF 로 F0 ----
    std::vector<float> hzRaw(frames, 0.f);
    std::vector<uint8_t> voiced(frames, 0);
    std::vector<double> prefix(kFrameLen + 1);
    std::vector<float> corr(kLagMax + 1, 0.f);
    for (int k = 0; k < frames; ++k) {
        const float* src = filt.data() + (size_t)k * kHop;
        double mean = 0;
        for (int i = 0; i < kFrameLen; ++i) mean += src[i];
        mean /= kFrameLen;
        prefix[0] = 0;
        for (int i = 0; i < kFrameLen; ++i) {
            frame[i] = (float)(src[i] - mean);
            prefix[i + 1] = prefix[i] + (double)frame[i] * frame[i];
        }
        const double e0 = prefix[kCorrWin];
        if (e0 <= 0) continue;
        int bestLag = -1;
        float bestScore = -kInf;
        for (int lag = kLagMin; lag <= kLagMax; ++lag) {
            const float* a = frame.data();
            const float* b = frame.data() + lag;
            float num = 0.f;
            for (int i = 0; i < kCorrWin; ++i) num += a[i] * b[i];
            const double eLag = prefix[lag + kCorrWin] - prefix[lag];
            const float r = (float)(num / std::sqrt(e0 * eLag + 1e-12));
            corr[lag] = r;
            const float score = r - kLagBias * (float)lag / (float)kLagMax;
            if (score > bestScore) { bestScore = score; bestLag = lag; }
        }
        if (bestLag < 0 || corr[bestLag] < kNccfVoiced || rmsDb[k] < voicedDb) continue;
        float delta = 0.f;
        if (bestLag > kLagMin && bestLag < kLagMax) {
            const float a = corr[bestLag - 1], b = corr[bestLag], c = corr[bestLag + 1];
            const float denom = a - 2.f * b + c;
            if (denom < -1e-9f) delta = std::max(-0.5f, std::min(0.5f, 0.5f * (a - c) / denom));
        }
        hzRaw[k] = (float)kRate / ((float)bestLag + delta);
        voiced[k] = 1;
    }

    // ---- 4. 후처리 ----
    // (1) 유성음 구간 안에서 5 프레임 중앙값 필터
    std::vector<float> hzMed = hzRaw;
    forEachRun(voiced, [&](int a, int b) {
        std::vector<float> win;
        for (int k = a; k < b; ++k) {
            win.clear();
            for (int j = std::max(a, k - kMedianHalf); j <= std::min(b - 1, k + kMedianHalf); ++j) win.push_back(hzRaw[j]);
            hzMed[k] = median(win);
        }
    });
    // (2) 옥타브 보정: ±5 프레임 이웃 중앙값과 비교
    std::vector<float> hz2 = hzMed;
    std::vector<uint8_t> v2 = voiced;
    {
        std::vector<float> nbr;
        for (int k = 0; k < frames; ++k) {
            if (!voiced[k]) continue;
            nbr.clear();
            for (int j = std::max(0, k - kOctaveNbr); j <= std::min(frames - 1, k + kOctaveNbr); ++j)
                if (j != k && voiced[j]) nbr.push_back(hzMed[j]);
            if (nbr.empty()) continue;
            const float med = median(nbr);
            if (med <= 0.f) continue;
            const float ratio = hzMed[k] / med;
            if (ratio > kOctaveHi) {
                if (std::fabs(ratio * 0.5f - 1.f) <= kOctaveTol) hz2[k] = hzMed[k] * 0.5f;
                else v2[k] = 0;
            } else if (ratio < kOctaveLo) {
                if (std::fabs(ratio * 2.f - 1.f) <= kOctaveTol) hz2[k] = hzMed[k] * 2.f;
                else v2[k] = 0;
            }
        }
    }
    // (3) 짧은 유성음 섬 제거
    forEachRun(v2, [&](int a, int b) {
        if (b - a < kMinIsland)
            for (int k = a; k < b; ++k) v2[k] = 0;
    });
    t.voiced = v2;
    for (int k = 0; k < frames; ++k) t.hz[k] = v2[k] ? hz2[k] : 0.f;

    // ---- 5. 단어 구간 · 말소리 구간 ----
    bool haveSpan = false;
    if (!words.empty()) {
        int firstStart = words[0].startMs, lastEnd = words[0].endMs;
        for (const auto& w : words) {
            firstStart = std::min(firstStart, w.startMs);
            lastEnd = std::max(lastEnd, w.endMs);
            int lo, hi;
            if (frameRange((float)(w.startMs - kWordPadMs), (float)(w.endMs + kWordPadMs), frames, &lo, &hi))
                for (int k = lo; k <= hi; ++k) t.inWord[k] = 1;
        }
        int lo, hi;
        if (frameRange((float)(firstStart - kSpeechPadMs), (float)(lastEnd + kSpeechPadMs), frames, &lo, &hi)) {
            t.speechStart = lo;
            t.speechEnd = hi + 1;
            haveSpan = true;
        }
    }
    if (!haveSpan) {
        // 단어가 없으면 (또는 단어 시각이 클립 밖이면) 에너지로 잡는다
        int first = -1, last = -1;
        for (int k = 0; k < frames; ++k) {
            const bool sp = rmsDb[k] >= voicedDb;
            t.inWord[k] = sp ? 1 : 0;
            if (sp) { if (first < 0) first = k; last = k; }
        }
        if (first >= 0) {
            t.speechStart = std::max(0, first - kSpeechPadFrames);
            t.speechEnd = std::min(frames, last + 1 + kSpeechPadFrames);
        } else {
            t.speechStart = t.speechEnd = 0;
        }
    }

    // ---- 5b. 상대 크기: 바닥 + 10 dB → 0, 말소리 구간의 90 퍼센타일 → 1 (효과음 한 방에 기준이 끌려가지 않게) ----
    {
        std::vector<float> speech;
        for (int k = 0; k < frames; ++k) if (t.inWord[k]) speech.push_back(rmsDb[k]);
        const float peakDb = speech.size() >= 10 ? percentile(speech, 0.90f) : percentile(rmsDb, 0.95f);
        const float loudRange = peakDb - floorDb - kLoudOffsetDb;
        t.hasDynamics = loudRange >= kMinDynamicsDb;
        for (int k = 0; k < frames; ++k)
            t.loud[k] = loudRange > 0.f ? clamp01((rmsDb[k] - floorDb - kLoudOffsetDb) / loudRange) : 0.f;
    }

    // ---- 6. 중앙값 기준 반음 ----
    std::vector<float> pool;
    for (int k = 0; k < frames; ++k) if (t.voiced[k] && t.inWord[k]) pool.push_back(t.hz[k]);
    if (pool.empty()) for (int k = 0; k < frames; ++k) if (t.voiced[k]) pool.push_back(t.hz[k]);
    t.medianHz = pool.empty() ? 0.f : median(pool);
    if (t.medianHz > 0.f)
        for (int k = 0; k < frames; ++k)
            if (t.voiced[k]) t.semitone[k] = 12.f * std::log2(t.hz[k] / t.medianHz);
    return t;
}

Result compare(const Track& orig, const std::vector<Word>& origWords,
               const Track& user, const std::vector<Word>& userWords,
               const std::string& refText, Lang lang) {
    Result r;
    auto setNote = [&](const char* s) { if (r.note.empty()) r.note = s; };

    // ---- 1. 말소리가 충분한가 ----
    const int spanO = orig.speechEnd - orig.speechStart, spanU = user.speechEnd - user.speechStart;
    if (spanU < kMinSpeechFrames) { r.note = "녹음이 너무 짧아 억양·리듬 비교 생략"; return r; }
    if (spanO < kMinSpeechFrames) { r.note = "원음에서 말소리를 찾지 못해 비교 생략"; return r; }

    // ---- 2. 기준 단어를 양쪽 whisper 단어에 붙인다 (일본어는 단어 경계가 없어 생략) ----
    std::vector<std::string> refTokens;
    std::vector<WordPair> anchors;
    if (lang != Lang::Ja && !origWords.empty() && !userWords.empty()) {
        refTokens = scoringTokens(refText, lang);
        std::vector<std::string> origTexts, userTexts;
        for (const auto& w : origWords) origTexts.push_back(w.text);
        for (const auto& w : userWords) userTexts.push_back(w.text);
        const auto mapO = alignTokens(refText, origTexts, lang);
        const auto mapU = alignTokens(refText, userTexts, lang);
        for (size_t i = 0; i < refTokens.size() && i < mapO.size() && i < mapU.size(); ++i) {
            if (mapO[i] < 0 || mapU[i] < 0) continue;
            const auto& wo = origWords[(size_t)mapO[i]];
            const auto& wu = userWords[(size_t)mapU[i]];
            WordPair p;
            p.refIdx = (int)i;
            p.text = refTokens[i];
            p.origStartMs = wo.startMs; p.origEndMs = wo.endMs;
            p.userStartMs = wu.startMs; p.userEndMs = wu.endMs;
            anchors.push_back(p);
        }
    }
    const int A = (int)anchors.size();

    // ---- 3. 말 전체 길이와 속도 ----
    float sO = (float)spanO * kHopMs, sU = (float)spanU * kHopMs;
    if (A >= 2) {
        const float ao = (float)(anchors.back().origEndMs - anchors.front().origStartMs);
        const float au = (float)(anchors.back().userEndMs - anchors.front().userStartMs);
        if (ao > 0.f && au > 0.f) { sO = ao; sU = au; }
    }
    r.speedRatio = sU / sO;
    const float logSpeed = std::log2(r.speedRatio);
    const float speedScore = 100.f * clamp01(1.f - (std::fabs(logSpeed) - kSpeedTolLog2) / kSpeedRangeLog2);

    // ---- 4. 리듬: 단어 길이 비율과 쉼 ----
    for (auto& p : anchors) {
        const float dO = (float)std::max(1, p.origEndMs - p.origStartMs), dU = (float)std::max(1, p.userEndMs - p.userStartMs);
        p.durRatio = (dU / sU) / (dO / sO);
    }
    const int needAnchors = (int)std::ceil(0.5 * (double)refTokens.size());
    if (A >= 2 && A >= needAnchors) {
        double d = 0;
        for (int i = 0; i < A; ++i) {
            const auto& p = anchors[(size_t)i];
            const float pO = (float)std::max(1, p.origEndMs - p.origStartMs) / sO;
            const float pU = (float)std::max(1, p.userEndMs - p.userStartMs) / sU;
            d += std::fabs(pO - pU);
            if (i + 1 < A) {
                const auto& q = anchors[(size_t)i + 1];
                const float gO = (float)std::max(0, q.origStartMs - p.origEndMs) / sO;
                const float gU = (float)std::max(0, q.userStartMs - p.userEndMs) / sU;
                d += std::fabs(gO - gU);
            }
        }
        const float shapeScore = 100.f * clamp01(1.f - ((float)d - kShapeTol) / kShapeRange);
        r.rhythm = kRhythmShapeW * shapeScore + kRhythmSpeedW * speedScore;
        r.rhythmSpeedOnly = false;
    } else {
        r.rhythm = speedScore;
        r.rhythmSpeedOnly = true;
    }
    r.haveRhythm = true;

    // ---- 5. DTW 로 시간 맞추기 ----
    std::vector<std::pair<int, int>> path;
    const float spanRatio = (float)spanU / (float)spanO;
    bool dtwSkippedBySpeed = false;
    if (spanRatio > kSpanRatioMax || spanRatio < kSpanRatioMin) {
        dtwSkippedBySpeed = true;
    } else if (dtwAlign(orig, user, &path)) {
        r.haveAlignment = true;
        r.origOnUser.assign((size_t)user.frames(), kNaN);
        std::vector<float> sum((size_t)user.frames(), 0.f);
        std::vector<int> cnt((size_t)user.frames(), 0);
        for (const auto& c : path) {
            if (!voicedSt(orig, c.first)) continue;
            sum[(size_t)c.second] += orig.semitone[c.first];
            ++cnt[(size_t)c.second];
        }
        for (int j = 0; j < user.frames(); ++j)
            if (cnt[(size_t)j]) r.origOnUser[(size_t)j] = sum[(size_t)j] / (float)cnt[(size_t)j];
    }

    // ---- 6. 억양 ----
    if (r.haveAlignment) {
        double n = 0, sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0, sAbs = 0;
        for (const auto& c : path) {
            if (!orig.inWord[c.first] || !voicedSt(orig, c.first) || !voicedSt(user, c.second)) continue;
            const double x = orig.semitone[c.first], y = user.semitone[c.second];
            n += 1; sx += x; sy += y; sxx += x * x; syy += y * y; sxy += x * y; sAbs += std::fabs(x - y);
        }
        if (n >= kMinVoicedCells) {
            const float dBar = (float)(sAbs / n);
            const float rho = pearson(n, sx, sy, sxx, syy, sxy);
            const float sDist = 100.f * clamp01(1.f - (dBar - kPitchDistTolSt) / kPitchDistRangeSt);
            const float sShape = 100.f * clamp01(rho / kPitchCorrFull);
            r.intonation = kIntonDistW * sDist + kIntonShapeW * sShape;
            r.pitchDiffSt = dBar;
            r.haveIntonation = true;
        } else {
            setNote("유성음 구간이 짧아 억양 비교 생략");
        }
        // 단어별 음높이 차이 (내 단어 구간 안의 셀, 둘 다 유성음)
        for (auto& p : anchors) {
            double s = 0;
            int cnt = 0;
            for (const auto& c : path) {
                const float ms = frameCenterMs(c.second);
                if (ms < (float)p.userStartMs || ms > (float)p.userEndMs) continue;
                if (!voicedSt(orig, c.first) || !voicedSt(user, c.second)) continue;
                s += user.semitone[c.second] - orig.semitone[c.first];
                ++cnt;
            }
            if (cnt >= kMinWordPitchCells) { p.pitchDiffSt = (float)(s / cnt); p.hasPitch = true; }
        }
    }

    // ---- 7. 강세 ----
    if (orig.hasDynamics && user.hasDynamics) {
        if (A >= 2) {
            double d = 0;
            for (auto& p : anchors) {
                p.loudDiff = meanLoud(user, p.userStartMs, p.userEndMs) - meanLoud(orig, p.origStartMs, p.origEndMs);
                d += std::fabs(p.loudDiff);
            }
            r.stress = 100.f * clamp01(1.f - ((float)(d / A) - kStressTol) / kStressRange);
            r.haveStress = true;
        } else if (r.haveAlignment) {
            double d = 0;
            int cnt = 0;
            for (const auto& c : path) {
                if (!orig.inWord[c.first]) continue;
                d += std::fabs(orig.loud[c.first] - user.loud[c.second]);
                ++cnt;
            }
            if (cnt >= kMinStressCells) {
                r.stress = 100.f * clamp01(1.f - ((float)(d / cnt) - kStressTol) / kStressRange);
                r.haveStress = true;
            } else {
                setNote("말소리 구간이 짧아 강세 비교 생략");
            }
        }
    } else {
        setNote("소리 크기 변화가 작아 강세 비교 생략");
    }
    if (dtwSkippedBySpeed) {
        // 속도 차이로 DTW 를 건너뛰었다. 강세는 단어 정렬로 냈을 수 있다
        r.note = r.haveStress ? "속도 차이가 커서 억양 비교 생략" : "속도 차이가 커서 억양·강세 비교 생략";
    }

    // ---- 8. 힌트: 문턱 대비 크기가 큰 것부터 최대 4개 ----
    struct Cand { float mag; Hint h; };
    std::vector<Cand> cands;
    if (r.speedRatio > kHintSpeedSlow)
        cands.push_back({std::fabs(logSpeed) / std::log2(kHintSpeedSlow), {-1, fmt("속도: 원음보다 %.1f배 느림", r.speedRatio)}});
    else if (r.speedRatio < kHintSpeedFast)
        cands.push_back({std::fabs(logSpeed) / std::log2(kHintSpeedSlow), {-1, fmt("속도: 원음보다 %.1f배 빠름", 1.0 / r.speedRatio)}});
    for (const auto& p : anchors) {
        const std::string q = "'" + p.text + "' ";
        const float ld = std::log2(std::max(p.durRatio, 1e-3f));
        // whisper 가 50 ms 짜리 겹친 기능어를 내는 일이 있어, 양쪽 모두 kHintMinWordMs 이상인 단어만 길이 힌트를 낸다
        const bool longEnough = p.origEndMs - p.origStartMs >= kHintMinWordMs && p.userEndMs - p.userStartMs >= kHintMinWordMs;
        if (longEnough && std::fabs(ld) > kHintDurLog2)
            cands.push_back({std::fabs(ld) / kHintDurLog2, {p.refIdx, q + (ld < 0.f ? "더 길게" : "더 짧게")}});
        if (r.haveStress && std::fabs(p.loudDiff) > kHintLoud)
            cands.push_back({std::fabs(p.loudDiff) / kHintLoud, {p.refIdx, q + (p.loudDiff > 0.f ? "더 약하게" : "더 세게")}});
        if (p.hasPitch && std::fabs(p.pitchDiffSt) > kHintPitchSt)
            cands.push_back({std::fabs(p.pitchDiffSt) / kHintPitchSt, {p.refIdx, q + (p.pitchDiffSt > 0.f ? "더 낮게" : "더 높게")}});
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.mag > b.mag; });
    for (size_t i = 0; i < cands.size() && (int)i < kMaxHints; ++i) r.hints.push_back(cands[i].h);

    r.words = std::move(anchors);
    return r;
}

std::string wordsToJson(const std::vector<Word>& words) {
    try {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& w : words) arr.push_back({{"t", w.text}, {"s", w.startMs}, {"e", w.endMs}});
        return arr.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    } catch (...) {
        return "[]";
    }
}

std::vector<Word> wordsFromJson(const std::string& json) {
    try {
        const auto doc = nlohmann::json::parse(json);
        if (!doc.is_array()) return {};
        std::vector<Word> out;
        for (const auto& it : doc) {
            if (!it.is_object()) continue;
            Word w;
            if (it.contains("t") && it["t"].is_string()) w.text = it["t"].get<std::string>();
            if (it.contains("s") && it["s"].is_number()) w.startMs = (int)std::lround(it["s"].get<double>());
            if (it.contains("e") && it["e"].is_number()) w.endMs = (int)std::lround(it["e"].get<double>());
            out.push_back(std::move(w));
        }
        return out;
    } catch (...) {
        return {};
    }
}

}  // namespace prosody

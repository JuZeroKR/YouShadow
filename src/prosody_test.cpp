// 억양 · 리듬 · 강세 모듈 검증용 콘솔 도구.
//  prosody_test                                              합성 신호 자체 테스트 (종료 코드 = 실패 수)
//  prosody_test <audio.wav> <startMs> <endMs> <rec.wav> <기준 문장...>   실제 원음 구간 vs 녹음 (whisper 로 단어 시각)
//  prosody_test --self <rec.wav>                             녹음을 자기 자신과 비교 (≈100 점이 나와야 한다)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "audio.h"
#include "paths.h"
#include "prosody.h"
#include "stt.h"
#include "transcript.h"

namespace {

constexpr int kRate = prosody::kRate;
constexpr int kPadMs = 100;         // 앞뒤 무음 여유
constexpr float kNoiseDb = -45.f;   // 배경 잡음 (실제 마이크 녹음과 비슷한 바닥을 만들어 크기 정규화가 현실적이게)
constexpr float kPi = 3.14159265358979f;

using Fn = std::function<float(float)>;  // 초 → 값

// 배음이 풍부한 톱니파 (4 kHz 까지 배음 합, 대역 제한). contour: 초 → Hz (0 이면 무음), amp: 초 → 진폭.
// 앞뒤 100 ms 무음 여유를 두고 전체에 백색 잡음을 더한다.
std::vector<float> tone(const Fn& contour, int durationMs, const Fn& amp, float noiseDb = kNoiseDb, unsigned seed = 7) {
    const int nPad = kPadMs * kRate / 1000, nBody = durationMs * kRate / 1000;
    std::vector<float> out((size_t)nPad * 2 + nBody, 0.f);
    double phase = 0;
    for (int i = 0; i < nBody; ++i) {
        const float t = (float)i / kRate;
        const float f = contour(t), a = amp(t);
        if (f > 0.f) { phase += f / kRate; if (phase >= 1.0) phase -= 1.0; }
        float s = 0.f;
        if (f > 0.f && a > 0.f) {
            const int H = std::max(1, (int)(4000.f / f));
            for (int h = 1; h <= H; ++h) s += std::sin(2.f * kPi * h * (float)phase) / (float)h;
            s *= 2.f / kPi;
        }
        out[(size_t)nPad + i] = a * s;
    }
    if (noiseDb > -200.f) {
        std::mt19937 rng(seed);
        std::normal_distribution<float> gauss(0.f, std::pow(10.f, noiseDb / 20.f));
        for (auto& v : out) v += gauss(rng);
    }
    return out;
}

struct WordSpec { const char* text; int startMs, endMs; float amp; };
struct Clip { std::vector<float> pcm; std::vector<Word> words; };

// 단어 구간에서만 소리를 내는 "문장". stretch 로 전체 시간을 늘리고 (contour 는 상대 시각으로 샘플링), hzScale 로 음역을 옮기고,
// loudIdx 번째 단어만 loudGain 배 크게 낸다.
Clip sentence(const std::vector<WordSpec>& specs, const Fn& contourHz, float stretch = 1.f, float hzScale = 1.f,
              int loudIdx = -1, float loudGain = 1.f) {
    int endMs = 0;
    for (const auto& s : specs) endMs = std::max(endMs, (int)std::lround(s.endMs * stretch));
    auto amp = [&](float t) {
        const float ms = t * 1000.f;
        for (size_t i = 0; i < specs.size(); ++i) {
            const auto& s = specs[i];
            if (ms >= s.startMs * stretch && ms < s.endMs * stretch) return s.amp * ((int)i == loudIdx ? loudGain : 1.f);
        }
        return 0.f;
    };
    auto contour = [&](float t) { return hzScale * contourHz(t / stretch); };
    Clip c;
    c.pcm = tone(contour, endMs, amp);
    for (const auto& s : specs)
        c.words.push_back({s.text, kPadMs + (int)std::lround(s.startMs * stretch), kPadMs + (int)std::lround(s.endMs * stretch)});
    return c;
}

const std::vector<WordSpec> kSpecs = {{"one", 0, 400, 0.3f}, {"two", 550, 900, 0.15f}, {"three", 1050, 1600, 0.2f}};
const char* kRefText = "one two three";
float contourA(float t) { return 160.f + 50.f * std::sin(2.f * kPi * 0.9f * t); }  // 자연스러운 오르내림
float contourRise(float t) { return 100.f + 100.f * t / 1.6f; }
float contourFall(float t) { return 200.f - 100.f * t / 1.6f; }

int gFails = 0;
void check(const char* name, bool ok, const std::string& detail) {
    std::printf("%s  %-44s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    if (!ok) ++gFails;
}
std::string f2(const char* fmt, double a, double b = 0, double c = 0, double d = 0) {
    char buf[256];
    std::snprintf(buf, sizeof buf, fmt, a, b, c, d);
    return buf;
}

float medianVoicedHz(const prosody::Track& t) {
    std::vector<float> v;
    for (int k = 0; k < t.frames(); ++k) if (t.voiced[k]) v.push_back(t.hz[k]);
    if (v.empty()) return 0.f;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
int voicedCount(const prosody::Track& t) {
    int n = 0;
    for (auto v : t.voiced) n += v;
    return n;
}
std::string hintsStr(const prosody::Result& r) {
    std::string s;
    for (const auto& h : r.hints) s += "[" + h.text + "] ";
    return s.empty() ? "(없음)" : s;
}
bool hasHint(const prosody::Result& r, const std::string& needle) {
    for (const auto& h : r.hints) if (h.text.find(needle) != std::string::npos) return true;
    return false;
}

void selfTests() {
    using namespace prosody;
    // (a) 일정한 음높이
    for (float hz : {120.f, 220.f}) {
        auto pcm = tone([&](float) { return hz; }, 1000, [](float) { return 0.3f; });
        auto t = analyze(pcm, {});
        const float med = medianVoicedHz(t);
        check(hz < 200 ? "(a) steady 120 Hz median within 2%" : "(a) steady 220 Hz median within 2%",
              std::fabs(med / hz - 1.f) < 0.02f, f2("median=%.2f Hz voiced=%.0f/%.0f", med, voicedCount(t), t.frames()));
    }
    // (b) 100 → 200 Hz 글라이드
    {
        auto pcm = tone([](float t) { return 100.f + 100.f * t; }, 1000, [](float) { return 0.3f; });
        auto t = analyze(pcm, {});
        float first = 0, last = 0, maxDrop = 0, prev = -1;
        int nVoiced = 0;
        for (int k = 0; k < t.frames(); ++k) {
            if (!t.voiced[k]) continue;
            if (!nVoiced) first = t.hz[k];
            last = t.hz[k];
            if (prev > 0) maxDrop = std::max(maxDrop, prev - t.hz[k]);
            prev = t.hz[k];
            ++nVoiced;
        }
        check("(b) glide monotonic (max drop <= 0.5 Hz)", nVoiced > 50 && maxDrop <= 0.5f, f2("maxDrop=%.3f Hz voiced=%.0f", maxDrop, nVoiced));
        check("(b) glide ends within 5%", std::fabs(first / 100.f - 1.f) < 0.05f && std::fabs(last / 200.f - 1.f) < 0.05f,
              f2("first=%.1f last=%.1f Hz", first, last));
    }
    // (c) 무음 · 백색 잡음
    {
        std::vector<float> silence((size_t)kRate, 0.f);
        auto t = analyze(silence, {});
        check("(c) digital silence: no voiced", voicedCount(t) == 0, f2("voiced=%.0f", voicedCount(t)));
        auto noise = tone([](float) { return 0.f; }, 1000, [](float) { return 0.f; }, -60.f);
        auto t2 = analyze(noise, {});
        check("(c) white noise -60 dB: no voiced", voicedCount(t2) == 0, f2("voiced=%.0f", voicedCount(t2)));
    }
    // (d) 같은 클립
    Clip a = sentence(kSpecs, contourA);
    Track ta = analyze(a.pcm, a.words);
    {
        auto r = compare(ta, a.words, ta, a.words, kRefText, Lang::En);
        const bool ok = r.haveIntonation && r.haveRhythm && r.haveStress && r.intonation >= 95 && r.rhythm >= 95 &&
                        r.stress >= 95 && std::fabs(r.speedRatio - 1.f) < 0.01f && r.hints.empty() && !r.rhythmSpeedOnly;
        check("(d) identical clips", ok,
              f2("int=%.1f rhy=%.1f str=%.1f speed=%.3f ", r.intonation, r.rhythm, r.stress, r.speedRatio) + hintsStr(r) +
                  (r.note.empty() ? "" : " note=" + r.note) + f2(" voiced=%.0f medHz=%.1f", voicedCount(ta), ta.medianHz));
    }
    // (e) +5 반음 옮긴 같은 곡선
    {
        Clip b = sentence(kSpecs, contourA, 1.f, std::pow(2.f, 5.f / 12.f));
        Track tb = analyze(b.pcm, b.words);
        auto r = compare(ta, a.words, tb, b.words, kRefText, Lang::En);
        check("(e) +5 st register shift: intonation >= 90", r.haveIntonation && r.intonation >= 90,
              f2("int=%.1f medHz %.1f vs %.1f dBar=%.2f", r.intonation, ta.medianHz, tb.medianHz, r.pitchDiffSt));
    }
    // (f) 뒤집힌 곡선
    {
        Clip up = sentence(kSpecs, contourRise), down = sentence(kSpecs, contourFall);
        Track tu = analyze(up.pcm, up.words), td = analyze(down.pcm, down.words);
        auto r = compare(tu, up.words, td, down.words, kRefText, Lang::En);
        check("(f) inverted contour: intonation <= 50", r.haveIntonation && r.intonation <= 50,
              f2("int=%.1f dBar=%.2f", r.intonation, r.pitchDiffSt));
    }
    // (g) 1.5 배 늘린 것
    {
        Clip c = sentence(kSpecs, contourA, 1.5f);
        Track tc = analyze(c.pcm, c.words);
        auto r = compare(ta, a.words, tc, c.words, kRefText, Lang::En);
        const bool ok = r.speedRatio >= 1.4f && r.speedRatio <= 1.6f && !r.rhythmSpeedOnly && r.haveIntonation && r.intonation >= 80;
        check("(g) 1.5x slower", ok,
              f2("speed=%.3f rhy=%.1f int=%.1f speedOnly=%.0f ", r.speedRatio, r.rhythm, r.intonation, r.rhythmSpeedOnly) + hintsStr(r));
    }
    // (h) 한 단어만 3 배 크게
    {
        Clip d = sentence(kSpecs, contourA, 1.f, 1.f, 1, 3.f);
        Track td = analyze(d.pcm, d.words);
        auto r = compare(ta, a.words, td, d.words, kRefText, Lang::En);
        std::string wp;
        for (const auto& w : r.words) wp += w.text + f2(":%.2f ", w.loudDiff);
        check("(h) one word 3x louder: stress < 80 + hint", r.haveStress && r.stress < 80 && hasHint(r, "'two' 더 약하게"),
              f2("str=%.1f ", r.stress) + "loudDiff " + wp + hintsStr(r));
    }
    // (i) 단어 없이 (일본어 경로)
    {
        Track t0 = analyze(a.pcm, {});
        auto r = compare(t0, {}, t0, {}, "", Lang::Ja);
        check("(i) no words (Ja path): identical", r.haveIntonation && r.intonation >= 95 && r.haveRhythm && r.rhythmSpeedOnly,
              f2("int=%.1f rhy=%.1f str=%.1f speech=[%.0f,", r.intonation, r.rhythm, r.stress, t0.speechStart) +
                  f2("%.0f) ", t0.speechEnd) + (r.note.empty() ? "" : "note=" + r.note));
    }
    // (j) JSON
    {
        std::vector<Word> w = {{"It's", 10, 250}, {"한글", 300, 640}, {"", 0, 0}};
        auto js = wordsToJson(w);
        auto back = wordsFromJson(js);
        bool ok = back.size() == w.size();
        for (size_t i = 0; ok && i < w.size(); ++i) ok = back[i].text == w[i].text && back[i].startMs == w[i].startMs && back[i].endMs == w[i].endMs;
        ok = ok && wordsFromJson("not json").empty() && wordsFromJson("{\"a\":1}").empty();
        check("(j) wordsToJson/FromJson round trip", ok, js);
    }
    // 속도: 10 초 클립 분석 시간
    {
        auto pcm = tone(contourA, 10000, [](float) { return 0.3f; });
        auto t0 = std::chrono::steady_clock::now();
        auto t = analyze(pcm, {});
        auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        check("(perf) analyze 10 s clip < 200 ms", ms < 200, f2("%.1f ms, frames=%.0f", ms, t.frames()));
    }
}

// ---- 실제 데이터 ----

void printWords(const char* label, const std::vector<Word>& words) {
    std::printf("%s (%d words):", label, (int)words.size());
    for (const auto& w : words) std::printf(" %s[%d-%d]", w.text.c_str(), w.startMs, w.endMs);
    std::printf("\n");
}

// 60 열 × 12 행 ASCII 그래프. a: '#', b (있으면): 'o', 겹치면 '@'. 반음 축 [-12, +12]
void plot(const char* title, const std::vector<float>& a, const std::vector<float>* b, int from, int to) {
    const int cols = 60, rows = 12;
    std::vector<std::string> grid((size_t)rows, std::string((size_t)cols, ' '));
    const int zeroRow = rows / 2;
    for (auto& ch : grid[(size_t)zeroRow]) ch = '.';
    auto colValue = [&](const std::vector<float>& v, int c) -> float {
        const int k0 = from + (int)((long long)c * (to - from) / cols), k1 = std::max(k0 + 1, from + (int)((long long)(c + 1) * (to - from) / cols));
        double s = 0;
        int n = 0;
        for (int k = k0; k < k1 && k < (int)v.size(); ++k) if (k >= 0 && !std::isnan(v[(size_t)k])) { s += v[(size_t)k]; ++n; }
        return n ? (float)(s / n) : std::numeric_limits<float>::quiet_NaN();
    };
    auto rowOf = [&](float st) { return std::max(0, std::min(rows - 1, (int)std::floor((12.f - st) / 24.f * rows))); };
    for (int c = 0; c < cols; ++c) {
        const float va = colValue(a, c);
        const float vb = b ? colValue(*b, c) : std::numeric_limits<float>::quiet_NaN();
        if (!std::isnan(va)) grid[(size_t)rowOf(va)][(size_t)c] = '#';
        if (!std::isnan(vb)) {
            char& ch = grid[(size_t)rowOf(vb)][(size_t)c];
            ch = ch == '#' ? '@' : 'o';
        }
    }
    std::printf("%s  (frames %d..%d = %d..%d ms)\n", title, from, to, from * 10, to * 10);
    for (int r = 0; r < rows; ++r) {
        const char* lab = r == 0 ? "+12|" : r == zeroRow ? "  0|" : r == rows - 1 ? "-12|" : "   |";
        std::printf("%s%s|\n", lab, grid[(size_t)r].c_str());
    }
    std::printf("   +%s+\n", std::string((size_t)cols, '-').c_str());
}

void printResult(const prosody::Track& to, const prosody::Track& tu, const prosody::Result& r, double analyzeMs, double compareMs) {
    std::printf("orig: frames=%d voiced=%d speech=[%d,%d) medianHz=%.1f dynamics=%d\n", to.frames(), voicedCount(to), to.speechStart,
                to.speechEnd, to.medianHz, (int)to.hasDynamics);
    std::printf("user: frames=%d voiced=%d speech=[%d,%d) medianHz=%.1f dynamics=%d\n", tu.frames(), voicedCount(tu), tu.speechStart,
                tu.speechEnd, tu.medianHz, (int)tu.hasDynamics);
    std::printf("억양 %s%.1f  리듬 %s%.1f%s  강세 %s%.1f\n", r.haveIntonation ? "" : "(없음) ", r.intonation, r.haveRhythm ? "" : "(없음) ",
                r.rhythm, r.rhythmSpeedOnly ? " (속도만)" : "", r.haveStress ? "" : "(없음) ", r.stress);
    std::printf("speedRatio=%.3f pitchDiffSt=%.2f haveAlignment=%d note=\"%s\"\n", r.speedRatio, r.pitchDiffSt, (int)r.haveAlignment,
                r.note.c_str());
    std::printf("hints: %s\n", hintsStr(r).c_str());
    std::printf("words (%d):\n", (int)r.words.size());
    for (const auto& w : r.words)
        std::printf("  #%d %-12s orig[%d-%d] user[%d-%d] dur=%.2f loud=%+.2f pitch=%s\n", w.refIdx, w.text.c_str(), w.origStartMs, w.origEndMs,
                    w.userStartMs, w.userEndMs, w.durRatio, w.loudDiff, w.hasPitch ? f2("%+.1f st", w.pitchDiffSt).c_str() : "-");
    std::printf("timing: analyze %.1f ms, compare %.1f ms\n", analyzeMs, compareMs);
    plot("orig semitone", to.semitone, nullptr, to.speechStart, to.speechEnd);
    plot("user semitone (#) + orig on user axis (o, @=both)", tu.semitone, r.haveAlignment ? &r.origOnUser : nullptr, tu.speechStart, tu.speechEnd);
}

int realData(int argc, char** argv) {
    paths::setup();
    const bool self = std::string(argv[1]) == "--self";
    if (!self && argc < 6) {
        std::printf("usage: prosody_test <audio.wav> <startMs> <endMs> <rec.wav> <reference text ...>\n");
        return 1;
    }
    Stt stt;
    std::string err;
    if (!stt.load(Stt::defaultModelPath(), &err)) { std::printf("whisper 모델 로드 실패: %s (%s)\n", err.c_str(), Stt::defaultModelPath().c_str()); return 1; }

    std::vector<float> orig, user;
    std::vector<Word> origWords, userWords;
    std::string refText;
    if (self) {
        user = AudioEngine::loadWav(argv[2], kRate);
        orig = user;
        userWords = stt.transcribe(user, {}, &err);
        origWords = userWords;
        for (const auto& w : userWords) refText += (refText.empty() ? "" : " ") + w.text;
    } else {
        orig = AudioEngine::loadWavSlice(argv[1], std::stoi(argv[2]), std::stoi(argv[3]), kRate);
        user = AudioEngine::loadWav(argv[4], kRate);
        for (int i = 5; i < argc; ++i) refText += (i > 5 ? " " : "") + std::string(argv[i]);
        origWords = stt.transcribe(orig, {}, &err);
        userWords = stt.transcribe(user, {}, &err);
    }
    std::printf("orig %d ms, user %d ms\n", (int)(orig.size() * 1000 / kRate), (int)(user.size() * 1000 / kRate));
    std::printf("ref : %s\n", refText.c_str());
    printWords("orig", origWords);
    printWords("user", userWords);

    auto t0 = std::chrono::steady_clock::now();
    auto to = prosody::analyze(orig, origWords);
    auto tu = prosody::analyze(user, userWords);
    auto t1 = std::chrono::steady_clock::now();
    auto r = prosody::compare(to, origWords, tu, userWords, refText, Lang::En);
    auto t2 = std::chrono::steady_clock::now();
    printResult(to, tu, r, std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t2 - t1).count());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc >= 2) return realData(argc, argv);
    selfTests();
    std::printf("%d failure(s)\n", gFails);
    return gFails;
}

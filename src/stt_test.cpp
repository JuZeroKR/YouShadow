// STT + 채점 검증용 콘솔 도구.
// 사용법: stt_test <video_id> <seg_idx>  → 영상 오디오의 해당 문장 구간을 whisper 로 인식해 원문과 비교한다.
#include <chrono>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "audio.h"
#include "paths.h"
#include "scoring.h"
#include "stt.h"
#include "transcript.h"

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 3) {
        std::cout << "usage: stt_test <video_id> <seg_idx>\n";
        return 1;
    }
    paths::setup();
    const std::string id = argv[1];
    const int idx = std::stoi(argv[2]);
    const std::string dir = paths::dataDir() + "/" + id;

    auto segs = transcript::load(dir + "/segments.json");
    if (idx < 0 || idx >= (int)segs.size()) { std::cout << "bad seg idx\n"; return 1; }
    const auto& s = segs[idx];

    Stt stt;
    std::string err;
    auto t0 = std::chrono::steady_clock::now();
    if (!stt.load(Stt::defaultModelPath(), &err)) { std::cout << err << "\n"; return 1; }
    auto t1 = std::chrono::steady_clock::now();

    auto pcm = AudioEngine::loadWav(dir + "/audio.wav", Stt::kRate);
    size_t a = (size_t)s.startMs * Stt::kRate / 1000, b = (size_t)s.endMs * Stt::kRate / 1000;
    b = std::min(b, pcm.size());
    std::vector<float> clip(pcm.begin() + a, pcm.begin() + b);

    auto t2 = std::chrono::steady_clock::now();
    std::string heard = stt.transcribeText(clip, &err);
    auto t3 = std::chrono::steady_clock::now();
    if (!err.empty()) { std::cout << err << "\n"; return 1; }

    auto r = scoreTranscript(s.text, heard);
    std::cout << "원문 : " << s.text << "\n";
    std::cout << "인식 : " << heard << "\n";
    std::cout << "점수 : " << (int)r.accuracy << "% (" << r.matched << "/" << r.total << ")\n";
    for (const auto& m : r.marks) {
        const char* k = m.kind == WordMark::Match ? "=" : m.kind == WordMark::Missing ? "-" : m.kind == WordMark::Wrong ? "~" : "+";
        std::cout << k << m.text << (m.kind == WordMark::Wrong ? "(" + m.heard + ")" : "") << " ";
    }
    std::cout << "\n";
    auto ms = [](auto d) { return std::chrono::duration_cast<std::chrono::milliseconds>(d).count(); };
    std::cout << "모델 로드 " << ms(t1 - t0) << "ms, 인식 " << ms(t3 - t2) << "ms (클립 " << clip.size() * 1000 / Stt::kRate << "ms)\n";

    // 채점 로직 자체 검증
    auto r2 = scoreTranscript("I think writing is just part of everyday life", "I think writing just part of every day life right");
    std::cout << "채점 테스트: " << (int)r2.accuracy << "% ";
    for (const auto& m : r2.marks) std::cout << (m.kind == WordMark::Match ? "=" : m.kind == WordMark::Missing ? "-" : m.kind == WordMark::Wrong ? "~" : "+") << m.text << " ";
    std::cout << "\n";
    return 0;
}

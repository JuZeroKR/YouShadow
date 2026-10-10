#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "lang.h"
#include "transcript.h"

// 억양 · 리듬 · 강세 비교 (원음 화자 vs 내 녹음). AI 없이 PC 안에서 계산한다.
// 파형을 직접 비교하지 않고 "말투" 에 해당하는 것만 뽑아 비교한다:
//  - 억양: 음높이 곡선. 각자 자기 중앙값 기준 반음으로 바꿔 목소리 높낮이 차이를 없앤 뒤 DTW 로 시간을 맞춰 비교
//  - 리듬: whisper 단어 시각으로 단어별 길이 비율과 쉼, 전체 속도
//  - 강세: 상대 크기(0~1) 의 단어별 차이
// 두 클립 모두 mono 16 kHz float. 프레임은 10 ms (kHop) 간격이라 whisper 센티초 시각 · 파형 피크(kPeakBinMs) 와 같은 눈금이다.
namespace prosody {

constexpr int kRate = 16000;
constexpr int kHopMs = 10;
constexpr int kHop = 160;

// 한 클립의 프레임별 분석 결과. 프레임 k 의 중심 시각은 k*10 + 20 ms 근처 (분석 창 길이에 따라).
struct Track {
    std::vector<float> semitone;    // 자기 중앙값 기준 반음. 무성음이면 NaN
    std::vector<float> hz;          // F0. 무성음이면 0
    std::vector<float> loud;        // 상대 크기 0~1 (바닥 + 10 dB → 0, 95 퍼센타일 → 1)
    std::vector<uint8_t> voiced;    // 1 이면 유성음 (주기성 + 에너지 조건 통과)
    std::vector<uint8_t> inWord;    // 1 이면 프레임 중심이 (앞뒤 여유를 둔) 단어 구간 안
    int speechStart = 0, speechEnd = 0;  // 말소리 구간 [start, end) 프레임
    float medianHz = 0.f;           // 유성음 F0 중앙값
    bool hasDynamics = true;        // 크기 변화가 충분한가 (95p − 5p − 10 dB ≥ 6 dB). 아니면 강세 비교 불가
    int frames() const { return (int)semitone.size(); }
};

// pcm16k: mono 16 kHz. words: 이 클립을 whisper 로 인식한 단어 (ms, 클립 시작 기준). 비어 있으면 에너지로 말소리 구간을 잡는다.
Track analyze(const std::vector<float>& pcm16k, const std::vector<Word>& words);

// 기준 문장의 단어 하나가 원음과 내 녹음 양쪽에서 어디에 있었는지와 그 차이 (힌트 · 눈금 표시용)
struct WordPair {
    int refIdx = -1;                // 기준 문장에서 몇 번째 단어인지 (scoring 의 토큰 기준: 추임새 · 괄호 제외)
    std::string text;               // 기준 문장의 단어 (원문 표기)
    int origStartMs = 0, origEndMs = 0;   // 원음 클립 기준
    int userStartMs = 0, userEndMs = 0;   // 내 녹음 클립 기준
    float durRatio = 1.f;           // (내 길이 / 내 말 전체) ÷ (원음 길이 / 원음 말 전체). > 1 이면 내가 상대적으로 길게 말함
    float loudDiff = 0.f;           // 내 평균 크기 − 원음 평균 크기 (0~1 단위). > 0 이면 내가 세게
    float pitchDiffSt = 0.f;        // 내 평균 음높이 − 원음 (반음). 둘 다 유성음인 프레임만. hasPitch 가 true 일 때만 의미 있음
    bool hasPitch = false;
};

// 사용자에게 보여 줄 한 줄 힌트. refIdx 가 -1 이면 문장 전체에 대한 것 (예: 속도)
struct Hint {
    int refIdx = -1;
    std::string text;               // 예: "'really' 더 길게", "속도: 원음보다 1.4배 느림"
};

struct Result {
    bool haveIntonation = false, haveRhythm = false, haveStress = false;
    bool rhythmSpeedOnly = true;    // 단어 정렬이 안 돼 속도만으로 리듬 점수를 냈다
    float intonation = 0.f, rhythm = 0.f, stress = 0.f;  // 0~100
    float speedRatio = 1.f;         // 내 말 길이 / 원음 말 길이 (> 1 이면 내가 느림)
    float pitchDiffSt = 0.f;        // 억양: 시간을 맞춘 뒤 반음 차이의 평균 (툴팁용)
    std::vector<WordPair> words;    // 양쪽 모두에서 찾은 기준 단어들 (기준 문장 순서)
    std::vector<Hint> hints;        // 많아야 4개 정도. 가장 큰 차이부터
    std::string note;               // 생략 · 신뢰도 안내 한 줄 (비어 있으면 없음). 예: "유성음 구간이 짧아 억양 비교 생략"
    // 곡선 표시용: 원음 음높이 곡선을 내 녹음 시간축으로 옮긴 것. user.frames() 길이, 반음, 없으면 NaN
    std::vector<float> origOnUser;
    bool haveAlignment = false;     // origOnUser 가 유효한가
};

// 원음(orig) 과 내 녹음(user) 을 비교한다. refText 는 기준 문장. *Words 는 각 클립의 whisper 단어 (비어 있을 수 있다 — 그러면 속도 · DTW 만).
// 일본어는 whisper 가 단어를 나누지 못하므로 단어 정렬 없이 DTW 와 속도만으로 평가한다.
Result compare(const Track& orig, const std::vector<Word>& origWords,
               const Track& user, const std::vector<Word>& userWords,
               const std::string& refText, Lang lang);

// JSON 직렬화 (DB 캐시 · 테스트 출력용). words 는 Word 배열 [{"t":..,"s":..,"e":..}]
std::string wordsToJson(const std::vector<Word>& words);
std::vector<Word> wordsFromJson(const std::string& json);
// 클립 기준 정보를 함께 담는 형식 {"base":클립 시작(영상 절대 ms),"pad":앞뒤 여유 ms,"words":[...]}. 원음 단어 시각 캐시(seg_words)에 쓴다.
// 읽을 때 옛 형식(배열만)이면 base/pad 에 -1 을 돌려준다
std::string wordsToJson(const std::vector<Word>& words, int baseMs, int padMs);
std::vector<Word> wordsFromJson(const std::string& json, int* baseMs, int* padMs);

}  // namespace prosody

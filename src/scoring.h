#pragma once
#include <string>
#include <vector>

#include "lang.h"

// 원문(reference)과 인식 결과(hypothesis)를 단어 단위로 정렬해 채점한다. (일본어는 글자 단위)
struct WordMark {
    enum Kind { Match, Missing, Wrong, Extra };
    std::string text;   // 원문 단어 (Extra 면 인식된 단어)
    std::string heard;  // Wrong 일 때 실제로 들린 단어
    Kind kind = Match;
};

struct ScoreResult {
    float accuracy = 0.0f;  // 0~100
    int matched = 0;
    int total = 0;
    std::string heard;      // 인식된 전체 문장
    std::vector<WordMark> marks;
};

ScoreResult scoreTranscript(const std::string& reference, const std::string& hypothesis, Lang lang = Lang::En);

// scoreTranscript 와 같은 규칙(정규화 · 추임새 · 괄호 제거)으로 토큰을 나눈다. 영어는 단어, 일본어는 글자.
std::vector<std::string> scoringTokens(const std::string& text, Lang lang = Lang::En);

// 기준 문장의 토큰 i 가 인식 결과의 몇 번째 토큰에 대응하는지 (일치 · 치환 모두 대응, 빠짐이면 -1).
// 반환 길이 = scoringTokens(reference).size(). 리듬 비교에서 whisper 단어 시각을 기준 단어에 붙이는 데 쓴다.
std::vector<int> alignTokens(const std::string& reference, const std::vector<std::string>& hypTokens, Lang lang = Lang::En);

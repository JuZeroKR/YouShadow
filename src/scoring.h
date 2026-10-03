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

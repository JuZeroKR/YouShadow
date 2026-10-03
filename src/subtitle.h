#pragma once
#include <string>
#include <vector>

#include "transcript.h"

// 자막 파일(SMI / SRT / VTT) 을 단어 목록으로 읽는다.
// 자막 블록에는 단어별 시각이 없으므로 블록 구간을 단어 길이에 비례해 나눠 준다.
// 인코딩: UTF-8(BOM 유무) / UTF-16 / 그 외는 CP949(한국어 Windows) 로 간주.
namespace subtitle {

bool isSubtitleExt(const std::string& ext);  // ".smi" ".srt" ".vtt" (대소문자 무시)

// 실패하면 std::runtime_error. SMI 에 여러 언어가 있으면 학습 언어(lang) 클래스를 고른다.
std::vector<Word> parseFile(const std::string& path, Lang lang = Lang::En);

// 자막 파일이 lang 자막으로 얼마나 알맞은지 (그 언어 글자 비율 0~1, 읽지 못하면 0).
// 같은 이름의 자막이 여러 개일 때 고르는 데 쓴다.
double langScore(const std::string& path, Lang lang);

// 자막 한 장면 (SMI 의 SYNC 하나, SRT 의 큐 하나)
struct Cue {
    int startMs = 0, endMs = 0;
    std::string text;
};

// 자막을 장면 단위로 읽는다. lang 이 Ko 면 한국어 자막(SMI 의 한국어 클래스, 한글 SRT)을 찾고,
// 없으면 빈 목록을 돌려준다 (예외 없음). En/Ja 는 parseFile 과 같은 규칙으로 고른다.
std::vector<Cue> parseCues(const std::string& path, Lang lang);

// 장면을 대사 단위 문장으로: "- A - B" 처럼 두 사람 대사가 한 장면에 있으면 나누고(시간은 글자 수 비례),
// 앞의 "- " 를 떼고, "year,it's" / "shot.Come" 처럼 붙은 문장 부호 뒤를 띄운다.
std::vector<Segment> cuesToLines(const std::vector<Cue>& cues);

// 다른 언어 자막(번역)을 문장마다 붙인다. 번역 대사 하나는 가장 많이 겹치는 문장 하나에만 들어간다.
// 결과는 segs 와 같은 길이 (없는 문장은 빈 문자열).
std::vector<std::string> alignLines(const std::vector<Segment>& segs, const std::vector<Cue>& other);

}  // namespace subtitle

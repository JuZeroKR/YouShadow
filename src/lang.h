#pragma once
#include <string>
#include <vector>

// 학습 언어. 영상마다 하나씩 붙고(videos.lang), 자막 언어 · whisper 언어 · 음성 · AI 프롬프트 · 채점 단위를 정한다.
// Ko 는 학습 언어가 아니라 음성(TTS) 선택에만 쓴다 (일본어 음성이 없을 때 한국어 발음 표기를 읽어 주는 용도).
enum class Lang { En = 0, Ja = 1, Ko = 2 };

const char* langCode(Lang l);   // "en" / "ja" / "ko"
const char* langName(Lang l);   // "영어" / "일본어" / "한국어"
Lang langFromCode(const std::string& code);

// 일본어 텍스트 보조 (사전 없이 되는 범위만)
namespace jp {

// UTF-8 문자열을 코드포인트 단위 문자열로 나눈다
std::vector<std::string> splitChars(const std::string& utf8);
unsigned decodeFirst(const std::string& utf8, size_t* len = nullptr);

bool isKanji(unsigned cp);
bool isHiragana(unsigned cp);
bool isKatakana(unsigned cp);
bool isJapanesePunct(unsigned cp);

// 가나/한자 비율이 높으면 true
bool looksJapanese(const std::string& utf8);

// 스크립트 경계 기반 간이 토큰화: 한자+뒤따르는 히라가나 / 히라가나 / 가타카나 / 영숫자. 구두점은 버린다.
// (사전이 없어 조사까지 붙는다. AI 키가 있으면 AI 토큰을 쓴다.)
std::vector<std::string> roughTokens(const std::string& utf8);

std::string katakanaToHiragana(const std::string& utf8);

// 가나 → 한국어 발음 (예: たべます → 타베마스, きって → 킷테, さん → 산). 한자 등 가나가 아닌 글자는 그대로 둔다.
std::string kanaToKorean(const std::string& utf8);

}  // namespace jp

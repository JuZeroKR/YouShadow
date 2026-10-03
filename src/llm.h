#pragma once
#include <string>
#include <vector>

#include "lang.h"

// Claude / OpenAI / Gemini 를 같은 인터페이스로 쓴다. 용도: 문장 해설(번역, 표현, 문법).
enum class Provider { Claude = 0, OpenAI = 1, Gemini = 2 };

const char* providerName(Provider p);

struct LlmConfig {
    Provider provider = Provider::Claude;
    std::string claudeKey, openaiKey, geminiKey;
    std::string claudeModel = "claude-opus-5";
    std::string openaiModel = "gpt-5-mini";
    std::string geminiModel = "gemini-2.5-flash";

    const std::string& key(Provider p) const { return p == Provider::Claude ? claudeKey : p == Provider::OpenAI ? openaiKey : geminiKey; }
    const std::string& model(Provider p) const { return p == Provider::Claude ? claudeModel : p == Provider::OpenAI ? openaiModel : geminiModel; }
    bool ready() const { return !key(provider).empty() && !model(provider).empty(); }
};

// 시스템 프롬프트 + 사용자 메시지 → 텍스트. JSON 응답을 요구하는 용도로 쓴다.
std::string llmComplete(const LlmConfig& cfg, const std::string& system, const std::string& user, std::string* err);

// 사용 가능한 모델 ID 목록
std::vector<std::string> llmListModels(const LlmConfig& cfg, Provider p, std::string* err);

// ---- 문장 해설 ----
struct Expression {
    std::string text;     // 표현 원문
    std::string meaning;  // 한국어 뜻
    std::string note;     // 뉘앙스, 쓰임
    std::string example;  // 예문
};

struct Explanation {
    std::string translation;
    std::vector<Expression> expressions;
    std::string grammar;
    std::string reading;        // 일본어: 문장 전체 읽기 (히라가나)
    std::string pronunciation;  // 일본어: 한국어 발음
    std::string provider, model;

    std::string toJson() const;
    static Explanation fromJson(const std::string& json);
};

// 앞뒤 문장을 문맥으로 주고 현재 문장을 해설한다.
Explanation explainSentence(const LlmConfig& cfg, const std::string& sentence,
                            const std::string& before, const std::string& after, std::string* err,
                            Lang lang = Lang::En);

// ---- 단어 뜻 ----
struct WordMeaning {
    std::string word;            // 기본형 (예: "running" → "run")
    std::string ipa;             // 발음 기호 (없으면 빈 문자열)
    std::string pos;             // 품사
    std::string meaning;         // 대표 뜻 (한국어, 사전 폴백이면 영어)
    std::string contextMeaning;  // 이 문장에서의 뜻 / 쓰임
    std::string example;         // 예문
    std::string reading;         // 일본어: 읽기 (히라가나)
    std::string korean;          // 일본어: 한국어 발음
    std::string provider, model; // 출처 (AI 공급자 또는 "사전")

    bool empty() const { return meaning.empty() && contextMeaning.empty(); }
    std::string toJson() const;
    static WordMeaning fromJson(const std::string& json);
};

// AI 로 문장 속 단어의 뜻을 묻는다 (한국어 설명). 일본어면 읽기와 한국어 발음도 받는다.
WordMeaning explainWord(const LlmConfig& cfg, const std::string& word, const std::string& sentence, std::string* err,
                        Lang lang = Lang::En);

// API 키가 없을 때의 폴백: Wiktionary(영어판) 에서 그 언어 단어의 영어 정의를 가져온다.
WordMeaning lookupDictionary(const std::string& word, std::string* err, Lang lang = Lang::En);

// ---- 일본어 읽기 ----
// 일본어 문장을 단어로 나누고 각 단어의 읽기(히라가나) · 한국어 발음 · 짧은 뜻을 받는다.
struct JaToken {
    std::string surface;  // 문장에 나온 그대로
    std::string reading;  // 히라가나
    std::string korean;   // 한국어 발음
    std::string meaning;  // 짧은 한국어 뜻 (조사는 "조사" 등)
};

struct JaReading {
    std::string reading;        // 문장 전체 읽기
    std::string pronunciation;  // 문장 전체 한국어 발음
    std::vector<JaToken> tokens;
    std::string provider, model;

    bool empty() const { return tokens.empty(); }
    std::string toJson() const;
    static JaReading fromJson(const std::string& json);
};

JaReading readJapanese(const LlmConfig& cfg, const std::string& sentence, std::string* err);

// AI 없이 만드는 대략의 읽기: 스크립트 경계 토큰 + 가나→한국어 (한자는 발음을 모른다)
JaReading roughJapaneseReading(const std::string& sentence);

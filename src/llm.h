#pragma once
#include <string>
#include <vector>

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
    std::string provider, model;

    std::string toJson() const;
    static Explanation fromJson(const std::string& json);
};

// 앞뒤 문장을 문맥으로 주고 현재 문장을 해설한다.
Explanation explainSentence(const LlmConfig& cfg, const std::string& sentence,
                            const std::string& before, const std::string& after, std::string* err);

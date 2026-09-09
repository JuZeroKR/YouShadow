#include "llm.h"

#include <algorithm>

#include "http.h"
#include "json.hpp"

using json = nlohmann::json;

const char* providerName(Provider p) {
    switch (p) {
        case Provider::Claude: return "Claude";
        case Provider::OpenAI: return "ChatGPT";
        default: return "Gemini";
    }
}

namespace {

std::string errorFromBody(const std::string& body, int status) {
    try {
        json j = json::parse(body);
        if (j.contains("error")) {
            const auto& e = j["error"];
            if (e.is_object() && e.contains("message")) return e["message"].get<std::string>();
            if (e.is_string()) return e.get<std::string>();
        }
    } catch (...) {}
    return "HTTP " + std::to_string(status) + ": " + body.substr(0, 300);
}

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// ---- Claude: POST /v1/messages ----
std::string claudeComplete(const LlmConfig& cfg, const std::string& system, const std::string& user, std::string* err) {
    json body = {
        {"model", cfg.claudeModel},
        {"max_tokens", 4096},
        {"system", system},
        {"messages", json::array({{{"role", "user"}, {"content", user}}})},
    };
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"},
        {"x-api-key", cfg.claudeKey},
        {"anthropic-version", "2023-06-01"},
    };
    // Claude Opus 5 / Fable 계열: 안전 분류기가 거절하면 서버가 다른 모델로 이어서 답하게 한다
    if (startsWith(cfg.claudeModel, "claude-opus-5") || startsWith(cfg.claudeModel, "claude-fable")) {
        headers.push_back({"anthropic-beta", "server-side-fallback-2026-07-01"});
        body["fallbacks"] = "default";
    }
    auto r = httpRequest("POST", "https://api.anthropic.com/v1/messages", headers, body.dump(), err);
    if (r.status == 0) return "";
    if (r.status != 200) { if (err) *err = errorFromBody(r.body, r.status); return ""; }
    try {
        json j = json::parse(r.body);
        if (j.value("stop_reason", "") == "refusal") { if (err) *err = "모델이 요청을 거절했습니다"; return ""; }
        std::string out;
        for (const auto& c : j["content"]) if (c.value("type", "") == "text") out += c.value("text", "");
        return out;
    } catch (const std::exception& e) {
        if (err) *err = std::string("응답 파싱 실패: ") + e.what();
        return "";
    }
}

std::vector<std::string> claudeModels(const LlmConfig& cfg, std::string* err) {
    std::vector<std::string> out;
    auto r = httpRequest("GET", "https://api.anthropic.com/v1/models?limit=100",
                         {{"x-api-key", cfg.claudeKey}, {"anthropic-version", "2023-06-01"}}, "", err);
    if (r.status == 0) return out;
    if (r.status != 200) { if (err) *err = errorFromBody(r.body, r.status); return out; }
    try {
        for (const auto& m : json::parse(r.body)["data"]) out.push_back(m.value("id", ""));
    } catch (...) { if (err) *err = "응답 파싱 실패"; }
    return out;
}

// ---- OpenAI: POST /v1/chat/completions ----
std::string openaiComplete(const LlmConfig& cfg, const std::string& system, const std::string& user, std::string* err) {
    json body = {
        {"model", cfg.openaiModel},
        {"messages", json::array({{{"role", "system"}, {"content", system}}, {{"role", "user"}, {"content", user}}})},
        {"response_format", {{"type", "json_object"}}},
    };
    auto r = httpRequest("POST", "https://api.openai.com/v1/chat/completions",
                         {{"Content-Type", "application/json"}, {"Authorization", "Bearer " + cfg.openaiKey}},
                         body.dump(), err);
    if (r.status == 0) return "";
    if (r.status != 200) { if (err) *err = errorFromBody(r.body, r.status); return ""; }
    try {
        json j = json::parse(r.body);
        return j["choices"][0]["message"].value("content", "");
    } catch (const std::exception& e) {
        if (err) *err = std::string("응답 파싱 실패: ") + e.what();
        return "";
    }
}

std::vector<std::string> openaiModels(const LlmConfig& cfg, std::string* err) {
    std::vector<std::string> out;
    auto r = httpRequest("GET", "https://api.openai.com/v1/models", {{"Authorization", "Bearer " + cfg.openaiKey}}, "", err);
    if (r.status == 0) return out;
    if (r.status != 200) { if (err) *err = errorFromBody(r.body, r.status); return out; }
    try {
        for (const auto& m : json::parse(r.body)["data"]) {
            std::string id = m.value("id", "");
            // 채팅용 모델만 (임베딩, 이미지, 음성 모델 제외)
            if (startsWith(id, "gpt-") || startsWith(id, "o1") || startsWith(id, "o3") || startsWith(id, "o4")) {
                if (id.find("embedding") == std::string::npos && id.find("audio") == std::string::npos &&
                    id.find("realtime") == std::string::npos && id.find("image") == std::string::npos &&
                    id.find("tts") == std::string::npos && id.find("transcribe") == std::string::npos)
                    out.push_back(id);
            }
        }
    } catch (...) { if (err) *err = "응답 파싱 실패"; }
    std::sort(out.begin(), out.end());
    return out;
}

// ---- Gemini: POST /v1beta/models/{model}:generateContent ----
std::string geminiComplete(const LlmConfig& cfg, const std::string& system, const std::string& user, std::string* err) {
    json body = {
        {"system_instruction", {{"parts", json::array({{{"text", system}}})}}},
        {"contents", json::array({{{"role", "user"}, {"parts", json::array({{{"text", user}}})}}})},
        {"generationConfig", {{"responseMimeType", "application/json"}}},
    };
    std::string url = "https://generativelanguage.googleapis.com/v1beta/models/" + cfg.geminiModel + ":generateContent";
    auto r = httpRequest("POST", url, {{"Content-Type", "application/json"}, {"x-goog-api-key", cfg.geminiKey}}, body.dump(), err);
    if (r.status == 0) return "";
    if (r.status != 200) { if (err) *err = errorFromBody(r.body, r.status); return ""; }
    try {
        json j = json::parse(r.body);
        std::string out;
        for (const auto& p : j["candidates"][0]["content"]["parts"]) out += p.value("text", "");
        return out;
    } catch (const std::exception& e) {
        if (err) *err = std::string("응답 파싱 실패: ") + e.what();
        return "";
    }
}

std::vector<std::string> geminiModels(const LlmConfig& cfg, std::string* err) {
    std::vector<std::string> out;
    auto r = httpRequest("GET", "https://generativelanguage.googleapis.com/v1beta/models?pageSize=200",
                         {{"x-goog-api-key", cfg.geminiKey}}, "", err);
    if (r.status == 0) return out;
    if (r.status != 200) { if (err) *err = errorFromBody(r.body, r.status); return out; }
    try {
        for (const auto& m : json::parse(r.body)["models"]) {
            bool gen = false;
            if (m.contains("supportedGenerationMethods"))
                for (const auto& g : m["supportedGenerationMethods"]) if (g == "generateContent") gen = true;
            if (!gen) continue;
            std::string name = m.value("name", "");
            if (startsWith(name, "models/")) name = name.substr(7);
            if (startsWith(name, "gemini")) out.push_back(name);
        }
    } catch (...) { if (err) *err = "응답 파싱 실패"; }
    std::sort(out.begin(), out.end());
    return out;
}

// 응답에서 JSON 본문만 뽑는다 (```json 펜스나 앞뒤 설명이 붙어도 동작)
std::string extractJson(const std::string& s) {
    size_t a = s.find('{');
    size_t b = s.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b < a) return s;
    return s.substr(a, b - a + 1);
}

}  // namespace

std::string llmComplete(const LlmConfig& cfg, const std::string& system, const std::string& user, std::string* err) {
    if (cfg.key(cfg.provider).empty()) { if (err) *err = std::string(providerName(cfg.provider)) + " API 키가 없습니다. 설정에서 입력하세요"; return ""; }
    switch (cfg.provider) {
        case Provider::Claude: return claudeComplete(cfg, system, user, err);
        case Provider::OpenAI: return openaiComplete(cfg, system, user, err);
        default: return geminiComplete(cfg, system, user, err);
    }
}

std::vector<std::string> llmListModels(const LlmConfig& cfg, Provider p, std::string* err) {
    if (cfg.key(p).empty()) { if (err) *err = "API 키를 먼저 입력하세요"; return {}; }
    switch (p) {
        case Provider::Claude: return claudeModels(cfg, err);
        case Provider::OpenAI: return openaiModels(cfg, err);
        default: return geminiModels(cfg, err);
    }
}

// ---------------- 문장 해설 ----------------

std::string Explanation::toJson() const {
    json j;
    j["translation"] = translation;
    j["grammar"] = grammar;
    j["provider"] = provider;
    j["model"] = model;
    j["expressions"] = json::array();
    for (const auto& e : expressions) j["expressions"].push_back({{"text", e.text}, {"meaning", e.meaning}, {"note", e.note}, {"example", e.example}});
    return j.dump();
}

Explanation Explanation::fromJson(const std::string& s) {
    Explanation ex;
    try {
        json j = json::parse(s);
        ex.translation = j.value("translation", "");
        ex.grammar = j.value("grammar", "");
        ex.provider = j.value("provider", "");
        ex.model = j.value("model", "");
        if (j.contains("expressions") && j["expressions"].is_array()) {
            for (const auto& e : j["expressions"]) {
                if (!e.is_object()) continue;
                Expression x;
                x.text = e.value("text", "");
                x.meaning = e.value("meaning", "");
                x.note = e.value("note", "");
                x.example = e.value("example", "");
                if (!x.text.empty()) ex.expressions.push_back(x);
            }
        }
    } catch (...) {}
    return ex;
}

static const char* kSystemPrompt =
    "You are an English tutor helping a Korean learner study spoken English from YouTube videos. "
    "You will get one sentence plus the sentences before and after it for context. "
    "Respond with ONLY a JSON object, no markdown, in this exact shape:\n"
    "{\"translation\": \"자연스러운 한국어 번역\", "
    "\"expressions\": [{\"text\": \"표현 원문 (문장에 나온 그대로)\", \"meaning\": \"한국어 뜻\", "
    "\"note\": \"뉘앙스, 쓰임새, 격식 정도를 한국어로 1~2문장\", \"example\": \"그 표현을 쓴 짧은 영어 예문 하나\"}], "
    "\"grammar\": \"문장 구조나 문법 포인트를 한국어로 1~2문장. 특별한 것이 없으면 빈 문자열\"}\n"
    "Pick 1 to 5 expressions worth learning: idioms, phrasal verbs, collocations, spoken patterns, "
    "words used in a non-obvious sense. Skip trivial words and proper nouns. "
    "Write meaning/note/grammar in Korean; keep text/example in English.";

Explanation explainSentence(const LlmConfig& cfg, const std::string& sentence,
                            const std::string& before, const std::string& after, std::string* err) {
    std::string user = "Context before: " + (before.empty() ? std::string("(none)") : before) +
                       "\nSentence: " + sentence +
                       "\nContext after: " + (after.empty() ? std::string("(none)") : after);
    std::string text = llmComplete(cfg, kSystemPrompt, user, err);
    Explanation ex;
    if (text.empty()) return ex;
    ex = Explanation::fromJson(extractJson(text));
    if (ex.translation.empty() && ex.expressions.empty()) {
        if (err) *err = "해설 형식을 읽지 못했습니다: " + text.substr(0, 200);
        return ex;
    }
    ex.provider = providerName(cfg.provider);
    ex.model = cfg.model(cfg.provider);
    return ex;
}

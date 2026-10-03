#include "llm.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

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
    if (!reading.empty()) j["reading"] = reading;
    if (!pronunciation.empty()) j["pronunciation"] = pronunciation;
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
        ex.reading = j.contains("reading") && j["reading"].is_string() ? j["reading"].get<std::string>() : "";
        ex.pronunciation = j.contains("pronunciation") && j["pronunciation"].is_string() ? j["pronunciation"].get<std::string>() : "";
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

static const char* kSystemPromptJa =
    "You are a Japanese tutor helping a Korean learner study spoken Japanese from YouTube videos. "
    "The learner cannot read kanji well, so readings and Korean pronunciation matter. "
    "You will get one sentence plus the sentences before and after it for context. "
    "Respond with ONLY a JSON object, no markdown, in this exact shape:\n"
    "{\"translation\": \"자연스러운 한국어 번역\", "
    "\"reading\": \"문장 전체를 히라가나로 (한자를 모두 읽기로 바꾼 것, 띄어쓰기로 단어 구분)\", "
    "\"pronunciation\": \"문장 전체의 한국어 발음 표기 (예: 와타시와 가쿠세이데스)\", "
    "\"expressions\": [{\"text\": \"표현 원문 (문장에 나온 그대로)\", \"meaning\": \"한국어 뜻\", "
    "\"note\": \"뉘앙스, 쓰임새, 경어 수준을 한국어로 1~2문장. 읽기(히라가나)도 함께\", \"example\": \"그 표현을 쓴 짧은 일본어 예문 하나\"}], "
    "\"grammar\": \"문형이나 문법 포인트를 한국어로 1~2문장. 특별한 것이 없으면 빈 문자열\"}\n"
    "Pick 1 to 5 expressions worth learning: set phrases, grammar patterns, verb conjugations, spoken contractions, "
    "words used in a non-obvious sense. Skip trivial particles and proper nouns. "
    "Write meaning/note/grammar in Korean; keep text/example in Japanese.";

Explanation explainSentence(const LlmConfig& cfg, const std::string& sentence,
                            const std::string& before, const std::string& after, std::string* err, Lang lang) {
    std::string user = "Context before: " + (before.empty() ? std::string("(none)") : before) +
                       "\nSentence: " + sentence +
                       "\nContext after: " + (after.empty() ? std::string("(none)") : after);
    std::string text = llmComplete(cfg, lang == Lang::Ja ? kSystemPromptJa : kSystemPrompt, user, err);
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

// ---------------- 단어 뜻 ----------------

std::string WordMeaning::toJson() const {
    json j;
    j["word"] = word;
    j["ipa"] = ipa;
    j["pos"] = pos;
    j["meaning"] = meaning;
    j["context"] = contextMeaning;
    j["example"] = example;
    if (!reading.empty()) j["reading"] = reading;
    if (!korean.empty()) j["korean"] = korean;
    j["provider"] = provider;
    j["model"] = model;
    return j.dump();
}

WordMeaning WordMeaning::fromJson(const std::string& s) {
    WordMeaning w;
    try {
        json j = json::parse(s);
        auto str = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
        w.word = str("word");
        w.ipa = str("ipa");
        w.pos = str("pos");
        w.meaning = str("meaning");
        w.contextMeaning = str("context");
        w.example = str("example");
        w.reading = str("reading");
        w.korean = str("korean");
        w.provider = str("provider");
        w.model = str("model");
    } catch (...) {}
    return w;
}

static const char* kWordPromptJa =
    "You are a Japanese tutor helping a Korean learner who cannot read kanji well. The learner clicked one word in a sentence from a YouTube video. "
    "Respond with ONLY a JSON object, no markdown, in this exact shape:\n"
    "{\"word\": \"기본형 (예: 食べました → 食べる, 조사나 고유명사는 그대로)\", "
    "\"reading\": \"그 단어(문장에 나온 형태)의 읽기를 히라가나로\", "
    "\"korean\": \"그 읽기의 한국어 발음 표기 (예: 타베마시타)\", "
    "\"pos\": \"품사를 한국어로 (명사/동사/い형용사/な형용사/부사/조사/조동사 등)\", "
    "\"meaning\": \"대표적인 뜻을 한국어로, 쉼표로 구분한 1~3개\", "
    "\"context\": \"이 문장에서는 어떤 뜻·활용으로 쓰였는지 한국어로 1~2문장. 활용형이면 기본형과 활용을 알려줄 것\", "
    "\"example\": \"그 단어를 같은 뜻으로 쓴 짧은 일본어 예문 하나 (뒤에 괄호로 한국어 발음)\"}\n"
    "Write meaning/context in Korean; keep word/example in Japanese. Be concise.";

static const char* kWordPrompt =
    "You are an English tutor helping a Korean learner. The learner clicked one word in a sentence from a YouTube video. "
    "Respond with ONLY a JSON object, no markdown, in this exact shape:\n"
    "{\"word\": \"기본형 (예: running → run, 고유명사나 구어 축약형은 그대로)\", "
    "\"ipa\": \"미국식 발음 기호, 모르면 빈 문자열\", "
    "\"pos\": \"품사를 한국어로 (명사/동사/형용사/부사/전치사/감탄사 등)\", "
    "\"meaning\": \"대표적인 뜻을 한국어로, 쉼표로 구분한 2~4개\", "
    "\"context\": \"이 문장에서는 어떤 뜻으로 쓰였는지 한국어로 1~2문장. 구동사나 숙어의 일부라면 그 표현 전체를 알려줄 것\", "
    "\"example\": \"그 단어를 같은 뜻으로 쓴 짧은 영어 예문 하나\"}\n"
    "Write meaning/context in Korean; keep word/example in English. Be concise.";

WordMeaning explainWord(const LlmConfig& cfg, const std::string& word, const std::string& sentence, std::string* err, Lang lang) {
    std::string user = "Sentence: " + sentence + "\nClicked word: " + word;
    std::string text = llmComplete(cfg, lang == Lang::Ja ? kWordPromptJa : kWordPrompt, user, err);
    WordMeaning w;
    if (text.empty()) return w;
    w = WordMeaning::fromJson(extractJson(text));
    if (w.empty()) {
        if (err) *err = "단어 뜻 형식을 읽지 못했습니다: " + text.substr(0, 200);
        return w;
    }
    if (w.word.empty()) w.word = word;
    w.provider = providerName(cfg.provider);
    w.model = cfg.model(cfg.provider);
    return w;
}

// Wiktionary 정의의 HTML 을 평문으로 (태그 제거, 기본 엔티티 해제, 공백 정리)
static std::string htmlToText(std::string html) {
    // <style>…</style> 는 내용까지 통째로 지운다 (Wiktionary 정의에 CSS 가 섞여 온다)
    for (size_t a; (a = html.find("<style")) != std::string::npos;) {
        size_t b = html.find("</style>", a);
        html.erase(a, b == std::string::npos ? std::string::npos : b + 8 - a);
    }
    std::string out;
    bool inTag = false;
    for (size_t i = 0; i < html.size(); ++i) {
        char c = html[i];
        if (c == '<') { inTag = true; continue; }
        if (c == '>') { inTag = false; continue; }
        if (inTag) continue;
        if (c == '&') {
            size_t semi = html.find(';', i);
            if (semi != std::string::npos && semi - i <= 7) {
                std::string ent = html.substr(i + 1, semi - i - 1);
                const char* rep = ent == "amp" ? "&" : ent == "lt" ? "<" : ent == "gt" ? ">" : ent == "quot" ? "\"" :
                                  (ent == "#39" || ent == "apos") ? "'" : ent == "nbsp" ? " " : nullptr;
                if (rep) { out += rep; i = semi; continue; }
            }
        }
        out += c;
    }
    // 공백 정리
    std::string s;
    bool space = true;
    for (char c : out) {
        if (std::isspace((unsigned char)c)) { if (!space) s += ' '; space = true; }
        else { s += c; space = false; }
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

WordMeaning lookupDictionary(const std::string& word, std::string* err, Lang lang) {
    // Wiktionary(영어판) 정의 API. 응답은 언어 코드별("en", "ja")로 묶여 온다. 소문자로 먼저 찾고, 없으면 원래 표기로 다시 찾는다.
    const std::string key = langCode(lang);
    auto fetch = [&](const std::string& w, std::string* e) {
        std::string enc;
        for (unsigned char c : w) {
            if (std::isalnum(c) || c == '-' || c == '\'' || c == '_') enc += (char)c;
            else { char buf[4]; snprintf(buf, sizeof buf, "%%%02X", c); enc += buf; }
        }
        return httpRequest("GET", "https://en.wiktionary.org/api/rest_v1/page/definition/" + enc,
                           {{"Accept", "application/json"}}, "", e, 15);
    };
    std::string lower = word;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::string e;
    HttpResponse r = fetch(lower, &e);
    if (r.status == 404 && lower != word) r = fetch(word, &e);
    WordMeaning w;
    if (r.status == 0) { if (err) *err = "사전 요청 실패: " + e; return w; }
    if (r.status == 404) { if (err) *err = "사전에 없는 단어입니다: " + word; return w; }
    if (r.status != 200) { if (err) *err = "사전 응답 오류 (HTTP " + std::to_string(r.status) + ")"; return w; }
    try {
        json j = json::parse(r.body);
        if (!j.contains(key) || !j[key].is_array()) {
            if (err) *err = std::string("사전에 ") + langName(lang) + " 항목이 없습니다: " + word;
            return WordMeaning();
        }
        std::string meanings;
        int nPos = 0, nTotal = 0;
        for (const auto& entry : j[key]) {
            if (nTotal >= 5) break;
            std::string pos = entry.value("partOfSpeech", "");
            if (!entry.contains("definitions") || !entry["definitions"].is_array()) continue;
            int nDef = 0;
            for (const auto& d : entry["definitions"]) {
                std::string text = htmlToText(d.value("definition", ""));
                if (text.empty()) continue;
                // Wiktionary 는 중첩된 하위 뜻을 별도 정의로도 한 번 더 주므로 이미 포함된 문장은 건너뛴다
                if (meanings.find(text) != std::string::npos) continue;
                if (text.size() > 160) text = text.substr(0, 157) + "...";
                if (!meanings.empty()) meanings += "\n";
                meanings += "(" + pos + ") " + text;
                if (w.example.empty() && d.contains("examples") && d["examples"].is_array() && !d["examples"].empty())
                    w.example = htmlToText(d["examples"][0].get<std::string>());
                ++nTotal;
                if (++nDef >= 2 || nTotal >= 5) break;
            }
            if (nDef > 0) { if (w.pos.empty()) w.pos = pos; if (++nPos >= 3) break; }
        }
        w.word = lower;
        w.meaning = meanings;
        w.provider = "사전";
        w.model = "Wiktionary";
    } catch (...) {
        if (err) *err = "사전 응답을 읽지 못했습니다";
        return WordMeaning();
    }
    if (w.empty() && err) *err = "사전에 영어 정의가 없습니다: " + word;
    return w;
}

// ---------------- 일본어 읽기 ----------------

std::string JaReading::toJson() const {
    json j;
    j["reading"] = reading;
    j["pronunciation"] = pronunciation;
    j["provider"] = provider;
    j["model"] = model;
    j["tokens"] = json::array();
    for (const auto& t : tokens) j["tokens"].push_back({{"surface", t.surface}, {"reading", t.reading}, {"korean", t.korean}, {"meaning", t.meaning}});
    return j.dump();
}

JaReading JaReading::fromJson(const std::string& s) {
    JaReading r;
    try {
        json j = json::parse(s);
        auto str = [](const json& o, const char* k) { return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string(); };
        r.reading = str(j, "reading");
        r.pronunciation = str(j, "pronunciation");
        r.provider = str(j, "provider");
        r.model = str(j, "model");
        if (j.contains("tokens") && j["tokens"].is_array()) {
            for (const auto& t : j["tokens"]) {
                if (!t.is_object()) continue;
                JaToken tk{str(t, "surface"), str(t, "reading"), str(t, "korean"), str(t, "meaning")};
                if (!tk.surface.empty()) r.tokens.push_back(tk);
            }
        }
    } catch (...) {}
    return r;
}

static const char* kReadingPrompt =
    "You help a Korean learner who cannot read kanji follow spoken Japanese. Split the given Japanese sentence into words "
    "(not characters; keep particles as their own tokens, keep a verb with its conjugation as one token). "
    "Respond with ONLY a JSON object, no markdown, in this exact shape:\n"
    "{\"reading\": \"문장 전체 읽기를 히라가나로, 단어 사이 띄어쓰기\", "
    "\"pronunciation\": \"문장 전체의 한국어 발음 표기, 단어 사이 띄어쓰기 (예: 와타시와 가쿠세이 데스)\", "
    "\"tokens\": [{\"surface\": \"문장에 나온 그대로\", \"reading\": \"히라가나 읽기 (가타카나 단어도 히라가나로)\", "
    "\"korean\": \"한국어 발음\", \"meaning\": \"짧은 한국어 뜻 (조사는 '조사(~은/는)' 식으로)\"}]}\n"
    "The surfaces, concatenated in order, must reproduce the sentence exactly except for spaces and punctuation. "
    "Korean pronunciation conventions: か=카 た=타 つ=츠 ふ=후, っ=받침 ㅅ, ん=받침 ㄴ, long vowels not doubled.";

JaReading readJapanese(const LlmConfig& cfg, const std::string& sentence, std::string* err) {
    std::string text = llmComplete(cfg, kReadingPrompt, "Sentence: " + sentence, err);
    JaReading r;
    if (text.empty()) return r;
    r = JaReading::fromJson(extractJson(text));
    if (r.empty()) {
        if (err) *err = "읽기 형식을 읽지 못했습니다: " + text.substr(0, 200);
        return r;
    }
    // 토큰에 발음이 비어 있으면 읽기에서 만들어 채운다
    for (auto& t : r.tokens) if (t.korean.empty() && !t.reading.empty()) t.korean = jp::kanaToKorean(t.reading);
    if (r.pronunciation.empty() && !r.reading.empty()) r.pronunciation = jp::kanaToKorean(r.reading);
    r.provider = providerName(cfg.provider);
    r.model = cfg.model(cfg.provider);
    return r;
}

JaReading roughJapaneseReading(const std::string& sentence) {
    JaReading r;
    for (const auto& tok : jp::roughTokens(sentence)) {
        JaToken t;
        t.surface = tok;
        bool kanaOnly = true;
        for (const auto& ch : jp::splitChars(tok)) {
            unsigned cp = jp::decodeFirst(ch);
            if (jp::isKanji(cp)) { kanaOnly = false; break; }
        }
        if (kanaOnly) {
            t.reading = jp::katakanaToHiragana(tok);
            t.korean = jp::kanaToKorean(tok);
        }
        r.tokens.push_back(t);
        if (!r.reading.empty()) { r.reading += ' '; r.pronunciation += ' '; }
        r.reading += t.reading.empty() ? tok : t.reading;
        r.pronunciation += t.korean.empty() ? tok : t.korean;  // 한자는 그대로 남는다
    }
    r.provider = "간이";
    r.model = "스크립트 경계 분할";
    return r;
}

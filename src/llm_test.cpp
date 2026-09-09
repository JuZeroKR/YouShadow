// LLM 연동 검증 도구.
//   llm_test                    → 세 서비스에 잘못된 키로 모델 목록을 요청해 HTTP/오류 처리 확인
//   llm_test <provider> <key> [model] [sentence]  → 실제 키로 문장 해설 한 번 (provider: claude|openai|gemini)
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "llm.h"

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    LlmConfig cfg;
    if (argc < 3) {
        cfg.claudeKey = cfg.openaiKey = cfg.geminiKey = "invalid-key-for-test";
        for (int p = 0; p < 3; ++p) {
            std::string err;
            auto models = llmListModels(cfg, (Provider)p, &err);
            std::cout << providerName((Provider)p) << ": " << models.size() << " models, err=" << err << "\n";
        }
        return 0;
    }
    std::string prov = argv[1];
    cfg.provider = prov == "openai" ? Provider::OpenAI : prov == "gemini" ? Provider::Gemini : Provider::Claude;
    if (cfg.provider == Provider::Claude) cfg.claudeKey = argv[2];
    else if (cfg.provider == Provider::OpenAI) cfg.openaiKey = argv[2];
    else cfg.geminiKey = argv[2];
    if (argc > 3) {
        if (cfg.provider == Provider::Claude) cfg.claudeModel = argv[3];
        else if (cfg.provider == Provider::OpenAI) cfg.openaiModel = argv[3];
        else cfg.geminiModel = argv[3];
    }
    std::string sentence = argc > 4 ? argv[4] : "Well, even if you're not planning on being a novelist, I think writing is just part of everyday life.";

    std::string err;
    auto models = llmListModels(cfg, cfg.provider, &err);
    std::cout << "models: " << models.size() << (err.empty() ? "" : " err=" + err) << "\n";
    for (size_t i = 0; i < models.size() && i < 8; ++i) std::cout << "  " << models[i] << "\n";

    err.clear();
    Explanation ex = explainSentence(cfg, sentence, "", "", &err);
    if (!err.empty()) { std::cout << "error: " << err << "\n"; return 1; }
    std::cout << "번역: " << ex.translation << "\n";
    for (const auto& e : ex.expressions) std::cout << "- " << e.text << " : " << e.meaning << " | " << e.note << " | " << e.example << "\n";
    std::cout << "문법: " << ex.grammar << "\n(" << ex.provider << " · " << ex.model << ")\n";
    return 0;
}

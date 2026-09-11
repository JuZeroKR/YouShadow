#include "stt.h"

#include <algorithm>
#include <cctype>
#include <thread>

#include "ggml.h"
#include "paths.h"
#include "whisper.h"

namespace {

void quietLog(enum ggml_log_level, const char*, void*) {}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

int threadCount() {
    unsigned n = std::thread::hardware_concurrency();
    return (int)std::clamp<unsigned>(n > 1 ? n - 1 : 1, 1, 8);
}

void progressThunk(whisper_context*, whisper_state*, int progress, void* user) {
    auto* fn = static_cast<const std::function<void(int)>*>(user);
    if (fn && *fn) (*fn)(progress);
}

}  // namespace

std::string Stt::defaultModelPath() { return paths::modelsDir() + "/ggml-base.en.bin"; }
std::string Stt::modelUrl() {
    return "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin";
}

Stt::~Stt() {
    if (ctx_) whisper_free(ctx_);
}

bool Stt::load(const std::string& modelPath, std::string* err) {
    std::lock_guard<std::mutex> lock(m_);
    whisper_log_set(quietLog, nullptr);
    ggml_log_set(quietLog, nullptr);
    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = false;
    ctx_ = whisper_init_from_file_with_params(modelPath.c_str(), cp);
    if (!ctx_) {
        if (err) *err = "STT 모델을 열 수 없습니다: " + modelPath;
        return false;
    }
    return true;
}

std::vector<Word> Stt::transcribe(const std::vector<float>& pcm16k,
                                  const std::function<void(int)>& progress, std::string* err) {
    std::vector<Word> words;
    if (!ctx_) {
        if (err) *err = "STT 모델이 로드되지 않았습니다";
        return words;
    }
    std::lock_guard<std::mutex> lock(m_);

    // whisper 는 1초 미만 입력을 거부하므로 짧은 녹음은 뒤에 무음을 붙인다.
    std::vector<float> pcm = pcm16k;
    if (pcm.size() < (size_t)kRate * 2) pcm.resize((size_t)kRate * 2, 0.0f);

    whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    p.language = "en";
    p.n_threads = threadCount();
    p.translate = false;
    p.no_context = true;
    p.print_progress = false;
    p.print_realtime = false;
    p.print_timestamps = false;
    p.print_special = false;
    p.token_timestamps = true;
    p.suppress_blank = true;
    p.progress_callback = progressThunk;
    p.progress_callback_user_data = const_cast<std::function<void(int)>*>(&progress);

    if (whisper_full(ctx_, p, pcm.data(), (int)pcm.size()) != 0) {
        if (err) *err = "whisper_full 실패";
        return words;
    }

    const int eot = whisper_token_eot(ctx_);
    const int ns = whisper_full_n_segments(ctx_);
    for (int i = 0; i < ns; ++i) {
        const int nt = whisper_full_n_tokens(ctx_, i);
        bool segStart = true;
        for (int j = 0; j < nt; ++j) {
            whisper_token_data td = whisper_full_get_token_data(ctx_, i, j);
            if (td.id >= eot) continue;  // 특수 토큰
            std::string t = whisper_full_get_token_text(ctx_, i, j);
            if (t.rfind("[_", 0) == 0) continue;
            const bool newWord = segStart || (!t.empty() && t[0] == ' ');
            const int t0 = (int)td.t0 * 10, t1 = (int)td.t1 * 10;
            if (newWord) {
                std::string tt = trim(t);
                if (tt.empty()) continue;
                words.push_back({tt, t0, std::max(t1, t0 + 50)});
                segStart = false;
            } else if (!words.empty()) {
                words.back().text += t;
                words.back().endMs = std::max(words.back().endMs, t1);
            }
        }
    }
    return words;
}

std::string Stt::transcribeText(const std::vector<float>& pcm16k, std::string* err) {
    std::string out;
    for (const auto& w : transcribe(pcm16k, {}, err)) {
        if (!out.empty()) out += ' ';
        out += w.text;
    }
    return out;
}

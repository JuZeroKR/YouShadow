#include "stt.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <thread>

#include "ggml.h"
#include "paths.h"
#include "whisper.h"

namespace {

// 환경 변수 YS_WHISPER_LOG=1 이면 whisper 로그를 stderr 로 흘린다 (디버그용)
void quietLog(enum ggml_log_level, const char* msg, void*) { if (std::getenv("YS_WHISPER_LOG")) fputs(msg, stderr); }

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

std::string Stt::defaultModelPath() { return modelPath(Lang::En); }
std::string Stt::modelUrl() { return modelUrl(Lang::En); }
std::string Stt::modelPath(Lang lang) {
    return paths::modelsDir() + (lang == Lang::Ja ? "/ggml-small.bin" : "/ggml-base.en.bin");
}
std::string Stt::modelUrl(Lang lang) {
    return std::string("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/") +
           (lang == Lang::Ja ? "ggml-small.bin" : "ggml-base.en.bin");
}
int Stt::modelSizeMB(Lang lang) { return lang == Lang::Ja ? 466 : 148; }

Stt::~Stt() {
    if (ctx_) whisper_free(ctx_);
}

bool Stt::load(const std::string& modelPath, std::string* err, Lang lang) {
    std::lock_guard<std::mutex> lock(m_);
    lang_ = lang;
    whisper_log_set(quietLog, nullptr);
    ggml_log_set(quietLog, nullptr);
    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = false;
    // 토큰 시각을 교차 주의 정렬(DTW) 로 구한다. 기본 휴리스틱은 짧은 클립에서 마지막 단어를 뒤의 무음 패딩에 놓거나
    // 첫 단어를 너무 길게 잡는 일이 잦아 리듬 · 강세 비교가 틀어진다. 모델이 프리셋과 맞지 않으면 whisper 가 스스로 끈다 (t_dtw = -1)
    cp.flash_attn = false;  // flash attention과 DTW 시각은 같이 쓸 수 없다 (켜져 있으면 whisper 가 DTW 를 끈다)
    cp.dtw_token_timestamps = true;
    cp.dtw_aheads_preset = lang == Lang::Ja ? WHISPER_AHEADS_SMALL : WHISPER_AHEADS_BASE_EN;
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
    p.language = langCode(lang_);
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
    const int clipMs = (int)((long long)pcm16k.size() * 1000 / kRate);
    // 단어 시작 = 첫 토큰의 DTW 시각 (없으면 휴리스틱 t0). 단어 끝 = 다음 단어의 시작 (마지막 단어는 휴리스틱 t1)
    struct Raw { std::string text; int start, endHint; int punctDtw = -1; };  // punctDtw: 뒤에 붙은 구두점 토큰의 DTW 시각 (≈ 단어가 끝나는 때)
    std::vector<Raw> raws;
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
            const int start = td.t_dtw >= 0 ? (int)td.t_dtw * 10 : t0;
            if (std::getenv("YS_DEBUG_WORDS")) fprintf(stderr, "[tok] '%s' t0=%d t1=%d dtw=%lld p=%.2f\n", t.c_str(), t0, t1, (long long)td.t_dtw, td.p);
            if (newWord) {
                std::string tt = trim(t);
                if (tt.empty()) continue;
                raws.push_back({tt, start, std::max(t1, start + 50)});
                segStart = false;
            } else if (!raws.empty()) {
                raws.back().text += t;
                raws.back().endHint = std::max(raws.back().endHint, t1);
                const std::string tt = trim(t);
                if (td.t_dtw >= 0 && !tt.empty() && !std::isalnum((unsigned char)tt[0]) && raws.back().punctDtw < 0 && start > raws.back().start)
                    raws.back().punctDtw = start;
            }
        }
    }
    for (size_t k = 0; k < raws.size(); ++k) {
        int start = std::min(raws[k].start, std::max(0, clipMs - 50));
        int end = k + 1 < raws.size() && raws[k + 1].start > start ? raws[k + 1].start : raws[k].endHint;
        if (raws[k].punctDtw > start) end = std::min(end, raws[k].punctDtw + 100);  // 구두점이 "말해진" 때가 단어 끝
        end = std::min(end, clipMs);  // 무음 패딩(2초 맞춤) 에 놓인 끝은 클립 끝으로
        words.push_back({raws[k].text, start, std::max(end, start + 50)});
    }

    // 단어 경계를 소리에 맞춘다: 경계 안쪽의 조용한 10 ms 칸을 잘라 낸다 (DTW 는 첫 단어를 0 ms 부터, 끝 단어를 클립 끝까지 잡는 버릇이 있다)
    const int nFr = clipMs / 10;
    if (nFr > 0 && !words.empty()) {
        std::vector<float> db(nFr);
        for (int f = 0; f < nFr; ++f) {
            double e = 0;
            const size_t a = (size_t)f * kRate / 100, b = std::min(pcm16k.size(), (size_t)(f + 1) * kRate / 100);
            for (size_t i = a; i < b; ++i) e += (double)pcm16k[i] * pcm16k[i];
            db[f] = 20.f * std::log10((float)std::sqrt(e / std::max<size_t>(1, b - a)) + 1e-6f);
        }
        std::vector<float> sorted = db;
        std::sort(sorted.begin(), sorted.end());
        // 조용함의 기준: 바닥(5 퍼센타일) + 10 dB 와 말소리 윗선(90 퍼센타일) − 28 dB 중 큰 쪽.
        // 바닥만 쓰면 녹음 앞의 방 소음이나 배경 음악이 모두 "소리" 가 돼 단어가 앞뒤로 늘어난다
        const float thr = std::max(sorted[sorted.size() / 20] + 10.f, sorted[sorted.size() * 9 / 10] - 28.f);
        if (std::getenv("YS_DEBUG_WORDS")) fprintf(stderr, "[trim] floor=%.1f p90=%.1f thr=%.1f dB\n", sorted[sorted.size() / 20], sorted[sorted.size() * 9 / 10], thr);
        int prevEnd = 0;  // 앞 단어의 끝 (프레임)
        for (auto& w : words) {
            int s = std::max(0, std::min(nFr - 1, w.startMs / 10)), e = std::max(s + 1, std::min(nFr, (w.endMs + 9) / 10));
            while (s < e - 5 && db[s] < thr) ++s;
            // 단어 안에 80 ms 이상 조용한 구간이 있으면 거기서 단어가 끝난 것이다 (다음 단어 시작까지 늘어난 끝을 되돌린다)
            for (int f = s + 5, run = 0; f < e; ++f) {
                run = db[f] < thr ? run + 1 : 0;
                if (run >= 8) { e = f - run + 1; break; }
            }
            while (e > s + 5 && db[e - 1] < thr) --e;
            // 앞 단어 끝과 이 단어 사이에 소리가 이어져 있으면 그 소리는 이 단어의 것이다 (첫 단어는 말소리 시작까지, 최대 300 ms)
            for (int back = 0; s > prevEnd && back < 30 && db[s - 1] >= thr; ++back) --s;
            w.startMs = s * 10;
            w.endMs = std::max(e * 10, w.startMs + 50);
            prevEnd = std::max(prevEnd, w.endMs / 10);
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

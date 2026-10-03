#pragma once
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "transcript.h"

struct whisper_context;

// whisper.cpp 래퍼. 입력은 항상 mono 16kHz float.
class Stt {
public:
    static constexpr int kRate = 16000;

    static std::string defaultModelPath();  // 영어 모델: <modelsDir>/ggml-base.en.bin
    static std::string modelUrl();
    // 언어별 모델. 영어는 base.en(148MB), 일본어는 다국어 small(466MB) — base 다국어는 일본어 인식이 많이 떨어진다.
    static std::string modelPath(Lang lang);
    static std::string modelUrl(Lang lang);
    static int modelSizeMB(Lang lang);

    Stt() = default;
    ~Stt();

    bool load(const std::string& modelPath, std::string* err, Lang lang = Lang::En);
    bool loaded() const { return ctx_ != nullptr; }
    Lang lang() const { return lang_; }

    // 단어 + 타임스탬프. progress 는 0~100.
    std::vector<Word> transcribe(const std::vector<float>& pcm16k,
                                 const std::function<void(int)>& progress = {},
                                 std::string* err = nullptr);

    // 텍스트만 (채점용 짧은 녹음).
    std::string transcribeText(const std::vector<float>& pcm16k, std::string* err = nullptr);

private:
    whisper_context* ctx_ = nullptr;
    Lang lang_ = Lang::En;
    std::mutex m_;  // whisper_context 는 동시에 하나의 스레드만 써야 한다
};

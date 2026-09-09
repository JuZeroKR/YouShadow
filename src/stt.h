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

    static std::string defaultModelPath();  // models/ggml-base.en.bin
    static std::string modelUrl();

    Stt() = default;
    ~Stt();

    bool load(const std::string& modelPath, std::string* err);
    bool loaded() const { return ctx_ != nullptr; }

    // 단어 + 타임스탬프. progress 는 0~100.
    std::vector<Word> transcribe(const std::vector<float>& pcm16k,
                                 const std::function<void(int)>& progress = {},
                                 std::string* err = nullptr);

    // 텍스트만 (채점용 짧은 녹음).
    std::string transcribeText(const std::vector<float>& pcm16k, std::string* err = nullptr);

private:
    whisper_context* ctx_ = nullptr;
    std::mutex m_;  // whisper_context 는 동시에 하나의 스레드만 써야 한다
};

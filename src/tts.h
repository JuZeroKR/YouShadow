#pragma once
#include <string>

// Windows SAPI 음성 합성. 영어 음성을 골라 단어/문장 발음을 들려준다 (오프라인).
class Tts {
public:
    Tts() = default;
    ~Tts();
    bool init();                       // 실패해도 앱은 동작 (available() false)
    bool available() const { return voice_ != nullptr; }
    const std::string& voiceName() const { return voiceName_; }

    // rate: -10(느림) ~ 10(빠름), 0 보통. 비동기로 말하고 이전 발화는 끊는다.
    void speak(const std::string& text, int rate = 0);
    void stop();
    void setVolume(int percent);       // 0~100

private:
    struct ISpVoice* voice_ = nullptr;
    std::string voiceName_;
    bool comInit_ = false;
};

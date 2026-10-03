#pragma once
#include <string>

#include "lang.h"

// 시스템 음성 합성 (Windows: SAPI, macOS: AVSpeechSynthesizer).
// 영어 · 일본어 음성을 골라 단어/문장 발음을 들려준다 (오프라인).
class Tts {
public:
    Tts() = default;
    ~Tts();
    bool init();                       // 실패해도 앱은 동작 (available() false)
    bool available() const { return voice_ != nullptr; }
    bool hasVoice(Lang lang) const;    // 그 언어 음성이 설치되어 있는지 (En / Ja / Ko)
    const std::string& voiceName(Lang lang = Lang::En) const { return lang == Lang::Ja ? voiceNameJa_ : lang == Lang::Ko ? voiceNameKo_ : voiceNameEn_; }

    // rate: -10(느림) ~ 10(빠름), 0 보통. 비동기로 말하고 이전 발화는 끊는다.
    // 그 언어 음성이 없으면 기본 음성으로 읽는다 (발음이 어색할 수 있다).
    void speak(const std::string& text, int rate = 0, Lang lang = Lang::En);
    void stop();
    void setVolume(int percent);       // 0~100

private:
#ifdef _WIN32
    struct ISpVoice* voice_ = nullptr;
    struct ISpObjectToken* tokEn_ = nullptr;
    struct ISpObjectToken* tokJa_ = nullptr;
    struct ISpObjectToken* tokKo_ = nullptr;
    Lang curLang_ = Lang::En;
#else
    void* voice_ = nullptr;  // macOS: 내부 구현 (tts_mac.mm)
#endif
    std::string voiceNameEn_, voiceNameJa_, voiceNameKo_;
    bool comInit_ = false;
};

// macOS TTS 구현 (AVSpeechSynthesizer). Windows 구현은 tts.cpp 에 있다.
#include "tts.h"

#import <AVFoundation/AVFoundation.h>

#include <algorithm>

namespace {

struct TtsImpl {
    AVSpeechSynthesizer* synth = nil;
    AVSpeechSynthesisVoice* voice = nil;
    float volume = 1.0f;
};

// 영어(en-US 우선) 음성을 찾는다
AVSpeechSynthesisVoice* findEnglishVoice() {
    if (AVSpeechSynthesisVoice* v = [AVSpeechSynthesisVoice voiceWithLanguage:@"en-US"]) return v;
    for (AVSpeechSynthesisVoice* v in [AVSpeechSynthesisVoice speechVoices]) {
        if ([v.language hasPrefix:@"en"]) return v;
    }
    return nil;
}

}  // namespace

Tts::~Tts() {
    if (voice_) {
        auto* p = (TtsImpl*)voice_;
        [p->synth stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
        [p->synth release];
        [p->voice release];
        delete p;
        voice_ = nullptr;
    }
}

bool Tts::init() {
    auto* p = new TtsImpl;
    p->synth = [[AVSpeechSynthesizer alloc] init];
    if (!p->synth) {
        delete p;
        return false;
    }
    p->voice = [findEnglishVoice() retain];
    if (p->voice && p->voice.name) voiceName_ = [p->voice.name UTF8String];
    voice_ = p;
    return true;
}

void Tts::speak(const std::string& text, int rate) {
    if (!voice_ || text.empty()) return;
    auto* p = (TtsImpl*)voice_;
    [p->synth stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
    NSString* s = [NSString stringWithUTF8String:text.c_str()];
    if (!s) return;
    AVSpeechUtterance* u = [AVSpeechUtterance speechUtteranceWithString:s];
    if (p->voice) u.voice = p->voice;
    // SAPI 의 -10~10 을 AVSpeech 의 0~1 로 맞춘다 (0 → 기본 0.5)
    u.rate = AVSpeechUtteranceDefaultSpeechRate + std::clamp(rate, -10, 10) * 0.04f;
    u.volume = p->volume;
    [p->synth speakUtterance:u];
}

void Tts::stop() {
    if (voice_) [((TtsImpl*)voice_)->synth stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
}

void Tts::setVolume(int percent) {
    if (voice_) ((TtsImpl*)voice_)->volume = std::clamp(percent, 0, 100) / 100.0f;
}

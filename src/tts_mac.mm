// macOS TTS 구현 (AVSpeechSynthesizer). Windows 구현은 tts.cpp 에 있다.
#include "tts.h"

#import <AVFoundation/AVFoundation.h>

#include <algorithm>

namespace {

struct TtsImpl {
    AVSpeechSynthesizer* synth = nil;
    AVSpeechSynthesisVoice* voiceEn = nil;
    AVSpeechSynthesisVoice* voiceJa = nil;
    AVSpeechSynthesisVoice* voiceKo = nil;
    float volume = 1.0f;
};

// 언어(en-US / ja-JP 우선, 없으면 같은 언어 아무 음성) 음성을 찾는다
AVSpeechSynthesisVoice* findVoice(NSString* preferred, NSString* prefix) {
    if (AVSpeechSynthesisVoice* v = [AVSpeechSynthesisVoice voiceWithLanguage:preferred]) return v;
    for (AVSpeechSynthesisVoice* v in [AVSpeechSynthesisVoice speechVoices]) {
        if ([v.language hasPrefix:prefix]) return v;
    }
    return nil;
}

}  // namespace

Tts::~Tts() {
    if (voice_) {
        auto* p = (TtsImpl*)voice_;
        [p->synth stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
        [p->synth release];
        [p->voiceEn release];
        [p->voiceJa release];
        [p->voiceKo release];
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
    p->voiceEn = [findVoice(@"en-US", @"en") retain];
    p->voiceJa = [findVoice(@"ja-JP", @"ja") retain];
    p->voiceKo = [findVoice(@"ko-KR", @"ko") retain];
    if (p->voiceEn && p->voiceEn.name) voiceNameEn_ = [p->voiceEn.name UTF8String];
    if (p->voiceJa && p->voiceJa.name) voiceNameJa_ = [p->voiceJa.name UTF8String];
    if (p->voiceKo && p->voiceKo.name) voiceNameKo_ = [p->voiceKo.name UTF8String];
    voice_ = p;
    return true;
}

bool Tts::hasVoice(Lang lang) const {
    if (!voice_) return false;
    auto* p = (TtsImpl*)voice_;
    return lang == Lang::Ja ? p->voiceJa != nil : lang == Lang::Ko ? p->voiceKo != nil : p->voiceEn != nil;
}

void Tts::speak(const std::string& text, int rate, Lang lang) {
    if (!voice_ || text.empty()) return;
    auto* p = (TtsImpl*)voice_;
    [p->synth stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
    NSString* s = [NSString stringWithUTF8String:text.c_str()];
    if (!s) return;
    AVSpeechUtterance* u = [AVSpeechUtterance speechUtteranceWithString:s];
    AVSpeechSynthesisVoice* v = lang == Lang::Ja ? p->voiceJa : lang == Lang::Ko ? p->voiceKo : p->voiceEn;
    if (!v) v = p->voiceEn ? p->voiceEn : p->voiceJa;
    if (v) u.voice = v;
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

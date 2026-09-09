#include "tts.h"

#include <windows.h>
#include <sapi.h>

#include <algorithm>

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const wchar_t* w) {
    if (!w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// 영어(en-US 우선, 없으면 아무 영어) 음성 토큰을 찾는다
ISpObjectToken* findEnglishVoice() {
    ISpObjectTokenCategory* cat = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(SpObjectTokenCategory), nullptr, CLSCTX_ALL, __uuidof(ISpObjectTokenCategory), (void**)&cat))) return nullptr;
    ISpObjectToken* found = nullptr;
    if (SUCCEEDED(cat->SetId(SPCAT_VOICES, FALSE))) {
        const wchar_t* prefs[] = {L"Language=409", L"Language=809", L"Language=C09"};  // en-US, en-GB, en-AU
        for (const wchar_t* attr : prefs) {
            IEnumSpObjectTokens* en = nullptr;
            if (SUCCEEDED(cat->EnumTokens(attr, nullptr, &en)) && en) {
                ISpObjectToken* tok = nullptr;
                if (en->Next(1, &tok, nullptr) == S_OK && tok) found = tok;
                en->Release();
            }
            if (found) break;
        }
    }
    cat->Release();
    return found;
}

}  // namespace

Tts::~Tts() {
    if (voice_) { voice_->Release(); voice_ = nullptr; }
    if (comInit_) CoUninitialize();
}

bool Tts::init() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    comInit_ = SUCCEEDED(hr);  // RPC_E_CHANGED_MODE 면 이미 다른 모드로 초기화된 것: 그대로 사용
    if (FAILED(CoCreateInstance(__uuidof(SpVoice), nullptr, CLSCTX_ALL, __uuidof(ISpVoice), (void**)&voice_))) {
        voice_ = nullptr;
        return false;
    }
    if (ISpObjectToken* tok = findEnglishVoice()) {
        voice_->SetVoice(tok);
        tok->Release();
    }
    ISpObjectToken* cur = nullptr;
    if (SUCCEEDED(voice_->GetVoice(&cur)) && cur) {
        wchar_t* desc = nullptr;
        if (SUCCEEDED(cur->GetStringValue(nullptr, &desc)) && desc) {
            voiceName_ = narrow(desc);
            CoTaskMemFree(desc);
        }
        cur->Release();
    }
    return true;
}

void Tts::speak(const std::string& text, int rate) {
    if (!voice_ || text.empty()) return;
    voice_->SetRate(std::clamp(rate, -10, 10));
    voice_->Speak(widen(text).c_str(), SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML, nullptr);
}

void Tts::stop() {
    if (voice_) voice_->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
}

void Tts::setVolume(int percent) {
    if (voice_) voice_->SetVolume((USHORT)std::clamp(percent, 0, 100));
}

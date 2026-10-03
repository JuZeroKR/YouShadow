// Windows SAPI 구현. macOS 구현은 tts_mac.mm 에 있다.
#ifdef _WIN32

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

// 언어 ID 목록(우선순위 순) 으로 음성 토큰을 찾는다.
// 기본 SAPI 목록에 없으면 Windows 설정의 "음성 추가" 로 설치되는 OneCore 음성 목록도 뒤진다
// (SAPI 가 기본으로는 열거하지 않지만 토큰으로 SetVoice 는 된다).
ISpObjectToken* findVoice(std::initializer_list<const wchar_t*> prefs) {
    const wchar_t* categories[] = {
        SPCAT_VOICES,
        L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices",
    };
    for (const wchar_t* category : categories) {
        ISpObjectTokenCategory* cat = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(SpObjectTokenCategory), nullptr, CLSCTX_ALL, __uuidof(ISpObjectTokenCategory), (void**)&cat))) return nullptr;
        ISpObjectToken* found = nullptr;
        if (SUCCEEDED(cat->SetId(category, FALSE))) {
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
        if (found) return found;
    }
    return nullptr;
}

std::string tokenName(ISpObjectToken* tok) {
    if (!tok) return "";
    wchar_t* desc = nullptr;
    std::string name;
    if (SUCCEEDED(tok->GetStringValue(nullptr, &desc)) && desc) {
        name = narrow(desc);
        CoTaskMemFree(desc);
    }
    return name;
}

}  // namespace

Tts::~Tts() {
    if (tokEn_) { tokEn_->Release(); tokEn_ = nullptr; }
    if (tokJa_) { tokJa_->Release(); tokJa_ = nullptr; }
    if (tokKo_) { tokKo_->Release(); tokKo_ = nullptr; }
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
    tokEn_ = findVoice({L"Language=409", L"Language=809", L"Language=C09"});  // en-US, en-GB, en-AU
    tokJa_ = findVoice({L"Language=411"});                                   // ja-JP (일본어 언어 팩의 Haruka/Ayumi/Ichiro)
    tokKo_ = findVoice({L"Language=412"});                                   // ko-KR (Heami) — 일본어 음성이 없을 때 발음 표기를 읽는 용도
    voiceNameEn_ = tokenName(tokEn_);
    voiceNameJa_ = tokenName(tokJa_);
    voiceNameKo_ = tokenName(tokKo_);
    if (tokEn_) voice_->SetVoice(tokEn_);
    if (voiceNameEn_.empty()) {
        ISpObjectToken* cur = nullptr;
        if (SUCCEEDED(voice_->GetVoice(&cur)) && cur) { voiceNameEn_ = tokenName(cur); cur->Release(); }
    }
    curLang_ = Lang::En;
    return true;
}

bool Tts::hasVoice(Lang lang) const {
    return voice_ && (lang == Lang::Ja ? tokJa_ != nullptr : lang == Lang::Ko ? tokKo_ != nullptr : tokEn_ != nullptr);
}

void Tts::speak(const std::string& text, int rate, Lang lang) {
    if (!voice_ || text.empty()) return;
    if (lang != curLang_) {
        ISpObjectToken* tok = lang == Lang::Ja ? tokJa_ : lang == Lang::Ko ? tokKo_ : tokEn_;
        if (tok) { voice_->SetVoice(tok); curLang_ = lang; }
    }
    voice_->SetRate(std::clamp(rate, -10, 10));
    voice_->Speak(widen(text).c_str(), SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML, nullptr);
}

void Tts::stop() {
    if (voice_) voice_->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
}

void Tts::setVolume(int percent) {
    if (voice_) voice_->SetVolume((USHORT)std::clamp(percent, 0, 100));
}

#endif  // _WIN32

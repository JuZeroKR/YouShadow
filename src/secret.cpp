#include "secret.h"

#include <windows.h>
#include <wincrypt.h>

#include <vector>

namespace {

std::string b64encode(const BYTE* data, DWORD n) {
    DWORD len = 0;
    CryptBinaryToStringA(data, n, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
    std::string out(len, '\0');
    CryptBinaryToStringA(data, n, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &out[0], &len);
    while (!out.empty() && (out.back() == '\0' || out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

std::vector<BYTE> b64decode(const std::string& s) {
    DWORD len = 0;
    if (!CryptStringToBinaryA(s.c_str(), (DWORD)s.size(), CRYPT_STRING_BASE64, nullptr, &len, nullptr, nullptr)) return {};
    std::vector<BYTE> out(len);
    CryptStringToBinaryA(s.c_str(), (DWORD)s.size(), CRYPT_STRING_BASE64, out.data(), &len, nullptr, nullptr);
    out.resize(len);
    return out;
}

}  // namespace

namespace secret {

std::string protect(const std::string& plain) {
    if (plain.empty()) return "";
    DATA_BLOB in{(DWORD)plain.size(), (BYTE*)plain.data()};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"YouShadow", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return "";
    std::string s = b64encode(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return s;
}

std::string unprotect(const std::string& encoded) {
    if (encoded.empty()) return "";
    std::vector<BYTE> bytes = b64decode(encoded);
    if (bytes.empty()) return "";
    DATA_BLOB in{(DWORD)bytes.size(), bytes.data()};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return "";
    std::string s((const char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return s;
}

}  // namespace secret

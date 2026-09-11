#include "secret.h"

#ifdef _WIN32

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

#else  // macOS: Keychain 의 마스터 키 + CommonCrypto AES-256-CBC

#include <CommonCrypto/CommonCryptor.h>
#include <Security/Security.h>

#include <cstdint>
#include <vector>

namespace {

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64encode(const uint8_t* data, size_t n) {
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = data[i] << 16;
        if (i + 1 < n) v |= data[i + 1] << 8;
        if (i + 2 < n) v |= data[i + 2];
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += (i + 1 < n) ? kB64[(v >> 6) & 63] : '=';
        out += (i + 2 < n) ? kB64[v & 63] : '=';
    }
    return out;
}

std::vector<uint8_t> b64decode(const std::string& s) {
    int8_t rev[256];
    for (int i = 0; i < 256; ++i) rev[i] = -1;
    for (int i = 0; i < 64; ++i) rev[(uint8_t)kB64[i]] = (int8_t)i;
    std::vector<uint8_t> out;
    uint32_t v = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        int8_t d = rev[(uint8_t)c];
        if (d < 0) return {};
        v = (v << 6) | (uint32_t)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)(v >> bits));
        }
    }
    return out;
}

// Keychain 에서 YouShadow 마스터 키(32바이트)를 읽고, 없으면 만들어 저장한다.
bool masterKey(uint8_t key[32]) {
    CFStringRef service = CFSTR("YouShadow");
    CFStringRef account = CFSTR("master-key");

    const void* qk[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecReturnData};
    const void* qv[] = {kSecClassGenericPassword, service, account, kCFBooleanTrue};
    CFDictionaryRef query = CFDictionaryCreate(nullptr, qk, qv, 4, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFTypeRef result = nullptr;
    OSStatus st = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (st == errSecSuccess && result) {
        CFDataRef data = (CFDataRef)result;
        bool ok = CFDataGetLength(data) == 32;
        if (ok) CFDataGetBytes(data, CFRangeMake(0, 32), key);
        CFRelease(result);
        if (ok) return true;
    }

    if (SecRandomCopyBytes(kSecRandomDefault, 32, key) != errSecSuccess) return false;
    CFDataRef data = CFDataCreate(nullptr, key, 32);
    const void* ak[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData};
    const void* av[] = {kSecClassGenericPassword, service, account, data};
    CFDictionaryRef add = CFDictionaryCreate(nullptr, ak, av, 4, &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
    st = SecItemAdd(add, nullptr);
    CFRelease(add);
    CFRelease(data);
    return st == errSecSuccess || st == errSecDuplicateItem;
}

}  // namespace

namespace secret {

std::string protect(const std::string& plain) {
    if (plain.empty()) return "";
    uint8_t key[32];
    if (!masterKey(key)) return "";
    uint8_t iv[16];
    if (SecRandomCopyBytes(kSecRandomDefault, 16, iv) != errSecSuccess) return "";

    std::vector<uint8_t> out(16 + plain.size() + kCCBlockSizeAES128);
    size_t written = 0;
    CCCryptorStatus st = CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, key, 32, iv,
                                 plain.data(), plain.size(), out.data() + 16, out.size() - 16, &written);
    if (st != kCCSuccess) return "";
    for (int i = 0; i < 16; ++i) out[i] = iv[i];
    return b64encode(out.data(), 16 + written);
}

std::string unprotect(const std::string& encoded) {
    if (encoded.empty()) return "";
    std::vector<uint8_t> bytes = b64decode(encoded);
    if (bytes.size() <= 16) return "";
    uint8_t key[32];
    if (!masterKey(key)) return "";

    std::vector<uint8_t> out(bytes.size() - 16);
    size_t written = 0;
    CCCryptorStatus st = CCCrypt(kCCDecrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, key, 32, bytes.data(),
                                 bytes.data() + 16, bytes.size() - 16, out.data(), out.size(), &written);
    if (st != kCCSuccess) return "";
    return std::string((const char*)out.data(), written);
}

}  // namespace secret

#endif

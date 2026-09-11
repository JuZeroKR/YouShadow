#pragma once
#include <string>

// 현재 사용자만 풀 수 있게 암호화한다 (API 키 저장용). 결과는 base64 문자열.
// Windows: DPAPI. macOS: Keychain 에 보관한 마스터 키로 AES-256-CBC.
namespace secret {
std::string protect(const std::string& plain);
std::string unprotect(const std::string& encoded);  // 실패하면 빈 문자열
}

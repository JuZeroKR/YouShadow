#pragma once
#include <string>

// Windows DPAPI 로 현재 사용자만 풀 수 있게 암호화한다 (API 키 저장용). 결과는 base64 문자열.
namespace secret {
std::string protect(const std::string& plain);
std::string unprotect(const std::string& encoded);  // 실패하면 빈 문자열
}

#pragma once
#include <string>
#include <utility>
#include <vector>

// 작은 HTTPS 클라이언트 (Windows: WinHTTP, macOS: 시스템 libcurl — 외부 의존성 없음)
struct HttpResponse {
    int status = 0;
    std::string body;
};

// method: "GET" / "POST". headers: {"Name", "Value"}. 실패하면 status 0 과 err.
HttpResponse httpRequest(const std::string& method, const std::string& url,
                         const std::vector<std::pair<std::string, std::string>>& headers,
                         const std::string& body, std::string* err, int timeoutSec = 120);

#include "http.h"

#include <windows.h>
#include <winhttp.h>

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
};

}  // namespace

HttpResponse httpRequest(const std::string& method, const std::string& url,
                         const std::vector<std::pair<std::string, std::string>>& headers,
                         const std::string& body, std::string* err, int timeoutSec) {
    HttpResponse res;
    auto fail = [&](const std::string& what) {
        if (err) *err = what + " (code " + std::to_string(GetLastError()) + ")";
        return res;
    };

    // URL 분해
    std::wstring wurl = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[4096];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 4096;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return fail("URL 해석 실패");

    Handle session;
    session.h = WinHttpOpen(L"YouShadow/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) return fail("WinHttpOpen 실패");
    WinHttpSetTimeouts(session.h, 15000, 15000, timeoutSec * 1000, timeoutSec * 1000);

    Handle conn;
    conn.h = WinHttpConnect(session.h, host, uc.nPort, 0);
    if (!conn.h) return fail("연결 실패");

    Handle req;
    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    req.h = WinHttpOpenRequest(conn.h, widen(method).c_str(), path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req.h) return fail("요청 생성 실패");

    std::wstring hdr;
    for (const auto& [k, v] : headers) hdr += widen(k) + L": " + widen(v) + L"\r\n";
    if (!hdr.empty()) WinHttpAddRequestHeaders(req.h, hdr.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(),
                            (DWORD)body.size(), 0)) {
        return fail("요청 전송 실패 (인터넷 연결 확인)");
    }
    if (!WinHttpReceiveResponse(req.h, nullptr)) return fail("응답 수신 실패");

    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    res.status = (int)status;

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(req.h, &chunk[0], avail, &got)) break;
        res.body.append(chunk, 0, got);
    }
    return res;
}

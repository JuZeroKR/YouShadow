#include "http.h"

#ifdef _WIN32

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

#else  // POSIX: 시스템 libcurl

#include <curl/curl.h>

#include <mutex>

namespace {

size_t writeBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

}  // namespace

HttpResponse httpRequest(const std::string& method, const std::string& url,
                         const std::vector<std::pair<std::string, std::string>>& headers,
                         const std::string& body, std::string* err, int timeoutSec) {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    HttpResponse res;
    CURL* curl = curl_easy_init();
    if (!curl) {
        if (err) *err = "curl 초기화 실패";
        return res;
    }

    curl_slist* hdrs = nullptr;
    for (const auto& [k, v] : headers) hdrs = curl_slist_append(hdrs, (k + ": " + v).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    if (!body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    }
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "YouShadow/1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeoutSec);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        if (err) *err = std::string("요청 실패: ") + curl_easy_strerror(rc) + " (인터넷 연결 확인)";
        res.body.clear();
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        res.status = (int)status;
    }
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return res;
}

#endif

#include "paths.h"

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::string exeDirImpl() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    fs::path p(std::wstring(buf, n));
    return narrow(p.parent_path().wstring());
}

std::string localAppData() {
    wchar_t* v = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&v, &len, L"LOCALAPPDATA") == 0 && v) {
        std::string s = narrow(v);
        free(v);
        return s;
    }
    return exeDirImpl();
}

std::string root() {
    static const std::string r = paths::devMode() ? std::string(".") : localAppData() + "\\YouShadow";
    return r;
}

}  // namespace

namespace paths {

std::string exeDir() { return exeDirImpl(); }

bool devMode() {
    static const bool dev = fs::exists("CMakeLists.txt") && fs::exists("src");
    return dev;
}

std::string dataDir() { return root() + "\\data"; }
std::string modelsDir() { return root() + "\\models"; }
std::string logDir() { return root() + "\\logs"; }

void setup() {
    fs::create_directories(dataDir());
    fs::create_directories(modelsDir());
    fs::create_directories(logDir());

    // 동봉한 도구(bin\yt-dlp.exe, ffmpeg.exe, deno.exe)를 PATH 앞에 붙인다
    std::wstring bin = widen(exeDir()) + L"\\bin";
    if (fs::exists(bin)) {
        DWORD n = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        std::wstring path(n ? n - 1 : 0, L'\0');
        if (n) GetEnvironmentVariableW(L"PATH", &path[0], n);
        SetEnvironmentVariableW(L"PATH", (bin + L";" + path).c_str());
    }
}

int runCommand(const std::string& cmd, const std::string& logPath) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE log = CreateFileW(widen(logPath).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log != INVALID_HANDLE_VALUE) {
        std::string header = "\r\n> " + cmd + "\r\n";
        DWORD written = 0;
        WriteFile(log, header.data(), (DWORD)header.size(), &written, nullptr);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr;
    si.hStdOutput = log != INVALID_HANDLE_VALUE ? log : nullptr;
    si.hStdError = log != INVALID_HANDLE_VALUE ? log : nullptr;
    PROCESS_INFORMATION pi{};

    std::wstring line = L"cmd.exe /d /s /c \"" + widen(cmd) + L"\"";
    std::vector<wchar_t> buf(line.begin(), line.end());
    buf.push_back(L'\0');

    int code = -1;
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD ec = 0;
        GetExitCodeProcess(pi.hProcess, &ec);
        code = (int)ec;
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    return code;
}

}  // namespace paths

#include "paths.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#include <mach-o/dyld.h>
#else
extern char** environ;
#endif
#endif

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#ifdef _WIN32

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

#else  // POSIX (macOS)

namespace {

std::string exeDirImpl() {
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return ".";
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::path(buf.data()), ec);
    if (ec) p = fs::path(buf.data());
    return p.parent_path().string();
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? "." : p.parent_path().string();
#endif
}

std::string root() {
    static const std::string r = [] {
        if (paths::devMode()) return std::string(".");
        const char* home = std::getenv("HOME");
#ifdef __APPLE__
        if (home) return std::string(home) + "/Library/Application Support/YouShadow";
#else
        if (home) return std::string(home) + "/.local/share/YouShadow";
#endif
        return exeDirImpl();
    }();
    return r;
}

}  // namespace

#endif

namespace paths {

std::string exeDir() { return exeDirImpl(); }

bool devMode() {
    static const bool dev = fs::exists("CMakeLists.txt") && fs::exists("src");
    return dev;
}

#ifdef _WIN32
std::string dataDir() { return root() + "\\data"; }
std::string modelsDir() { return root() + "\\models"; }
std::string logDir() { return root() + "\\logs"; }
#else
std::string dataDir() { return root() + "/data"; }
std::string modelsDir() { return root() + "/models"; }
std::string logDir() { return root() + "/logs"; }
#endif

void setup() {
    fs::create_directories(dataDir());
    fs::create_directories(modelsDir());
    fs::create_directories(logDir());

#ifdef _WIN32
    // 동봉한 도구(bin\yt-dlp.exe, ffmpeg.exe, deno.exe)를 PATH 앞에 붙인다
    std::wstring bin = widen(exeDir()) + L"\\bin";
    if (fs::exists(bin)) {
        DWORD n = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        std::wstring path(n ? n - 1 : 0, L'\0');
        if (n) GetEnvironmentVariableW(L"PATH", &path[0], n);
        SetEnvironmentVariableW(L"PATH", (bin + L";" + path).c_str());
    }
#else
    // 동봉한 도구(bin/)와 Homebrew 경로를 PATH 앞에 붙인다.
    // Finder 에서 실행하면 셸 프로필이 적용되지 않아 /opt/homebrew/bin 이 PATH 에 없다.
    std::string add;
    std::string bin = exeDir() + "/bin";
    if (fs::exists(bin)) add += bin + ":";
    const char* cur = std::getenv("PATH");
    std::string path = cur ? cur : "";
    for (const char* extra : {"/opt/homebrew/bin", "/usr/local/bin"}) {
        if (path.find(extra) == std::string::npos && fs::exists(extra)) add += std::string(extra) + ":";
    }
    if (!add.empty()) setenv("PATH", (add + path).c_str(), 1);
#endif
}

#ifdef _WIN32
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
#else
int runCommand(const std::string& cmd, const std::string& logPath) {
    int log = open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log >= 0) {
        std::string header = "\n> " + cmd + "\n";
        (void)!write(log, header.data(), header.size());
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (log >= 0) {
        posix_spawn_file_actions_adddup2(&fa, log, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&fa, log, STDERR_FILENO);
    }

#ifdef __APPLE__
    char** envp = *_NSGetEnviron();
#else
    char** envp = environ;
#endif
    const char* argv[] = {"/bin/sh", "-c", cmd.c_str(), nullptr};
    pid_t pid = 0;
    int code = -1;
    if (posix_spawn(&pid, "/bin/sh", &fa, nullptr, const_cast<char**>(argv), envp) == 0) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        if (WIFEXITED(status)) code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) code = 128 + WTERMSIG(status);
    }
    posix_spawn_file_actions_destroy(&fa);
    if (log >= 0) close(log);
    return code;
}
#endif

}  // namespace paths

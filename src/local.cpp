#include "local.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "paths.h"
#include "subtitle.h"
#include "youtube.h"

namespace fs = std::filesystem;

namespace local {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string u8(const fs::path& p) { return p.u8string(); }

// dir 안의 subtitle.* 를 찾는다
std::string existingSubtitle(const std::string& dir) {
    if (!fs::exists(fs::u8path(dir))) return "";
    for (const auto& e : fs::directory_iterator(fs::u8path(dir))) {
        if (e.path().stem() == "subtitle" && subtitle::isSubtitleExt(u8(e.path().extension()))) return u8(e.path());
    }
    return "";
}

void copySubtitle(const std::string& subtitlePath, const std::string& dir) {
    fs::path src = fs::u8path(subtitlePath);
    if (!fs::exists(src)) throw std::runtime_error("자막 파일이 없습니다: " + subtitlePath);
    // 기존 subtitle.* 제거
    for (const auto& e : fs::directory_iterator(fs::u8path(dir)))
        if (e.path().stem() == "subtitle") fs::remove(e.path());
    fs::path dst = fs::u8path(dir) / ("subtitle" + lower(u8(src.extension())));
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
}

// 영상 안에 든 자막 트랙(학습 언어 우선) 을 srt 로 뽑는다. 없으면 빈 문자열.
std::string extractEmbedded(const std::string& videoPath, const std::string& dir, Lang lang) {
    const std::string out = dir + "/subtitle.srt";
    const std::string log = yt::logPath();
    const char* mapsEn[] = {"0:s:m:language:eng", "0:s:m:language:en", "0:s:0"};
    const char* mapsJa[] = {"0:s:m:language:jpn", "0:s:m:language:ja", "0:s:0"};
    for (const char* map : (lang == Lang::Ja ? mapsJa : mapsEn)) {
        std::string cmd = "ffmpeg -y -loglevel error -i \"" + videoPath + "\" -map " + map + " -c:s srt \"" + out + "\"";
        int rc = paths::runCommand(cmd, log);
        std::error_code ec;
        if (rc == 0 && fs::exists(fs::u8path(out), ec) && fs::file_size(fs::u8path(out), ec) > 0) return out;
        fs::remove(fs::u8path(out), ec);
    }
    return "";
}

void extractAudio(const std::string& videoPath, const std::string& audioPath) {
    if (fs::exists(fs::u8path(audioPath))) return;
    std::string cmd = "ffmpeg -y -loglevel error -i \"" + videoPath + "\" -vn -ac 1 -ar 16000 -f wav \"" + audioPath + "\"";
    int rc = paths::runCommand(cmd, yt::logPath());
    if (rc != 0 || !fs::exists(fs::u8path(audioPath))) {
        throw std::runtime_error("ffmpeg 로 오디오를 뽑지 못했습니다 (exit code " + std::to_string(rc) + "). 로그: " + yt::logPath());
    }
}

std::string readSource(const std::string& dir) {
    std::ifstream in(fs::u8path(dir + "/source.txt"), std::ios::binary);
    std::string s;
    std::getline(in, s);
    if (!s.empty() && s.back() == '\r') s.pop_back();
    return s;
}

}  // namespace

bool isVideoExt(const std::string& ext) {
    std::string e = lower(ext);
    return e == ".mkv" || e == ".mp4" || e == ".avi" || e == ".mov" || e == ".webm" || e == ".m4v" ||
           e == ".ts" || e == ".wmv" || e == ".flv" || e == ".mpg" || e == ".mpeg";
}

bool isLocalId(const std::string& id) { return id.rfind("local_", 0) == 0; }

std::string makeId(const std::string& videoPath) {
    // FNV-1a 64bit over the absolute path (UTF-8, 소문자 드라이브 문자 차이 흡수)
    std::error_code ec;
    fs::path abs = fs::absolute(fs::u8path(videoPath), ec);
    std::string key = lower(u8(abs.lexically_normal()));
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : key) { h ^= c; h *= 1099511628211ULL; }
    char buf[32];
    snprintf(buf, sizeof buf, "local_%012llx", (unsigned long long)(h & 0xFFFFFFFFFFFFULL));
    return buf;
}

std::string findSiblingSubtitle(const std::string& videoPath, Lang lang) {
    fs::path v = fs::u8path(videoPath);
    std::error_code ec;
    if (!fs::exists(v, ec)) return "";
    const std::string stem = lower(u8(v.stem()));
    std::string best;
    int bestScore = -1;
    for (const auto& e : fs::directory_iterator(v.parent_path(), ec)) {
        if (!e.is_regular_file(ec)) continue;
        const std::string ext = u8(e.path().extension());
        if (!subtitle::isSubtitleExt(ext)) continue;
        const std::string name = lower(u8(e.path().stem()));  // "movie", "movie.en", "movie.eng", "movie.ja"
        if (name != stem && name.rfind(stem + ".", 0) != 0) continue;
        const std::string tag = name == stem ? "" : name.substr(stem.size());  // ".en" 등
        const bool wantJa = lang == Lang::Ja;
        const bool isEn = tag.find(".en") != std::string::npos;
        const bool isJa = tag.find(".ja") != std::string::npos || tag.find(".jp") != std::string::npos;
        const bool isKo = tag.find(".ko") != std::string::npos;
        int score = 0;
        if (tag.empty()) score = 2;                       // 같은 이름 (언어 표시 없음)
        else if (wantJa ? isJa : isEn) score = 3;         // 학습 언어 표시
        else if (isKo || (wantJa ? isEn : isJa)) score = 0;  // 다른 언어
        else score = 1;
        if (score > bestScore) { bestScore = score; best = u8(e.path()); }
    }
    return best;
}

Prepared prepare(const std::string& videoPath, const std::string& subtitlePath, const std::string& dir, Lang lang) {
    fs::path v = fs::u8path(videoPath);
    std::error_code ec;
    if (!fs::exists(v, ec)) throw std::runtime_error("영상 파일이 없습니다: " + videoPath);
    fs::create_directories(fs::u8path(dir));

    Prepared p;
    p.videoPath = u8(fs::absolute(v));
    p.title = u8(v.stem());
    {
        std::ofstream out(fs::u8path(dir + "/source.txt"), std::ios::binary);
        out << p.videoPath << "\n";
    }
    {
        std::ofstream out(fs::u8path(dir + "/title.txt"), std::ios::binary);
        out << p.title << "\n";
    }
    if (!subtitlePath.empty()) copySubtitle(subtitlePath, dir);
    p.subtitlePath = existingSubtitle(dir);
    if (p.subtitlePath.empty()) p.subtitlePath = extractEmbedded(p.videoPath, dir, lang);

    p.audioPath = dir + "/audio.wav";
    extractAudio(p.videoPath, p.audioPath);
    return p;
}

Prepared reopen(const std::string& dir, Lang lang) {
    std::string src = readSource(dir);
    if (src.empty()) throw std::runtime_error("등록된 영상 파일 경로를 찾을 수 없습니다 (source.txt 없음)");
    std::error_code ec;
    if (!fs::exists(fs::u8path(src), ec)) {
        throw std::runtime_error("영상 파일이 원래 위치에 없습니다:\n" + src + "\n파일을 옮겼다면 다시 [파일 열기] 로 불러오세요");
    }
    Prepared p;
    p.videoPath = src;
    p.title = u8(fs::u8path(src).stem());
    p.subtitlePath = existingSubtitle(dir);
    if (p.subtitlePath.empty()) p.subtitlePath = extractEmbedded(p.videoPath, dir, lang);
    p.audioPath = dir + "/audio.wav";
    extractAudio(p.videoPath, p.audioPath);
    return p;
}

void replaceSubtitle(const std::string& subtitlePath, const std::string& dir) {
    copySubtitle(subtitlePath, dir);
    std::error_code ec;
    fs::remove(fs::u8path(dir + "/segments.json"), ec);
}

}  // namespace local

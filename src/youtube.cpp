#include "youtube.h"

#include <filesystem>
#include <fstream>
#include <regex>

#include "paths.h"

namespace fs = std::filesystem;

namespace yt {

std::optional<std::string> extractVideoId(const std::string& input) {
    static const std::regex patterns[] = {
        std::regex(R"([?&]v=([A-Za-z0-9_-]{11}))"),
        std::regex(R"(youtu\.be/([A-Za-z0-9_-]{11}))"),
        std::regex(R"(/shorts/([A-Za-z0-9_-]{11}))"),
        std::regex(R"(/embed/([A-Za-z0-9_-]{11}))"),
        std::regex(R"(/live/([A-Za-z0-9_-]{11}))"),
        std::regex(R"(^([A-Za-z0-9_-]{11})$)"),
    };
    for (const auto& re : patterns) {
        std::smatch m;
        if (std::regex_search(input, m, re)) return m[1].str();
    }
    return std::nullopt;
}

static std::string findSubtitle(const std::string& dir, Lang lang) {
    // video.en.json3 / video.en-orig.json3 / (구버전) audio.en.json3 등을 찾는다. 일본어는 .ja.*
    const std::string code = langCode(lang);
    const std::string manualSuffix = "." + code + ".json3";
    std::string best;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() != ".json3") continue;
        const auto name = e.path().filename().string();
        if (name.find("." + code) == std::string::npos) continue;  // 다른 언어 자막은 건너뛴다
        // 수동 자막(.en.json3)이 있으면 자동 자막(en-orig)보다 우선한다.
        const bool manual = name.size() > manualSuffix.size() &&
                            name.compare(name.size() - manualSuffix.size(), manualSuffix.size(), manualSuffix) == 0;
        if (best.empty() || manual) best = e.path().string();
    }
    return best;
}

static std::string readTitle(const std::string& dir) {
    std::ifstream in(dir + "/title.txt");
    std::string title;
    std::getline(in, title);
    return title;
}

std::string logPath() { return paths::logDir() + "/tools.log"; }

DownloadResult download(const std::string& videoId, const std::string& dir, Lang lang) {
    fs::create_directories(dir);
    const std::string video = dir + "/video.mp4";
    const std::string audio = dir + "/audio.wav";
    const std::string log = logPath();
    const char* subLangs = lang == Lang::Ja ? "ja,ja-orig,ja-JP" : "en,en-orig,en-US,en-GB";

    // 영상은 있는데 이 언어의 자막이 없으면 자막만 다시 받는다
    if (fs::exists(video) && findSubtitle(dir, lang).empty()) {
        std::string cmd =
            "yt-dlp --no-playlist -i --skip-download "
            "--write-subs --write-auto-subs --sub-langs \"" + std::string(subLangs) + "\" --sub-format json3 "
            "--sleep-subtitles 2 "
            "-o \"" + dir + "/video.%(ext)s\" "
            "\"https://www.youtube.com/watch?v=" + videoId + "\"";
        paths::runCommand(cmd, log);
    }

    if (!fs::exists(video)) {
        // -i: 자막 다운로드가 실패해도(유튜브가 자막 요청을 429 로 막는 경우가 잦다) 영상은 받는다.
        // 자막이 없으면 호출 측에서 whisper 로 대본을 만든다.
        std::string cmd =
            "yt-dlp --no-playlist -i "
            "-f \"bv*[height<=720][ext=mp4]+ba[ext=m4a]/b[height<=720][ext=mp4]/b\" "
            "--merge-output-format mp4 "
            "--write-subs --write-auto-subs --sub-langs \"" + std::string(subLangs) + "\" --sub-format json3 "
            "--sleep-subtitles 2 "
            "--print-to-file \"%(title)s\" \"" + dir + "/title.txt\" "
            "-o \"" + dir + "/video.%(ext)s\" "
            "\"https://www.youtube.com/watch?v=" + videoId + "\"";
        int rc = paths::runCommand(cmd, log);
        if (!fs::exists(video)) {
            throw std::runtime_error(
                "yt-dlp 로 영상을 받지 못했습니다 (exit code " + std::to_string(rc) +
                "). 로그: " + log + "\n비공개/연령 제한 영상이거나 yt-dlp 업데이트가 필요할 수 있습니다");
        }
    }

    if (!fs::exists(audio)) {
        std::string cmd = "ffmpeg -y -loglevel error -i \"" + video +
                          "\" -vn -ac 1 -ar 16000 -f wav \"" + audio + "\"";
        int rc = paths::runCommand(cmd, log);
        if (rc != 0 || !fs::exists(audio)) {
            throw std::runtime_error("ffmpeg 실패 (exit code " + std::to_string(rc) + "). 로그: " + log);
        }
    }

    DownloadResult r;
    r.videoPath = video;
    r.audioPath = audio;
    r.subtitlePath = findSubtitle(dir, lang);
    r.title = readTitle(dir);
    return r;
}

}  // namespace yt

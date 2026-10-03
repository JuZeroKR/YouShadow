#pragma once
#include <string>
#include <optional>

#include "lang.h"

namespace yt {

// URL 또는 11자리 ID에서 유튜브 영상 ID를 뽑는다. 실패하면 nullopt.
std::optional<std::string> extractVideoId(const std::string& input);

struct DownloadResult {
    std::string videoPath;     // mp4 영상 (720p 이하)
    std::string audioPath;     // mono 48kHz wav (영상에서 추출)
    std::string subtitlePath;  // json3 자막 (없으면 빈 문자열)
    std::string title;
};

// yt-dlp로 영상과 영어 자막을 dir 아래에 받고 ffmpeg로 wav를 뽑는다.
// 이미 있으면 다시 받지 않는다.
DownloadResult download(const std::string& videoId, const std::string& dir, Lang lang = Lang::En);

// 외부 도구(yt-dlp, ffmpeg, curl) 출력이 기록되는 로그 파일
std::string logPath();

}  // namespace yt

#pragma once
#include <string>

#include "lang.h"

// 내 PC 의 영상 파일(mkv, mp4 …) 을 유튜브 영상처럼 등록한다.
// 영상은 복사하지 않고 원본 경로를 data/<id>/source.txt 에 적어 두며, 자막은 data/<id>/subtitle.<ext> 로 복사한다.
namespace local {

bool isVideoExt(const std::string& ext);  // .mkv .mp4 .avi .mov .webm .m4v .ts .wmv .flv (대소문자 무시)
bool isLocalId(const std::string& id);    // "local_" 로 시작

// 영상 파일 경로(UTF-8) 로부터 안정적인 ID 를 만든다 ("local_" + 해시)
std::string makeId(const std::string& videoPath);

// 같은 폴더의 같은 이름 자막(.smi/.srt/.vtt, name.en.srt 같은 변형 포함) 을 찾는다. 없으면 빈 문자열.
std::string findSiblingSubtitle(const std::string& videoPath, Lang lang = Lang::En);

struct Prepared {
    std::string videoPath;     // 원본 영상 (절대 경로)
    std::string audioPath;     // dir/audio.wav (mono 16kHz)
    std::string subtitlePath;  // dir/subtitle.<ext> (없으면 빈 문자열)
    std::string title;         // 파일 이름 (확장자 제외)
};

// 처음 등록: dir 를 만들고 source.txt 를 쓰고, 자막을 복사하고(없으면 영상 안의 영어 자막 트랙 추출 시도), ffmpeg 로 오디오를 뽑는다.
Prepared prepare(const std::string& videoPath, const std::string& subtitlePath, const std::string& dir, Lang lang = Lang::En);

// 다시 열기: source.txt 의 영상이 있는지 확인하고 빠진 산출물을 보충한다. 영상이 없어졌으면 throw.
Prepared reopen(const std::string& dir, Lang lang = Lang::En);

// 자막 교체: 새 자막을 dir 로 복사하고 기존 segments.json 을 지운다
void replaceSubtitle(const std::string& subtitlePath, const std::string& dir);

}  // namespace local

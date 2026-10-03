#pragma once
#include <string>
#include <vector>

#include "transcript.h"

// 자막 파일(SMI / SRT / VTT) 을 단어 목록으로 읽는다.
// 자막 블록에는 단어별 시각이 없으므로 블록 구간을 단어 길이에 비례해 나눠 준다.
// 인코딩: UTF-8(BOM 유무) / UTF-16 / 그 외는 CP949(한국어 Windows) 로 간주.
namespace subtitle {

bool isSubtitleExt(const std::string& ext);  // ".smi" ".srt" ".vtt" (대소문자 무시)

// 실패하면 std::runtime_error. SMI 에 여러 언어가 있으면 영어 클래스를 고른다.
std::vector<Word> parseFile(const std::string& path);

}  // namespace subtitle

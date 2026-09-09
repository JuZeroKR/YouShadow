#pragma once
#include <string>
#include <vector>

struct Segment {
    int idx = 0;
    int startMs = 0;
    int endMs = 0;
    std::string text;
};

// 타임스탬프가 붙은 단어. 자막(json3)이나 whisper 결과에서 만든다.
struct Word {
    std::string text;
    int startMs = 0;
    int endMs = 0;
};

namespace transcript {

// 단어 목록을 문장 부호 / 침묵 / 길이 기준으로 문장 세그먼트로 나눈다.
std::vector<Segment> splitWords(std::vector<Word> words);

// 유튜브 json3 자막 파일을 문장 단위 세그먼트로 나눈다.
std::vector<Segment> parseJson3(const std::string& path);

// 한 번 나눈 세그먼트는 고정해 두기 위해 파일로 저장/로드한다.
void save(const std::vector<Segment>& segs, const std::string& path);
std::vector<Segment> load(const std::string& path);

std::string formatTime(int ms);

}  // namespace transcript

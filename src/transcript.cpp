#include "transcript.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "json.hpp"

using json = nlohmann::json;

namespace transcript {

namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

// 자막 태그 제거: [Music], [laughter], [ __ ] 같은 대괄호 표기와 화자 전환 표시 ">>"
std::string stripTags(std::string s) {
    for (;;) {
        size_t a = s.find('[');
        if (a == std::string::npos) break;
        size_t b = s.find(']', a);
        if (b == std::string::npos) { s.erase(a); break; }
        s.erase(a, b - a + 1);
    }
    for (size_t p; (p = s.find(">>")) != std::string::npos;) s.erase(p, 2);
    return s;
}

std::vector<Word> collectWords(const json& doc) {
    std::vector<Word> words;
    if (!doc.contains("events")) return words;

    for (const auto& ev : doc["events"]) {
        if (!ev.contains("segs")) continue;
        const int evStart = ev.value("tStartMs", 0);
        const int evDur = ev.value("dDurationMs", 0);
        const int evEnd = evStart + evDur;

        for (const auto& seg : ev["segs"]) {
            std::string utf8 = stripTags(seg.value("utf8", ""));
            const int offset = seg.value("tOffsetMs", 0);
            // 자동 자막은 단어마다 seg 하나, 수동 자막은 한 seg에 문장 전체가 온다.
            std::istringstream ss(utf8);
            std::string tok;
            while (ss >> tok) {
                tok = trim(tok);
                if (tok.empty()) continue;
                words.push_back({tok, evStart + offset, evEnd});
            }
        }
    }
    return words;
}

bool endsSentence(const std::string& w) {
    if (w.empty()) return false;
    char c = w.back();
    if (c == '"' || c == '\'' || c == ')') {
        if (w.size() < 2) return false;
        c = w[w.size() - 2];
    }
    return c == '.' || c == '?' || c == '!';
}

}  // namespace

std::vector<Segment> splitWords(std::vector<Word> words) {
    std::vector<Segment> segs;
    if (words.empty()) return segs;

    // 단어 끝 시간은 다음 단어 시작으로 잡는 편이 더 정확하다.
    for (size_t i = 0; i + 1 < words.size(); ++i) {
        words[i].endMs = std::min(words[i].endMs, std::max(words[i].startMs, words[i + 1].startMs));
    }

    // 분할 규칙: 문장 부호로 끝나거나, 침묵이 길거나, 너무 길어지면 끊는다.
    constexpr int kLongGapMs = 800;
    constexpr int kSoftGapMs = 250;
    constexpr int kSoftMaxWords = 16;
    constexpr int kHardMaxWords = 28;

    size_t begin = 0;
    for (size_t i = 0; i < words.size(); ++i) {
        const size_t count = i - begin + 1;
        const bool last = (i + 1 == words.size());
        const int gap = last ? 0 : words[i + 1].startMs - words[i].endMs;

        bool cut = last || endsSentence(words[i].text) || gap >= kLongGapMs ||
                   (count >= kSoftMaxWords && gap >= kSoftGapMs) || count >= kHardMaxWords;
        if (!cut) continue;

        Segment s;
        s.startMs = words[begin].startMs;
        s.endMs = words[i].endMs;
        for (size_t k = begin; k <= i; ++k) {
            if (k > begin) s.text += ' ';
            s.text += words[k].text;
        }

        // 너무 짧은 조각은 직전 세그먼트에 붙인다.
        if (!segs.empty() && count <= 2 && s.startMs - segs.back().endMs < 500 &&
            !endsSentence(segs.back().text)) {
            segs.back().endMs = s.endMs;
            segs.back().text += ' ' + s.text;
        } else {
            segs.push_back(s);
        }
        begin = i + 1;
    }

    // 끝 부분에 약간의 여유를 주되 다음 세그먼트와 겹치지 않게 한다.
    for (size_t i = 0; i < segs.size(); ++i) {
        int pad = segs[i].endMs + 200;
        if (i + 1 < segs.size()) pad = std::min(pad, segs[i + 1].startMs);
        segs[i].endMs = std::max(pad, segs[i].startMs + 300);
        segs[i].idx = (int)i;
    }
    return segs;
}

std::vector<Segment> parseJson3(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("자막 파일을 열 수 없음: " + path);
    json doc = json::parse(in, nullptr, true, true);
    return splitWords(collectWords(doc));
}

void save(const std::vector<Segment>& segs, const std::string& path) {
    json arr = json::array();
    for (const auto& s : segs) {
        arr.push_back({{"idx", s.idx}, {"start", s.startMs}, {"end", s.endMs}, {"text", s.text}});
    }
    std::ofstream out(path);
    out << arr.dump(2);
}

std::vector<Segment> load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {};
    json arr = json::parse(in);
    std::vector<Segment> segs;
    for (const auto& j : arr) {
        segs.push_back({j["idx"], j["start"], j["end"], j["text"]});
    }
    return segs;
}

std::string formatTime(int ms) {
    int total = ms / 1000;
    char buf[32];
    snprintf(buf, sizeof buf, "%02d:%02d.%d", total / 60, total % 60, (ms % 1000) / 100);
    return buf;
}

}  // namespace transcript

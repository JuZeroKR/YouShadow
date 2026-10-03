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

namespace {

size_t utf8Len(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}

// 일본어 문장 끝: 。！？ 또는 ASCII . ? !  (닫는 괄호/따옴표가 뒤에 와도 인식)
bool endsSentenceJa(const std::string& w) {
    auto chars = jp::splitChars(w);
    while (!chars.empty()) {
        unsigned cp = jp::decodeFirst(chars.back());
        if (cp == 0x300D || cp == 0x300F || cp == 0xFF09 || cp == ')' || cp == '"' || cp == '\'') { chars.pop_back(); continue; }
        return cp == 0x3002 || cp == 0xFF01 || cp == 0xFF1F || cp == '.' || cp == '?' || cp == '!';
    }
    return false;
}

// 일본어는 띄어쓰기 없이 잇되, 영숫자끼리 맞닿으면 공백을 둔다
void appendJa(std::string& out, const std::string& w) {
    if (!out.empty()) {
        unsigned char a = out.back(), b = w.empty() ? 0 : (unsigned char)w[0];
        if (a < 0x80 && b < 0x80 && std::isalnum(a) && std::isalnum(b)) out += ' ';
    }
    out += w;
}

}  // namespace

std::vector<Segment> splitWords(std::vector<Word> words, Lang lang) {
    std::vector<Segment> segs;
    if (words.empty()) return segs;
    const bool ja = lang == Lang::Ja;

    // 단어 끝 시각은 다음 단어 시작으로 잡는 편이 더 정확하다.
    for (size_t i = 0; i + 1 < words.size(); ++i) {
        words[i].endMs = std::min(words[i].endMs, std::max(words[i].startMs, words[i + 1].startMs));
    }

    if (ja) {
        // 자막 한 줄 안에 문장이 여러 개면 (。！？ 뒤에서) 미리 나눠 둔다. 시간은 글자 수에 비례해 배분.
        std::vector<Word> split;
        for (const auto& w : words) {
            auto chars = jp::splitChars(w.text);
            std::vector<std::string> pieces;
            std::string cur;
            for (size_t k = 0; k < chars.size(); ++k) {
                cur += chars[k];
                unsigned cp = jp::decodeFirst(chars[k]);
                if (cp == 0x3002 || cp == 0xFF01 || cp == 0xFF1F) {
                    // 뒤따르는 닫는 괄호/따옴표는 같은 조각에 둔다
                    while (k + 1 < chars.size()) {
                        unsigned n = jp::decodeFirst(chars[k + 1]);
                        if (n == 0x300D || n == 0x300F || n == 0xFF09) cur += chars[++k];
                        else break;
                    }
                    pieces.push_back(cur);
                    cur.clear();
                }
            }
            if (!cur.empty()) pieces.push_back(cur);
            if (pieces.size() <= 1) { split.push_back(w); continue; }
            const size_t total = std::max<size_t>(1, utf8Len(w.text));
            const int dur = std::max(200, w.endMs - w.startMs);
            int t = w.startMs;
            for (size_t k = 0; k < pieces.size(); ++k) {
                int span = (int)((long long)dur * utf8Len(pieces[k]) / total);
                int end = k + 1 == pieces.size() ? w.endMs : t + span;
                split.push_back({pieces[k], t, std::max(end, t + 50)});
                t = end;
            }
        }
        words.swap(split);
    }

    // 분할 규칙: 문장 부호로 끝나거나, 침묵이 길거나, 너무 길어지면 끊는다.
    // 영어는 단어 수, 일본어는 글자 수(자막 덩어리에는 띄어쓰기가 없다) 기준.
    constexpr int kLongGapMs = 800;
    constexpr int kSoftGapMs = 250;
    const int softMax = ja ? 15 : 16;
    const int hardMax = ja ? 40 : 28;

    size_t begin = 0;
    int length = 0;  // 현재 조각의 단어 수(영어) 또는 글자 수(일본어)
    for (size_t i = 0; i < words.size(); ++i) {
        length += ja ? (int)utf8Len(words[i].text) : 1;
        const bool last = (i + 1 == words.size());
        const int gap = last ? 0 : words[i + 1].startMs - words[i].endMs;
        const bool punct = ja ? endsSentenceJa(words[i].text) : endsSentence(words[i].text);

        bool cut = last || punct || gap >= kLongGapMs ||
                   (length >= softMax && gap >= kSoftGapMs) || length >= hardMax;
        if (!cut) continue;

        Segment s;
        s.startMs = words[begin].startMs;
        s.endMs = words[i].endMs;
        for (size_t k = begin; k <= i; ++k) {
            if (ja) appendJa(s.text, words[k].text);
            else { if (k > begin) s.text += ' '; s.text += words[k].text; }
        }

        // 너무 짧은 조각은 직전 세그먼트에 붙인다
        const bool tiny = ja ? utf8Len(s.text) <= 3 : (i - begin + 1) <= 2;
        const bool prevEnds = segs.empty() ? true : (ja ? endsSentenceJa(segs.back().text) : endsSentence(segs.back().text));
        if (!segs.empty() && tiny && s.startMs - segs.back().endMs < 500 && !prevEnds) {
            segs.back().endMs = s.endMs;
            if (ja) appendJa(segs.back().text, s.text);
            else segs.back().text += ' ' + s.text;
        } else {
            segs.push_back(s);
        }
        begin = i + 1;
        length = 0;
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

std::vector<Segment> parseJson3(const std::string& path, Lang lang) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("자막 파일을 열 수 없음: " + path);
    json doc = json::parse(in, nullptr, true, true);
    return splitWords(collectWords(doc), lang);
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

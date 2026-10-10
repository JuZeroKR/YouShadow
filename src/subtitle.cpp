#include "subtitle.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <iconv.h>
#endif

namespace fs = std::filesystem;

namespace subtitle {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

// ---- 인코딩 ----

bool validUtf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || i + n >= s.size() + (n == 0 ? 1 : 0)) { if (n < 0 || i + n > s.size()) return false; }
        for (int k = 1; k <= n; ++k) {
            if (i + k >= s.size() || (((unsigned char)s[i + k]) >> 6) != 2) return false;
        }
        i += n + 1;
    }
    return true;
}

std::string utf16ToUtf8(const std::string& bytes, bool bigEndian) {
    std::string out;
    auto unit = [&](size_t i) -> unsigned {
        unsigned char a = bytes[i], b = bytes[i + 1];
        return bigEndian ? (a << 8 | b) : (b << 8 | a);
    };
    auto put = [&](unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    };
    for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
        unsigned u = unit(i);
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < bytes.size()) {
            unsigned lo = unit(i + 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) { put(0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00)); i += 2; continue; }
        }
        put(u);
    }
    return out;
}

std::string cp949ToUtf8(const std::string& s) {
#ifdef _WIN32
    int n = MultiByteToWideChar(949, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return s;
    std::wstring w(n, L'\0');
    MultiByteToWideChar(949, 0, s.data(), (int)s.size(), &w[0], n);
    int m = WideCharToMultiByte(CP_UTF8, 0, w.data(), n, nullptr, 0, nullptr, nullptr);
    std::string out(m, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), n, &out[0], m, nullptr, nullptr);
    return out;
#else
    iconv_t cd = iconv_open("UTF-8", "CP949");
    if (cd == (iconv_t)-1) return s;
    std::string out(s.size() * 3 + 16, '\0');
    char* in = const_cast<char*>(s.data());
    size_t inLeft = s.size();
    char* op = &out[0];
    size_t outLeft = out.size();
    iconv(cd, &in, &inLeft, &op, &outLeft);
    iconv_close(cd);
    out.resize(out.size() - outLeft);
    return out;
#endif
}

std::string readText(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) throw std::runtime_error("자막 파일을 열 수 없습니다: " + path);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) return bytes.substr(3);
    if (bytes.size() >= 2 && (unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xFE) return utf16ToUtf8(bytes.substr(2), false);
    if (bytes.size() >= 2 && (unsigned char)bytes[0] == 0xFE && (unsigned char)bytes[1] == 0xFF) return utf16ToUtf8(bytes.substr(2), true);
    if (validUtf8(bytes)) return bytes;
    return cp949ToUtf8(bytes);
}

// ---- HTML 정리 ----

std::string htmlToText(const std::string& html) {
    std::string out;
    bool inTag = false;
    std::string tag;
    for (size_t i = 0; i < html.size(); ++i) {
        char c = html[i];
        if (c == '<') { inTag = true; tag.clear(); continue; }
        if (c == '>') {
            inTag = false;
            std::string t = lower(trim(tag));
            if (t.rfind("br", 0) == 0 || t.rfind("/p", 0) == 0 || t.rfind("p", 0) == 0) out += ' ';
            continue;
        }
        if (inTag) { tag += c; continue; }
        if (c == '&') {
            size_t semi = html.find(';', i);
            if (semi != std::string::npos && semi - i <= 8) {
                std::string ent = lower(html.substr(i + 1, semi - i - 1));
                const char* rep = ent == "nbsp" ? " " : ent == "amp" ? "&" : ent == "lt" ? "<" : ent == "gt" ? ">" :
                                  ent == "quot" ? "\"" : (ent == "#39" || ent == "apos") ? "'" : nullptr;
                if (rep) { out += rep; i = semi; continue; }
            }
        }
        if (c == '\r' || c == '\n') c = ' ';
        out += c;
    }
    // 공백 정리
    std::string s;
    bool space = true;
    for (char c : out) {
        if (std::isspace((unsigned char)c)) { if (!space) s += ' '; space = true; }
        else { s += c; space = false; }
    }
    return trim(s);
}

struct Block {
    int startMs = 0, endMs = 0;
    std::string text;
};

// 블록을 단어로 나누고 시간을 글자 수에 비례해 배분한다
std::vector<Word> blocksToWords(const std::vector<Block>& blocks) {
    std::vector<Word> words;
    for (const auto& b : blocks) {
        std::istringstream ss(b.text);
        std::vector<std::string> ws;
        for (std::string w; ss >> w;) ws.push_back(w);
        if (ws.empty()) continue;
        int total = 0;
        for (const auto& w : ws) total += (int)w.size() + 1;
        const int dur = std::max(200, b.endMs - b.startMs);
        int t = b.startMs;
        for (size_t i = 0; i < ws.size(); ++i) {
            int span = (int)((long long)dur * ((int)ws[i].size() + 1) / total);
            int end = i + 1 == ws.size() ? b.endMs : t + span;
            words.push_back({ws[i], t, std::max(end, t + 50)});
            t = end;
        }
    }
    return words;
}

// ---- SMI (SAMI) ----

// <STYLE> 에서 .CLASS { lang: en-US } 처럼 해당 언어로 선언된 클래스 이름들을 찾는다
std::vector<std::string> classesForLang(const std::string& doc, Lang lang) {
    const std::string code = langCode(lang);
    const std::string nameWord = lang == Lang::Ja ? "japanese" : lang == Lang::Ko ? "korean" : "english";
    std::vector<std::string> out;
    std::string low = lower(doc);
    size_t s = low.find("<style");
    if (s == std::string::npos) return out;
    size_t e = low.find("</style>", s);
    std::string style = low.substr(s, e == std::string::npos ? std::string::npos : e - s);
    for (size_t p = 0; (p = style.find('.', p)) != std::string::npos;) {
        size_t nameEnd = p + 1;
        while (nameEnd < style.size() && (std::isalnum((unsigned char)style[nameEnd]) || style[nameEnd] == '_' || style[nameEnd] == '-')) ++nameEnd;
        std::string name = style.substr(p + 1, nameEnd - p - 1);
        size_t open = style.find('{', nameEnd), close = style.find('}', nameEnd);
        if (!name.empty() && open != std::string::npos && close != std::string::npos && open < close) {
            std::string body = style.substr(open, close - open);
            size_t l = body.find("lang:");
            if (l != std::string::npos) {
                std::string declared = trim(body.substr(l + 5, 6));
                if (declared.rfind(code, 0) == 0) out.push_back(name);
            } else if (body.find(nameWord) != std::string::npos) {
                out.push_back(name);
            }
            p = close;
        } else {
            p = nameEnd;
        }
    }
    return out;
}

// 영어다움: 아스키 글자 비율
double asciiRatio(const std::string& s) {
    int letters = 0, ascii = 0;
    for (unsigned char c : s) {
        if (std::isspace(c) || std::ispunct(c)) continue;
        ++letters;
        if (c < 128) ++ascii;
    }
    return letters ? (double)ascii / letters : 0.0;
}

// 일본어다움: 가나/한자 비율
double japaneseRatio(const std::string& s) {
    int letters = 0, jpn = 0;
    for (const auto& ch : jp::splitChars(s)) {
        unsigned cp = jp::decodeFirst(ch);
        if (cp < 0x80 && !std::isalpha((int)cp)) continue;
        ++letters;
        if (jp::isKanji(cp) || jp::isHiragana(cp) || jp::isKatakana(cp)) ++jpn;
    }
    return letters ? (double)jpn / letters : 0.0;
}

// 한국어다움: 한글 비율
double hangulRatio(const std::string& s) {
    int letters = 0, ko = 0;
    for (const auto& ch : jp::splitChars(s)) {
        unsigned cp = jp::decodeFirst(ch);
        if (cp < 0x80 && !std::isalpha((int)cp)) continue;
        ++letters;
        if ((cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0x3130 && cp <= 0x318F)) ++ko;
    }
    return letters ? (double)ko / letters : 0.0;
}

double langRatio(const std::string& s, Lang lang) {
    return lang == Lang::Ja ? japaneseRatio(s) : lang == Lang::Ko ? hangulRatio(s) : asciiRatio(s);
}

std::vector<Block> parseSmi(const std::string& doc, Lang lang) {
    std::string low = lower(doc);
    struct Raw { int startMs; std::string cls; std::string html; };
    std::vector<Raw> raws;

    size_t pos = low.find("<sync");
    while (pos != std::string::npos) {
        size_t tagEnd = low.find('>', pos);
        if (tagEnd == std::string::npos) break;
        std::string tag = low.substr(pos, tagEnd - pos);
        int start = 0;
        size_t st = tag.find("start");
        if (st != std::string::npos) {
            size_t eq = tag.find('=', st);
            if (eq != std::string::npos) {
                size_t k = eq + 1;
                while (k < tag.size() && !std::isdigit((unsigned char)tag[k]) && tag[k] != '-') ++k;
                start = std::atoi(tag.c_str() + k);
            }
        }
        size_t next = low.find("<sync", tagEnd);
        size_t bodyEnd = low.find("</body", tagEnd);
        size_t end = std::min(next == std::string::npos ? doc.size() : next, bodyEnd == std::string::npos ? doc.size() : bodyEnd);
        std::string content = doc.substr(tagEnd + 1, end - tagEnd - 1);
        std::string contentLow = low.substr(tagEnd + 1, end - tagEnd - 1);

        // <P class=XXX> 단위로 나눈다 (없으면 통째로 하나)
        size_t p = contentLow.find("<p");
        if (p == std::string::npos) {
            raws.push_back({start, "", content});
        } else {
            while (p != std::string::npos) {
                size_t pEnd = contentLow.find('>', p);
                if (pEnd == std::string::npos) break;
                std::string ptag = contentLow.substr(p, pEnd - p);
                std::string cls;
                size_t c = ptag.find("class");
                if (c != std::string::npos) {
                    size_t eq = ptag.find('=', c);
                    if (eq != std::string::npos) {
                        size_t k = eq + 1;
                        while (k < ptag.size() && (std::isspace((unsigned char)ptag[k]) || ptag[k] == '"' || ptag[k] == '\'')) ++k;
                        size_t j = k;
                        while (j < ptag.size() && (std::isalnum((unsigned char)ptag[j]) || ptag[j] == '_' || ptag[j] == '-')) ++j;
                        cls = ptag.substr(k, j - k);
                    }
                }
                size_t nextP = contentLow.find("<p", pEnd);
                raws.push_back({start, cls, content.substr(pEnd + 1, (nextP == std::string::npos ? content.size() : nextP) - pEnd - 1)});
                p = nextP;
            }
        }
        pos = next;
    }
    if (raws.empty()) throw std::runtime_error("SMI 자막에서 <SYNC> 블록을 찾지 못했습니다");

    // 클래스 선택: STYLE 의 언어 선언 → 하나뿐이면 그것 → 그 언어 글자 비율이 가장 높은 것
    std::map<std::string, std::string> sample;
    for (const auto& r : raws) if (sample[r.cls].size() < 4000) sample[r.cls] += htmlToText(r.html) + " ";
    std::string chosen;
    auto declared = classesForLang(doc, lang);
    for (const auto& c : declared) if (sample.count(c)) { chosen = c; break; }
    if (chosen.empty()) {
        double best = -1;
        for (const auto& [cls, text] : sample) {
            double r = langRatio(text, lang);
            if (r > best) { best = r; chosen = cls; }
        }
        // 한국어(번역)는 없을 수도 있다: 한글이 거의 없으면 고르지 않는다
        if (lang == Lang::Ko && best < 0.3) return {};
    }

    std::vector<Block> blocks;
    for (const auto& r : raws) {
        if (r.cls != chosen) continue;
        std::string text = htmlToText(r.html);
        // 이전 블록은 다음 SYNC(같은 클래스) 에서 끝난다. 빈 블록(&nbsp;)은 지우기 용도.
        if (!blocks.empty() && blocks.back().endMs == 0) blocks.back().endMs = r.startMs;
        if (!text.empty()) blocks.push_back({r.startMs, 0, text});
    }
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (blocks[i].endMs == 0) blocks[i].endMs = i + 1 < blocks.size() ? blocks[i + 1].startMs : blocks[i].startMs + 4000;
        blocks[i].endMs = std::min(blocks[i].endMs, blocks[i].startMs + 10000);  // 너무 긴 표시는 자른다
        blocks[i].endMs = std::max(blocks[i].endMs, blocks[i].startMs + 200);
    }
    return blocks;
}

// ---- SRT / VTT ----

int parseTimestamp(const std::string& s) {
    // 00:00:01,000 / 00:00:01.000 / 00:01.000
    int h = 0, m = 0, sec = 0, ms = 0;
    std::string t = s;
    std::replace(t.begin(), t.end(), ',', '.');
    int parts = (int)std::count(t.begin(), t.end(), ':');
    if (parts == 2) {
        if (sscanf(t.c_str(), "%d:%d:%d.%d", &h, &m, &sec, &ms) < 3) return -1;
    } else if (parts == 1) {
        if (sscanf(t.c_str(), "%d:%d.%d", &m, &sec, &ms) < 2) return -1;
    } else {
        return -1;
    }
    return ((h * 60 + m) * 60 + sec) * 1000 + ms;
}

std::vector<Block> parseSrtVtt(const std::string& doc) {
    std::vector<Block> blocks;
    std::istringstream in(doc);
    std::string line;
    Block cur;
    bool inCue = false;
    auto flush = [&] {
        cur.text = htmlToText(cur.text);
        if (inCue && !cur.text.empty()) blocks.push_back(cur);
        cur = Block();
        inCue = false;
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t arrow = line.find("-->");
        if (arrow != std::string::npos) {
            flush();
            std::string a = trim(line.substr(0, arrow));
            std::string rest = trim(line.substr(arrow + 3));
            std::string b = rest.substr(0, rest.find(' '));
            int s = parseTimestamp(a), e = parseTimestamp(b);
            if (s < 0 || e < 0) continue;
            cur.startMs = s;
            cur.endMs = std::max(e, s + 200);
            inCue = true;
            continue;
        }
        if (trim(line).empty()) { flush(); continue; }
        if (!inCue) continue;  // 번호 줄, WEBVTT 헤더, NOTE 등
        cur.text += line + " ";
    }
    flush();
    if (blocks.empty()) throw std::runtime_error("자막에서 시간 정보가 있는 블록을 찾지 못했습니다");
    return blocks;
}

}  // namespace

bool isSubtitleExt(const std::string& ext) {
    std::string e = lower(ext);
    return e == ".smi" || e == ".sami" || e == ".srt" || e == ".vtt";
}

namespace {

std::vector<Block> readBlocks(const std::string& path, Lang lang) {
    std::string doc = readText(path);
    std::string ext = lower(fs::u8path(path).extension().u8string());
    std::string head = lower(doc.substr(0, std::min<size_t>(doc.size(), 2000)));
    std::vector<Block> blocks;
    if (ext == ".smi" || ext == ".sami" || head.find("<sami") != std::string::npos || head.find("<sync") != std::string::npos) {
        blocks = parseSmi(doc, lang);
    } else {
        blocks = parseSrtVtt(doc);
        if (lang == Lang::Ko) {  // 언어가 하나뿐인 SRT/VTT: 한국어 자막인지 확인
            std::string sample;
            for (const auto& b : blocks) { if (sample.size() > 4000) break; sample += b.text + " "; }
            if (hangulRatio(sample) < 0.3) blocks.clear();
        }
    }
    std::sort(blocks.begin(), blocks.end(), [](const Block& a, const Block& b) { return a.startMs < b.startMs; });
    return blocks;
}

bool isAsciiAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// 자막 제작 실수로 붙은 문장 부호 뒤를 띄운다: "year,it's" → "year, it's", "shot.Come" → "shot. Come", "what?I" → "what? I"
// "U.S." "3.5" "e.g." 처럼 원래 붙여 쓰는 것은 건드리지 않는다.
std::string tidy(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        out += c;
        if (i == 0 || i + 1 >= in.size()) continue;
        char prev = in[i - 1], next = in[i + 1];
        if (c == ',' && isAsciiAlpha(prev) && isAsciiAlpha(next)) out += ' ';
        else if (c == '.' && std::islower((unsigned char)prev) && i >= 2 && isAsciiAlpha(in[i - 2]) && std::isupper((unsigned char)next)) out += ' ';
        else if ((c == '?' || c == '!') && isAsciiAlpha(prev) && isAsciiAlpha(next)) out += ' ';
    }
    return trim(out);
}

// 앞의 대사 표시 "-" 떼기
std::string stripDash(std::string s) {
    s = trim(s);
    while (!s.empty() && s[0] == '-' && (s.size() == 1 || s[1] != '-')) s = trim(s.substr(1));
    return s;
}

// 연습할 대사인가: 글자가 하나도 없는 줄("**", "♬") 과 자막 제작자 주소 줄은 뺀다
bool isDialogue(const std::string& s) {
    const std::string l = lower(s);
    if (l.find("http") != std::string::npos || l.find("www.") != std::string::npos || l.find(".com") != std::string::npos) return false;
    for (const auto& ch : jp::splitChars(s)) {
        unsigned cp = jp::decodeFirst(ch);
        if (cp < 0x80 ? std::isalnum((int)cp) != 0 : ((cp >= 0xAC00 && cp <= 0xD7A3) || jp::isKanji(cp) || jp::isHiragana(cp) || jp::isKatakana(cp))) return true;
    }
    return false;
}

// 한 장면을 대사로 나눈다. "-" 로 시작하는 장면만 " - " 에서 나눈다 ("Every year -- every" 같은 줄표는 그대로).
std::vector<Cue> splitSpeakers(const Cue& c) {
    std::string t = trim(c.text);
    std::vector<std::string> parts;
    if (!t.empty() && t[0] == '-' && (t.size() < 2 || t[1] != '-')) {
        size_t start = 0;
        for (size_t p; (p = t.find(" - ", start == 0 ? 1 : start)) != std::string::npos;) {
            parts.push_back(t.substr(start, p - start));
            start = p + 1;
        }
        parts.push_back(t.substr(start));
    } else {
        parts.push_back(t);
    }
    std::vector<std::string> clean;
    for (auto& p : parts) { std::string s = tidy(stripDash(p)); if (!s.empty() && isDialogue(s)) clean.push_back(s); }
    std::vector<Cue> out;
    if (clean.empty()) return out;
    size_t total = 0;
    for (const auto& s : clean) total += s.size() + 4;
    const int dur = std::max(200, c.endMs - c.startMs);
    int t0 = c.startMs;
    for (size_t i = 0; i < clean.size(); ++i) {
        int end = i + 1 == clean.size() ? c.startMs + dur : t0 + (int)((long long)dur * (clean[i].size() + 4) / total);
        out.push_back({t0, std::max(end, t0 + 200), clean[i]});
        t0 = end;
    }
    return out;
}

}  // namespace

std::vector<Word> parseFile(const std::string& path, Lang lang) {
    return blocksToWords(readBlocks(path, lang));
}

double langScore(const std::string& path, Lang lang) {
    try {
        std::string sample;
        for (const auto& b : readBlocks(path, lang)) { if (sample.size() > 8000) break; sample += b.text + " "; }
        return langRatio(sample, lang);
    } catch (const std::exception&) {
        return 0.0;
    }
}

std::vector<Cue> parseCues(const std::string& path, Lang lang) {
    std::vector<Block> blocks;
    if (lang == Lang::Ko) {
        try { blocks = readBlocks(path, lang); } catch (const std::exception&) { return {}; }
    } else {
        blocks = readBlocks(path, lang);
    }
    std::vector<Cue> cues;
    for (const auto& b : blocks) cues.push_back({b.startMs, b.endMs, b.text});
    return cues;
}

std::vector<Segment> cuesToLines(const std::vector<Cue>& cues) {
    std::vector<Segment> segs;
    for (const auto& c : cues) {
        for (const auto& line : splitSpeakers(c)) {
            Segment s;
            s.idx = (int)segs.size();
            s.startMs = line.startMs;
            s.endMs = line.endMs;
            s.text = line.text;
            segs.push_back(s);
        }
    }
    return segs;
}

namespace {

// 붙은 자리에 흔히 오는 짧은 단어(기능어 · 아주 흔한 말). 사전에는 "lo", "caf", "eau" 같은 희귀어도 있어서 사전에 있다는 것만으로는
// "longas" 를 "lo ngas" 로, 이름 "caffrey" 를 "caf frey" 로 잘못 나눌 수 있다. 3글자 이하 조각은 이 목록에 있어야 하고, 목록 단어가 든 후보에 점수를 더 준다
bool isCommonWord(const std::string& w) {
    static const char* const kWords[] = {
        "a", "i", "ah", "am", "an", "as", "at", "be", "by", "do", "go", "he", "if", "in", "is", "it", "me", "my", "no", "of", "oh",
        "ok", "on", "or", "so", "to", "up", "us", "we", "all", "and", "any", "are", "ask", "bad", "big", "boy", "but", "buy", "can",
        "car", "day", "did", "end", "far", "few", "for", "get", "god", "got", "guy", "had", "has", "her", "hey", "him", "his", "how",
        "its", "job", "let", "lot", "man", "men", "new", "not", "now", "off", "old", "one", "our", "out", "put", "run",
        "saw", "say", "see", "she", "sit", "the", "too", "try", "two", "use", "was", "way", "who", "why", "yes", "yet", "you",
        "back", "been", "come", "does", "down", "else", "even", "ever", "from", "good", "have", "here", "into", "just", "know",
        "last", "like", "long", "look", "many", "more", "most", "much", "must", "need", "next", "once", "only", "onto", "over",
        "same", "some", "soon", "such", "take", "tell", "than", "that", "them", "then", "they", "this", "till", "time",
        "upon", "used", "very", "want", "well", "went", "were", "what", "when", "will", "with", "your",
        "about", "after", "again", "apart", "away", "could", "every", "first", "going", "never", "other", "right", "shall",
        "since", "still", "their", "there", "these", "thing", "think", "those", "under", "until", "where", "which", "while",
        "would", "always", "around", "before", "really", "should", "though", "because", "through", "whenever"};
    for (const char* k : kWords) if (w == k) return true;
    return false;
}

// 흔한 단어이거나 흔한 단어의 축약형인가 ("it's", "don't", "you're" 는 참, "kin's" 는 거짓)
bool isCommonish(const std::string& p) {
    if (isCommonWord(p)) return true;
    const size_t q = p.find('\'');
    if (q == std::string::npos) return false;
    std::string base = p.substr(0, q);
    if (p.compare(q, std::string::npos, "'t") == 0 && !base.empty() && base.back() == 'n') base.pop_back();  // don't → do
    return isCommonWord(base);
}

// 축약 조각은 끝이 축약 어미여야 한다 ("wasn't" 는 되고 "wasn'ts" 는 안 된다). 아포스트로피가 없으면 참
bool apostropheOk(const std::string& p) {
    const size_t q = p.find('\'');
    if (q == std::string::npos) return true;
    if (p.find('\'', q + 1) != std::string::npos || q == 0) return false;
    const std::string suf = p.substr(q);
    return (suf == "'s" || suf == "'re" || suf == "'ll" || suf == "'ve" || suf == "'d" || suf == "'m" || (suf == "'t" && q >= 2 && p[q - 1] == 'n'));
}

// 붙은 토큰 하나를 나눈다. core 는 아포스트로피를 포함한 ASCII 소문자뿐. 못 나누면 빈 문자열.
// 후보가 여럿이면 긴 조각이 더 길고 흔한 단어가 든 것을 고른다 ("longas" → "long|as", "warehousedown" → "warehouse|down").
// 점수가 낮은 후보(짧은 조각 둘, 흔한 단어 없음: "gins|berg", "web|page")는 이름이나 사전에 없는 합성어일 수 있어 두지 않는다.
std::string splitCore(const std::string& core, const std::function<bool(const std::string&)>& isWord) {
    auto ok = [&](const std::string& part) {
        if (isCommonWord(part)) return true;  // 사전에 빠진 기능어("am")도 있다
        if (part.size() <= 3) return false;
        if (!apostropheOk(part)) return false;
        return isWord(part);
    };
    std::string best;
    int bestScore = -1;
    const bool shortTok = core.size() < 6;  // 5글자 이하는 두 조각이 모두 흔한 단어일 때만 ("lotof", "youat", "upmy")
    for (size_t i = 1; i < core.size(); ++i) {
        const std::string p1 = core.substr(0, i), p2 = core.substr(i);
        if (!ok(p1) || !ok(p2)) continue;
        // 흔한 단어 또는 흔한 단어의 축약형("don't", "that's") 이 든 후보에 점수를 더 준다 (붙은 자리에는 대개 이런 말이 온다).
        // 소유격 이름("kin's") 은 흔한 말이 아니므로 점수를 주지 않는다 → "pushkin's" 가 "push kin's" 로 갈라지지 않는다
        const bool c1 = isCommonish(p1), c2 = isCommonish(p2);
        // 한 글자 조각("a", "i")은 상대가 흔한 단어일 때만 ("alwaysa" → "always a" 는 되고 "agoand" → "a goand" 는 안 된다)
        if ((p1.size() == 1 && !c2) || (p2.size() == 1 && !c1)) continue;
        int score = (int)std::max(p1.size(), p2.size()) * 4 + (int)std::min(p1.size(), p2.size());
        if (c2) score += 12;
        if (c1) score += 10;
        // 5글자 이하는 두 조각이 모두 흔한 단어일 때만 ("lotof", "youat", "upmy") — 그때는 점수 문턱도 보지 않는다
        if (shortTok) { if (!(c1 && c2)) continue; score += 100; }
        if (score > bestScore) { bestScore = score; best = p1 + " " + p2; }
    }
    // 흔한 단어가 없는 짧은 조각 둘("gins|berg" 20, "road|blocks" 28, "web|page" 19)은 이름이나 사전에 없는 합성어일 수 있어 두지 않는다.
    // 4+2 ("bond|at" 30), 5+5 축약형 ("don't|shake" 35), 3+4 ("you|find" 29) 는 된다
    return bestScore >= 29 ? best : "";
}

}  // namespace

std::string splitGlued(const std::string& text, const std::function<bool(const std::string&)>& isWord) {
    if (!isWord) return text;
    std::string out;
    std::istringstream ss(text);
    std::string tok;
    bool first = true;
    while (ss >> tok) {
        if (!first) out += ' ';
        // 둥근 아포스트로피(’)는 곧은 것으로 (사전 키와 맞춘다)
        for (size_t p; (p = tok.find("\xE2\x80\x99")) != std::string::npos;) tok.replace(p, 3, "'");
        // 앞뒤 문장 부호를 떼고 가운데(core)만 본다
        size_t b = 0, e = tok.size();
        while (b < e && !std::isalpha((unsigned char)tok[b])) ++b;
        while (e > b && !std::isalpha((unsigned char)tok[e - 1])) --e;
        const std::string core = tok.substr(b, e - b);
        bool plain = core.size() >= 4;
        for (char c : core) if (!isAsciiAlpha(c) && c != '\'') { plain = false; break; }
        if (e < tok.size() && tok[e] == '\'') plain = false;  // "freakin'" 처럼 g 를 뺀 말투는 그대로 둔다
        // 문장 중간의 대문자 토큰은 이름일 수 있어 두지 않는다 (문장 첫 토큰은 대문자가 정상)
        if (plain && !first && std::isupper((unsigned char)core[0])) plain = false;
        std::string lowered = lower(core);
        if (plain && !isWord(lowered)) {
            std::string split = splitCore(lowered, isWord);
            if (!split.empty()) {
                // 원래 대소문자 유지: 나눈 자리에만 공백을 넣는다
                const size_t cut = split.find(' ');
                tok = tok.substr(0, b) + core.substr(0, cut) + " " + core.substr(cut) + tok.substr(e);
            }
        }
        out += tok;
        first = false;
    }
    return out;
}

std::vector<std::string> alignLines(const std::vector<Segment>& segs, const std::vector<Cue>& other) {
    std::vector<std::string> out(segs.size());
    if (segs.empty()) return out;
    for (const auto& c : other) {
        for (const auto& line : splitSpeakers(c)) {
            // 가장 많이 겹치는 문장 (시작 시각으로 이분 탐색 후 주변만 본다)
            auto it = std::lower_bound(segs.begin(), segs.end(), line.startMs, [](const Segment& s, int t) { return s.endMs <= t; });
            int best = -1, bestOv = 0;
            for (auto j = it; j != segs.end() && j->startMs < line.endMs; ++j) {
                int ov = std::min(j->endMs, line.endMs) - std::max(j->startMs, line.startMs);
                if (ov > bestOv) { bestOv = ov; best = (int)(j - segs.begin()); }
            }
            if (best < 0) continue;
            std::string& dst = out[best];
            if (!dst.empty()) dst += ' ';
            dst += line.text;
        }
    }
    return out;
}

}  // namespace subtitle

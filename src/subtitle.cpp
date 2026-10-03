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
    const std::string nameWord = lang == Lang::Ja ? "japanese" : "english";
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
            double r = lang == Lang::Ja ? japaneseRatio(text) : asciiRatio(text);
            if (r > best) { best = r; chosen = cls; }
        }
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

std::vector<Word> parseFile(const std::string& path, Lang lang) {
    std::string doc = readText(path);
    std::string ext = lower(fs::u8path(path).extension().u8string());
    std::string head = lower(doc.substr(0, std::min<size_t>(doc.size(), 2000)));
    std::vector<Block> blocks;
    if (ext == ".smi" || ext == ".sami" || head.find("<sami") != std::string::npos || head.find("<sync") != std::string::npos)
        blocks = parseSmi(doc, lang);
    else
        blocks = parseSrtVtt(doc);
    std::sort(blocks.begin(), blocks.end(), [](const Block& a, const Block& b) { return a.startMs < b.startMs; });
    return blocksToWords(blocks);
}

}  // namespace subtitle

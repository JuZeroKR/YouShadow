// 일본어 오프라인 사전 팩 빌드 도구 (개발용).
//   ja_dict_build <mecab-ipadic 디렉터리> <JMdict_e 파일(압축 해제)> <출력 디렉터리>
// 출력: ipadic.bin (형태소 사전 + 연결 비용 + 미지어 규칙), jmdict.bin (일영 사전)
// 형식은 src/jadict.cpp 의 JaDict::load 와 짝을 이룬다.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <iconv.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string eucjpToUtf8(const std::string& s) {
#ifdef _WIN32
    int n = MultiByteToWideChar(20932, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return "";
    std::wstring w(n, L'\0');
    MultiByteToWideChar(20932, 0, s.data(), (int)s.size(), &w[0], n);
    int m = WideCharToMultiByte(CP_UTF8, 0, w.data(), n, nullptr, 0, nullptr, nullptr);
    std::string out(m, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), n, &out[0], m, nullptr, nullptr);
    return out;
#else
    iconv_t cd = iconv_open("UTF-8", "EUC-JP");
    if (cd == (iconv_t)-1) return "";
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

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct Out {
    std::string b;
    void u8(unsigned v) { b += (char)(v & 0xFF); }
    void u16(unsigned v) { u8(v); u8(v >> 8); }
    void i16(int v) { u16((unsigned)(uint16_t)v); }
    void u32(uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); }
    void str8(const std::string& s) { std::string t = s.substr(0, 255); u8((unsigned)t.size()); b += t; }
    void str16(const std::string& s) { std::string t = s.substr(0, 65535); u16((unsigned)t.size()); b += t; }
};

std::vector<std::string> splitCsv(const std::string& line) {
    // IPADIC CSV 는 따옴표 안의 쉼표를 쓴다 (예: "1,2" 는 드물지만 처리)
    std::vector<std::string> f;
    std::string cur;
    bool q = false;
    for (char c : line) {
        if (c == '"') { q = !q; continue; }
        if (c == ',' && !q) { f.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    f.push_back(cur);
    return f;
}

struct Entry {
    std::string surf, base, read, pron;  // pron: 발음 (장음 ー, 조사 は→ワ 등이 반영됨). 읽기와 같으면 비워 둔다
    uint16_t left, right, pos;
    int16_t cost;
};

// ---- IPADIC ----
bool buildIpadic(const fs::path& dir, const fs::path& outPath) {
    std::vector<std::string> posTable;
    std::map<std::string, uint16_t> posId;
    auto posIndex = [&](const std::string& p1, const std::string& p2) {
        std::string key = p1 + (p2 == "*" || p2.empty() ? "" : "," + p2);
        auto it = posId.find(key);
        if (it != posId.end()) return it->second;
        uint16_t id = (uint16_t)posTable.size();
        posTable.push_back(key);
        posId[key] = id;
        return id;
    };

    std::vector<Entry> entries;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() != ".csv") continue;
        std::string text = eucjpToUtf8(readAll(e.path()));
        std::istringstream in(text);
        std::string line;
        size_t n = 0;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            auto f = splitCsv(line);
            if (f.size() < 13) continue;
            Entry en;
            en.surf = f[0];
            en.left = (uint16_t)std::stoi(f[1]);
            en.right = (uint16_t)std::stoi(f[2]);
            en.cost = (int16_t)std::clamp(std::stoi(f[3]), -32768, 32767);
            en.pos = posIndex(f[4], f[5]);
            en.base = (f[10] == "*" || f[10] == en.surf) ? "" : f[10];
            en.read = f[11] == "*" ? "" : f[11];
            en.pron = (f[12] == "*" || f[12] == en.read) ? "" : f[12];
            if (en.surf.empty() || en.surf.size() > 255) continue;
            entries.push_back(std::move(en));
            ++n;
        }
        std::cout << e.path().filename().string() << ": " << n << "\n";
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.surf < b.surf; });
    std::cout << "entries: " << entries.size() << ", pos: " << posTable.size() << "\n";

    // matrix.def
    std::string mtext = readAll(dir / "matrix.def");
    std::istringstream min(mtext);
    int lsize = 0, rsize = 0;
    min >> lsize >> rsize;
    std::vector<int16_t> conn((size_t)lsize * rsize, 0);
    int l, r, c;
    size_t cnt = 0;
    while (min >> l >> r >> c) { if (l < lsize && r < rsize) conn[(size_t)l * rsize + r] = (int16_t)std::clamp(c, -32768, 32767); ++cnt; }
    std::cout << "matrix: " << lsize << "x" << rsize << " (" << cnt << " cells)\n";

    // char.def
    struct Cat { std::string name; int invoke = 0, group = 0, length = 0; };
    std::vector<Cat> cats;
    std::map<std::string, uint8_t> catId;
    struct Range { uint32_t lo, hi; uint8_t cat; };
    std::vector<Range> ranges;
    {
        std::string ctext = eucjpToUtf8(readAll(dir / "char.def"));
        std::istringstream in(ctext);
        std::string line;
        while (std::getline(in, line)) {
            size_t h = line.find('#');
            if (h != std::string::npos) line.erase(h);
            std::istringstream ls(line);
            std::string a;
            if (!(ls >> a)) continue;
            if (a.rfind("0x", 0) == 0) {
                uint32_t lo, hi;
                size_t dd = a.find("..");
                if (dd == std::string::npos) { lo = hi = std::stoul(a, nullptr, 16); }
                else { lo = std::stoul(a.substr(0, dd), nullptr, 16); hi = std::stoul(a.substr(dd + 2), nullptr, 16); }
                std::string cname;
                if (!(ls >> cname)) continue;
                auto it = catId.find(cname);
                if (it == catId.end()) continue;
                ranges.push_back({lo, hi, it->second});
            } else {
                Cat ct; ct.name = a;
                ls >> ct.invoke >> ct.group >> ct.length;
                catId[a] = (uint8_t)cats.size();
                cats.push_back(ct);
            }
        }
    }
    std::cout << "categories: " << cats.size() << ", ranges: " << ranges.size() << "\n";

    // unk.def
    struct Unk { uint8_t cat; uint16_t left, right, pos; int16_t cost; };
    std::vector<Unk> unks;
    {
        std::string utext = eucjpToUtf8(readAll(dir / "unk.def"));
        std::istringstream in(utext);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto f = splitCsv(line);
            if (f.size() < 6) continue;
            auto it = catId.find(f[0]);
            if (it == catId.end()) continue;
            Unk u; u.cat = it->second; u.left = (uint16_t)std::stoi(f[1]); u.right = (uint16_t)std::stoi(f[2]);
            u.cost = (int16_t)std::clamp(std::stoi(f[3]), -32768, 32767); u.pos = posIndex(f[4], f.size() > 5 ? f[5] : "*");
            unks.push_back(u);
        }
    }
    std::cout << "unk entries: " << unks.size() << "\n";

    Out o;
    o.b += "YSJD";
    o.u32(2);
    o.u32((uint32_t)posTable.size());
    for (const auto& p : posTable) o.str16(p);
    o.u16(lsize); o.u16(rsize);
    for (int16_t v : conn) o.i16(v);
    o.u32((uint32_t)entries.size());
    for (const auto& e : entries) {
        o.str8(e.surf); o.u16(e.left); o.u16(e.right); o.i16(e.cost); o.u16(e.pos);
        o.str8(e.base); o.str8(e.read); o.str8(e.pron);
    }
    o.u8((unsigned)cats.size());
    for (const auto& c : cats) { o.str8(c.name); o.u8(c.invoke); o.u8(c.group); o.u8(c.length); }
    o.u32((uint32_t)ranges.size());
    for (const auto& g : ranges) { o.u32(g.lo); o.u32(g.hi); o.u8(g.cat); }
    o.u16((unsigned)unks.size());
    for (const auto& u : unks) { o.u8(u.cat); o.u16(u.left); o.u16(u.right); o.i16(u.cost); o.u16(u.pos); }
    std::ofstream out(outPath, std::ios::binary);
    out.write(o.b.data(), (std::streamsize)o.b.size());
    std::cout << "ipadic.bin: " << o.b.size() / 1024 / 1024 << " MB\n";
    return true;
}

// ---- JMdict ----
std::string between(const std::string& s, size_t from, const char* open, const char* close, size_t* next) {
    size_t a = s.find(open, from);
    if (a == std::string::npos) { *next = std::string::npos; return ""; }
    a += std::strlen(open);
    size_t b = s.find(close, a);
    if (b == std::string::npos) { *next = std::string::npos; return ""; }
    *next = b + std::strlen(close);
    return s.substr(a, b - a);
}

std::string unescapeXml(std::string s) {
    auto rep = [&](const char* from, const char* to) {
        for (size_t p; (p = s.find(from)) != std::string::npos;) s.replace(p, std::strlen(from), to);
    };
    rep("&amp;", "&"); rep("&lt;", "<"); rep("&gt;", ">"); rep("&quot;", "\""); rep("&apos;", "'");
    return s;
}

bool buildJmdict(const fs::path& xmlPath, const fs::path& outPath) {
    std::string xml = readAll(xmlPath);
    if (xml.empty()) { std::cerr << "JMdict 파일을 읽을 수 없습니다\n"; return false; }
    struct E { std::vector<std::string> kanji, readings; std::string pos; std::vector<std::string> senses; };
    std::vector<E> entries;
    size_t p = 0;
    while (true) {
        size_t a = xml.find("<entry>", p);
        if (a == std::string::npos) break;
        size_t b = xml.find("</entry>", a);
        if (b == std::string::npos) break;
        std::string en = xml.substr(a, b - a);
        p = b + 8;
        E e;
        for (size_t q = 0;;) { std::string k = between(en, q, "<keb>", "</keb>", &q); if (q == std::string::npos) break; if (k.size() <= 255) e.kanji.push_back(unescapeXml(k)); if (e.kanji.size() >= 4) break; }
        for (size_t q = 0;;) { std::string r = between(en, q, "<reb>", "</reb>", &q); if (q == std::string::npos) break; if (r.size() <= 255) e.readings.push_back(unescapeXml(r)); if (e.readings.size() >= 4) break; }
        // 의미: 처음 3개 sense, 각 sense 의 gloss 를 "; " 로 잇는다
        for (size_t q = 0; e.senses.size() < 3;) {
            size_t sa = en.find("<sense>", q);
            if (sa == std::string::npos) break;
            size_t sb = en.find("</sense>", sa);
            if (sb == std::string::npos) break;
            std::string sense = en.substr(sa, sb - sa);
            q = sb + 8;
            if (e.pos.empty()) {
                for (size_t r = 0;;) {
                    std::string ps = between(sense, r, "<pos>&", ";</pos>", &r);
                    if (r == std::string::npos) break;
                    if (!e.pos.empty()) e.pos += ",";
                    e.pos += ps;
                }
            }
            std::string glosses;
            int ng = 0;
            for (size_t r = 0; ng < 4;) {
                size_t ga = sense.find("<gloss", r);
                if (ga == std::string::npos) break;
                size_t gt = sense.find('>', ga);
                size_t gb = sense.find("</gloss>", gt);
                if (gt == std::string::npos || gb == std::string::npos) break;
                std::string tag = sense.substr(ga, gt - ga);
                std::string g = unescapeXml(sense.substr(gt + 1, gb - gt - 1));
                r = gb + 8;
                if (tag.find("xml:lang") != std::string::npos) continue;  // 영어판이지만 혹시 모를 다른 언어
                if (!glosses.empty()) glosses += "; ";
                glosses += g;
                ++ng;
            }
            if (!glosses.empty()) e.senses.push_back(glosses.substr(0, 400));
        }
        if (e.readings.empty() || e.senses.empty()) continue;
        // "보통 가나로 쓰는 말" 표시 → 가나 표기 단어를 찾을 때 우선한다
        if (en.find("<misc>&uk;</misc>") != std::string::npos) e.pos += (e.pos.empty() ? "uk" : ",uk");
        entries.push_back(std::move(e));
    }
    std::cout << "jmdict entries: " << entries.size() << "\n";

    struct Key { std::string key; uint32_t entry; };
    std::vector<Key> index;
    for (uint32_t i = 0; i < entries.size(); ++i) {
        for (const auto& k : entries[i].kanji) index.push_back({k, i});
        for (const auto& r : entries[i].readings) index.push_back({r, i});
    }
    std::sort(index.begin(), index.end(), [](const Key& a, const Key& b) { return a.key < b.key || (a.key == b.key && a.entry < b.entry); });

    Out o;
    o.b += "YSJM";
    o.u32(1);
    o.u32((uint32_t)entries.size());
    for (const auto& e : entries) {
        o.u8((unsigned)e.kanji.size()); for (const auto& k : e.kanji) o.str8(k);
        o.u8((unsigned)e.readings.size()); for (const auto& r : e.readings) o.str8(r);
        o.str8(e.pos);
        o.u8((unsigned)e.senses.size()); for (const auto& s : e.senses) o.str16(s);
    }
    o.u32((uint32_t)index.size());
    for (const auto& k : index) { o.str8(k.key); o.u32(k.entry); }
    std::ofstream out(outPath, std::ios::binary);
    out.write(o.b.data(), (std::streamsize)o.b.size());
    std::cout << "jmdict.bin: " << o.b.size() / 1024 / 1024 << " MB\n";
    return true;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 4) {
        std::cout << "usage: ja_dict_build <mecab-ipadic dir> <JMdict_e file> <out dir>\n";
        return 1;
    }
    fs::path ipadic = fs::u8path(argv[1]), jm = fs::u8path(argv[2]), out = fs::u8path(argv[3]);
    fs::create_directories(out);
    if (!buildIpadic(ipadic, out / "ipadic.bin")) return 1;
    if (!buildJmdict(jm, out / "jmdict.bin")) return 1;
    return 0;
}

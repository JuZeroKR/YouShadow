#include "endict.h"

#include <cctype>
#include <filesystem>
#include <fstream>

#include "paths.h"

namespace fs = std::filesystem;

namespace {

// en-dict.tsv 형식 (UTF-8, 첫 줄 "#YSED\t<버전>")
//   L \t 표제어 \t IPA \t 품사 묶음들(\x1e 로 구분)      묶음 = 품사 \x1f 한국어 뜻들(\x1d) \x1f 영어 정의들(\x1d)
//   F \t 변화형 \t 표제어 \t 설명("과거형" 등)
constexpr int kVersion = 1;

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    for (size_t b; (b = s.find(sep, a)) != std::string::npos; a = b + 1) out.push_back(s.substr(a, b - a));
    out.push_back(s.substr(a));
    return out;
}

std::string lowerKey(const std::string& w) {
    std::string s;
    for (size_t i = 0; i < w.size(); ++i) {
        unsigned char c = w[i];
        // 둥근 따옴표 ’ (E2 80 99) → '
        if (c == 0xE2 && i + 2 < w.size() && (unsigned char)w[i + 1] == 0x80 && (unsigned char)w[i + 2] == 0x99) { s += '\''; i += 2; continue; }
        s += (char)std::tolower(c);
    }
    // 앞뒤 문장 부호
    size_t a = 0, b = s.size();
    auto isP = [](unsigned char c) { return c < 128 && !std::isalnum(c); };
    while (a < b && isP(s[a])) ++a;
    while (b > a && isP(s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool endsWith(const std::string& s, const std::string& suf) {
    return s.size() > suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

bool isVowel(char c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'; }

}  // namespace

std::string EnDict::dir() { return paths::modelsDir() + "/en-dict"; }
std::string EnDict::packUrl() { return "https://github.com/JuZeroKR/YouShadow/releases/download/en-dict-v1/en-dict-v1.zip"; }
int EnDict::packSizeMB() { return 6; }
bool EnDict::installed() { std::error_code ec; return fs::exists(fs::u8path(dir() + "/en-dict.tsv"), ec); }

bool EnDict::load(std::string* err) {
    std::ifstream in(fs::u8path(dir() + "/en-dict.tsv"), std::ios::binary);
    if (!in) { if (err) *err = "영어 사전 파일(en-dict.tsv)을 읽을 수 없습니다: " + dir(); return false; }
    std::string line;
    std::getline(in, line);
    if (line.rfind("#YSED\t", 0) != 0 || std::atoi(line.c_str() + 6) != kVersion) {
        if (err) *err = "영어 사전 버전이 맞지 않습니다. 사전을 다시 받아 주세요";
        return false;
    }
    lemmas_.clear();
    forms_.clear();
    lemmas_.reserve(200000);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 3 || line[1] != '\t') continue;
        auto f = split(line, '\t');
        if (line[0] == 'L' && f.size() >= 4) lemmas_[f[1]] = {f[2], f[3]};
        else if (line[0] == 'F' && f.size() >= 4) forms_[f[1]].push_back({f[2], f[3]});
    }
    if (lemmas_.empty()) { if (err) *err = "영어 사전이 비어 있습니다"; return false; }
    return true;
}

const EnDict::Lemma* EnDict::find(const std::string& key) const {
    auto it = lemmas_.find(key);
    return it == lemmas_.end() ? nullptr : &it->second;
}

EnEntry EnDict::make(const std::string& key, const Lemma& l) const {
    EnEntry e;
    e.word = key;
    e.ipa = l.ipa;
    for (const auto& block : split(l.blocks, '\x1e')) {
        auto parts = split(block, '\x1f');
        EnSense s;
        s.pos = parts[0];
        if (parts.size() > 1 && !parts[1].empty()) s.korean = split(parts[1], '\x1d');
        if (parts.size() > 2 && !parts[2].empty()) s.english = split(parts[2], '\x1d');
        if (!s.korean.empty() || !s.english.empty()) e.senses.push_back(std::move(s));
    }
    return e;
}

EnEntry EnDict::lookup(const std::string& word) const {
    const std::string key = lowerKey(word);
    if (key.empty()) return {};

    // 1) 그대로 + 변화형 표
    auto fit = forms_.find(key);
    const Lemma* own = find(key);
    auto hasKorean = [](const EnEntry& e) {
        for (const auto& s : e.senses) if (!s.korean.empty()) return true;
        return false;
    };
    if (own) {
        EnEntry e = make(key, *own);
        // got / was / bucks 처럼 자체 항목은 드문 뜻뿐이고 원형(get / be / buck) 쪽에 한국어 뜻이 있으면 원형을 보여 준다
        if (!hasKorean(e) && fit != forms_.end()) {
            for (const auto& f : fit->second) {
                const Lemma* l = find(f.lemma);
                if (!l) continue;
                EnEntry le = make(f.lemma, *l);
                if (!hasKorean(le)) continue;
                le.formNote = "\"" + key + "\" = " + f.lemma + " 의 " + f.note;
                return le;
            }
        }
        if (fit != forms_.end()) {
            for (const auto& f : fit->second) {
                if (f.lemma == key) continue;
                e.formNote = "\"" + key + "\" 는 " + f.lemma + " 의 " + f.note + "이기도 합니다";
                break;
            }
        }
        if (!e.empty()) return e;
    }
    if (fit != forms_.end()) {
        for (const auto& f : fit->second) {
            if (const Lemma* l = find(f.lemma)) {
                EnEntry e = make(f.lemma, *l);
                e.formNote = "\"" + key + "\" = " + f.lemma + " 의 " + f.note;
                if (!e.empty()) return e;
            }
        }
    }

    // 2) 소유격 / 축약형
    for (const auto& [suf, rest] : std::vector<std::pair<std::string, std::string>>{
             {"n't", " not"}, {"'s", ""}, {"'re", " are"}, {"'ll", " will"}, {"'ve", " have"}, {"'d", " would/had"}, {"'m", " am"}, {"'", ""}}) {
        if (!endsWith(key, suf)) continue;
        std::string base = key.substr(0, key.size() - suf.size());
        if (suf == "n't") {
            if (base == "wo") base = "will";
            else if (base == "ca") base = "can";
            else if (base == "sha") base = "shall";
        }
        EnEntry e = lookup(base);
        if (e.empty()) continue;
        e.formNote = "\"" + key + "\" = " + base + rest + (e.formNote.empty() ? "" : "  (" + e.formNote + ")");
        return e;
    }

    // 3) 규칙 변화: -s / -es / -ies / -ed / -ing / -er / -est / -ly
    struct Rule { const char* suf; const char* add; const char* note; };
    static const Rule rules[] = {
        {"ies", "y", "복수형/3인칭 단수"}, {"es", "", "복수형/3인칭 단수"}, {"s", "", "복수형/3인칭 단수"},
        {"ied", "y", "과거형"}, {"ed", "", "과거형"}, {"ed", "e", "과거형"}, {"d", "", "과거형"},
        {"ying", "ie", "-ing 형"}, {"ing", "", "-ing 형"}, {"ing", "e", "-ing 형"},
        {"ier", "y", "비교급"}, {"er", "", "비교급"}, {"er", "e", "비교급"}, {"iest", "y", "최상급"}, {"est", "", "최상급"}, {"est", "e", "최상급"},
        {"ily", "y", "부사형"}, {"ly", "", "부사형"}, {"ly", "le", "부사형"},
    };
    for (const auto& r : rules) {
        if (!endsWith(key, r.suf)) continue;
        std::string stem = key.substr(0, key.size() - std::string(r.suf).size());
        std::vector<std::string> cands = {stem + r.add};
        // stopped → stop (자음 겹침)
        if (!*r.add && stem.size() >= 3 && stem.back() == stem[stem.size() - 2] && !isVowel(stem.back())) cands.push_back(stem.substr(0, stem.size() - 1));
        for (const auto& c : cands) {
            if (c.size() < 2) continue;
            if (const Lemma* l = find(c)) {
                EnEntry e = make(c, *l);
                if (e.empty()) continue;
                e.formNote = "\"" + key + "\" = " + c + " 의 " + r.note;
                return e;
            }
        }
    }
    return {};
}

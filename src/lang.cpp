#include "lang.h"

#include <cctype>
#include <map>

const char* langCode(Lang l) { return l == Lang::Ja ? "ja" : l == Lang::Ko ? "ko" : "en"; }
const char* langName(Lang l) { return l == Lang::Ja ? "일본어" : l == Lang::Ko ? "한국어" : "영어"; }
Lang langFromCode(const std::string& code) { return code.rfind("ja", 0) == 0 ? Lang::Ja : Lang::En; }

namespace jp {

unsigned decodeFirst(const std::string& s, size_t* len) {
    if (s.empty()) { if (len) *len = 0; return 0; }
    unsigned char c = s[0];
    size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (n > s.size()) n = 1;
    unsigned cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (size_t i = 1; i < n; ++i) cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
    if (len) *len = n;
    return cp;
}

static std::string encode(unsigned cp) {
    std::string out;
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    return out;
}

std::vector<std::string> splitChars(const std::string& utf8) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < utf8.size()) {
        size_t n = 0;
        decodeFirst(utf8.substr(i, 4), &n);
        if (n == 0) break;
        out.push_back(utf8.substr(i, n));
        i += n;
    }
    return out;
}

bool isKanji(unsigned cp) { return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || cp == 0x3005 || cp == 0x3006; }
bool isHiragana(unsigned cp) { return cp >= 0x3041 && cp <= 0x309F; }
bool isKatakana(unsigned cp) { return (cp >= 0x30A0 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF) || (cp >= 0xFF66 && cp <= 0xFF9D); }
bool isJapanesePunct(unsigned cp) {
    return (cp >= 0x3000 && cp <= 0x303F && !isKanji(cp)) || cp == 0x30FB || cp == 0xFF01 || cp == 0xFF1F || cp == 0xFF0C ||
           cp == 0xFF0E || cp == 0xFF08 || cp == 0xFF09 || cp == 0x2026 || cp == 0x2015 || cp == 0xFF5E || cp == 0x301C;
}

bool looksJapanese(const std::string& utf8) {
    int jp = 0, total = 0;
    for (const auto& ch : splitChars(utf8)) {
        unsigned cp = decodeFirst(ch);
        if (cp < 0x80 && !std::isalpha((int)cp)) continue;
        ++total;
        if (isKanji(cp) || isHiragana(cp) || isKatakana(cp)) ++jp;
    }
    return total > 0 && jp * 10 >= total * 3;
}

std::vector<std::string> roughTokens(const std::string& utf8) {
    enum Cls { Other, Kanji, Hira, Kata, Latin };
    auto classify = [](unsigned cp) {
        if (isKanji(cp)) return Kanji;
        if (isHiragana(cp)) return Hira;
        if (isKatakana(cp) || cp == 0x30FC) return Kata;
        if ((cp < 0x80 && std::isalnum((int)cp)) || cp == '\'' || (cp >= 0xFF10 && cp <= 0xFF5A)) return Latin;
        return Other;
    };
    std::vector<std::string> out;
    std::string cur;
    Cls curCls = Other;
    for (const auto& ch : splitChars(utf8)) {
        Cls c = classify(decodeFirst(ch));
        if (c == Other) { if (!cur.empty()) out.push_back(cur); cur.clear(); curCls = Other; continue; }
        bool cont = cur.empty() || c == curCls || (curCls == Kanji && c == Hira);  // 한자 + 오쿠리가나
        if (!cont) { out.push_back(cur); cur.clear(); }
        cur += ch;
        curCls = (curCls == Kanji && c == Hira) ? Kanji : c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string katakanaToHiragana(const std::string& utf8) {
    std::string out;
    for (const auto& ch : splitChars(utf8)) {
        unsigned cp = decodeFirst(ch);
        if (cp >= 0x30A1 && cp <= 0x30F6) out += encode(cp - 0x60);
        else out += ch;
    }
    return out;
}

namespace {

// 한글 음절 조립/분해
bool isHangul(unsigned cp) { return cp >= 0xAC00 && cp <= 0xD7A3; }
std::string withFinal(const std::string& syllable, int jong) {
    unsigned cp = decodeFirst(syllable);
    if (!isHangul(cp)) return syllable;
    unsigned base = cp - 0xAC00;
    if (base % 28 != 0) return syllable;  // 이미 받침이 있음
    return encode(cp + jong);
}

const std::map<std::string, std::string>& kanaTable() {
    static const std::map<std::string, std::string> t = {
        // 요음 (2글자) 먼저
        {"きゃ", "캬"}, {"きゅ", "큐"}, {"きょ", "쿄"}, {"ぎゃ", "갸"}, {"ぎゅ", "규"}, {"ぎょ", "교"},
        {"しゃ", "샤"}, {"しゅ", "슈"}, {"しょ", "쇼"}, {"じゃ", "자"}, {"じゅ", "주"}, {"じょ", "조"},
        {"ちゃ", "차"}, {"ちゅ", "추"}, {"ちょ", "초"}, {"ぢゃ", "자"}, {"ぢゅ", "주"}, {"ぢょ", "조"},
        {"にゃ", "냐"}, {"にゅ", "뉴"}, {"にょ", "뇨"}, {"ひゃ", "햐"}, {"ひゅ", "휴"}, {"ひょ", "효"},
        {"びゃ", "뱌"}, {"びゅ", "뷰"}, {"びょ", "뵤"}, {"ぴゃ", "퍄"}, {"ぴゅ", "퓨"}, {"ぴょ", "표"},
        {"みゃ", "먀"}, {"みゅ", "뮤"}, {"みょ", "묘"}, {"りゃ", "랴"}, {"りゅ", "류"}, {"りょ", "료"},
        // 외래어 표기용 (가타카나를 히라가나로 바꾼 뒤 매칭)
        {"ふぁ", "파"}, {"ふぃ", "피"}, {"ふぇ", "페"}, {"ふぉ", "포"}, {"てぃ", "티"}, {"でぃ", "디"},
        {"とぅ", "투"}, {"どぅ", "두"}, {"うぃ", "위"}, {"うぇ", "웨"}, {"うぉ", "워"}, {"しぇ", "셰"},
        {"じぇ", "제"}, {"ちぇ", "체"}, {"つぁ", "차"}, {"つぃ", "치"}, {"つぇ", "체"}, {"つぉ", "초"},
        {"ゔぁ", "바"}, {"ゔぃ", "비"}, {"ゔぇ", "베"}, {"ゔぉ", "보"}, {"いぇ", "예"},
        // 기본 (1글자)
        {"あ", "아"}, {"い", "이"}, {"う", "우"}, {"え", "에"}, {"お", "오"},
        {"か", "카"}, {"き", "키"}, {"く", "쿠"}, {"け", "케"}, {"こ", "코"},
        {"が", "가"}, {"ぎ", "기"}, {"ぐ", "구"}, {"げ", "게"}, {"ご", "고"},
        {"さ", "사"}, {"し", "시"}, {"す", "스"}, {"せ", "세"}, {"そ", "소"},
        {"ざ", "자"}, {"じ", "지"}, {"ず", "즈"}, {"ぜ", "제"}, {"ぞ", "조"},
        {"た", "타"}, {"ち", "치"}, {"つ", "츠"}, {"て", "테"}, {"と", "토"},
        {"だ", "다"}, {"ぢ", "지"}, {"づ", "즈"}, {"で", "데"}, {"ど", "도"},
        {"な", "나"}, {"に", "니"}, {"ぬ", "누"}, {"ね", "네"}, {"の", "노"},
        {"は", "하"}, {"ひ", "히"}, {"ふ", "후"}, {"へ", "헤"}, {"ほ", "호"},
        {"ば", "바"}, {"び", "비"}, {"ぶ", "부"}, {"べ", "베"}, {"ぼ", "보"},
        {"ぱ", "파"}, {"ぴ", "피"}, {"ぷ", "푸"}, {"ぺ", "페"}, {"ぽ", "포"},
        {"ま", "마"}, {"み", "미"}, {"む", "무"}, {"め", "메"}, {"も", "모"},
        {"や", "야"}, {"ゆ", "유"}, {"よ", "요"},
        {"ら", "라"}, {"り", "리"}, {"る", "루"}, {"れ", "레"}, {"ろ", "로"},
        {"わ", "와"}, {"ゐ", "이"}, {"ゑ", "에"}, {"を", "오"}, {"ゔ", "부"},
        {"ぁ", "아"}, {"ぃ", "이"}, {"ぅ", "우"}, {"ぇ", "에"}, {"ぉ", "오"},
        {"ゃ", "야"}, {"ゅ", "유"}, {"ょ", "요"}, {"ゎ", "와"},
    };
    return t;
}

}  // namespace

std::string kanaToKorean(const std::string& utf8) {
    const auto chars = splitChars(katakanaToHiragana(utf8));
    const auto& table = kanaTable();
    std::vector<std::string> out;  // 음절 단위 (받침을 붙이기 위해)
    for (size_t i = 0; i < chars.size(); ++i) {
        const std::string& ch = chars[i];
        unsigned cp = decodeFirst(ch);
        if (ch == "っ") {  // 촉음: 앞 음절에 ㅅ 받침
            if (!out.empty()) out.back() = withFinal(out.back(), 19);
            continue;
        }
        if (ch == "ん") {  // 발음(撥音): 앞 음절에 ㄴ 받침
            if (!out.empty() && isHangul(decodeFirst(out.back())) && (decodeFirst(out.back()) - 0xAC00) % 28 == 0) out.back() = withFinal(out.back(), 4);
            else out.push_back("응");
            continue;
        }
        if (cp == 0x30FC || ch == "ー") continue;  // 장음은 한국어 표기에서 생략
        if (i + 1 < chars.size()) {
            auto it = table.find(ch + chars[i + 1]);
            if (it != table.end()) { out.push_back(it->second); ++i; continue; }
        }
        auto it = table.find(ch);
        if (it != table.end()) out.push_back(it->second);
        else out.push_back(ch);  // 한자, 구두점, 영숫자는 그대로
    }
    std::string s;
    for (const auto& o : out) s += o;
    return s;
}

}  // namespace jp

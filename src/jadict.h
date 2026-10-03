#pragma once
#include <cstdint>
#include <string>
#include <vector>

// 오프라인 일본어 사전 (AI 없이 단어 분할 · 읽기 · 영어 뜻).
//  - ipadic.bin : MeCab 의 IPADIC 을 압축한 형태소 사전 (단어 분할 + 읽기). 비터비 탐색으로 가장 자연스러운 분할을 고른다.
//  - jmdict.bin : JMdict (일영 사전) 의 표제어 · 읽기 · 품사 · 영어 뜻.
// 두 파일은 tools/ja_dict_build 로 만들고 GitHub 릴리스(ja-dict-v1) 에서 받는다.
struct JaMorph {
    std::string surface;   // 문장에 나온 그대로
    std::string reading;   // 히라가나 (모르면 빈 문자열: 사전에 없는 한자어)
    std::string pron;      // 발음 (히라가나, 장음은 ー). 조사 は→わ, とうきょう→とーきょー. 한국어 발음은 이것으로 만든다
    std::string base;      // 기본형 (활용하지 않는 말은 surface 와 같다)
    std::string pos;       // "名詞,一般" 처럼 품사 1·2 단계
    bool unknown = false;  // 사전에 없어 글자 종류로 추정한 말
};

struct JaGloss {
    std::string headword;  // 표제어 (한자 표기, 없으면 가나)
    std::string reading;   // 히라가나
    std::string pos;       // JMdict 품사 태그들 ("v1, vt" 등)
    std::vector<std::string> senses;  // 뜻 (영어), 의미 묶음마다 하나
};

class JaDict {
public:
    static std::string dir();          // <modelsDir>/ja-dict
    static std::string packUrl();      // ja-dict-v1.zip
    static int packSizeMB();
    static bool installed();           // ipadic.bin 이 있는지

    bool load(std::string* err);       // dir() 의 파일을 읽는다 (jmdict 는 없어도 된다)
    bool loaded() const { return !entries_.empty(); }
    bool hasGlosses() const { return !jm_.empty(); }

    std::vector<JaMorph> tokenize(const std::string& text) const;
    // 문장 전체 읽기 (히라가나). spaced 면 형태소 사이에 공백. 읽기를 모르는 글자는 그대로 둔다.
    // pronunciation 이면 읽기 대신 발음(は→わ, 장음) 을 잇는다 — 한국어 발음 표기용.
    std::string toReading(const std::string& text, bool spaced, bool pronunciation = false) const;
    // 기본형/표기 또는 읽기로 찾는다. 읽기가 맞고 품사(ipadicPos 힌트)가 맞는 항목을 앞에 둔다. 최대 limit 개.
    std::vector<JaGloss> lookup(const std::string& word, const std::string& readingHira, const std::string& ipadicPos = "", int limit = 3) const;
    // 형태소의 한국어 발음 (발음 → 없으면 읽기 → 가나만이면 표기)
    static std::string korean(const JaMorph& m);

    // 한국어 품사 이름 ("名詞,一般" → "명사", JMdict "v1" → "1단 동사")
    static std::string posKorean(const std::string& ipadicPos);
    static std::string jmPosKorean(const std::string& tags);

private:
    struct Entry {
        uint32_t surfOff = 0; uint8_t surfLen = 0;
        uint16_t left = 0, right = 0; int16_t cost = 0; uint16_t pos = 0;
        uint32_t baseOff = 0; uint8_t baseLen = 0;
        uint32_t readOff = 0; uint8_t readLen = 0;
        uint32_t pronOff = 0; uint8_t pronLen = 0;
    };
    struct Category { std::string name; uint8_t invoke = 0, group = 0, length = 0; };
    struct Range { uint32_t lo = 0, hi = 0; uint8_t cat = 0; };
    struct Unk { uint8_t cat = 0; uint16_t left = 0, right = 0; int16_t cost = 0; uint16_t pos = 0; };
    struct JmEntry {
        std::vector<std::string> kanji, readings;
        std::string pos;
        std::vector<std::string> senses;
    };
    struct JmKey { std::string key; uint32_t entry; };

    std::string str(uint32_t off, uint8_t len) const { return strs_.substr(off, len); }
    int compareSurface(const Entry& e, const char* s, size_t n) const;
    uint8_t categoryOf(unsigned cp) const;

    std::string strs_;                 // 모든 문자열 (표기 · 기본형 · 읽기)
    std::vector<Entry> entries_;       // 표기 바이트 순 정렬
    std::vector<std::string> pos_;
    uint16_t lsize_ = 0, rsize_ = 0;
    std::vector<int16_t> conn_;        // [prevRight * rsize + curLeft]
    std::vector<Category> cats_;
    std::vector<Range> ranges_;
    std::vector<Unk> unk_;
    std::vector<JmEntry> jm_;
    std::vector<JmKey> jmIndex_;       // key 순 정렬
};

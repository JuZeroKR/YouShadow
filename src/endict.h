#pragma once
#include <string>
#include <unordered_map>
#include <vector>

// 오프라인 영어 사전 (AI 없이 단어 뜻 · 발음 기호 · 품사).
//  - en-dict.tsv : 위키낱말사전(한국어판)의 영어 표제어 한국어 풀이 + 영어 위키낱말사전의 한국어 번역 · 영어 정의 · IPA · 변화형.
// tools/en_dict_build.py 로 만들고 GitHub 릴리스(en-dict-v1) 에서 받는다.
struct EnSense {
    std::string pos;                  // 한국어 품사 이름 ("명사", "동사" …)
    std::vector<std::string> korean;  // 한국어 뜻
    std::vector<std::string> english; // 영어 정의 (한국어 뜻이 없거나 적을 때 보조)
};

struct EnEntry {
    std::string word;   // 표제어 (기본형)
    std::string ipa;    // 발음 기호 (/…/ 없이)
    std::vector<EnSense> senses;
    std::string formNote;  // "get 의 과거형" 처럼 문장 속 형태 설명 (변화형으로 찾았을 때)
    bool empty() const { return senses.empty(); }
};

class EnDict {
public:
    static std::string dir();      // <modelsDir>/en-dict
    static std::string packUrl();  // en-dict-v1.zip
    static int packSizeMB();
    static bool installed();       // en-dict.tsv 가 있는지

    bool load(std::string* err);
    bool loaded() const { return !lemmas_.empty(); }

    // 문장에 나온 단어로 찾는다: 그대로 → 변화형(got → get) → 규칙(-s, -ed, -ing …) → 축약형(doesn't → does).
    EnEntry lookup(const std::string& word) const;

private:
    struct Lemma {
        std::string ipa;
        std::string blocks;  // 원문 (필요할 때 풀어 쓴다)
    };
    struct Form {
        std::string lemma;
        std::string note;   // "과거형" 등
    };
    const Lemma* find(const std::string& key) const;
    EnEntry make(const std::string& key, const Lemma& l) const;

    std::unordered_map<std::string, Lemma> lemmas_;
    std::unordered_map<std::string, std::vector<Form>> forms_;
};

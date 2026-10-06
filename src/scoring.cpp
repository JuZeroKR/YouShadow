#include "scoring.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace {

struct Tok {
    std::string display;
    std::string norm;
};

std::string normalize(const std::string& w) {
    std::string out;
    for (unsigned char c : w) {
        if (std::isalnum(c)) out += (char)std::tolower(c);
    }
    // 숫자/단어 붙임은 그대로 둔다. 아포스트로피는 제거되어 "i'm" == "im".
    return out;
}

// 채점에서 무시할 것: 추임새, whisper 의 효과음 표기 "(clicking)" / "[music]"
bool isFiller(const std::string& n) {
    static const char* fillers[] = {"uh", "um", "umm", "uhh", "hmm", "hm", "mm", "ah", "er", "erm", "mhm"};
    for (const char* f : fillers) if (n == f) return true;
    return false;
}

std::vector<Tok> tokenize(const std::string& s) {
    std::vector<Tok> toks;
    std::istringstream ss(s);
    std::string w;
    bool inBracket = false;
    while (ss >> w) {
        if (!inBracket && (w[0] == '(' || w[0] == '[')) inBracket = true;
        if (inBracket) {
            if (w.back() == ')' || w.back() == ']') inBracket = false;
            continue;
        }
        std::string n = normalize(w);
        if (!n.empty() && !isFiller(n)) toks.push_back({w, n});
    }
    return toks;
}

// 일본어: 글자 단위. 구두점/공백은 빼고, 가타카나는 히라가나로, 전각 영숫자는 반각 소문자로 맞춰 비교한다.
std::vector<Tok> tokenizeJa(const std::string& s) {
    std::vector<Tok> toks;
    for (const auto& ch : jp::splitChars(s)) {
        unsigned cp = jp::decodeFirst(ch);
        if (cp < 0x80) {
            if (!std::isalnum((int)cp)) continue;
            toks.push_back({ch, std::string(1, (char)std::tolower((int)cp))});
            continue;
        }
        if (jp::isJapanesePunct(cp) || cp == 0x3000) continue;
        std::string norm = ch;
        if (cp >= 0xFF10 && cp <= 0xFF5A) norm = std::string(1, (char)std::tolower((int)(cp - 0xFF10 + '0')));
        else norm = jp::katakanaToHiragana(ch);
        toks.push_back({ch, norm});
    }
    return toks;
}

}  // namespace

ScoreResult scoreTranscript(const std::string& reference, const std::string& hypothesis, Lang lang) {
    const auto R = lang == Lang::Ja ? tokenizeJa(reference) : tokenize(reference);
    const auto H = lang == Lang::Ja ? tokenizeJa(hypothesis) : tokenize(hypothesis);
    const size_t n = R.size(), m = H.size();

    // 편집 거리 DP (치환/삽입/삭제 비용 1)
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (size_t i = 0; i <= n; ++i) dp[i][0] = (int)i;
    for (size_t j = 0; j <= m; ++j) dp[0][j] = (int)j;
    for (size_t i = 1; i <= n; ++i) {
        for (size_t j = 1; j <= m; ++j) {
            int sub = dp[i - 1][j - 1] + (R[i - 1].norm == H[j - 1].norm ? 0 : 1);
            dp[i][j] = std::min({sub, dp[i - 1][j] + 1, dp[i][j - 1] + 1});
        }
    }

    // 역추적
    ScoreResult r;
    r.total = (int)n;
    r.heard = hypothesis;
    std::vector<WordMark> rev;
    size_t i = n, j = m;
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0) {
            const bool eq = R[i - 1].norm == H[j - 1].norm;
            if (dp[i][j] == dp[i - 1][j - 1] + (eq ? 0 : 1)) {
                WordMark mk;
                mk.text = R[i - 1].display;
                if (eq) { mk.kind = WordMark::Match; ++r.matched; }
                else { mk.kind = WordMark::Wrong; mk.heard = H[j - 1].display; }
                rev.push_back(mk);
                --i; --j;
                continue;
            }
        }
        if (i > 0 && dp[i][j] == dp[i - 1][j] + 1) {
            rev.push_back({R[i - 1].display, "", WordMark::Missing});
            --i;
        } else {
            rev.push_back({H[j - 1].display, "", WordMark::Extra});
            --j;
        }
    }
    r.marks.assign(rev.rbegin(), rev.rend());
    r.accuracy = n ? 100.0f * r.matched / (float)n : 0.0f;
    return r;
}

std::vector<std::string> scoringTokens(const std::string& text, Lang lang) {
    std::vector<std::string> out;
    for (const auto& t : (lang == Lang::Ja ? tokenizeJa(text) : tokenize(text))) out.push_back(t.display);
    return out;
}

std::vector<int> alignTokens(const std::string& reference, const std::vector<std::string>& hypTokens, Lang lang) {
    const auto R = lang == Lang::Ja ? tokenizeJa(reference) : tokenize(reference);
    std::vector<std::string> H;
    for (const auto& h : hypTokens) {
        auto t = lang == Lang::Ja ? tokenizeJa(h) : tokenize(h);
        // 인식 단어 하나가 토큰 하나가 아닐 수 있다 (추임새 → 0개, 일본어 → 여러 글자). 첫 토큰의 정규형으로 대표하고 빈 것은 빈 문자열
        H.push_back(t.empty() ? std::string() : t[0].norm);
    }
    const size_t n = R.size(), m = H.size();
    // 치환 비용은 두 단어가 비슷할수록 싸다 (ears ↔ years 0.3, ears ↔ yeah 0.7). 비용이 같은 길이 여럿일 때
    // 엉뚱한 단어에 붙는 것을 막는다 — whisper 가 잘못 들은 단어는 대개 원래 단어와 철자가 비슷하다
    auto subCost = [&](size_t i, size_t j) -> float {
        const std::string& a = R[i].norm;
        const std::string& b = H[j];
        if (b.empty()) return 1.f;
        if (a == b) return 0.f;
        std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
        for (size_t y = 0; y <= b.size(); ++y) prev[y] = (int)y;
        for (size_t x = 1; x <= a.size(); ++x) {
            cur[0] = (int)x;
            for (size_t y = 1; y <= b.size(); ++y)
                cur[y] = std::min({prev[y - 1] + (a[x - 1] == b[y - 1] ? 0 : 1), prev[y] + 1, cur[y - 1] + 1});
            std::swap(prev, cur);
        }
        const float d = (float)prev[b.size()] / (float)std::max(a.size(), b.size());
        return 0.3f + 0.7f * d;
    };
    std::vector<std::vector<float>> dp(n + 1, std::vector<float>(m + 1, 0.f));
    for (size_t i = 0; i <= n; ++i) dp[i][0] = (float)i;
    for (size_t j = 0; j <= m; ++j) dp[0][j] = (float)j;
    for (size_t i = 1; i <= n; ++i)
        for (size_t j = 1; j <= m; ++j)
            dp[i][j] = std::min({dp[i - 1][j - 1] + subCost(i - 1, j - 1), dp[i - 1][j] + 1.f, dp[i][j - 1] + 1.f});
    std::vector<int> map(n, -1);
    size_t i = n, j = m;
    const float eps = 1e-4f;
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0 && std::fabs(dp[i][j] - (dp[i - 1][j - 1] + subCost(i - 1, j - 1))) < eps) { map[i - 1] = (int)(j - 1); --i; --j; continue; }
        if (i > 0 && std::fabs(dp[i][j] - (dp[i - 1][j] + 1.f)) < eps) --i;
        else --j;
    }
    return map;
}

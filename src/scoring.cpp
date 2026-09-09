#include "scoring.h"

#include <algorithm>
#include <cctype>
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

}  // namespace

ScoreResult scoreTranscript(const std::string& reference, const std::string& hypothesis) {
    const auto R = tokenize(reference);
    const auto H = tokenize(hypothesis);
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

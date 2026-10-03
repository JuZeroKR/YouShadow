#include "jadict.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>

#include "lang.h"
#include "paths.h"

namespace fs = std::filesystem;

namespace {

struct Reader {
    const std::string& b;
    size_t p = 0;
    explicit Reader(const std::string& buf) : b(buf) {}
    bool ok(size_t n) const { return p + n <= b.size(); }
    uint8_t u8() { return ok(1) ? (uint8_t)b[p++] : 0; }
    uint16_t u16() { uint16_t v = (uint8_t)b[p] | ((uint8_t)b[p + 1] << 8); p += 2; return v; }
    int16_t i16() { return (int16_t)u16(); }
    uint32_t u32() { uint32_t v = (uint8_t)b[p] | ((uint8_t)b[p + 1] << 8) | ((uint8_t)b[p + 2] << 16) | ((uint32_t)(uint8_t)b[p + 3] << 24); p += 4; return v; }
    std::string bytes(size_t n) { std::string s = b.substr(p, n); p += n; return s; }
};

std::string readFile(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) return "";
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct Node {
    int start = 0, end = 0;   // 글자 인덱스
    int entry = -1;           // 사전 항목 (없으면 미지어)
    int unk = -1;             // unk_ 인덱스
    uint16_t left = 0, right = 0;
    int32_t wcost = 0;
    int64_t best = 0;
    int prev = -1;
};

}  // namespace

std::string JaDict::dir() { return paths::modelsDir() + "/ja-dict"; }
std::string JaDict::packUrl() { return "https://github.com/JuZeroKR/YouShadow/releases/download/ja-dict-v1/ja-dict-v1.zip"; }
int JaDict::packSizeMB() { return 17; }
bool JaDict::installed() { std::error_code ec; return fs::exists(fs::u8path(dir() + "/ipadic.bin"), ec); }

bool JaDict::load(std::string* err) {
    std::string buf = readFile(dir() + "/ipadic.bin");
    if (buf.size() < 16 || buf.compare(0, 4, "YSJD") != 0) { if (err) *err = "일본어 사전 파일(ipadic.bin)을 읽을 수 없습니다: " + dir(); return false; }
    Reader r(buf);
    r.p = 4;
    uint32_t version = r.u32();
    if (version != 2) { if (err) *err = "일본어 사전 버전이 맞지 않습니다 (" + std::to_string(version) + "). 사전을 다시 받아 주세요"; return false; }
    uint32_t nPos = r.u32();
    pos_.clear();
    for (uint32_t i = 0; i < nPos; ++i) { uint16_t n = r.u16(); pos_.push_back(r.bytes(n)); }
    lsize_ = r.u16(); rsize_ = r.u16();
    conn_.resize((size_t)lsize_ * rsize_);
    for (auto& c : conn_) c = r.i16();
    uint32_t nEntries = r.u32();
    entries_.clear(); entries_.reserve(nEntries);
    strs_.clear(); strs_.reserve(buf.size());
    for (uint32_t i = 0; i < nEntries; ++i) {
        Entry e;
        e.surfLen = r.u8(); e.surfOff = (uint32_t)strs_.size(); strs_ += r.bytes(e.surfLen);
        e.left = r.u16(); e.right = r.u16(); e.cost = r.i16(); e.pos = r.u16();
        e.baseLen = r.u8(); e.baseOff = (uint32_t)strs_.size(); strs_ += r.bytes(e.baseLen);
        e.readLen = r.u8(); e.readOff = (uint32_t)strs_.size(); strs_ += r.bytes(e.readLen);
        e.pronLen = r.u8(); e.pronOff = (uint32_t)strs_.size(); strs_ += r.bytes(e.pronLen);
        entries_.push_back(e);
    }
    uint8_t nCat = r.u8();
    cats_.clear();
    for (int i = 0; i < nCat; ++i) { Category c; uint8_t n = r.u8(); c.name = r.bytes(n); c.invoke = r.u8(); c.group = r.u8(); c.length = r.u8(); cats_.push_back(c); }
    uint32_t nRanges = r.u32();
    ranges_.clear();
    for (uint32_t i = 0; i < nRanges; ++i) { Range g; g.lo = r.u32(); g.hi = r.u32(); g.cat = r.u8(); ranges_.push_back(g); }
    uint16_t nUnk = r.u16();
    unk_.clear();
    for (int i = 0; i < nUnk; ++i) { Unk u; u.cat = r.u8(); u.left = r.u16(); u.right = r.u16(); u.cost = r.i16(); u.pos = r.u16(); unk_.push_back(u); }
    if (!r.ok(0) || entries_.empty()) { if (err) *err = "일본어 사전 파일이 손상되었습니다"; entries_.clear(); return false; }

    // JMdict (선택)
    jm_.clear(); jmIndex_.clear();
    std::string jb = readFile(dir() + "/jmdict.bin");
    if (jb.size() > 12 && jb.compare(0, 4, "YSJM") == 0) {
        Reader j(jb);
        j.p = 4;
        if (j.u32() == 1) {
            uint32_t n = j.u32();
            jm_.reserve(n);
            for (uint32_t i = 0; i < n; ++i) {
                JmEntry e;
                uint8_t nk = j.u8(); for (int k = 0; k < nk; ++k) { uint8_t l = j.u8(); e.kanji.push_back(j.bytes(l)); }
                uint8_t nr = j.u8(); for (int k = 0; k < nr; ++k) { uint8_t l = j.u8(); e.readings.push_back(j.bytes(l)); }
                uint8_t lp = j.u8(); e.pos = j.bytes(lp);
                uint8_t ng = j.u8(); for (int k = 0; k < ng; ++k) { uint16_t l = j.u16(); e.senses.push_back(j.bytes(l)); }
                jm_.push_back(std::move(e));
            }
            uint32_t ni = j.u32();
            jmIndex_.reserve(ni);
            for (uint32_t i = 0; i < ni; ++i) { JmKey k; uint8_t l = j.u8(); k.key = j.bytes(l); k.entry = j.u32(); jmIndex_.push_back(std::move(k)); }
        }
    }
    return true;
}

int JaDict::compareSurface(const Entry& e, const char* s, size_t n) const {
    size_t m = std::min<size_t>(e.surfLen, n);
    int c = std::memcmp(strs_.data() + e.surfOff, s, m);
    if (c != 0) return c;
    return (int)e.surfLen - (int)n;
}

uint8_t JaDict::categoryOf(unsigned cp) const {
    for (const auto& g : ranges_) if (cp >= g.lo && cp <= g.hi) return g.cat;
    return 0;  // DEFAULT
}

std::vector<JaMorph> JaDict::tokenize(const std::string& text) const {
    std::vector<JaMorph> out;
    if (!loaded()) return out;

    // 글자 단위로 나누고 공백에서 끊는다 (MeCab 도 공백을 건너뛴다)
    std::vector<std::string> chars = jp::splitChars(text);
    std::vector<unsigned> cps;
    for (const auto& c : chars) cps.push_back(jp::decodeFirst(c));

    auto isSpace = [](unsigned cp) { return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x3000; };

    size_t chunkStart = 0;
    while (chunkStart < chars.size()) {
        while (chunkStart < chars.size() && isSpace(cps[chunkStart])) ++chunkStart;
        size_t chunkEnd = chunkStart;
        while (chunkEnd < chars.size() && !isSpace(cps[chunkEnd])) ++chunkEnd;
        if (chunkStart >= chunkEnd) break;

        // 바이트 오프셋
        const int n = (int)(chunkEnd - chunkStart);
        std::vector<size_t> off(n + 1, 0);
        std::string chunk;
        for (int i = 0; i < n; ++i) { off[i] = chunk.size(); chunk += chars[chunkStart + i]; }
        off[n] = chunk.size();

        // 격자 만들기
        std::vector<Node> nodes;
        std::vector<std::vector<int>> endingAt(n + 1);  // 이 글자 위치에서 끝나는 노드들
        Node bos; bos.start = bos.end = 0; bos.left = bos.right = 0; bos.best = 0; bos.prev = -1;
        nodes.push_back(bos);
        endingAt[0].push_back(0);

        for (int i = 0; i < n; ++i) {
            bool found = false;
            // 사전: 공통 접두어 검색 (최대 24글자)
            for (int j = i + 1; j <= std::min(n, i + 24); ++j) {
                const char* s = chunk.data() + off[i];
                size_t len = off[j] - off[i];
                if (len > 255) break;
                auto lo = std::lower_bound(entries_.begin(), entries_.end(), 0, [&](const Entry& e, int) { return compareSurface(e, s, len) < 0; });
                for (auto it = lo; it != entries_.end() && compareSurface(*it, s, len) == 0; ++it) {
                    Node nd; nd.start = i; nd.end = j; nd.entry = (int)(it - entries_.begin());
                    nd.left = it->left; nd.right = it->right; nd.wcost = it->cost;
                    nodes.push_back(nd);
                    endingAt[j].push_back((int)nodes.size() - 1);
                    found = true;
                }
            }
            // 미지어: 글자 종류로 추정
            const uint8_t cat = categoryOf(cps[chunkStart + i]);
            const Category& c = cats_.empty() ? Category() : cats_[std::min<size_t>(cat, cats_.size() - 1)];
            if (!found || c.invoke) {
                std::vector<int> lens;
                if (c.group) {
                    int j = i + 1;
                    while (j < n && categoryOf(cps[chunkStart + j]) == cat && j - i < 48) ++j;
                    lens.push_back(j - i);
                }
                for (int l = 1; l <= c.length && i + l <= n; ++l) {
                    bool same = true;
                    for (int k = 1; k < l; ++k) if (categoryOf(cps[chunkStart + i + k]) != cat) { same = false; break; }
                    if (same && std::find(lens.begin(), lens.end(), l) == lens.end()) lens.push_back(l);
                }
                if (lens.empty()) lens.push_back(1);
                for (int l : lens) {
                    for (size_t u = 0; u < unk_.size(); ++u) {
                        if (unk_[u].cat != cat) continue;
                        Node nd; nd.start = i; nd.end = i + l; nd.unk = (int)u;
                        nd.left = unk_[u].left; nd.right = unk_[u].right; nd.wcost = unk_[u].cost;
                        nodes.push_back(nd);
                        endingAt[i + l].push_back((int)nodes.size() - 1);
                    }
                }
            }
        }
        Node eos; eos.start = eos.end = n; eos.left = eos.right = 0;
        nodes.push_back(eos);
        const int eosIdx = (int)nodes.size() - 1;

        // 비터비: 끝 위치 순서로 (노드는 start 오름차순으로 만들어졌으므로 인덱스 순으로 처리해도 된다)
        const int64_t INF = std::numeric_limits<int64_t>::max() / 4;
        for (int k = 1; k < (int)nodes.size(); ++k) {
            Node& nd = nodes[k];
            nd.best = INF;
            for (int p : endingAt[nd.start]) {
                const Node& pv = nodes[p];
                if (pv.best >= INF) continue;
                int16_t cc = 0;
                if (pv.right < lsize_ && nd.left < rsize_) cc = conn_[(size_t)pv.right * rsize_ + nd.left];
                int64_t c = pv.best + cc + nd.wcost;
                if (c < nd.best) { nd.best = c; nd.prev = p; }
            }
        }
        // EOS 는 마지막 위치에서 끝나는 노드들과 잇는다
        {
            Node& e = nodes[eosIdx];
            e.best = INF;
            for (int p : endingAt[n]) {
                if (p == eosIdx) continue;
                const Node& pv = nodes[p];
                if (pv.best >= INF) continue;
                int16_t cc = pv.right < lsize_ ? conn_[(size_t)pv.right * rsize_ + 0] : 0;
                int64_t c = pv.best + cc;
                if (c < e.best) { e.best = c; e.prev = p; }
            }
        }

        // 역추적
        std::vector<JaMorph> chunkOut;
        for (int k = nodes[eosIdx].prev; k > 0; k = nodes[k].prev) {
            const Node& nd = nodes[k];
            JaMorph m;
            m.surface = chunk.substr(off[nd.start], off[nd.end] - off[nd.start]);
            if (nd.entry >= 0) {
                const Entry& e = entries_[nd.entry];
                m.base = e.baseLen ? str(e.baseOff, e.baseLen) : m.surface;
                m.reading = e.readLen ? jp::katakanaToHiragana(str(e.readOff, e.readLen)) : "";
                m.pron = e.pronLen ? jp::katakanaToHiragana(str(e.pronOff, e.pronLen)) : m.reading;
                m.pos = e.pos < pos_.size() ? pos_[e.pos] : "";
            } else {
                m.unknown = true;
                m.base = m.surface;
                m.pos = nd.unk >= 0 && unk_[nd.unk].pos < pos_.size() ? pos_[unk_[nd.unk].pos] : "";
                // 가나만으로 된 미지어는 읽기를 안다
                bool kanaOnly = true;
                for (const auto& ch : jp::splitChars(m.surface)) { unsigned cp = jp::decodeFirst(ch); if (!(jp::isHiragana(cp) || jp::isKatakana(cp) || cp == 0x30FC)) { kanaOnly = false; break; } }
                if (kanaOnly) m.reading = jp::katakanaToHiragana(m.surface);
                m.pron = m.reading;
            }
            chunkOut.push_back(std::move(m));
            if (nd.prev <= 0) break;
        }
        out.insert(out.end(), chunkOut.rbegin(), chunkOut.rend());
        chunkStart = chunkEnd;
    }
    return out;
}

std::string JaDict::toReading(const std::string& text, bool spaced, bool pronunciation) const {
    std::string out;
    for (const auto& m : tokenize(text)) {
        if (m.pos.rfind("記号", 0) == 0) continue;  // 구두점은 읽기에서 뺀다
        if (spaced && !out.empty()) out += ' ';
        const std::string& r = pronunciation ? m.pron : m.reading;
        out += r.empty() ? m.surface : r;
    }
    return out;
}

std::string JaDict::korean(const JaMorph& m) {
    const std::string& r = !m.pron.empty() ? m.pron : m.reading;
    if (!r.empty()) return jp::kanaToKorean(r);
    return "";
}

namespace {

// IPADIC 품사 → JMdict 품사 태그 묶음 (뜻 후보 고르기용)
bool jmPosMatches(const std::string& ipadicPos, const std::string& jmTags) {
    auto has = [&](const char* t) {
        // 태그 목록(쉼표 구분) 에 t 가 통째로 들어 있는지
        size_t p = 0;
        while (p <= jmTags.size()) {
            size_t q = jmTags.find(',', p);
            std::string tag = jmTags.substr(p, q == std::string::npos ? std::string::npos : q - p);
            if (tag == t) return true;
            if (q == std::string::npos) break;
            p = q + 1;
        }
        return false;
    };
    auto prefix = [&](const char* t) {
        size_t p = 0;
        while (p <= jmTags.size()) {
            size_t q = jmTags.find(',', p);
            std::string tag = jmTags.substr(p, q == std::string::npos ? std::string::npos : q - p);
            if (tag.rfind(t, 0) == 0) return true;
            if (q == std::string::npos) break;
            p = q + 1;
        }
        return false;
    };
    if (ipadicPos.rfind("助詞", 0) == 0) return has("prt") || has("conj");
    if (ipadicPos.rfind("助動詞", 0) == 0) return prefix("aux") || has("cop") || has("prt");
    if (ipadicPos.rfind("動詞", 0) == 0) return prefix("v1") || prefix("v5") || has("vk") || prefix("vs") || has("vz") || prefix("aux-v") || has("v4r") || has("v2a-s");
    if (ipadicPos.rfind("形容詞", 0) == 0) return prefix("adj-i");
    if (ipadicPos.rfind("副詞", 0) == 0) return prefix("adv");
    if (ipadicPos.rfind("接続詞", 0) == 0) return has("conj");
    if (ipadicPos.rfind("感動詞", 0) == 0) return has("int");
    if (ipadicPos.rfind("連体詞", 0) == 0) return has("adj-pn");
    if (ipadicPos.rfind("接頭詞", 0) == 0) return has("pref") || prefix("n-pref");
    if (ipadicPos.rfind("名詞,接尾", 0) == 0) return has("suf") || has("n-suf") || has("ctr");
    if (ipadicPos.rfind("名詞,形容動詞語幹", 0) == 0) return has("adj-na") || has("n");
    if (ipadicPos.rfind("名詞,代名詞", 0) == 0) return has("pn");
    if (ipadicPos.rfind("名詞", 0) == 0) return has("n") || prefix("n-") || has("num") || has("vs") || has("adj-na") || has("pn") || has("exp");
    return false;
}

}  // namespace

std::vector<JaGloss> JaDict::lookup(const std::string& word, const std::string& readingHira, const std::string& ipadicPos, int limit) const {
    std::vector<JaGloss> out;
    if (jm_.empty() || word.empty()) return out;
    auto find = [&](const std::string& key) {
        std::vector<uint32_t> ids;
        auto lo = std::lower_bound(jmIndex_.begin(), jmIndex_.end(), key, [](const JmKey& k, const std::string& s) { return k.key < s; });
        for (auto it = lo; it != jmIndex_.end() && it->key == key; ++it) ids.push_back(it->entry);
        return ids;
    };
    std::vector<uint32_t> ids = find(word);
    if (ids.empty() && !readingHira.empty() && readingHira != word) ids = find(readingHira);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

    // 가나로만 쓴 단어인가 (한자가 없으면 '보통 가나로 쓰는' 항목이나 한자 표기가 없는 항목을 우선)
    bool kanaWord = true;
    for (const auto& ch : jp::splitChars(word)) if (jp::isKanji(jp::decodeFirst(ch))) { kanaWord = false; break; }

    auto score = [&](uint32_t id) {
        const JmEntry& e = jm_[id];
        int s = 0;
        if (!readingHira.empty()) for (const auto& r : e.readings) if (r == readingHira) { s += 2; break; }
        if (!ipadicPos.empty() && jmPosMatches(ipadicPos, e.pos)) s += 4;
        if (kanaWord) {
            bool uk = e.pos.find("uk") != std::string::npos;
            if (e.kanji.empty() || uk) s += 3;
            for (const auto& k : e.kanji) if (k == word) { s += 1; break; }
        } else {
            for (const auto& k : e.kanji) if (k == word) { s += 1; break; }
        }
        return s;
    };
    std::stable_sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) { return score(a) > score(b); });
    for (uint32_t id : ids) {
        if ((int)out.size() >= limit) break;
        const JmEntry& e = jm_[id];
        JaGloss g;
        g.headword = e.kanji.empty() ? (e.readings.empty() ? word : e.readings[0]) : e.kanji[0];
        g.reading = e.readings.empty() ? "" : e.readings[0];
        for (const auto& r : e.readings) if (r == readingHira) { g.reading = r; break; }
        g.pos = e.pos;
        g.senses = e.senses;
        out.push_back(std::move(g));
    }
    return out;
}

std::string JaDict::posKorean(const std::string& p) {
    static const std::pair<const char*, const char*> table[] = {
        {"名詞,固有名詞", "고유명사"}, {"名詞,代名詞", "대명사"}, {"名詞,数", "수사"}, {"名詞,副詞可能", "명사(부사적)"},
        {"名詞,サ変接続", "명사(する동사 어간)"}, {"名詞,形容動詞語幹", "な형용사 어간"}, {"名詞,接尾", "접미사"}, {"名詞,非自立", "형식명사"},
        {"名詞", "명사"}, {"動詞,自立", "동사"}, {"動詞,非自立", "보조동사"}, {"動詞", "동사"}, {"形容詞", "い형용사"},
        {"副詞", "부사"}, {"助詞,格助詞", "격조사"}, {"助詞,係助詞", "계조사(은/는 등)"}, {"助詞,接続助詞", "접속조사"},
        {"助詞,終助詞", "종조사"}, {"助詞,副助詞", "부조사"}, {"助詞,連体化", "조사(の)"}, {"助詞", "조사"},
        {"助動詞", "조동사"}, {"連体詞", "연체사"}, {"接続詞", "접속사"}, {"感動詞", "감탄사"}, {"接頭詞", "접두사"},
        {"フィラー", "추임새"}, {"記号", "기호"},
    };
    for (const auto& [k, v] : table) if (p.rfind(k, 0) == 0) return v;
    return p;
}

std::string JaDict::jmPosKorean(const std::string& tags) {
    static const std::map<std::string, std::string> table = {
        {"n", "명사"}, {"pn", "대명사"}, {"num", "수사"}, {"ctr", "조수사"}, {"n-suf", "명사 접미"}, {"n-pref", "명사 접두"}, {"n-adv", "부사적 명사"}, {"n-t", "시간 명사"},
        {"v1", "1단 동사"}, {"v5u", "5단 동사"}, {"v5k", "5단 동사"}, {"v5g", "5단 동사"}, {"v5s", "5단 동사"}, {"v5t", "5단 동사"}, {"v5n", "5단 동사"},
        {"v5b", "5단 동사"}, {"v5m", "5단 동사"}, {"v5r", "5단 동사"}, {"v5r-i", "5단 동사"}, {"v5k-s", "5단 동사"}, {"v5u-s", "5단 동사"}, {"vk", "불규칙 동사(来る)"}, {"vs-i", "불규칙 동사(する)"}, {"vs", "する동사"}, {"vs-s", "する동사"},
        {"vt", "타동사"}, {"vi", "자동사"}, {"adj-i", "い형용사"}, {"adj-ix", "い형용사"}, {"adj-na", "な형용사"}, {"adj-no", "の형용사"}, {"adj-f", "연체 수식"}, {"adj-t", "たる형용사"}, {"adj-pn", "연체사"},
        {"adv", "부사"}, {"adv-to", "と부사"}, {"prt", "조사"}, {"conj", "접속사"}, {"int", "감탄사"}, {"exp", "표현"}, {"aux", "조동사"}, {"aux-v", "조동사"}, {"aux-adj", "보조 형용사"},
        {"pref", "접두사"}, {"suf", "접미사"}, {"cop", "계사(だ)"}, {"unc", "미분류"},
    };
    std::string out;
    size_t p = 0;
    while (p <= tags.size()) {
        size_t q = tags.find(',', p);
        std::string t = tags.substr(p, q == std::string::npos ? std::string::npos : q - p);
        while (!t.empty() && t[0] == ' ') t.erase(0, 1);
        if (!t.empty() && t != "uk") {
            auto it = table.find(t);
            std::string k = it != table.end() ? it->second : t;
            if (out.find(k) == std::string::npos) { if (!out.empty()) out += " · "; out += k; }
        }
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return out;
}

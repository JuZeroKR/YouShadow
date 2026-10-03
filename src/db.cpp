#include "db.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "sqlite3.h"

namespace {

// prepared statement 를 RAII 로 감싼 작은 헬퍼
class Stmt {
public:
    Stmt(sqlite3* db, const std::string& sql) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("sqlite prepare: ") + sqlite3_errmsg(db));
        }
    }
    ~Stmt() { if (st_) sqlite3_finalize(st_); }
    Stmt& bind(int i, const std::string& v) { sqlite3_bind_text(st_, i, v.c_str(), -1, SQLITE_TRANSIENT); return *this; }
    Stmt& bind(int i, int v) { sqlite3_bind_int(st_, i, v); return *this; }
    Stmt& bind(int i, long long v) { sqlite3_bind_int64(st_, i, v); return *this; }
    Stmt& bind(int i, double v) { sqlite3_bind_double(st_, i, v); return *this; }
    bool step() { return sqlite3_step(st_) == SQLITE_ROW; }
    void run() { sqlite3_step(st_); }
    int colInt(int i) const { return sqlite3_column_int(st_, i); }
    long long colInt64(int i) const { return sqlite3_column_int64(st_, i); }
    double colDouble(int i) const { return sqlite3_column_double(st_, i); }
    bool colNull(int i) const { return sqlite3_column_type(st_, i) == SQLITE_NULL; }
    std::string colText(int i) const {
        const unsigned char* t = sqlite3_column_text(st_, i);
        return t ? reinterpret_cast<const char*>(t) : "";
    }

private:
    sqlite3_stmt* st_ = nullptr;
};

const char* kSchema = R"(
CREATE TABLE IF NOT EXISTS videos(
  id TEXT PRIMARY KEY, title TEXT, duration_ms INTEGER, added_at TEXT, last_opened_at TEXT);
CREATE TABLE IF NOT EXISTS segments(
  video_id TEXT, idx INTEGER, start_ms INTEGER, end_ms INTEGER, text TEXT,
  PRIMARY KEY(video_id, idx));
CREATE TABLE IF NOT EXISTS practices(
  id INTEGER PRIMARY KEY AUTOINCREMENT, video_id TEXT, seg_idx INTEGER, mode TEXT,
  at TEXT, recording_path TEXT, score REAL);
CREATE TABLE IF NOT EXISTS segment_state(
  video_id TEXT, seg_idx INTEGER, bookmarked INTEGER DEFAULT 0, ease REAL DEFAULT 2.5,
  interval_days REAL DEFAULT 0, due_at TEXT, reviews INTEGER DEFAULT 0, lapses INTEGER DEFAULT 0,
  PRIMARY KEY(video_id, seg_idx));
CREATE INDEX IF NOT EXISTS idx_practices_video ON practices(video_id, seg_idx);
CREATE INDEX IF NOT EXISTS idx_state_due ON segment_state(due_at);
CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE IF NOT EXISTS explanations(
  video_id TEXT, seg_idx INTEGER, json TEXT, created_at TEXT, PRIMARY KEY(video_id, seg_idx));
CREATE TABLE IF NOT EXISTS expressions(
  id INTEGER PRIMARY KEY AUTOINCREMENT, video_id TEXT, seg_idx INTEGER,
  text TEXT, meaning TEXT, note TEXT, example TEXT, created_at TEXT,
  ease REAL DEFAULT 2.5, interval_days REAL DEFAULT 0, due_at TEXT, reviews INTEGER DEFAULT 0, lapses INTEGER DEFAULT 0);
CREATE INDEX IF NOT EXISTS idx_expr_due ON expressions(due_at);
CREATE TABLE IF NOT EXISTS word_meanings(
  word TEXT, sentence TEXT, json TEXT, created_at TEXT, PRIMARY KEY(word, sentence));
)";

const char* kReviewSelect =
    "SELECT s.video_id, COALESCE(v.title, s.video_id), s.seg_idx, COALESCE(g.start_ms, 0), COALESCE(g.text, ''), COALESCE(s.due_at, '') "
    "FROM segment_state s "
    "LEFT JOIN segments g ON g.video_id = s.video_id AND g.idx = s.seg_idx "
    "LEFT JOIN videos v ON v.id = s.video_id ";

std::vector<ReviewItem> collect(Stmt& st) {
    std::vector<ReviewItem> out;
    while (st.step()) {
        ReviewItem it;
        it.videoId = st.colText(0);
        it.title = st.colText(1);
        it.segIdx = st.colInt(2);
        it.startMs = st.colInt(3);
        it.text = st.colText(4);
        it.dueAt = st.colText(5);
        out.push_back(it);
    }
    return out;
}

}  // namespace

Db::~Db() {
    if (db_) sqlite3_close(db_);
}

void Db::exec(const std::string& sql) {
    char* msg = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &msg) != SQLITE_OK) {
        std::string e = msg ? msg : "unknown";
        sqlite3_free(msg);
        throw std::runtime_error("sqlite exec: " + e);
    }
}

bool Db::open(const std::string& path, std::string* err) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        if (err) *err = std::string("DB 열기 실패: ") + sqlite3_errmsg(db_);
        return false;
    }
    try {
        exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
        exec(kSchema);
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
    return true;
}

// ---------------- 영상 / 세그먼트 ----------------

void Db::upsertVideo(const std::string& id, const std::string& title, int durationMs) {
    Stmt(db_, "INSERT INTO videos(id, title, duration_ms, added_at, last_opened_at) VALUES(?,?,?,?,?) "
              "ON CONFLICT(id) DO UPDATE SET title=excluded.title, duration_ms=excluded.duration_ms, last_opened_at=excluded.last_opened_at")
        .bind(1, id).bind(2, title).bind(3, durationMs).bind(4, now()).bind(5, now()).run();
}

void Db::upsertSegments(const std::string& id, const std::vector<Segment>& segs) {
    exec("BEGIN");
    for (const auto& s : segs) {
        Stmt(db_, "INSERT OR REPLACE INTO segments(video_id, idx, start_ms, end_ms, text) VALUES(?,?,?,?,?)")
            .bind(1, id).bind(2, s.idx).bind(3, s.startMs).bind(4, s.endMs).bind(5, s.text).run();
    }
    exec("COMMIT");
}

// ---------------- 연습 이력 ----------------

long long Db::addPractice(const PracticeRow& row) {
    Stmt st(db_, "INSERT INTO practices(video_id, seg_idx, mode, at, recording_path, score) VALUES(?,?,?,?,?,?)");
    st.bind(1, row.videoId).bind(2, row.segIdx).bind(3, row.mode).bind(4, row.at).bind(5, row.recordingPath);
    if (row.score >= 0) st.bind(6, row.score);
    st.run();
    // ensureState 의 INSERT 가 last_insert_rowid 를 덮어쓰므로 먼저 읽어 둔다
    long long id = sqlite3_last_insert_rowid(db_);
    ensureState(row.videoId, row.segIdx);
    return id;
}

void Db::setPracticeScore(long long id, double score) {
    Stmt(db_, "UPDATE practices SET score=? WHERE id=?").bind(1, score).bind(2, id).run();
}

std::map<int, int> Db::countsFor(const std::string& videoId) const {
    std::map<int, int> out;
    Stmt st(db_, "SELECT seg_idx, COUNT(*) FROM practices WHERE video_id=? GROUP BY seg_idx");
    st.bind(1, videoId);
    while (st.step()) out[st.colInt(0)] = st.colInt(1);
    return out;
}

std::map<int, double> Db::bestScoresFor(const std::string& videoId) const {
    std::map<int, double> out;
    Stmt st(db_, "SELECT seg_idx, MAX(score) FROM practices WHERE video_id=? AND score IS NOT NULL GROUP BY seg_idx");
    st.bind(1, videoId);
    while (st.step()) out[st.colInt(0)] = st.colDouble(1);
    return out;
}

std::string Db::lastRecording(const std::string& videoId, int segIdx) const {
    Stmt st(db_, "SELECT recording_path FROM practices WHERE video_id=? AND seg_idx=? AND recording_path<>'' ORDER BY id DESC LIMIT 1");
    st.bind(1, videoId).bind(2, segIdx);
    return st.step() ? st.colText(0) : "";
}

// ---------------- 간격 반복 / 북마크 ----------------

void Db::ensureState(const std::string& videoId, int segIdx) {
    // 처음 연습한 문장은 내일 복습 대상으로 자동 등록한다.
    Stmt(db_, "INSERT OR IGNORE INTO segment_state(video_id, seg_idx, interval_days, due_at) VALUES(?,?,1,?)")
        .bind(1, videoId).bind(2, segIdx).bind(3, fromNow(1)).run();
}

SegmentState Db::state(const std::string& videoId, int segIdx) const {
    SegmentState s;
    Stmt st(db_, "SELECT bookmarked, ease, interval_days, due_at, reviews, lapses FROM segment_state WHERE video_id=? AND seg_idx=?");
    st.bind(1, videoId).bind(2, segIdx);
    if (st.step()) {
        s.bookmarked = st.colInt(0) != 0;
        s.ease = st.colDouble(1);
        s.intervalDays = st.colDouble(2);
        s.dueAt = st.colText(3);
        s.reviews = st.colInt(4);
        s.lapses = st.colInt(5);
    }
    return s;
}

void Db::rate(const std::string& videoId, int segIdx, Grade grade) {
    ensureState(videoId, segIdx);
    SegmentState s = state(videoId, segIdx);
    // SM-2 를 단순화한 규칙
    if (grade == Hard) {
        s.ease = std::max(1.3, s.ease - 0.2);
        s.intervalDays = 0.5;  // 반나절 뒤 다시
        s.lapses++;
    } else {
        if (s.reviews == 0) s.intervalDays = 1;
        else if (s.reviews == 1) s.intervalDays = 3;
        else s.intervalDays = std::max(1.0, s.intervalDays * s.ease);
        if (grade == Easy) {
            s.intervalDays *= 1.3;
            s.ease += 0.15;
        }
    }
    s.reviews++;
    s.dueAt = fromNow(s.intervalDays);
    Stmt(db_, "UPDATE segment_state SET ease=?, interval_days=?, due_at=?, reviews=?, lapses=? WHERE video_id=? AND seg_idx=?")
        .bind(1, s.ease).bind(2, s.intervalDays).bind(3, s.dueAt).bind(4, s.reviews).bind(5, s.lapses)
        .bind(6, videoId).bind(7, segIdx).run();
}

void Db::setBookmark(const std::string& videoId, int segIdx, bool on) {
    Stmt(db_, "INSERT INTO segment_state(video_id, seg_idx, bookmarked) VALUES(?,?,?) "
              "ON CONFLICT(video_id, seg_idx) DO UPDATE SET bookmarked=excluded.bookmarked")
        .bind(1, videoId).bind(2, segIdx).bind(3, on ? 1 : 0).run();
}

std::set<int> Db::bookmarksFor(const std::string& videoId) const {
    std::set<int> out;
    Stmt st(db_, "SELECT seg_idx FROM segment_state WHERE video_id=? AND bookmarked=1");
    st.bind(1, videoId);
    while (st.step()) out.insert(st.colInt(0));
    return out;
}

std::vector<ReviewItem> Db::due(int limit) const {
    Stmt st(db_, std::string(kReviewSelect) + "WHERE s.due_at IS NOT NULL AND s.due_at <= ? ORDER BY s.due_at LIMIT ?");
    st.bind(1, now()).bind(2, limit);
    return collect(st);
}

int Db::dueCount() const {
    Stmt st(db_, "SELECT COUNT(*) FROM segment_state WHERE due_at IS NOT NULL AND due_at <= ?");
    st.bind(1, now());
    return st.step() ? st.colInt(0) : 0;
}

std::vector<ReviewItem> Db::bookmarks(int limit) const {
    Stmt st(db_, std::string(kReviewSelect) + "WHERE s.bookmarked=1 ORDER BY v.title, s.seg_idx LIMIT ?");
    st.bind(1, limit);
    return collect(st);
}

// ---------------- 라이브러리 / 통계 / 이력 ----------------

std::vector<VideoSummary> Db::videos() const {
    std::vector<VideoSummary> out;
    Stmt st(db_,
        "SELECT v.id, COALESCE(v.title, v.id), COALESCE(v.duration_ms, 0), COALESCE(v.added_at, ''), COALESCE(v.last_opened_at, ''), "
        " (SELECT COUNT(*) FROM segments g WHERE g.video_id = v.id), "
        " (SELECT COUNT(DISTINCT p.seg_idx) FROM practices p WHERE p.video_id = v.id), "
        " (SELECT COUNT(*) FROM practices p WHERE p.video_id = v.id), "
        " (SELECT AVG(p.score) FROM practices p WHERE p.video_id = v.id AND p.score IS NOT NULL), "
        " (SELECT COALESCE(MAX(p.at), '') FROM practices p WHERE p.video_id = v.id), "
        " (SELECT COUNT(*) FROM segment_state s WHERE s.video_id = v.id AND s.due_at IS NOT NULL AND s.due_at <= ?) "
        "FROM videos v ORDER BY v.last_opened_at DESC");
    st.bind(1, now());
    while (st.step()) {
        VideoSummary v;
        v.id = st.colText(0);
        v.title = st.colText(1);
        v.durationMs = st.colInt(2);
        v.addedAt = st.colText(3);
        v.lastOpenedAt = st.colText(4);
        v.segCount = st.colInt(5);
        v.practicedSegs = st.colInt(6);
        v.practiceCount = st.colInt(7);
        v.avgScore = st.colNull(8) ? -1 : st.colDouble(8);
        v.lastPracticeAt = st.colText(9);
        v.dueCount = st.colInt(10);
        out.push_back(v);
    }
    return out;
}

std::vector<DayStat> Db::dailyStats(int days) const {
    std::vector<DayStat> out;
    Stmt st(db_, "SELECT substr(at, 1, 10) AS d, COUNT(*), AVG(score) FROM practices "
                 "WHERE at >= ? GROUP BY d ORDER BY d");
    st.bind(1, fromNow(-days).substr(0, 10));
    while (st.step()) {
        DayStat s;
        s.date = st.colText(0);
        s.count = st.colInt(1);
        s.avgScore = st.colNull(2) ? -1 : st.colDouble(2);
        out.push_back(s);
    }
    return out;
}

std::vector<PracticeHistory> Db::practicesFor(const std::string& videoId, int segIdx) const {
    std::vector<PracticeHistory> out;
    Stmt st(db_, "SELECT id, mode, at, COALESCE(recording_path, ''), score FROM practices "
                 "WHERE video_id = ? AND seg_idx = ? ORDER BY id DESC");
    st.bind(1, videoId).bind(2, segIdx);
    while (st.step()) {
        PracticeHistory h;
        h.id = st.colInt(0);
        h.mode = st.colText(1);
        h.at = st.colText(2);
        h.recordingPath = st.colText(3);
        h.score = st.colNull(4) ? -1 : st.colDouble(4);
        out.push_back(h);
    }
    return out;
}

int Db::totalPractices() const {
    Stmt st(db_, "SELECT COUNT(*) FROM practices");
    return st.step() ? st.colInt(0) : 0;
}

void Db::deleteVideo(const std::string& videoId) {
    exec("BEGIN");
    Stmt(db_, "DELETE FROM practices WHERE video_id = ?").bind(1, videoId).run();
    Stmt(db_, "DELETE FROM segment_state WHERE video_id = ?").bind(1, videoId).run();
    Stmt(db_, "DELETE FROM segments WHERE video_id = ?").bind(1, videoId).run();
    Stmt(db_, "DELETE FROM videos WHERE id = ?").bind(1, videoId).run();
    exec("COMMIT");
}

// ---------------- 설정 / 해설 / 표현 카드 ----------------

std::string Db::getSetting(const std::string& key, const std::string& def) const {
    Stmt st(db_, "SELECT value FROM settings WHERE key=?");
    st.bind(1, key);
    return st.step() ? st.colText(0) : def;
}

void Db::setSetting(const std::string& key, const std::string& value) {
    Stmt(db_, "INSERT INTO settings(key, value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value")
        .bind(1, key).bind(2, value).run();
}

std::string Db::getExplanation(const std::string& videoId, int segIdx) const {
    Stmt st(db_, "SELECT json FROM explanations WHERE video_id=? AND seg_idx=?");
    st.bind(1, videoId).bind(2, segIdx);
    return st.step() ? st.colText(0) : "";
}

void Db::setExplanation(const std::string& videoId, int segIdx, const std::string& json) {
    Stmt(db_, "INSERT OR REPLACE INTO explanations(video_id, seg_idx, json, created_at) VALUES(?,?,?,?)")
        .bind(1, videoId).bind(2, segIdx).bind(3, json).bind(4, now()).run();
}

std::string Db::getWordMeaning(const std::string& word, const std::string& sentence) const {
    Stmt st(db_, "SELECT json FROM word_meanings WHERE word=? AND sentence=?");
    st.bind(1, word).bind(2, sentence);
    return st.step() ? st.colText(0) : "";
}

void Db::setWordMeaning(const std::string& word, const std::string& sentence, const std::string& json) {
    Stmt(db_, "INSERT OR REPLACE INTO word_meanings(word, sentence, json, created_at) VALUES(?,?,?,?)")
        .bind(1, word).bind(2, sentence).bind(3, json).bind(4, now()).run();
}

std::set<int> Db::explainedSegments(const std::string& videoId) const {
    std::set<int> out;
    Stmt st(db_, "SELECT seg_idx FROM explanations WHERE video_id=?");
    st.bind(1, videoId);
    while (st.step()) out.insert(st.colInt(0));
    return out;
}

long long Db::addExpression(const std::string& videoId, int segIdx, const std::string& text,
                            const std::string& meaning, const std::string& note, const std::string& example) {
    Stmt(db_, "INSERT INTO expressions(video_id, seg_idx, text, meaning, note, example, created_at, interval_days, due_at) "
              "VALUES(?,?,?,?,?,?,?,1,?)")
        .bind(1, videoId).bind(2, segIdx).bind(3, text).bind(4, meaning).bind(5, note).bind(6, example)
        .bind(7, now()).bind(8, fromNow(1)).run();
    return sqlite3_last_insert_rowid(db_);
}

void Db::deleteExpression(long long id) {
    Stmt(db_, "DELETE FROM expressions WHERE id=?").bind(1, id).run();
}

bool Db::hasExpression(const std::string& videoId, int segIdx, const std::string& text) const {
    Stmt st(db_, "SELECT 1 FROM expressions WHERE video_id=? AND seg_idx=? AND text=? LIMIT 1");
    st.bind(1, videoId).bind(2, segIdx).bind(3, text);
    return st.step();
}

std::vector<ExpressionCard> Db::expressions(int limit) const {
    std::vector<ExpressionCard> out;
    Stmt st(db_, "SELECT id, video_id, seg_idx, text, meaning, note, example, created_at, COALESCE(due_at,''), reviews, interval_days "
                 "FROM expressions ORDER BY id DESC LIMIT ?");
    st.bind(1, limit);
    while (st.step()) {
        ExpressionCard c;
        c.id = st.colInt64(0);
        c.videoId = st.colText(1);
        c.segIdx = st.colInt(2);
        c.text = st.colText(3);
        c.meaning = st.colText(4);
        c.note = st.colText(5);
        c.example = st.colText(6);
        c.createdAt = st.colText(7);
        c.dueAt = st.colText(8);
        c.reviews = st.colInt(9);
        c.intervalDays = st.colDouble(10);
        out.push_back(c);
    }
    return out;
}

std::vector<ReviewItem> Db::dueExpressions(int limit) const {
    std::vector<ReviewItem> out;
    Stmt st(db_, "SELECT e.id, e.video_id, COALESCE(v.title, e.video_id), e.seg_idx, COALESCE(g.start_ms, 0), COALESCE(g.text, ''), "
                 "e.due_at, e.text, e.meaning, e.note, e.example "
                 "FROM expressions e LEFT JOIN segments g ON g.video_id=e.video_id AND g.idx=e.seg_idx "
                 "LEFT JOIN videos v ON v.id=e.video_id "
                 "WHERE e.due_at IS NOT NULL AND e.due_at <= ? ORDER BY e.due_at LIMIT ?");
    st.bind(1, now()).bind(2, limit);
    while (st.step()) {
        ReviewItem it;
        it.expressionId = st.colInt64(0);
        it.videoId = st.colText(1);
        it.title = st.colText(2);
        it.segIdx = st.colInt(3);
        it.startMs = st.colInt(4);
        const std::string sentence = st.colText(5);
        it.dueAt = st.colText(6);
        it.text = st.colText(7);        // 카드 앞면 = 표현
        it.meaning = st.colText(8);
        it.note = st.colText(9);
        it.example = st.colText(10);
        if (it.example.empty()) it.example = sentence;  // 예문이 없으면 원문 문장
        out.push_back(it);
    }
    return out;
}

int Db::dueExpressionCount() const {
    Stmt st(db_, "SELECT COUNT(*) FROM expressions WHERE due_at IS NOT NULL AND due_at <= ?");
    st.bind(1, now());
    return st.step() ? st.colInt(0) : 0;
}

void Db::rateExpression(long long id, Grade grade) {
    Stmt sel(db_, "SELECT ease, interval_days, reviews, lapses FROM expressions WHERE id=?");
    sel.bind(1, id);
    if (!sel.step()) return;
    double ease = sel.colDouble(0), interval = sel.colDouble(1);
    int reviews = sel.colInt(2), lapses = sel.colInt(3);
    if (grade == Hard) {
        ease = std::max(1.3, ease - 0.2);
        interval = 0.5;
        lapses++;
    } else {
        if (reviews == 0) interval = 1;
        else if (reviews == 1) interval = 3;
        else interval = std::max(1.0, interval * ease);
        if (grade == Easy) { interval *= 1.3; ease += 0.15; }
    }
    reviews++;
    Stmt(db_, "UPDATE expressions SET ease=?, interval_days=?, due_at=?, reviews=?, lapses=? WHERE id=?")
        .bind(1, ease).bind(2, interval).bind(3, fromNow(interval)).bind(4, reviews).bind(5, lapses).bind(6, id).run();
}

// ---------------- TSV 가져오기 ----------------

void Db::importTsv(const std::string& path) {
    {
        Stmt st(db_, "SELECT COUNT(*) FROM practices");
        if (st.step() && st.colInt(0) > 0) return;
    }
    std::ifstream in(path);
    if (!in) return;
    exec("BEGIN");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        PracticeRow r;
        std::string idx;
        std::getline(ss, r.videoId, '\t');
        std::getline(ss, idx, '\t');
        std::getline(ss, r.mode, '\t');
        std::getline(ss, r.at, '\t');
        std::getline(ss, r.recordingPath, '\t');
        if (r.videoId.empty() || idx.empty()) continue;
        r.segIdx = std::stoi(idx);
        addPractice(r);
    }
    exec("COMMIT");
}

// ---------------- 시간 ----------------

std::string Db::now() { return fromNow(0); }

std::string Db::fromNow(double days) {
    auto t = std::chrono::system_clock::now() + std::chrono::seconds((long long)std::llround(days * 86400.0));
    std::time_t tt = std::chrono::system_clock::to_time_t(t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", std::localtime(&tt));
    return buf;
}

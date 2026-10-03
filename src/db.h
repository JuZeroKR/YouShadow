#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "transcript.h"

struct sqlite3;

struct PracticeRow {
    std::string videoId;
    int segIdx = 0;
    std::string mode;       // play / repeat / shadow / echo
    std::string at;         // ISO-8601 local
    std::string recordingPath;
    double score = -1;      // -1 = 없음
};

struct SegmentState {
    bool bookmarked = false;
    double ease = 2.5;
    double intervalDays = 0;
    std::string dueAt;      // 비어 있으면 복습 대상 아님
    int reviews = 0;
    int lapses = 0;
};

struct ReviewItem {
    std::string videoId;
    std::string title;
    Lang lang = Lang::En;
    int segIdx = 0;
    int startMs = 0;
    std::string text;
    std::string dueAt;
    // 표현 카드일 때 (expressionId > 0)
    long long expressionId = 0;
    std::string meaning, note, example;
};

// 저장한 표현 카드
struct ExpressionCard {
    long long id = 0;
    std::string videoId;
    int segIdx = 0;
    std::string text, meaning, note, example;
    std::string createdAt, dueAt;
    int reviews = 0;
    double intervalDays = 0;
};

// 라이브러리 화면용 영상 요약
struct VideoSummary {
    std::string id;
    std::string title;
    Lang lang = Lang::En;
    int durationMs = 0;
    std::string addedAt;
    std::string lastOpenedAt;
    int segCount = 0;
    int practicedSegs = 0;   // 한 번이라도 연습한 문장 수
    int practiceCount = 0;
    double avgScore = -1;    // 채점된 연습의 평균 (-1: 없음)
    std::string lastPracticeAt;
    int dueCount = 0;        // 이 영상에서 복습 시점이 된 문장 수
};

// 통계
struct DayStat {
    std::string date;   // YYYY-MM-DD
    int count = 0;
    double avgScore = -1;
};

struct PracticeHistory {
    long long id = 0;
    std::string mode;
    std::string at;
    std::string recordingPath;
    double score = -1;
};

// SQLite 기반 학습 기록. 문장(세그먼트) 단위로 연습 이력, 간격 반복 상태, 북마크를 저장한다.
class Db {
public:
    Db() = default;
    ~Db();
    bool open(const std::string& path, std::string* err);

    // 영상 / 세그먼트
    void upsertVideo(const std::string& id, const std::string& title, int durationMs, Lang lang = Lang::En);
    Lang videoLang(const std::string& id) const;          // 등록되지 않은 영상이면 En
    bool hasVideo(const std::string& id) const;
    void upsertSegments(const std::string& id, const std::vector<Segment>& segs);

    // 연습 이력
    long long addPractice(const PracticeRow& row);   // rowid 반환
    void setPracticeScore(long long id, double score);
    std::map<int, int> countsFor(const std::string& videoId) const;
    std::map<int, double> bestScoresFor(const std::string& videoId) const;
    std::string lastRecording(const std::string& videoId, int segIdx) const;

    // 간격 반복 / 북마크
    enum Grade { Hard = 0, Good = 1, Easy = 2 };
    SegmentState state(const std::string& videoId, int segIdx) const;
    void rate(const std::string& videoId, int segIdx, Grade grade);
    void setBookmark(const std::string& videoId, int segIdx, bool on);
    std::set<int> bookmarksFor(const std::string& videoId) const;
    std::vector<ReviewItem> due(int limit = 200) const;
    int dueCount() const;
    std::vector<ReviewItem> bookmarks(int limit = 500) const;

    // 라이브러리 / 통계 / 이력
    std::vector<VideoSummary> videos() const;
    std::vector<DayStat> dailyStats(int days) const;     // 최근 days 일, 오래된 날짜부터
    std::vector<PracticeHistory> practicesFor(const std::string& videoId, int segIdx) const;
    int totalPractices() const;
    void deleteVideo(const std::string& videoId);        // 영상과 관련 기록 전부 삭제

    // 설정 (key/value)
    std::string getSetting(const std::string& key, const std::string& def = "") const;
    void setSetting(const std::string& key, const std::string& value);

    // 문장 해설 캐시 (LLM 결과 JSON)
    std::string getExplanation(const std::string& videoId, int segIdx) const;
    void setExplanation(const std::string& videoId, int segIdx, const std::string& json);
    std::set<int> explainedSegments(const std::string& videoId) const;

    // 단어 뜻 캐시 (단어 + 문장 기준, JSON)
    std::string getWordMeaning(const std::string& word, const std::string& sentence) const;
    void setWordMeaning(const std::string& word, const std::string& sentence, const std::string& json);

    // 일본어 읽기(토큰 · 후리가나 · 한국어 발음) 캐시 (문장 기준, JSON)
    std::string getReading(const std::string& videoId, int segIdx) const;
    void setReading(const std::string& videoId, int segIdx, const std::string& json);

    // 표현 카드 (간격 반복 포함)
    long long addExpression(const std::string& videoId, int segIdx, const std::string& text,
                            const std::string& meaning, const std::string& note, const std::string& example);
    void deleteExpression(long long id);
    bool hasExpression(const std::string& videoId, int segIdx, const std::string& text) const;
    std::vector<ExpressionCard> expressions(int limit = 1000) const;
    std::vector<ReviewItem> dueExpressions(int limit = 200) const;
    int dueExpressionCount() const;
    void rateExpression(long long id, Grade grade);

    // 이전 버전의 practice.tsv 가져오기 (이력이 비어 있을 때만)
    void importTsv(const std::string& path);

    static std::string now();
    static std::string fromNow(double days);

private:
    void exec(const std::string& sql);
    void ensureState(const std::string& videoId, int segIdx);
    sqlite3* db_ = nullptr;
};

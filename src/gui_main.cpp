// YouShadow GUI: 영상(libmpv) + 문장 목록 + 쉐도잉/따라말하기 녹음 + 채점 + 복습 (Dear ImGui)

#include <windows.h>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "audio.h"
#include "db.h"
#include "paths.h"
#include "player.h"
#include "scoring.h"
#include "stt.h"
#include "transcript.h"
#include "youtube.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kPeakBinMs = 10;

// ---------------- 백그라운드 로딩 ----------------

struct LoadedVideo {
    std::string id, title, dir, videoPath, audioPath;
    std::vector<Segment> segs;
    std::vector<float> peaks;  // 파형용 [min,max] per 10ms
};

struct Loader {
    std::thread th;
    std::mutex m;
    std::string status;
    bool busy = false, done = false, ok = false;
    LoadedVideo result;
    Stt* stt = nullptr;

    void setStatus(const std::string& s) {
        std::lock_guard<std::mutex> lock(m);
        status = s;
    }

    void start(const std::string& id) {
        if (th.joinable()) th.join();
        {
            std::lock_guard<std::mutex> lock(m);
            busy = true;
            done = ok = false;
            status = "다운로드 중... (콘솔 창에 진행 상황이 표시됩니다)";
        }
        th = std::thread([this, id] {
            LoadedVideo v;
            std::string err;
            try {
                v.id = id;
                v.dir = paths::dataDir() + "\\" + id;
                auto dl = yt::download(id, v.dir);
                v.title = dl.title.empty() ? id : dl.title;
                v.videoPath = fs::absolute(dl.videoPath).string();
                v.audioPath = dl.audioPath;

                const std::string segPath = v.dir + "/segments.json";
                v.segs = transcript::load(segPath);
                if (v.segs.empty()) {
                    if (!dl.subtitlePath.empty()) {
                        v.segs = transcript::parseJson3(dl.subtitlePath);
                    } else {
                        if (!stt || !stt->loaded()) {
                            throw std::runtime_error(
                                "영어 자막을 받지 못했습니다 (자막이 없거나 유튜브가 자막 요청을 일시 차단). "
                                "STT 모델이 준비되면 whisper 로 대본을 만들 수 있으니 잠시 후 다시 불러오세요");
                        }
                        setStatus("자막을 받지 못해 whisper 로 대본 생성 중... 0% (영상 길이에 따라 몇 분 걸립니다)");
                        auto pcm16 = AudioEngine::loadWav(dl.audioPath, Stt::kRate);
                        std::string serr;
                        auto words = stt->transcribe(pcm16, [this](int p) {
                            setStatus("자막을 받지 못해 whisper 로 대본 생성 중... " + std::to_string(p) + "%");
                        }, &serr);
                        if (!serr.empty()) throw std::runtime_error(serr);
                        v.segs = transcript::splitWords(std::move(words));
                        if (v.segs.empty()) throw std::runtime_error("음성에서 문장을 찾지 못했습니다");
                    }
                    transcript::save(v.segs, segPath);
                }
                setStatus("파형 분석 중...");
                v.peaks = AudioEngine::loadPeaks(dl.audioPath, kPeakBinMs);
            } catch (const std::exception& e) {
                err = e.what();
            }
            std::lock_guard<std::mutex> lock(m);
            busy = false;
            done = true;
            ok = err.empty();
            status = ok ? "" : "오류: " + err;
            result = std::move(v);
        });
    }
    ~Loader() { if (th.joinable()) th.join(); }
};

// STT 모델 다운로드 / 로드 (백그라운드)
struct SttLoader {
    std::thread th;
    std::mutex m;
    std::string status;
    bool busy = false;
    bool ready = false;

    void start(Stt* stt, bool download) {
        if (th.joinable()) th.join();
        { std::lock_guard<std::mutex> lock(m); busy = true; status = download ? "STT 모델 다운로드 중 (약 148MB)..." : "STT 모델 로딩 중..."; }
        th = std::thread([this, stt, download] {
            std::string err;
            const std::string path = Stt::defaultModelPath();
            try {
                if (download && !fs::exists(path)) {
                    fs::create_directories(fs::path(path).parent_path());
                    std::string part = path + ".part";
                    std::string cmd = "curl -L --fail -o \"" + part + "\" \"" + Stt::modelUrl() + "\"";
                    if (paths::runCommand(cmd, yt::logPath()) != 0 || !fs::exists(part)) {
                        throw std::runtime_error("모델 다운로드 실패. 로그: " + yt::logPath());
                    }
                    fs::rename(part, path);
                }
                { std::lock_guard<std::mutex> lock(m); status = "STT 모델 로딩 중..."; }
                if (!stt->load(path, &err)) throw std::runtime_error(err);
            } catch (const std::exception& e) {
                err = e.what();
            }
            std::lock_guard<std::mutex> lock(m);
            busy = false;
            ready = err.empty();
            status = ready ? "" : "STT 오류: " + err;
        });
    }
    ~SttLoader() { if (th.joinable()) th.join(); }
};

// 녹음 채점 (백그라운드)
struct ScoreJob {
    std::thread th;
    std::mutex m;
    bool running = false, done = false;
    long long practiceId = 0;
    ScoreResult result;
    std::string err;

    void start(Stt* stt, long long pid, std::string reference, std::vector<float> pcm48k) {
        if (th.joinable()) th.join();
        { std::lock_guard<std::mutex> lock(m); running = true; done = false; practiceId = pid; err.clear(); }
        th = std::thread([this, stt, reference, pcm = std::move(pcm48k)] {
            ScoreResult r;
            std::string e;
            try {
                auto pcm16 = AudioEngine::resample(pcm, AudioEngine::kSampleRate, Stt::kRate);
                std::string heard = stt->transcribeText(pcm16, &e);
                if (e.empty()) r = scoreTranscript(reference, heard);
            } catch (const std::exception& ex) {
                e = ex.what();
            }
            std::lock_guard<std::mutex> lock(m);
            result = std::move(r);
            err = e;
            done = true;
        });
    }
    ~ScoreJob() { if (th.joinable()) th.join(); }
};

// ---------------- 앱 상태 ----------------

enum class Mode { Idle, Segment, Shadow, ShadowTail, EchoListen, EchoRecord, EchoCompareOrig, EchoCompareMine, Record, MyRec };
enum class Tab { None, Sentences, Review, Bookmarks };

const char* modeLabel(Mode m) {
    switch (m) {
        case Mode::Segment: return "문장 재생 중";
        case Mode::Shadow: return "쉐도잉: 원본과 함께 말하세요";
        case Mode::ShadowTail: return "쉐도잉: 마저 말하세요 (끝나면 자동 종료)";
        case Mode::EchoListen: return "따라말하기: 원본 듣기";
        case Mode::EchoRecord: return "따라말하기: 지금 말하세요";
        case Mode::EchoCompareOrig: return "비교: 원본";
        case Mode::EchoCompareMine: return "비교: 내 녹음";
        case Mode::Record: return "녹음: 지금 말하세요";
        case Mode::MyRec: return "내 녹음 재생 중";
        default: return "";
    }
}

struct App {
    MpvPlayer mpv;
    Recorder recorder;
    Player recPlayer;
    Db db;
    Stt stt;
    Loader loader;
    SttLoader sttLoader;
    ScoreJob scoreJob;

    LoadedVideo video;
    bool loaded = false;
    std::map<int, int> counts;
    std::map<int, double> bestScores;
    std::set<int> bookmarks;

    char urlBuf[512] = "";
    std::string message;
    float uiScale = 1.0f;

    Mode mode = Mode::Idle;
    int current = -1;
    int loopsSetting = 1;
    int loopsLeft = 0;
    float speed = 1.0f;
    bool stopAtEnd = true;
    Clock::time_point seekGrace;

    // 녹음 종료 제어
    bool autoStop = true;
    float silenceSec = 1.5f;
    bool manualStop = false;
    bool speechDetected = false;
    float noiseFloor = 1.0f;   // 녹음 시작 직후 측정한 배경 소음 (포락선 최소값)
    float voiceThreshold = 0.03f;
    Clock::time_point recStart, lastVoiceAt, recMinEnd, recMaxEnd;
    bool followList = true;    // 자유 재생 중 목록이 현재 문장을 따라가기

    // 자동 테스트 스크립트 (--script 파일)
    std::vector<std::string> script;
    size_t scriptPos = 0;
    Clock::time_point scriptWaitUntil;
    GLFWwindow* window = nullptr;
    std::string errorPopup;

    std::vector<float> myRec;
    std::vector<float> myRecPeaks;
    std::string myRecPath;
    bool scrollToCurrent = false;

    // 채점 결과 (현재 문장)
    bool haveScore = false;
    bool scoring = false;
    int scoreSeg = -1;
    ScoreResult lastScore;

    // 홈(라이브러리) 화면
    bool showHome = true;
    bool libraryDirty = true;
    std::vector<VideoSummary> library;
    std::vector<DayStat> daily;
    int streakDays = 0, todayCount = 0, totalCount = 0;

    // 복습 세션
    struct ReviewSession {
        bool active = false;
        std::vector<ReviewItem> queue;
        size_t pos = 0;
        int hard = 0, good = 0, easy = 0;
        float scoreSum = 0;
        int scoreN = 0;
    } session;
    std::string infoPopup;

    // 문장별 기록 탭
    std::vector<PracticeHistory> history;
    int historySeg = -1;
    bool historyDirty = true;

    // 복습 탭
    Tab forceTab = Tab::None;  // 한 프레임 동안 강제로 선택할 탭
    std::vector<ReviewItem> reviewItems;
    std::vector<ReviewItem> bookmarkItems;
    bool reviewDirty = true;
    int dueCount = 0;
    int pendingSeg = -1;       // 다른 영상의 복습 항목을 열 때, 로드 후 재생할 문장
    bool pendingPlay = false;

    // ---- 헬퍼 ----
    bool valid(int i) const { return loaded && i >= 0 && i < (int)video.segs.size(); }
    const Segment& seg(int i) const { return video.segs[i]; }
    bool recording() const { return recorder.active(); }

    void seekTo(double sec) {
        mpv.seek(sec);
        seekGrace = Clock::now() + std::chrono::milliseconds(300);
    }
    bool pastSegmentEnd(double t) const {
        if (!valid(current)) return false;
        if (Clock::now() < seekGrace || mpv.seeking()) return false;
        return t >= seg(current).endMs / 1000.0;
    }

    void refreshStats() {
        counts = db.countsFor(video.id);
        bestScores = db.bestScoresFor(video.id);
        bookmarks = db.bookmarksFor(video.id);
        reviewDirty = true;
        libraryDirty = true;
        historyDirty = true;
    }

    // 홈 화면 데이터 갱신 (영상 요약, 일별 통계, 연속 학습일)
    void refreshLibrary() {
        library = db.videos();
        daily = db.dailyStats(60);
        totalCount = db.totalPractices();
        std::map<std::string, int> byDay;
        for (const auto& d : daily) byDay[d.date] = d.count;
        const std::string today = Db::now().substr(0, 10);
        todayCount = byDay.count(today) ? byDay[today] : 0;
        streakDays = 0;
        for (int back = (todayCount > 0 ? 0 : 1); back < 365; ++back) {
            std::string day = Db::fromNow(-back).substr(0, 10);
            if (byDay.count(day) && byDay[day] > 0) ++streakDays; else break;
        }
        libraryDirty = false;
    }

    // ---- 복습 세션 ----
    void startSession() {
        session = ReviewSession();
        session.queue = db.due();
        if (session.queue.empty()) { infoPopup = "지금 복습할 문장이 없습니다.\n연습한 문장은 다음 날 복습 목록에 나타납니다."; return; }
        session.active = true;
        openSessionItem();
    }

    void openSessionItem() {
        if (session.pos >= session.queue.size()) { endSession(); return; }
        showHome = false;
        openReviewItem(session.queue[session.pos]);
    }

    bool sessionItemIsCurrent() const {
        return session.active && session.pos < session.queue.size() && loaded &&
               session.queue[session.pos].videoId == video.id && session.queue[session.pos].segIdx == current;
    }

    void sessionSkip() {
        if (!session.active) return;
        session.pos++;
        openSessionItem();
    }

    void endSession() {
        if (!session.active) return;
        session.active = false;
        char buf[256];
        int done = session.hard + session.good + session.easy;
        if (session.scoreN > 0)
            snprintf(buf, sizeof buf, "복습 완료: %d문장 평가 (어려움 %d · 보통 %d · 쉬움 %d)\n채점 평균 %d%%", done, session.hard, session.good, session.easy, (int)(session.scoreSum / session.scoreN));
        else
            snprintf(buf, sizeof buf, "복습 완료: %d문장 평가 (어려움 %d · 보통 %d · 쉬움 %d)", done, session.hard, session.good, session.easy);
        infoPopup = buf;
        stopAll();
        showHome = true;
        libraryDirty = true;
    }

    void openVideoFromLibrary(const std::string& id) {
        if (loaded && video.id == id) { showHome = false; return; }
        requestLoad(id);
        snprintf(urlBuf, sizeof urlBuf, "%s", id.c_str());
    }

    void goHome() {
        stopAll();
        showHome = true;
        libraryDirty = true;
    }

    long long log(int i, const char* modeName, const std::string& rec = "") {
        long long id = db.addPractice({video.id, i, modeName, Db::now(), rec, -1});
        refreshStats();
        return id;
    }

    std::string saveRecording(int i, const std::vector<float>& pcm) {
        fs::create_directories(video.dir + "/rec");
        std::string ts = Db::now();
        for (auto& c : ts) if (c == ':') c = '-';
        std::string path = video.dir + "/rec/" + std::to_string(i) + "_" + ts + ".wav";
        AudioEngine::saveWav(path, pcm);
        return path;
    }

    void setMyRec(std::vector<float> pcm, std::string path) {
        myRec = std::move(pcm);
        myRecPeaks = AudioEngine::peaksFromPcm(myRec, kPeakBinMs);
        myRecPath = std::move(path);
    }

    void stopAll() {
        if (recorder.active()) recorder.stop();
        recPlayer.stop();
        mpv.setPaused(true);
        mode = Mode::Idle;
    }

    // ---- 녹음 시작/종료 ----
    void beginRecording(int minMs, int maxMs) {
        recorder.start();
        manualStop = false;
        speechDetected = false;
        noiseFloor = 1.0f;
        voiceThreshold = 0.03f;
        recStart = lastVoiceAt = Clock::now();
        recMinEnd = recStart + std::chrono::milliseconds(minMs);
        recMaxEnd = recStart + std::chrono::milliseconds(maxMs);
    }

    // 무음 감지 / 수동 / 최대 길이 기준으로 녹음을 끝낼지 판단
    bool recordingShouldStop() {
        const auto now = Clock::now();
        const float lv = recorder.level();
        const auto elapsed = now - recStart;
        if (manualStop) return true;
        // 첫 0.4초 동안 배경 소음을 재고, 그 4배(최소 0.01)를 음성 기준으로 삼는다.
        if (elapsed < std::chrono::milliseconds(400)) {
            noiseFloor = std::min(noiseFloor, lv);
            return false;
        }
        voiceThreshold = std::clamp(noiseFloor * 4.0f + 0.005f, 0.01f, 0.08f);
        if (lv > voiceThreshold) { speechDetected = true; lastVoiceAt = now; }
        if (now < recMinEnd) return false;
        if (now >= recMaxEnd) return true;
        if (!autoStop) return false;
        const auto silence = std::chrono::milliseconds((int)(silenceSec * 1000));
        if (speechDetected && now - lastVoiceAt > silence) return true;
        if (!speechDetected && elapsed > std::chrono::seconds(10)) return true;
        return false;
    }

    // 녹음 종료 → 저장, 기록, 채점 시작
    long long finishRecording(const char* modeName) {
        auto rec = recorder.stop();
        std::string path = saveRecording(current, rec);
        setMyRec(std::move(rec), path);
        long long pid = log(current, modeName, path);
        haveScore = false;
        scoreSeg = current;
        if (stt.loaded()) {
            scoring = true;
            scoreJob.start(&stt, pid, seg(current).text, myRec);
        }
        return pid;
    }

    // ---- 동작 ----
    void playSegment(int i, int loops) {
        if (!valid(i)) return;
        stopAll();
        current = i;
        loopsLeft = std::max(1, loops);
        seekTo(seg(i).startMs / 1000.0);
        mpv.setPaused(false);
        // "문장 끝에서 정지" 를 끈 상태면 그 위치부터 자유 재생 (목록은 시간에 따라 따라감)
        mode = stopAtEnd ? Mode::Segment : Mode::Idle;
        scrollToCurrent = true;
    }

    // 원본 듣기 없이 바로 녹음 → 내 녹음 재생 + 채점
    void startRecordOnly(int i) {
        if (!valid(i)) return;
        stopAll();
        current = i;
        const int dur = seg(i).endMs - seg(i).startMs;
        try { beginRecording(1500, dur * 3 + 10000); } catch (const std::exception& e) { message = e.what(); return; }
        mode = Mode::Record;
        scrollToCurrent = true;
    }

    void startShadow(int i) {
        if (!valid(i)) return;
        stopAll();
        const int dur = seg(i).endMs - seg(i).startMs;
        try { beginRecording(dur, dur * 3 + 10000); } catch (const std::exception& e) { message = e.what(); return; }
        current = i;
        seekTo(seg(i).startMs / 1000.0);
        mpv.setPaused(false);
        mode = Mode::Shadow;
        scrollToCurrent = true;
    }

    void startEcho(int i) {
        if (!valid(i)) return;
        stopAll();
        current = i;
        seekTo(seg(i).startMs / 1000.0);
        mpv.setPaused(false);
        mode = Mode::EchoListen;
        scrollToCurrent = true;
    }

    void playMyRecording() {
        if (myRec.empty() || scoreSeg != current) {
            std::string path = valid(current) ? db.lastRecording(video.id, current) : "";
            if (path.empty() || !fs::exists(path)) { message = "이 문장의 녹음이 없습니다"; return; }
            setMyRec(AudioEngine::loadWav(path), path);
        }
        stopAll();
        recPlayer.play(myRec);
        mode = Mode::MyRec;
    }

    void togglePause() {
        if (!mpv.hasFile()) return;
        if (mode != Mode::Idle && mode != Mode::Segment) { stopAll(); return; }
        // Idle 에서 재생하면 자유 재생: 문장 끝에서 멈추지 않고 목록만 따라간다
        mpv.setPaused(!mpv.paused());
    }

    void toggleBookmark(int i) {
        if (!valid(i)) return;
        bool on = !bookmarks.count(i);
        db.setBookmark(video.id, i, on);
        refreshStats();
    }

    void rateCurrent(Db::Grade g) {
        if (!valid(current)) return;
        db.rate(video.id, current, g);
        refreshStats();
        message = g == Db::Hard ? "반나절 뒤 다시 복습" : g == Db::Good ? "복습 간격 늘림" : "쉬움: 복습 간격 크게 늘림";
        // 복습 세션 중이면 집계하고 다음 문장으로
        if (sessionItemIsCurrent()) {
            if (g == Db::Hard) session.hard++; else if (g == Db::Good) session.good++; else session.easy++;
            if (haveScore && scoreSeg == current) { session.scoreSum += lastScore.accuracy; session.scoreN++; }
            session.pos++;
            openSessionItem();
        }
    }

    void openReviewItem(const ReviewItem& it) {
        if (loaded && it.videoId == video.id) {
            playSegment(it.segIdx, loopsSetting);
            forceTab = Tab::Sentences;
        } else {
            pendingSeg = it.segIdx;
            snprintf(urlBuf, sizeof urlBuf, "%s", it.videoId.c_str());
            requestLoad(it.videoId);
        }
    }

    // ---- 매 프레임 상태 갱신 ----
    void update() {
        pollJobs();
        if (!loaded) return;
        const double t = mpv.timePos();

        // 재생 위치에 따른 현재 문장 (자유 재생 시). 시킹 직후에는 잠시 기다린다.
        if (mode == Mode::Idle && !mpv.paused() && Clock::now() >= seekGrace) {
            int idx = -1;
            for (int i = 0; i < (int)video.segs.size(); ++i) {
                if (seg(i).startMs / 1000.0 <= t) idx = i; else break;
            }
            if (idx != current) { current = idx; if (followList) scrollToCurrent = true; }
        }

        // 녹음 중에는 영상이 절대 진행하지 않도록 고정
        if (mode == Mode::EchoRecord || mode == Mode::Record || mode == Mode::ShadowTail) {
            if (!mpv.paused()) mpv.setPaused(true);
        }

        switch (mode) {
            case Mode::Segment:
                if (pastSegmentEnd(t)) {
                    if (--loopsLeft > 0) {
                        seekTo(seg(current).startMs / 1000.0);
                    } else {
                        mpv.setPaused(true);
                        mode = Mode::Idle;
                        log(current, loopsSetting > 1 ? "repeat" : "play");
                    }
                }
                break;
            case Mode::Record:
                if (recordingShouldStop()) {
                    finishRecording("record");
                    recPlayer.play(myRec);
                    mode = Mode::MyRec;
                }
                break;
            case Mode::Shadow:
                recordingShouldStop();  // 음성 감지 상태 갱신
                if (manualStop || pastSegmentEnd(t)) {
                    mpv.setPaused(true);
                    mode = Mode::ShadowTail;
                }
                break;
            case Mode::ShadowTail:
                if (recordingShouldStop()) {
                    finishRecording("shadow");
                    recPlayer.play(myRec);
                    mode = Mode::MyRec;
                }
                break;
            case Mode::EchoListen:
                if (pastSegmentEnd(t)) {
                    mpv.setPaused(true);
                    const int dur = seg(current).endMs - seg(current).startMs;
                    try { beginRecording(std::min(dur, 1500), dur * 3 + 10000); }
                    catch (const std::exception& e) { message = e.what(); mode = Mode::Idle; break; }
                    mode = Mode::EchoRecord;
                }
                break;
            case Mode::EchoRecord:
                if (recordingShouldStop()) {
                    finishRecording("echo");
                    seekTo(seg(current).startMs / 1000.0);
                    mpv.setPaused(false);
                    mode = Mode::EchoCompareOrig;
                }
                break;
            case Mode::EchoCompareOrig:
                if (pastSegmentEnd(t)) {
                    mpv.setPaused(true);
                    recPlayer.play(myRec);
                    mode = Mode::EchoCompareMine;
                }
                break;
            case Mode::EchoCompareMine:
            case Mode::MyRec:
                if (!recPlayer.playing()) mode = Mode::Idle;
                break;
            default:
                break;
        }
    }

    void pollJobs() {
        // 영상 로더
        {
            std::lock_guard<std::mutex> lock(loader.m);
            if (loader.done) {
                loader.done = false;
                if (loader.ok) onLoaded(std::move(loader.result));
                else {
                    message = loader.status;
                    errorPopup = loader.status;
                    pendingSeg = -1;
                    if (session.active) endSession();
                }
            }
        }
        // STT 로더
        {
            std::lock_guard<std::mutex> lock(sttLoader.m);
            if (!sttLoader.busy && !sttLoader.status.empty()) { message = sttLoader.status; sttLoader.status.clear(); }
        }
        // 채점
        {
            std::lock_guard<std::mutex> lock(scoreJob.m);
            if (scoreJob.done) {
                scoreJob.done = false;
                scoreJob.running = false;
                scoring = false;
                if (scoreJob.err.empty()) {
                    lastScore = scoreJob.result;
                    haveScore = true;
                    if (scoreJob.practiceId > 0) db.setPracticeScore(scoreJob.practiceId, lastScore.accuracy);
                    bestScores = db.bestScoresFor(video.id);
                } else {
                    message = "채점 실패: " + scoreJob.err;
                }
            }
        }
    }

    void onLoaded(LoadedVideo&& v) {
        stopAll();
        video = std::move(v);
        loaded = true;
        current = -1;
        myRec.clear();
        myRecPeaks.clear();
        haveScore = false;
        scoreSeg = -1;
        int durMs = (int)(video.peaks.size() / 2) * kPeakBinMs;
        db.upsertVideo(video.id, video.title, durMs);
        db.upsertSegments(video.id, video.segs);
        refreshStats();
        mpv.load(video.videoPath);
        message.clear();
        forceTab = Tab::Sentences;
        showHome = false;
        historySeg = -1;
        if (pendingSeg >= 0) {
            // 영상이 mpv 에 실제로 열린 뒤 재생해야 하므로 프레임 뒤로 미룬다
            current = std::min(pendingSeg, (int)video.segs.size() - 1);
            scrollToCurrent = true;
            pendingSeg = -1;
            pendingPlay = true;
        }
    }

    void requestLoad(const std::string& input) {
        auto id = yt::extractVideoId(input);
        if (!id) { message = "유튜브 영상 ID를 찾을 수 없습니다"; return; }
        loader.stt = &stt;
        loader.start(*id);
    }

    // ---------------- UI ----------------

    // peaks: [min,max] 쌍이 bins 개. 픽셀 열마다 해당 구간의 bin 들을 합쳐 그린다.
    void drawWaveform(const float* peaks, size_t bins, ImVec2 size, ImU32 color, float playhead) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(30, 30, 30, 255));
        if (bins > 0) {
            const int cols = (int)size.x;
            const float mid = p.y + size.y / 2;
            float peak = 0.05f;
            for (size_t k = 0; k < bins; ++k) peak = std::max({peak, -peaks[2 * k], peaks[2 * k + 1]});
            const float gain = 0.95f / peak;
            for (int x = 0; x < cols; ++x) {
                size_t a = bins * x / cols, b = std::max(a + 1, bins * (x + 1) / cols);
                float lo = 0, hi = 0;
                for (size_t k = a; k < b && k < bins; ++k) { lo = std::min(lo, peaks[2 * k]); hi = std::max(hi, peaks[2 * k + 1]); }
                dl->AddLine(ImVec2(p.x + x, mid - hi * gain * size.y / 2), ImVec2(p.x + x, mid - lo * gain * size.y / 2 + 1), color);
            }
        }
        if (playhead >= 0.0f && playhead <= 1.0f) {
            float x = p.x + playhead * size.x;
            dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + size.y), IM_COL32(255, 80, 80, 255), 2.0f);
        }
        ImGui::Dummy(size);
    }

    void drawVideoPanel(ImVec2 size) {
        ImGui::BeginChild("video", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 origin = ImGui::GetCursorScreenPos();
        unsigned tex = mpv.render((int)avail.x, (int)avail.y);
        if (tex && mpv.hasFile()) {
            ImGui::Image((ImTextureID)(intptr_t)tex, avail);
        } else {
            ImGui::GetWindowDrawList()->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(0, 0, 0, 255));
            ImGui::Dummy(avail);
            if (!loaded) {
                const char* hint = "위에 유튜브 링크를 넣고 [불러오기]를 누르세요";
                ImVec2 ts = ImGui::CalcTextSize(hint);
                ImGui::GetWindowDrawList()->AddText(ImVec2(origin.x + (avail.x - ts.x) / 2, origin.y + avail.y / 2), IM_COL32(180, 180, 180, 255), hint);
            }
        }
        // 로딩 중 오버레이
        {
            bool busy;
            std::string status;
            { std::lock_guard<std::mutex> lock(loader.m); busy = loader.busy; status = loader.status; }
            if (busy) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(0, 0, 0, 160));
                ImFont* font = ImGui::GetFont();
                float fsize = ImGui::GetFontSize() * 1.6f;
                std::string line = "불러오는 중: " + std::string(urlBuf);
                ImVec2 ts = font->CalcTextSizeA(fsize, FLT_MAX, avail.x * 0.9f, line.c_str());
                dl->AddText(font, fsize, ImVec2(origin.x + (avail.x - ts.x) / 2, origin.y + avail.y / 2 - ts.y - 8), IM_COL32(255, 220, 120, 255), line.c_str(), nullptr, avail.x * 0.9f);
                ImVec2 ts2 = font->CalcTextSizeA(fsize, FLT_MAX, avail.x * 0.9f, status.c_str());
                dl->AddText(font, fsize, ImVec2(origin.x + (avail.x - ts2.x) / 2, origin.y + avail.y / 2 + 8), IM_COL32(255, 255, 255, 255), status.c_str(), nullptr, avail.x * 0.9f);
                // 회전하는 점으로 진행 중임을 표시
                float a = (float)ImGui::GetTime() * 4.0f;
                ImVec2 c(origin.x + avail.x / 2, origin.y + avail.y / 2 + ts2.y + 40);
                for (int k = 0; k < 8; ++k) {
                    float ang = a + k * 0.785f;
                    dl->AddCircleFilled(ImVec2(c.x + cosf(ang) * 14, c.y + sinf(ang) * 14), 3.5f, IM_COL32(255, 255, 255, 40 + 25 * k));
                }
            }
        }
        // 자막 오버레이
        if (valid(current)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const std::string& text = seg(current).text;
            float wrap = avail.x * 0.9f;
            ImFont* font = ImGui::GetFont();
            float fsize = ImGui::GetFontSize() * 1.35f;
            ImVec2 ts = font->CalcTextSizeA(fsize, FLT_MAX, wrap, text.c_str());
            ImVec2 pos(origin.x + (avail.x - ts.x) / 2, origin.y + avail.y - ts.y - 24);
            dl->AddRectFilled(ImVec2(pos.x - 10, pos.y - 6), ImVec2(pos.x + ts.x + 10, pos.y + ts.y + 6), IM_COL32(0, 0, 0, 170), 6.0f);
            dl->AddText(font, fsize, pos, IM_COL32(255, 255, 255, 255), text.c_str(), nullptr, wrap);
        }
        ImGui::EndChild();
    }

    void drawSentenceList() {
        ImGui::TextDisabled("%s", video.title.c_str());
        ImGui::TextDisabled("문장 %d개  |  클릭: 재생  |  ★ 북마크", (int)video.segs.size());
        ImGui::Separator();
        for (int i = 0; i < (int)video.segs.size(); ++i) {
            const auto& s = seg(i);
            ImGui::PushID(i);
            char label[96];
            int c = counts.count(i) ? counts[i] : 0;
            std::string extra;
            if (c > 0) extra += "  (" + std::to_string(c) + "회";
            if (bestScores.count(i)) extra += (c > 0 ? " · " : "  (") + std::to_string((int)bestScores[i]) + "%";
            if (!extra.empty()) extra += ")";
            snprintf(label, sizeof label, "%s[%d] %s%s", bookmarks.count(i) ? "★ " : "", i, transcript::formatTime(s.startMs).c_str(), extra.c_str());

            const float wrap = ImGui::GetContentRegionAvail().x - 8;
            ImVec2 textSize = ImGui::CalcTextSize(s.text.c_str(), nullptr, false, wrap);
            float h = ImGui::GetTextLineHeight() + textSize.y + 6;
            bool selected = (i == current);
            if (ImGui::Selectable("##row", selected, 0, ImVec2(0, h))) playSegment(i, loopsSetting);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) toggleBookmark(i);
            if (selected && scrollToCurrent) { ImGui::SetScrollHereY(0.3f); scrollToCurrent = false; }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 top = ImGui::GetItemRectMin();
            ImFont* font = ImGui::GetFont();
            float fs = ImGui::GetFontSize();
            ImU32 labelCol = bookmarks.count(i) ? IM_COL32(255, 200, 80, 255) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
            dl->AddText(font, fs, ImVec2(top.x + 4, top.y + 2), labelCol, label);
            ImU32 col = selected ? IM_COL32(255, 230, 100, 255) : ImGui::GetColorU32(ImGuiCol_Text);
            dl->AddText(font, fs, ImVec2(top.x + 4, top.y + 2 + ImGui::GetTextLineHeight()), col, s.text.c_str(), nullptr, wrap);
            ImGui::PopID();
        }
    }

    void drawReviewList(const std::vector<ReviewItem>& items, const char* emptyText) {
        if (items.empty()) { ImGui::TextDisabled("%s", emptyText); return; }
        std::string lastVideo;
        for (size_t k = 0; k < items.size(); ++k) {
            const auto& it = items[k];
            ImGui::PushID((int)k);
            if (it.videoId != lastVideo) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s", it.title.c_str());
                lastVideo = it.videoId;
            }
            const float wrap = ImGui::GetContentRegionAvail().x - 8;
            ImVec2 textSize = ImGui::CalcTextSize(it.text.c_str(), nullptr, false, wrap);
            float h = ImGui::GetTextLineHeight() + textSize.y + 6;
            bool here = loaded && it.videoId == video.id && it.segIdx == current;
            if (ImGui::Selectable("##rv", here, 0, ImVec2(0, h))) openReviewItem(it);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 top = ImGui::GetItemRectMin();
            ImFont* font = ImGui::GetFont();
            float fs = ImGui::GetFontSize();
            char label[96];
            snprintf(label, sizeof label, "[%d] %s   %s", it.segIdx, transcript::formatTime(it.startMs).c_str(), it.dueAt.substr(0, 16).c_str());
            dl->AddText(font, fs, ImVec2(top.x + 4, top.y + 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
            dl->AddText(font, fs, ImVec2(top.x + 4, top.y + 2 + ImGui::GetTextLineHeight()), ImGui::GetColorU32(ImGuiCol_Text), it.text.c_str(), nullptr, wrap);
            ImGui::PopID();
        }
    }

    // 현재 문장의 연습 이력
    void drawHistoryList() {
        if (!valid(current)) { ImGui::TextDisabled("문장을 선택하면 그 문장의 연습 기록이 나옵니다."); return; }
        if (historyDirty || historySeg != current) {
            history = db.practicesFor(video.id, current);
            historySeg = current;
            historyDirty = false;
        }
        ImGui::TextDisabled("[%d] %s", current, seg(current).text.c_str());
        ImGui::Separator();
        if (history.empty()) { ImGui::TextDisabled("아직 연습 기록이 없습니다."); return; }
        if (ImGui::BeginTable("hist", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("시각", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("모드", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("점수", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableHeadersRow();
            for (size_t k = 0; k < history.size(); ++k) {
                const auto& h = history[k];
                ImGui::PushID((int)k);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(h.at.substr(0, 16).c_str());
                ImGui::TableNextColumn();
                const char* modeName = h.mode == "shadow" ? "쉐도잉" : h.mode == "echo" ? "따라말하기" : h.mode == "record" ? "녹음" : h.mode == "repeat" ? "반복" : "재생";
                ImGui::TextUnformatted(modeName);
                ImGui::TableNextColumn();
                if (h.score >= 0) {
                    ImVec4 col = h.score >= 85 ? ImVec4(0.4f, 1, 0.5f, 1) : h.score >= 60 ? ImVec4(1, 0.85f, 0.3f, 1) : ImVec4(1, 0.5f, 0.5f, 1);
                    ImGui::TextColored(col, "%d%%", (int)h.score);
                } else ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                if (!h.recordingPath.empty() && fs::exists(h.recordingPath)) {
                    if (ImGui::SmallButton("듣기")) {
                        try {
                            setMyRec(AudioEngine::loadWav(h.recordingPath), h.recordingPath);
                            stopAll();
                            recPlayer.play(myRec);
                            mode = Mode::MyRec;
                        } catch (const std::exception& e) { message = e.what(); }
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    void drawRightPanel(ImVec2 size) {
        ImGui::BeginChild("right", size, ImGuiChildFlags_Borders);
        if (reviewDirty) {
            reviewItems = db.due();
            bookmarkItems = db.bookmarks();
            dueCount = (int)reviewItems.size();
            reviewDirty = false;
        }
        char reviewTab[32];
        snprintf(reviewTab, sizeof reviewTab, "복습 (%d)###review", dueCount);
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("문장", nullptr, forceTab == Tab::Sentences ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::BeginChild("list");
                if (loaded) drawSentenceList();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(reviewTab, nullptr, forceTab == Tab::Review ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::BeginChild("review");
                ImGui::TextDisabled("복습할 때가 된 문장. 클릭하면 이동합니다.");
                drawReviewList(reviewItems, "지금 복습할 문장이 없습니다. 연습한 문장은 다음 날 여기에 나타납니다.");
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("북마크", nullptr, forceTab == Tab::Bookmarks ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::BeginChild("bookmarks");
                ImGui::TextDisabled("문장 목록에서 오른쪽 클릭 또는 ★ 버튼으로 추가");
                drawReviewList(bookmarkItems, "북마크가 없습니다.");
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("기록")) {
                ImGui::BeginChild("history");
                drawHistoryList();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        forceTab = Tab::None;  // SetSelected 는 한 프레임만
        ImGui::EndChild();
    }

    void drawScoreLine() {
        if (scoring) { ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "채점 중..."); return; }
        if (!haveScore || scoreSeg != current) {
            if (!stt.loaded()) ImGui::TextDisabled("STT 모델을 받으면 녹음을 자동으로 채점합니다.");
            else ImGui::TextDisabled("쉐도잉 / 따라말하기 후 여기에 채점 결과가 표시됩니다.");
            return;
        }
        const auto& r = lastScore;
        ImVec4 col = r.accuracy >= 85 ? ImVec4(0.4f, 1, 0.5f, 1) : r.accuracy >= 60 ? ImVec4(1, 0.85f, 0.3f, 1) : ImVec4(1, 0.5f, 0.5f, 1);
        ImGui::TextColored(col, "정확도 %d%% (%d/%d)", (int)r.accuracy, r.matched, r.total);
        // 단어를 색으로 표시하되 패널 폭에 맞춰 직접 줄바꿈한다
        const float lineRight = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x - ImGui::GetStyle().WindowPadding.x;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        for (const auto& mk : r.marks) {
            std::string label;
            ImVec4 c;
            switch (mk.kind) {
                case WordMark::Match: label = mk.text; c = ImVec4(0.6f, 1, 0.6f, 1); break;
                case WordMark::Missing: label = mk.text; c = ImVec4(1, 0.4f, 0.4f, 1); break;
                case WordMark::Wrong: label = mk.text + "(" + mk.heard + ")"; c = ImVec4(1, 0.7f, 0.3f, 1); break;
                case WordMark::Extra: label = "+" + mk.text; c = ImVec4(0.6f, 0.6f, 0.6f, 1); break;
            }
            float w = ImGui::CalcTextSize(label.c_str()).x;
            if (ImGui::GetItemRectMax().x + spacing + w <= lineRight) ImGui::SameLine();
            ImGui::TextColored(c, "%s", label.c_str());
        }
        ImGui::TextDisabled("초록: 맞음  빨강: 빠짐  주황: 다르게 들림(들린 단어)  회색: 추가로 들린 단어  |  들린 문장: %s", r.heard.c_str());
    }

    // ---------------- 홈 (라이브러리 + 통계) ----------------

    void drawDailyChart(ImVec2 size) {
        constexpr int kDays = 30;
        std::map<std::string, int> byDay;
        for (const auto& d : daily) byDay[d.date] = d.count;
        int maxCount = 1;
        std::vector<std::pair<std::string, int>> bars;
        for (int i = kDays - 1; i >= 0; --i) {
            std::string day = Db::fromNow(-i).substr(0, 10);
            int c = byDay.count(day) ? byDay[day] : 0;
            maxCount = std::max(maxCount, c);
            bars.push_back({day, c});
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float labelH = ImGui::GetTextLineHeight() + 4;
        const float chartH = size.y - labelH;
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + chartH), IM_COL32(30, 30, 30, 255));
        const float bw = size.x / kDays;
        for (int i = 0; i < kDays; ++i) {
            const auto& [day, c] = bars[i];
            float x0 = p.x + i * bw + 2, x1 = p.x + (i + 1) * bw - 2;
            float h = c > 0 ? std::max(3.0f, chartH * c / (float)maxCount) : 0;
            if (c > 0) dl->AddRectFilled(ImVec2(x0, p.y + chartH - h), ImVec2(x1, p.y + chartH), IM_COL32(90, 170, 255, 255), 2.0f);
            if (i % 5 == 4 || i == kDays - 1) {
                std::string lbl = day.substr(5);
                ImVec2 ts = ImGui::CalcTextSize(lbl.c_str());
                dl->AddText(ImVec2((x0 + x1) / 2 - ts.x / 2, p.y + chartH + 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), lbl.c_str());
            }
            if (ImGui::IsMouseHoveringRect(ImVec2(x0, p.y), ImVec2(x1, p.y + chartH))) {
                ImGui::SetTooltip("%s: %d회", day.c_str(), c);
            }
        }
        ImGui::Dummy(size);
    }

    void drawHome(ImVec2 avail) {
        if (libraryDirty) refreshLibrary();
        const float rightW = std::clamp(avail.x * 0.36f, 300.0f * uiScale, 560.0f * uiScale);

        // 왼쪽: 영상 목록
        ImGui::BeginChild("library", ImVec2(avail.x - rightW - 8, avail.y), ImGuiChildFlags_Borders);
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "내 영상 (%d)", (int)library.size());
        ImGui::SameLine();
        ImGui::TextDisabled("클릭: 열기  |  오른쪽 클릭: 기록 삭제");
        ImGui::Separator();
        if (library.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("아직 연습한 영상이 없습니다. 위에 유튜브 링크를 넣고 [불러오기]를 누르세요.");
        }
        for (size_t k = 0; k < library.size(); ++k) {
            const auto& v = library[k];
            ImGui::PushID((int)k);
            const float lh = ImGui::GetTextLineHeight();
            const float h = lh * 3 + 14;
            bool isOpen = loaded && video.id == v.id;
            if (ImGui::Selectable("##card", isOpen, 0, ImVec2(0, h))) openVideoFromLibrary(v.id);
            if (ImGui::BeginPopupContextItem("ctx")) {
                ImGui::TextDisabled("%s", v.title.c_str());
                if (ImGui::MenuItem("이 영상의 연습 기록 삭제 (파일은 유지)")) {
                    db.deleteVideo(v.id);
                    libraryDirty = true;
                    if (loaded && video.id == v.id) refreshStats();
                }
                ImGui::EndPopup();
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 top = ImGui::GetItemRectMin();
            ImVec2 bot = ImGui::GetItemRectMax();
            ImFont* font = ImGui::GetFont();
            float fs = ImGui::GetFontSize();
            const float wrap = bot.x - top.x - 8;
            dl->AddText(font, fs, ImVec2(top.x + 6, top.y + 4), IM_COL32(255, 255, 255, 255), v.title.c_str(), nullptr, wrap);
            // 진행률 바
            float frac = v.segCount > 0 ? v.practicedSegs / (float)v.segCount : 0.0f;
            ImVec2 b0(top.x + 6, top.y + 4 + lh + 4), b1(top.x + 6 + wrap * 0.5f, b0.y + lh - 4);
            dl->AddRectFilled(b0, b1, IM_COL32(50, 50, 50, 255), 3.0f);
            dl->AddRectFilled(b0, ImVec2(b0.x + (b1.x - b0.x) * frac, b1.y), IM_COL32(90, 170, 255, 255), 3.0f);
            char prog[96];
            snprintf(prog, sizeof prog, "  %d / %d 문장 (%d%%)", v.practicedSegs, v.segCount, (int)(frac * 100));
            dl->AddText(font, fs, ImVec2(b1.x, b0.y - 2), ImGui::GetColorU32(ImGuiCol_Text), prog);
            // 요약 줄
            std::string line = "연습 " + std::to_string(v.practiceCount) + "회";
            if (v.avgScore >= 0) line += "  ·  평균 " + std::to_string((int)v.avgScore) + "%";
            if (v.dueCount > 0) line += "  ·  복습 대기 " + std::to_string(v.dueCount);
            if (!v.lastPracticeAt.empty()) line += "  ·  마지막 연습 " + v.lastPracticeAt.substr(0, 16);
            else if (!v.lastOpenedAt.empty()) line += "  ·  마지막 열람 " + v.lastOpenedAt.substr(0, 16);
            line += "  ·  " + transcript::formatTime(v.durationMs).substr(0, 5);
            ImU32 col = v.dueCount > 0 ? IM_COL32(255, 200, 80, 255) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
            dl->AddText(font, fs, ImVec2(top.x + 6, top.y + 4 + lh * 2 + 6), col, line.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();

        // 오른쪽: 오늘 / 통계
        ImGui::SameLine();
        ImGui::BeginChild("stats", ImVec2(rightW, avail.y), ImGuiChildFlags_Borders);
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "오늘");
        ImGui::Separator();
        int due = db.dueCount();
        if (due > 0) ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "복습할 문장 %d개", due);
        else ImGui::TextDisabled("복습할 문장이 없습니다");
        ImGui::SameLine();
        ImGui::BeginDisabled(due == 0);
        if (ImGui::Button("복습 시작")) startSession();
        ImGui::EndDisabled();
        ImGui::Text("오늘 연습 %d회   연속 학습 %d일", todayCount, streakDays);
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "최근 30일");
        ImGui::Separator();
        drawDailyChart(ImVec2(ImGui::GetContentRegionAvail().x, 140 * uiScale));
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "전체");
        ImGui::Separator();
        int practicedTotal = 0;
        for (const auto& v : library) practicedTotal += v.practicedSegs;
        ImGui::Text("영상 %d개   연습한 문장 %d개   총 연습 %d회", (int)library.size(), practicedTotal, totalCount);
        ImGui::Spacing();
        ImGui::TextDisabled("단축키: Space 재생/정지, ← → 문장 이동, S 쉐도잉, E 따라말하기, T 녹음, B 북마크");
        ImGui::EndChild();
    }

    void drawControls(ImVec2 size) {
        ImGui::BeginChild("controls", size, ImGuiChildFlags_Borders);
        const bool can = loaded && mpv.hasFile();

        // 복습 세션 배너
        if (session.active) {
            ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "복습 세션 %d / %d   (어려움 %d · 보통 %d · 쉬움 %d)   아래 [어려움/보통/쉬움] 을 누르면 다음 문장으로 넘어갑니다",
                               (int)session.pos + 1, (int)session.queue.size(), session.hard, session.good, session.easy);
            ImGui::SameLine(0, 16);
            if (ImGui::SmallButton("건너뛰기")) sessionSkip();
            ImGui::SameLine();
            if (ImGui::SmallButton("세션 종료")) endSession();
        }

        ImGui::BeginDisabled(!can);

        // 1행: 재생 제어
        if (ImGui::Button("이전")) playSegment(std::max(0, current - 1), loopsSetting);
        ImGui::SameLine();
        if (ImGui::Button(mpv.paused() || mode == Mode::Idle ? "재생" : "정지")) togglePause();
        ImGui::SameLine();
        if (ImGui::Button("다음")) playSegment(std::min((int)video.segs.size() - 1, current + 1), loopsSetting);
        ImGui::SameLine();
        if (ImGui::Button("다시")) playSegment(std::max(0, current), loopsSetting);
        ImGui::SameLine(0, 24);
        ImGui::SetNextItemWidth(90 * uiScale);
        if (ImGui::InputInt("반복", &loopsSetting)) loopsSetting = std::clamp(loopsSetting, 1, 20);
        ImGui::SameLine(0, 24);
        ImGui::SetNextItemWidth(140 * uiScale);
        if (ImGui::SliderFloat("속도", &speed, 0.5f, 1.5f, "%.2fx")) mpv.setSpeed(speed);
        ImGui::SameLine(0, 24);
        ImGui::Checkbox("문장 끝에서 정지", &stopAtEnd);
        ImGui::SameLine();
        ImGui::Checkbox("목록 따라가기", &followList);
        ImGui::SameLine(0, 24);
        if (ImGui::Button(valid(current) && bookmarks.count(current) ? "★ 북마크 해제" : "☆ 북마크")) toggleBookmark(current);

        // 2행: 연습 모드
        if (ImGui::Button("쉐도잉 (S)")) startShadow(std::max(0, current));
        ImGui::SameLine();
        if (ImGui::Button("따라말하기 (E)")) startEcho(std::max(0, current));
        ImGui::SameLine();
        if (ImGui::Button("녹음만 (T)")) startRecordOnly(std::max(0, current));
        ImGui::SameLine();
        if (ImGui::Button("내 녹음 듣기")) playMyRecording();
        ImGui::SameLine();
        if (ImGui::Button("중지")) stopAll();
        ImGui::SameLine(0, 24);
        if (recording()) {
            if (ImGui::Button("녹음 끝내기 (Space)")) manualStop = true;
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "● REC %.1fs", recorder.recordedMs() / 1000.0);
            ImGui::SameLine();
            ImGui::ProgressBar(std::min(1.0f, recorder.level() * 3.0f), ImVec2(120 * uiScale, 0), "");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", speechDetected ? "음성 감지됨" : "음성 대기");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.6f, 0.9f, 1, 1), "%s", modeLabel(mode));
        } else {
            ImGui::Checkbox("무음 감지 자동 종료", &autoStop);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120 * uiScale);
            ImGui::SliderFloat("##silence", &silenceSec, 0.5f, 3.0f, "%.1f초 무음");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.6f, 0.9f, 1, 1), "%s", modeLabel(mode));
        }
        ImGui::EndDisabled();

        // 3행: 시크바
        double dur = mpv.duration();
        float t = (float)mpv.timePos();
        ImGui::SetNextItemWidth(-1);
        char tbuf[32];
        snprintf(tbuf, sizeof tbuf, "%s / %s", transcript::formatTime((int)(t * 1000)).c_str(), transcript::formatTime((int)(dur * 1000)).c_str());
        if (ImGui::SliderFloat("##seek", &t, 0.0f, (float)std::max(dur, 0.001), tbuf) && can) {
            stopAll();
            seekTo(t);
            mpv.setPaused(false);
        }

        // 4행: 채점 결과 + 난이도 평가
        ImGui::BeginDisabled(!can);
        drawScoreLine();
        ImGui::BeginDisabled(!valid(current));
        ImGui::TextDisabled("이 문장은:");
        ImGui::SameLine();
        if (ImGui::Button("어려움")) rateCurrent(Db::Hard);
        ImGui::SameLine();
        if (ImGui::Button("보통")) rateCurrent(Db::Good);
        ImGui::SameLine();
        if (ImGui::Button("쉬움")) rateCurrent(Db::Easy);
        if (valid(current)) {
            ImGui::SameLine();
            SegmentState st = db.state(video.id, current);
            if (!st.dueAt.empty()) ImGui::TextDisabled("다음 복습 %s  (%d회, 간격 %.1f일)", st.dueAt.substr(0, 16).c_str(), st.reviews, st.intervalDays);
            else ImGui::TextDisabled("아직 복습 등록 안 됨 (연습하면 자동 등록)");
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        // 5행: 파형 (원본 문장 / 내 녹음)
        float w = ImGui::GetContentRegionAvail().x;
        float h = std::max(24.0f, (ImGui::GetContentRegionAvail().y - 8) / 2);
        const size_t totalBins = video.peaks.size() / 2;
        if (valid(current)) {
            const auto& s = seg(current);
            size_t a = std::min(totalBins, (size_t)s.startMs / kPeakBinMs);
            size_t b = std::min(totalBins, (size_t)s.endMs / kPeakBinMs);
            float ph = (t * 1000 - s.startMs) / (float)(s.endMs - s.startMs);
            drawWaveform(video.peaks.data() + 2 * a, b - a, ImVec2(w, h), IM_COL32(90, 170, 255, 255), mpv.paused() ? -1.0f : ph);
        } else {
            drawWaveform(nullptr, 0, ImVec2(w, h), 0, -1.0f);
        }
        float ph2 = recPlayer.playing() && !myRec.empty() ? recPlayer.positionMs() / (float)AudioEngine::durationMs(myRec) : -1.0f;
        drawWaveform(myRecPeaks.data(), myRecPeaks.size() / 2, ImVec2(w, h), IM_COL32(120, 230, 120, 255), ph2);

        ImGui::EndChild();
    }

    void handleKeys() {
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput || showHome || !loaded) return;
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            if (recording()) manualStop = true;
            else togglePause();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) playSegment(std::max(0, current - 1), loopsSetting);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) playSegment(std::min((int)video.segs.size() - 1, current + 1), loopsSetting);
        if (ImGui::IsKeyPressed(ImGuiKey_R, false) && valid(current)) playSegment(current, loopsSetting);
        if (ImGui::IsKeyPressed(ImGuiKey_S, false) && valid(current) && !recording()) startShadow(current);
        if (ImGui::IsKeyPressed(ImGuiKey_E, false) && valid(current) && !recording()) startEcho(current);
        if (ImGui::IsKeyPressed(ImGuiKey_T, false) && valid(current) && !recording()) startRecordOnly(current);
        if (ImGui::IsKeyPressed(ImGuiKey_B, false) && valid(current)) toggleBookmark(current);
    }

    // --script 파일의 명령을 한 줄씩 실행 (테스트용): load <id> / wait <sec> / play <n> / echo <n> / record / stop / quit
    void runScript() {
        if (scriptPos >= script.size() || Clock::now() < scriptWaitUntil) return;
        std::istringstream ss(script[scriptPos++]);
        std::string cmd, arg;
        ss >> cmd >> arg;
        fprintf(stderr, "[script] %s %s\n", cmd.c_str(), arg.c_str());
        if (cmd == "load") { snprintf(urlBuf, sizeof urlBuf, "%s", arg.c_str()); requestLoad(arg); }
        else if (cmd == "wait") scriptWaitUntil = Clock::now() + std::chrono::milliseconds((int)(std::stod(arg) * 1000));
        else if (cmd == "play") playSegment(std::stoi(arg), loopsSetting);
        else if (cmd == "echo") startEcho(std::stoi(arg));
        else if (cmd == "record") startRecordOnly(std::max(0, current));
        else if (cmd == "stop") stopAll();
        else if (cmd == "home") goHome();
        else if (cmd == "rescore" && valid(current) && stt.loaded()) {
            // 현재 문장의 마지막 녹음을 다시 채점 (마이크 없이 채점 화면 확인용)
            std::string path = db.lastRecording(video.id, current);
            if (!path.empty() && fs::exists(path)) {
                setMyRec(AudioEngine::loadWav(path), path);
                scoring = true;
                haveScore = false;
                scoreSeg = current;
                scoreJob.start(&stt, 0, seg(current).text, myRec);
            }
        }
        else if (cmd == "review") startSession();
        else if (cmd == "rate") rateCurrent(arg == "hard" ? Db::Hard : arg == "easy" ? Db::Easy : Db::Good);
        else if (cmd == "quit" && window) glfwSetWindowShouldClose(window, GLFW_TRUE);
    }

    void drawFrame() {
        runScript();
        update();
        // 복습 항목에서 넘어온 경우 영상이 열린 뒤 재생
        if (pendingPlay && loaded && mpv.hasFile() && valid(current)) {
            pendingPlay = false;
            playSegment(current, loopsSetting);
        }

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##main", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
        handleKeys();

        // 상단: URL 입력 + 상태
        bool busy;
        std::string status;
        { std::lock_guard<std::mutex> lock(loader.m); busy = loader.busy; status = loader.status; }
        bool sttBusy;
        std::string sttStatus;
        { std::lock_guard<std::mutex> lock(sttLoader.m); sttBusy = sttLoader.busy; sttStatus = sttLoader.status; }

        if (showHome) ImGui::BeginDisabled(!loaded);
        if (ImGui::Button(showHome ? "학습 화면" : "홈")) { if (showHome) showHome = false; else goHome(); }
        if (showHome) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(420 * uiScale);
        bool enter = ImGui::InputTextWithHint("##url", "유튜브 URL 또는 영상 ID", urlBuf, sizeof urlBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("불러오기") || enter) requestLoad(urlBuf);
        ImGui::EndDisabled();
        ImGui::SameLine(0, 16);
        if (!stt.loaded()) {
            if (sttBusy) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", sttStatus.c_str());
            else if (ImGui::Button("STT 모델 받기 (148MB, 채점/자막 생성용)")) sttLoader.start(&stt, true);
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "STT 준비됨");
        }
        ImGui::SameLine(0, 16);
        if (busy) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", status.c_str());
        else if (!message.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", message.c_str());
        else if (loaded) ImGui::TextDisabled("%s", video.title.c_str());
        else ImGui::TextDisabled(" ");  // SameLine 뒤에 항목이 없으면 다음 줄이 옆으로 붙는다

        // 본문 레이아웃
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (showHome || !loaded) {
            mpv.render(0, 0);  // 이벤트 큐만 비운다 (FILE_LOADED 감지)
            drawHome(avail);
        } else {
            const float rightW = std::clamp(avail.x * 0.34f, 280.0f * uiScale, 520.0f * uiScale);
            const float bottomH = 300.0f * uiScale;
            drawVideoPanel(ImVec2(avail.x - rightW - 8, avail.y - bottomH - 8));
            ImGui::SameLine();
            drawRightPanel(ImVec2(rightW, avail.y - bottomH - 8));
            drawControls(ImVec2(avail.x, bottomH));
        }

        // 알림 팝업
        if (!infoPopup.empty() && !ImGui::IsPopupOpen("알림")) ImGui::OpenPopup("알림");
        if (ImGui::BeginPopupModal("알림", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(600 * uiScale);
            ImGui::TextUnformatted(infoPopup.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (ImGui::Button("확인", ImVec2(120 * uiScale, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
                infoPopup.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // 오류 팝업
        if (!errorPopup.empty() && !ImGui::IsPopupOpen("오류")) ImGui::OpenPopup("오류");
        if (ImGui::BeginPopupModal("오류", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(600 * uiScale);
            ImGui::TextUnformatted(errorPopup.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (ImGui::Button("확인", ImVec2(120 * uiScale, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                errorPopup.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::End();
    }
};

void glfwError(int code, const char* desc) { fprintf(stderr, "GLFW error %d: %s\n", code, desc); }

// 처리되지 않은 예외(크래시)를 logs\crash.log 에 남기고 안내창을 띄운다
LONG WINAPI crashHandler(EXCEPTION_POINTERS* ep) {
    char buf[512];
    snprintf(buf, sizeof buf, "YouShadow v%s crashed: exception 0x%08lX at %p\r\n", YS_VERSION,
             ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0,
             ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr);
    std::string path = paths::logDir() + "\\crash.log";
    if (FILE* f = fopen(path.c_str(), "ab")) { fputs(buf, f); fclose(f); }
    MessageBoxA(nullptr, (std::string("프로그램에 문제가 생겨 종료합니다.\n\n") + buf + "\n로그: " + path).c_str(), "YouShadow", MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

#ifndef YS_VERSION
#define YS_VERSION "dev"
#endif

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    paths::setup();
    SetUnhandledExceptionFilter(crashHandler);

    glfwSetErrorCallback(glfwError);
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(1280, 800, "YouShadow v" YS_VERSION, nullptr, nullptr);
    if (!window) return 1;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().FrameRounding = 4.0f;
    float scaleX = 1.0f, scaleY = 1.0f;
    glfwGetWindowContentScale(window, &scaleX, &scaleY);
    const float uiScale = std::max(1.0f, scaleX);
    ImGui::GetStyle().ScaleAllSizes(uiScale);
    const char* fontPath = "C:/Windows/Fonts/malgun.ttf";
    if (fs::exists(fontPath)) io.Fonts->AddFontFromFileTTF(fontPath, 18.0f * uiScale);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    App app;
    app.uiScale = uiScale;
    app.window = window;
    std::string err;
    if (!app.mpv.init([](const char* n) { return (void*)glfwGetProcAddress(n); }, &err)) {
        MessageBoxA(nullptr, ("영상 재생기(libmpv) 초기화 실패: " + err).c_str(), "YouShadow", MB_ICONERROR);
        return 1;
    }
    if (!app.db.open(paths::dataDir() + "\\youshadow.db", &err)) {
        MessageBoxA(nullptr, err.c_str(), "YouShadow", MB_ICONERROR);
        return 1;
    }
    app.db.importTsv(paths::dataDir() + "\\practice.tsv");
    if (fs::exists(Stt::defaultModelPath())) app.sttLoader.start(&app.stt, false);
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--script" && i + 1 < argc) {
            std::ifstream in(argv[++i]);
            std::string line;
            while (std::getline(in, line)) if (!line.empty() && line[0] != '#') app.script.push_back(line);
        } else {
            snprintf(app.urlBuf, sizeof app.urlBuf, "%s", a.c_str());
            app.requestLoad(a);
        }
    }

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        app.drawFrame();

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    app.stopAll();
    app.mpv.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

// YouShadow GUI: 영상(libmpv) + 문장 목록 + 쉐도잉/따라말하기 녹음 + 채점 + 복습 (Dear ImGui)

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#else
#include <CoreFoundation/CoreFoundation.h>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <atomic>
#include <cctype>
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
#include "llm.h"
#include "local.h"
#include "paths.h"
#include "subtitle.h"
#include "player.h"
#include "secret.h"
#include "tts.h"
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

    // 로컬 영상을 처음 등록할 때만 채운다 (비어 있으면 data/<id>/source.txt 로 다시 연다)
    std::string localVideo, localSub;

    void start(const std::string& id) {
        if (th.joinable()) th.join();
        const bool isLocal = local::isLocalId(id);
        {
            std::lock_guard<std::mutex> lock(m);
            busy = true;
            done = ok = false;
            status = isLocal ? "영상 준비 중... (오디오 추출, 자막 읽기)" : "다운로드 중... (콘솔 창에 진행 상황이 표시됩니다)";
        }
        const std::string lv = localVideo, ls = localSub;
        localVideo.clear();
        localSub.clear();
        th = std::thread([this, id, isLocal, lv, ls] {
            LoadedVideo v;
            std::string err;
            try {
                v.id = id;
                v.dir = paths::dataDir() + "/" + id;
                std::string subtitlePath;
                bool json3 = false;
                if (isLocal) {
                    local::Prepared p = lv.empty() ? local::reopen(v.dir) : local::prepare(lv, ls, v.dir);
                    v.title = p.title;
                    v.videoPath = p.videoPath;
                    v.audioPath = p.audioPath;
                    subtitlePath = p.subtitlePath;
                } else {
                    auto dl = yt::download(id, v.dir);
                    v.title = dl.title.empty() ? id : dl.title;
                    v.videoPath = fs::absolute(dl.videoPath).string();
                    v.audioPath = dl.audioPath;
                    subtitlePath = dl.subtitlePath;
                    json3 = true;
                }

                const std::string segPath = v.dir + "/segments.json";
                v.segs = transcript::load(segPath);
                if (v.segs.empty()) {
                    if (!subtitlePath.empty()) {
                        v.segs = json3 ? transcript::parseJson3(subtitlePath)
                                       : transcript::splitWords(subtitle::parseFile(subtitlePath));
                        if (v.segs.empty()) throw std::runtime_error("자막에서 문장을 만들지 못했습니다: " + subtitlePath);
                    } else {
                        if (!stt || !stt->loaded()) {
                            throw std::runtime_error(isLocal
                                ? "자막을 찾지 못했습니다. 영상과 같은 이름의 .smi/.srt/.vtt 파일을 같은 폴더에 두거나 영상과 함께 끌어다 놓으세요. "
                                  "STT 모델이 준비되면 whisper 로 대본을 만들 수도 있습니다"
                                : "영어 자막을 받지 못했습니다 (자막이 없거나 유튜브가 자막 요청을 일시 차단). "
                                  "STT 모델이 준비되면 whisper 로 대본을 만들 수 있으니 잠시 후 다시 불러오세요");
                        }
                        setStatus("자막이 없어 whisper 로 대본 생성 중... 0% (영상 길이에 따라 몇 분 걸립니다)");
                        auto pcm16 = AudioEngine::loadWav(v.audioPath, Stt::kRate);
                        std::string serr;
                        auto words = stt->transcribe(pcm16, [this](int p) {
                            setStatus("자막이 없어 whisper 로 대본 생성 중... " + std::to_string(p) + "%");
                        }, &serr);
                        if (!serr.empty()) throw std::runtime_error(serr);
                        v.segs = transcript::splitWords(std::move(words));
                        if (v.segs.empty()) throw std::runtime_error("음성에서 문장을 찾지 못했습니다");
                    }
                    transcript::save(v.segs, segPath);
                }
                setStatus("파형 분석 중...");
                v.peaks = AudioEngine::loadPeaks(v.audioPath, kPeakBinMs);
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
enum class Tab { None, Sentences, Quiz, Review, Bookmarks, History, Explain, Cards };

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

    // ---- AI 해설 (Claude / ChatGPT / Gemini) ----
    LlmConfig llm;
    bool showSettings = false;
    int providerSel = 0;
    char keyBuf[3][256] = {};
    char modelBuf[3][128] = {};
    std::vector<std::string> modelList[3];
    std::string settingsMsg;

    // 모델 목록 / 연결 테스트 (백그라운드)
    struct LlmSmallJob {
        std::thread th;
        std::mutex m;
        bool running = false, done = false;
        int kind = 0;  // 0: 모델 목록, 1: 연결 테스트
        int provider = 0;
        std::vector<std::string> models;
        std::string text, err;
        ~LlmSmallJob() { if (th.joinable()) th.join(); }
    } llmJob;

    // 문장 해설 작업 (한 문장 또는 여러 문장 순차)
    struct ExplainJob {
        std::thread th;
        std::mutex m;
        bool running = false;
        std::string videoId;
        std::vector<int> queue;
        int done = 0;
        std::vector<std::pair<int, Explanation>> ready;  // 완료된 결과 (메인 스레드가 가져감)
        std::string err;
        std::atomic<bool> cancel{false};
        ~ExplainJob() { cancel = true; if (th.joinable()) th.join(); }
    } explainJob;
    std::map<int, Explanation> explanations;  // 현재 영상의 해설 캐시
    std::set<int> explained;
    std::vector<ExpressionCard> cards;
    bool cardsDirty = true;
    bool cardRevealed = false;  // 복습 세션에서 표현 카드 뜻 보기

    // ---- 단어 뜻 (클릭한 단어) ----
    struct WordJob {
        std::thread th;
        std::mutex m;
        bool running = false, done = false;
        std::string word, sentence;          // 진행 중인 요청
        WordMeaning result;
        std::string err;
        bool hasPending = false;             // 진행 중에 다른 단어를 클릭하면 끝난 뒤 이어서 찾는다
        std::string pendingWord, pendingSentence;
        ~WordJob() { if (th.joinable()) th.join(); }
    } wordJob;
    std::string wordQuery, wordKey, wordSentence;  // 팝업에 표시 중인 단어 (원문 / 소문자 키 / 문장)
    WordMeaning wordInfo;
    std::string wordErr;
    bool wordLoading = false;
    ImVec2 wordPopupPos;

    // 단어 드래그 선택 (복사)
    struct WordDrag {
        int anchor = -1;          // 마우스를 누른 단어 (-1: 없음)
        int a = -1, b = -1;       // 선택 범위
        bool dragging = false;
        ImVec2 pressPos;
        Clock::time_point copiedAt;
    } wordDrag;
    int scriptWordClick = -1;  // --script 의 word 명령: 다음 프레임에 이 단어를 클릭한 것으로 처리

    // ---- 문장 학습 (리스닝 받아쓰기 / 영작) ----
    int quizKind = 0;            // 0: 리스닝(자막 없이 받아쓰기), 1: 영작(한국어 → 영어)
    int quizSeg = -1;            // 출제 중인 문장 (-1: 없음)
    bool quizRevealed = false;   // 정답 공개됨
    bool quizChecked = false;    // 채점됨
    char quizBuf[2048] = {};
    ScoreResult quizScore;

    // ---- 음량 / 발음 ----
    Tts tts;
    int videoVolume = 100;   // 영상 음량 (0~100)
    int recVolume = 100;     // 내 녹음 재생 음량 (0~200 %)
    bool ttsSlow = false;    // 발음을 천천히

    void applyAudioSettings() {
        mpv.setVolume(videoVolume);
        recPlayer.setGain(recVolume / 100.0f);
        tts.setVolume(100);
    }

    void speak(const std::string& text) {
        if (!tts.available()) { message = "이 PC 에 음성 합성 엔진이 없습니다"; return; }
        tts.speak(text, ttsSlow ? -4 : 0);
    }

    // 단어 버튼용: 앞뒤 문장 부호 제거
    static std::string bareWord(const std::string& w) {
        size_t a = 0, b = w.size();
        auto isP = [](unsigned char c) { return c < 128 && !std::isalnum(c) && c != '\''; };
        while (a < b && isP((unsigned char)w[a])) ++a;
        while (b > a && isP((unsigned char)w[b - 1])) --b;
        return w.substr(a, b - a);
    }

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
        for (auto& e : db.dueExpressions()) session.queue.push_back(e);
        if (session.queue.empty()) { infoPopup = "지금 복습할 문장이 없습니다.\n연습한 문장과 저장한 표현은 다음 날 복습 목록에 나타납니다."; return; }
        session.active = true;
        openSessionItem();
    }

    void openSessionItem() {
        if (session.pos >= session.queue.size()) { endSession(); return; }
        showHome = false;
        cardRevealed = false;
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

    // ---- AI 설정 ----
    void loadSettings() {
        llm.provider = (Provider)std::clamp(std::stoi(db.getSetting("llm.provider", "0")), 0, 2);
        llm.claudeKey = secret::unprotect(db.getSetting("llm.claude.key"));
        llm.openaiKey = secret::unprotect(db.getSetting("llm.openai.key"));
        llm.geminiKey = secret::unprotect(db.getSetting("llm.gemini.key"));
        llm.claudeModel = db.getSetting("llm.claude.model", llm.claudeModel);
        llm.openaiModel = db.getSetting("llm.openai.model", llm.openaiModel);
        llm.geminiModel = db.getSetting("llm.gemini.model", llm.geminiModel);
        providerSel = (int)llm.provider;
        videoVolume = std::clamp(std::stoi(db.getSetting("audio.videoVolume", "100")), 0, 100);
        recVolume = std::clamp(std::stoi(db.getSetting("audio.recVolume", "100")), 0, 200);
        ttsSlow = db.getSetting("tts.slow", "0") == "1";
        snprintf(keyBuf[0], sizeof keyBuf[0], "%s", llm.claudeKey.c_str());
        snprintf(keyBuf[1], sizeof keyBuf[1], "%s", llm.openaiKey.c_str());
        snprintf(keyBuf[2], sizeof keyBuf[2], "%s", llm.geminiKey.c_str());
        snprintf(modelBuf[0], sizeof modelBuf[0], "%s", llm.claudeModel.c_str());
        snprintf(modelBuf[1], sizeof modelBuf[1], "%s", llm.openaiModel.c_str());
        snprintf(modelBuf[2], sizeof modelBuf[2], "%s", llm.geminiModel.c_str());
    }

    // 설정 창의 입력값을 llm 에 반영하고 DB 에 저장 (키는 DPAPI 로 암호화)
    void applySettings() {
        llm.provider = (Provider)providerSel;
        llm.claudeKey = keyBuf[0]; llm.openaiKey = keyBuf[1]; llm.geminiKey = keyBuf[2];
        llm.claudeModel = modelBuf[0]; llm.openaiModel = modelBuf[1]; llm.geminiModel = modelBuf[2];
        db.setSetting("llm.provider", std::to_string(providerSel));
        db.setSetting("llm.claude.key", secret::protect(llm.claudeKey));
        db.setSetting("llm.openai.key", secret::protect(llm.openaiKey));
        db.setSetting("llm.gemini.key", secret::protect(llm.geminiKey));
        db.setSetting("llm.claude.model", llm.claudeModel);
        db.setSetting("llm.openai.model", llm.openaiModel);
        db.setSetting("llm.gemini.model", llm.geminiModel);
    }

    LlmConfig configFromBuffers() const {
        LlmConfig c = llm;
        c.provider = (Provider)providerSel;
        c.claudeKey = keyBuf[0]; c.openaiKey = keyBuf[1]; c.geminiKey = keyBuf[2];
        c.claudeModel = modelBuf[0]; c.openaiModel = modelBuf[1]; c.geminiModel = modelBuf[2];
        return c;
    }

    void startModelList(int provider) {
        if (llmJob.running) return;
        if (llmJob.th.joinable()) llmJob.th.join();
        llmJob.running = true; llmJob.done = false; llmJob.kind = 0; llmJob.provider = provider; llmJob.err.clear();
        LlmConfig cfg = configFromBuffers();
        llmJob.th = std::thread([this, cfg, provider] {
            std::string e;
            auto list = llmListModels(cfg, (Provider)provider, &e);
            std::lock_guard<std::mutex> lock(llmJob.m);
            llmJob.models = list; llmJob.err = e; llmJob.done = true;
        });
    }

    void startConnectionTest() {
        if (llmJob.running) return;
        if (llmJob.th.joinable()) llmJob.th.join();
        llmJob.running = true; llmJob.done = false; llmJob.kind = 1; llmJob.err.clear();
        LlmConfig cfg = configFromBuffers();
        llmJob.th = std::thread([this, cfg] {
            std::string e;
            std::string t = llmComplete(cfg, "Reply with exactly: OK", "ping", &e);
            std::lock_guard<std::mutex> lock(llmJob.m);
            llmJob.text = t; llmJob.err = e; llmJob.done = true;
        });
    }

    // ---- 문장 해설 ----
    void loadExplanations() {
        explanations.clear();
        explained = db.explainedSegments(video.id);
        for (int i : explained) explanations[i] = Explanation::fromJson(db.getExplanation(video.id, i));
    }

    void requestExplain(std::vector<int> idxs) {
        if (!loaded || idxs.empty()) return;
        if (!llm.ready()) { message = "AI 설정에서 API 키와 모델을 먼저 입력하세요"; showSettings = true; return; }
        if (explainJob.running) { message = "이미 해설을 만드는 중입니다"; return; }
        if (explainJob.th.joinable()) explainJob.th.join();
        explainJob.running = true;
        explainJob.cancel = false;
        explainJob.videoId = video.id;
        explainJob.queue = std::move(idxs);
        explainJob.done = 0;
        explainJob.err.clear();
        explainJob.ready.clear();
        LlmConfig cfg = llm;
        std::vector<Segment> segs = video.segs;
        std::vector<int> queue = explainJob.queue;
        explainJob.th = std::thread([this, cfg, segs, queue] {
            for (int i : queue) {
                if (explainJob.cancel) break;
                std::string before = i > 0 ? segs[i - 1].text : "";
                std::string after = i + 1 < (int)segs.size() ? segs[i + 1].text : "";
                std::string e;
                Explanation ex = explainSentence(cfg, segs[i].text, before, after, &e);
                std::lock_guard<std::mutex> lock(explainJob.m);
                explainJob.done++;
                if (e.empty()) explainJob.ready.push_back({i, ex});
                else { explainJob.err = e; if (queue.size() == 1) break; }
            }
            std::lock_guard<std::mutex> lock(explainJob.m);
            explainJob.running = false;
        });
    }

    void requestExplainAll() {
        std::vector<int> idxs;
        for (int i = 0; i < (int)video.segs.size(); ++i) if (!explained.count(i)) idxs.push_back(i);
        if (idxs.empty()) { message = "모든 문장에 해설이 있습니다"; return; }
        requestExplain(idxs);
    }

    void saveExpression(int segIdx, const Expression& e) {
        if (db.hasExpression(video.id, segIdx, e.text)) return;
        db.addExpression(video.id, segIdx, e.text, e.meaning, e.note, e.example);
        cardsDirty = true;
        libraryDirty = true;
        message = "표현 노트에 저장: " + e.text;
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

    // ---- 문장 학습 ----
    // 문제 진행 중(정답 공개 전)에는 화면 곳곳의 원문 표시를 숨긴다
    bool quizHidden() const { return valid(quizSeg) && quizSeg == current && !quizRevealed; }

    void startQuiz(int i, int kind) {
        if (!valid(i)) return;
        quizKind = kind;
        quizSeg = i;
        quizRevealed = false;
        quizChecked = false;
        quizBuf[0] = '\0';
        if (kind == 0) {
            playSegment(i, 1);  // 리스닝: 자막 없이 듣는다
        } else {
            // 영작: 소리도 정답이라 재생하지 않는다
            stopAll();
            current = i;
            scrollToCurrent = true;
        }
        forceTab = Tab::Quiz;
    }

    void endQuiz() {
        quizSeg = -1;
        quizRevealed = false;
        quizChecked = false;
        quizBuf[0] = '\0';
    }

    void checkQuiz() {
        if (!valid(quizSeg)) return;
        quizScore = scoreTranscript(seg(quizSeg).text, quizBuf);
        quizChecked = true;
        quizRevealed = true;
        long long pid = log(quizSeg, quizKind == 0 ? "listen" : "compose");
        db.setPracticeScore(pid, quizScore.accuracy);
        refreshStats();
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
        // 복습 세션의 현재 항목이 표현 카드면 표현에, 아니면 문장에 평가를 적용한다
        if (sessionItemIsCurrent() && session.queue[session.pos].expressionId > 0) {
            db.rateExpression(session.queue[session.pos].expressionId, g);
            cardsDirty = true;
        } else {
            db.rate(video.id, current, g);
        }
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
        // AI 모델 목록 / 연결 테스트
        {
            std::lock_guard<std::mutex> lock(llmJob.m);
            if (llmJob.done) {
                llmJob.done = false;
                llmJob.running = false;
                if (llmJob.kind == 0) {
                    if (llmJob.err.empty()) { modelList[llmJob.provider] = llmJob.models; settingsMsg = std::to_string(llmJob.models.size()) + "개 모델을 찾았습니다. 목록에서 고르세요"; }
                    else settingsMsg = "모델 목록 실패: " + llmJob.err;
                } else {
                    settingsMsg = llmJob.err.empty() ? "연결 성공: " + llmJob.text.substr(0, 40) : "연결 실패: " + llmJob.err;
                }
            }
        }
        // 문장 해설 결과 수거
        {
            std::lock_guard<std::mutex> lock(explainJob.m);
            if (!explainJob.ready.empty()) {
                for (auto& [i, ex] : explainJob.ready) {
                    db.setExplanation(explainJob.videoId, i, ex.toJson());
                    if (loaded && video.id == explainJob.videoId) { explanations[i] = ex; explained.insert(i); }
                }
                explainJob.ready.clear();
            }
            if (!explainJob.running && !explainJob.err.empty()) { message = "해설 실패: " + explainJob.err; explainJob.err.clear(); }
        }
        // 채점
        // 단어 뜻 결과 수거
        {
            std::lock_guard<std::mutex> lock(wordJob.m);
            if (wordJob.done) {
                wordJob.done = false;
                wordJob.running = false;
                // AI 결과만 캐시한다 (사전은 빠르고 무료라 매번 새로 받는다)
                if (!wordJob.result.empty() && wordJob.result.provider != "사전") db.setWordMeaning(wordJob.word, wordJob.sentence, wordJob.result.toJson());
                if (wordJob.word == wordKey && wordJob.sentence == wordSentence) {
                    wordInfo = wordJob.result;
                    wordErr = wordJob.err;
                    wordLoading = false;
                }
                if (wordJob.hasPending) startWordJob(wordJob.pendingWord, wordJob.pendingSentence);
            }
        }
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
        loadExplanations();
        if (pendingSeg >= 0) {
            // 영상이 mpv 에 실제로 열린 뒤 재생해야 하므로 프레임 뒤로 미룬다
            current = std::min(pendingSeg, (int)video.segs.size() - 1);
            scrollToCurrent = true;
            pendingSeg = -1;
            pendingPlay = true;
        }
    }

    void requestLoad(const std::string& input) {
        std::string in = input;
        while (!in.empty() && std::isspace((unsigned char)in.back())) in.pop_back();
        while (!in.empty() && std::isspace((unsigned char)in.front())) in.erase(0, 1);
        if (in.size() >= 2 && in.front() == '"' && in.back() == '"') in = in.substr(1, in.size() - 2);
        if (local::isLocalId(in)) { loader.stt = &stt; loader.start(in); return; }
        // 내 PC 의 파일 경로?
        std::error_code ec;
        if (fs::is_regular_file(fs::u8path(in), ec)) {
            std::string ext = fs::u8path(in).extension().u8string();
            if (local::isVideoExt(ext)) { requestLoadFile(in, ""); return; }
            if (subtitle::isSubtitleExt(ext)) { attachSubtitle(in); return; }
            message = "지원하지 않는 파일입니다 (영상: mkv/mp4/avi/mov/webm, 자막: smi/srt/vtt)";
            return;
        }
        auto id = yt::extractVideoId(in);
        if (!id) { message = "유튜브 영상 ID 나 영상 파일 경로로 인식되지 않습니다"; return; }
        loader.stt = &stt;
        loader.start(*id);
    }

    // ---- 내 영상 파일 ----
    void requestLoadFile(const std::string& videoPath, const std::string& subPath) {
        if (loader.busy) { message = "다른 영상을 불러오는 중입니다"; return; }
        const std::string id = local::makeId(videoPath);
        const std::string dir = paths::dataDir() + "/" + id;
        std::error_code ec;
        const bool registered = fs::exists(fs::u8path(dir + "/segments.json"), ec);
        std::string sub = subPath;
        if (registered) {
            // 이미 등록된 영상: 자막을 새로 지정했으면 교체 (학습 기록이 있으면 문장 번호가 어긋나므로 거부)
            if (!sub.empty()) {
                if (db.countsFor(id).empty()) local::replaceSubtitle(sub, dir);
                else message = "이미 학습 기록이 있는 영상이라 자막은 바꾸지 않고 그대로 엽니다";
            }
            sub.clear();
        } else if (sub.empty()) {
            sub = local::findSiblingSubtitle(videoPath);
        }
        loader.localVideo = videoPath;
        loader.localSub = sub;
        snprintf(urlBuf, sizeof urlBuf, "%s", videoPath.c_str());
        loader.stt = &stt;
        loader.start(id);
    }

    // 자막 파일만 들어왔을 때: 열려 있는 로컬 영상의 자막으로 바꾼다
    void attachSubtitle(const std::string& subPath) {
        if (!loaded || !local::isLocalId(video.id)) { message = "자막을 적용할 영상 파일을 먼저 여세요 (영상과 자막을 함께 끌어다 놓아도 됩니다)"; return; }
        if (loader.busy) { message = "다른 영상을 불러오는 중입니다"; return; }
        if (!db.countsFor(video.id).empty()) { message = "이미 학습 기록이 있는 영상이라 자막을 바꿀 수 없습니다 (문장 번호가 어긋납니다)"; return; }
        local::replaceSubtitle(subPath, video.dir);
        loader.stt = &stt;
        loader.start(video.id);
    }

    void onDropFiles(const std::vector<std::string>& paths) {
        std::string videoPath, subPath;
        for (const auto& p : paths) {
            std::string ext = fs::u8path(p).extension().u8string();
            if (local::isVideoExt(ext) && videoPath.empty()) videoPath = p;
            else if (subtitle::isSubtitleExt(ext) && subPath.empty()) subPath = p;
        }
        if (!videoPath.empty()) requestLoadFile(videoPath, subPath);
        else if (!subPath.empty()) attachSubtitle(subPath);
        else message = "영상 파일(mkv/mp4 …) 이나 자막 파일(smi/srt/vtt) 을 끌어다 놓으세요";
    }

#ifdef _WIN32
    void openFileDialog() {
        wchar_t file[4096] = L"";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof ofn;
        ofn.hwndOwner = GetActiveWindow();
        ofn.lpstrFilter = L"영상 파일\0*.mkv;*.mp4;*.avi;*.mov;*.webm;*.m4v;*.ts;*.wmv;*.flv;*.mpg;*.mpeg\0모든 파일\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = (DWORD)(sizeof file / sizeof file[0]);
        ofn.lpstrTitle = L"영상 파일 열기 (같은 이름의 .smi/.srt/.vtt 자막이 있으면 함께 읽습니다)";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
        if (!GetOpenFileNameW(&ofn)) return;
        int n = WideCharToMultiByte(CP_UTF8, 0, file, -1, nullptr, 0, nullptr, nullptr);
        std::string path(n > 0 ? n - 1 : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, file, -1, &path[0], n, nullptr, nullptr);
        if (!path.empty()) requestLoadFile(path, "");
    }
#endif

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
        // 자막 오버레이 (문장 학습 문제 중에는 정답이라 숨긴다)
        if (valid(current) && !quizHidden()) {
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
            if (explained.count(i)) extra += "  해설";
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
            const char* rowText = (i == quizSeg && valid(quizSeg) && !quizRevealed) ? "(학습 문제 진행 중 — 정답 공개 전까지 숨김)" : s.text.c_str();
            dl->AddText(font, fs, ImVec2(top.x + 4, top.y + 2 + ImGui::GetTextLineHeight()), col, rowText, nullptr, wrap);
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
        if (quizHidden()) { ImGui::TextDisabled("문장 학습 문제 진행 중에는 이 문장의 기록을 숨깁니다."); return; }
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
                const char* modeName = h.mode == "shadow" ? "쉐도잉" : h.mode == "echo" ? "따라말하기" : h.mode == "record" ? "녹음"
                                     : h.mode == "repeat" ? "반복" : h.mode == "listen" ? "리스닝" : h.mode == "compose" ? "영작" : "재생";
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

    // AI 해설 탭: 현재 문장의 번역, 표현, 문법
    // 문장 학습 탭: 리스닝(자막 없이 받아쓰기) / 영작(한국어 번역 보고 영어로 쓰기)
    void drawQuizTab() {
        if (!loaded) { ImGui::TextDisabled("영상을 불러오면 문장 학습을 할 수 있습니다."); return; }

        ImGui::TextDisabled("자막 없이 듣고 받아쓰거나, 한국어 번역만 보고 영어로 써 보세요. 대소문자와 문장 부호는 채점에서 무시합니다.");
        ImGui::RadioButton("리스닝 (받아쓰기)", &quizKind, 0);
        ImGui::SameLine();
        ImGui::RadioButton("영작 (한국어 → 영어)", &quizKind, 1);
        ImGui::Separator();

        if (!valid(quizSeg)) {
            int base = valid(current) ? current : 0;
            char label[64];
            snprintf(label, sizeof label, "[%d]번 문장으로 시작", base);
            if (ImGui::Button(label)) startQuiz(base, quizKind);
            ImGui::TextDisabled("[문장] 탭에서 문장을 고른 뒤 시작하세요. 시작하면 정답이 공개될 때까지 자막과 원문이 숨겨집니다.");
            return;
        }

        const auto& s = seg(quizSeg);
        ImGui::TextDisabled("[%d] %s  ·  %s", quizSeg, transcript::formatTime(s.startMs).c_str(), quizKind == 0 ? "리스닝" : "영작");

        if (quizKind == 0) {
            if (ImGui::Button("다시 듣기")) playSegment(quizSeg, 1);
            ImGui::SameLine();
            ImGui::TextDisabled("(속도는 아래 [속도] 슬라이더로 조절)");
        } else {
            bool explaining;
            { std::lock_guard<std::mutex> lock(explainJob.m); explaining = explainJob.running; }
            auto it = explanations.find(quizSeg);
            if (it != explanations.end() && !it->second.translation.empty()) {
                ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "한국어:");
                ImGui::TextWrapped("%s", it->second.translation.c_str());
            } else if (explaining) {
                ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "이 문장의 번역(AI 해설)을 만드는 중...");
            } else if (llm.ready()) {
                ImGui::TextWrapped("이 문장의 한국어 번역이 아직 없습니다. AI 해설을 만들면 번역이 문제로 나옵니다.");
                if (ImGui::Button("이 문장 해설 생성")) requestExplain({quizSeg});
            } else {
                ImGui::TextWrapped("영작 문제는 AI 해설의 한국어 번역을 씁니다. 상단 [AI 설정]에서 API 키를 넣으세요.");
                if (ImGui::Button("AI 설정 열기")) showSettings = true;
            }
        }

        ImGui::Spacing();
        ImGui::TextDisabled(quizKind == 0 ? "들리는 대로 영어로 쓰세요:" : "영어로 옮겨 보세요:");
        ImGui::InputTextMultiline("##quizinput", quizBuf, sizeof quizBuf,
                                  ImVec2(-1, ImGui::GetTextLineHeight() * 4),
                                  quizChecked ? ImGuiInputTextFlags_ReadOnly : 0);

        if (!quizRevealed) {
            ImGui::BeginDisabled(quizBuf[0] == '\0');
            if (ImGui::Button("확인 (채점)")) checkQuiz();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("정답 보기 (포기)")) quizRevealed = true;
            ImGui::SameLine();
            if (ImGui::Button("그만하기")) endQuiz();
        } else {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "정답:");
            ImGui::SameLine();
            if (tts.available() && ImGui::SmallButton("발음 듣기")) speak(s.text);
            ImGui::SameLine();
            if (ImGui::SmallButton("영상으로 듣기")) playSegment(quizSeg, 1);
            ImGui::TextWrapped("%s", s.text.c_str());
            ImGui::Spacing();
            if (quizChecked) {
                drawScoreMarks(quizScore);
                ImGui::TextDisabled("초록: 맞음  빨강: 빠짐  주황: 다르게 씀(내가 쓴 단어)  회색: 추가로 쓴 단어");
            } else {
                ImGui::TextDisabled("채점 없이 정답을 봤습니다.");
            }
            ImGui::Spacing();
            if (ImGui::Button("같은 문장 다시")) startQuiz(quizSeg, quizKind);
            ImGui::SameLine();
            ImGui::BeginDisabled(!valid(quizSeg + 1));
            if (ImGui::Button("다음 문장")) startQuiz(quizSeg + 1, quizKind);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("끝내기")) endQuiz();
        }
    }

    void drawExplainTab() {
        if (quizHidden()) {
            ImGui::TextDisabled("문장 학습 문제 진행 중에는 해설을 숨깁니다. [학습] 탭에서 정답을 확인한 뒤 보세요.");
            return;
        }
        bool running;
        int done, total;
        { std::lock_guard<std::mutex> lock(explainJob.m); running = explainJob.running; done = explainJob.done; total = (int)explainJob.queue.size(); }

        if (!llm.ready()) {
            ImGui::TextWrapped("AI 해설을 쓰려면 API 키가 필요합니다. 상단 [AI 설정]에서 Claude / ChatGPT / Gemini 중 하나를 골라 키를 넣으세요.");
            if (ImGui::Button("AI 설정 열기")) showSettings = true;
            return;
        }
        ImGui::TextDisabled("%s · %s   해설 %d / %d 문장", providerName(llm.provider), llm.model(llm.provider).c_str(), (int)explained.size(), (int)video.segs.size());
        if (running) {
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "해설 생성 중 %d / %d", done, total);
            ImGui::SameLine();
            if (ImGui::SmallButton("중단")) explainJob.cancel = true;
        } else {
            if (ImGui::Button("이 문장 해설")) { if (valid(current)) requestExplain({current}); }
            ImGui::SameLine();
            if (ImGui::Button("모든 문장 해설")) requestExplainAll();
            ImGui::SameLine();
            ImGui::TextDisabled("(%d개 남음)", (int)video.segs.size() - (int)explained.size());
        }
        ImGui::Separator();
        if (!valid(current)) { ImGui::TextDisabled("문장을 선택하세요."); return; }
        ImGui::TextWrapped("[%d] %s", current, seg(current).text.c_str());
        ImGui::Spacing();
        auto it = explanations.find(current);
        if (it == explanations.end()) {
            ImGui::TextDisabled(running ? "이 문장의 해설을 기다리는 중..." : "아직 해설이 없습니다. [이 문장 해설]을 누르세요.");
            return;
        }
        const Explanation& ex = it->second;
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "번역");
        ImGui::TextWrapped("%s", ex.translation.c_str());
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "표현");
        for (size_t k = 0; k < ex.expressions.size(); ++k) {
            const auto& e = ex.expressions[k];
            ImGui::PushID((int)k);
            bool saved = db.hasExpression(video.id, current, e.text);
            ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.5f, 1.0f), "%s", e.text.c_str());
            ImGui::SameLine();
            if (tts.available() && ImGui::SmallButton("듣기")) speak(e.text);
            ImGui::SameLine();
            if (saved) ImGui::TextDisabled("저장됨");
            else if (ImGui::SmallButton("저장")) saveExpression(current, e);
            ImGui::TextWrapped("뜻: %s", e.meaning.c_str());
            if (!e.note.empty()) ImGui::TextWrapped("%s", e.note.c_str());
            if (!e.example.empty()) { ImGui::TextDisabled("예: "); ImGui::SameLine(); ImGui::TextWrapped("%s", e.example.c_str()); }
            ImGui::Spacing();
            ImGui::PopID();
        }
        if (!ex.grammar.empty()) {
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "문법");
            ImGui::TextWrapped("%s", ex.grammar.c_str());
        }
        ImGui::Spacing();
        ImGui::TextDisabled("%s · %s", ex.provider.c_str(), ex.model.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("다시 생성")) { explained.erase(current); explanations.erase(current); requestExplain({current}); }
    }

    // 표현 노트 탭: 저장한 표현 카드 전체
    void drawCardsTab() {
        if (cardsDirty) { cards = db.expressions(); cardsDirty = false; }
        ImGui::TextDisabled("저장한 표현 %d개  |  복습 시점이 되면 홈의 [복습 시작]에 문장과 함께 나옵니다", (int)cards.size());
        ImGui::Separator();
        if (cards.empty()) { ImGui::TextDisabled("[표현] 탭에서 해설을 만들고 [저장]을 누르면 여기에 모입니다."); return; }
        for (size_t k = 0; k < cards.size(); ++k) {
            const auto& c = cards[k];
            ImGui::PushID((int)c.id);
            ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.5f, 1.0f), "%s", c.text.c_str());
            ImGui::SameLine();
            if (tts.available() && ImGui::SmallButton("듣기")) speak(c.text);
            ImGui::SameLine();
            ImGui::TextDisabled("(%d회, 다음 %s)", c.reviews, c.dueAt.substr(0, 10).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("문장")) {
                ReviewItem it; it.videoId = c.videoId; it.segIdx = c.segIdx;
                openReviewItem(it);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("삭제")) { db.deleteExpression(c.id); cardsDirty = true; }
            ImGui::TextWrapped("%s", c.meaning.c_str());
            if (!c.example.empty()) { ImGui::TextDisabled("예: "); ImGui::SameLine(); ImGui::TextWrapped("%s", c.example.c_str()); }
            ImGui::Spacing();
            ImGui::PopID();
        }
    }

    // AI 설정 창
    void drawSettings() {
        if (showSettings && !ImGui::IsPopupOpen("AI 설정")) ImGui::OpenPopup("AI 설정");
        ImGui::SetNextWindowSize(ImVec2(720 * uiScale, 0));
        if (!ImGui::BeginPopupModal("AI 설정", &showSettings, ImGuiWindowFlags_AlwaysAutoResize)) return;
        bool jobRunning;
        { std::lock_guard<std::mutex> lock(llmJob.m); jobRunning = llmJob.running; }

        ImGui::TextWrapped("문장 해설(번역, 표현, 문법)에 쓸 AI 를 고르고 API 키를 넣으세요. 키는 이 PC 의 사용자 계정으로만 풀 수 있게 암호화해서 저장합니다.");
        ImGui::Spacing();
        ImGui::TextDisabled("사용할 서비스");
        ImGui::RadioButton("Claude (Anthropic)", &providerSel, 0); ImGui::SameLine();
        ImGui::RadioButton("ChatGPT (OpenAI)", &providerSel, 1); ImGui::SameLine();
        ImGui::RadioButton("Gemini (Google)", &providerSel, 2);
        ImGui::Separator();

        const char* names[3] = {"Claude", "ChatGPT", "Gemini"};
        const char* keyHints[3] = {"sk-ant-...  (console.anthropic.com)", "sk-...  (platform.openai.com)", "AIza...  (aistudio.google.com)"};
        for (int p = 0; p < 3; ++p) {
            ImGui::PushID(p);
            bool active = (p == providerSel);
            if (!active) ImGui::BeginDisabled();
            ImGui::TextColored(active ? ImVec4(0.6f, 0.8f, 1.0f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", names[p]);
            ImGui::SetNextItemWidth(420 * uiScale);
            ImGui::InputTextWithHint("API 키", keyHints[p], keyBuf[p], sizeof keyBuf[p], ImGuiInputTextFlags_Password);
            ImGui::SetNextItemWidth(300 * uiScale);
            ImGui::InputText("모델", modelBuf[p], sizeof modelBuf[p]);
            ImGui::SameLine();
            ImGui::BeginDisabled(jobRunning);
            if (ImGui::Button("모델 목록")) startModelList(p);
            ImGui::EndDisabled();
            if (!modelList[p].empty()) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(300 * uiScale);
                if (ImGui::BeginCombo("##models", "목록에서 선택")) {
                    for (const auto& m : modelList[p]) if (ImGui::Selectable(m.c_str())) snprintf(modelBuf[p], sizeof modelBuf[p], "%s", m.c_str());
                    ImGui::EndCombo();
                }
            }
            if (!active) ImGui::EndDisabled();
            ImGui::Spacing();
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::BeginDisabled(jobRunning);
        if (ImGui::Button("연결 테스트")) startConnectionTest();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("저장")) { applySettings(); settingsMsg = "저장했습니다"; }
        ImGui::SameLine();
        if (ImGui::Button("닫기")) { showSettings = false; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (jobRunning) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "요청 중...");
        else if (!settingsMsg.empty()) ImGui::TextWrapped("%s", settingsMsg.c_str());
        ImGui::EndPopup();
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
            if (ImGui::BeginTabItem("학습", nullptr, forceTab == Tab::Quiz ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::BeginChild("quiz");
                drawQuizTab();
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
            if (ImGui::BeginTabItem("기록", nullptr, forceTab == Tab::History ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::BeginChild("history");
                drawHistoryList();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("표현", nullptr, forceTab == Tab::Explain ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::BeginChild("explain");
                drawExplainTab();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("표현 노트", nullptr, forceTab == Tab::Cards ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::BeginChild("cards");
                drawCardsTab();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        forceTab = Tab::None;  // SetSelected 는 한 프레임만
        ImGui::EndChild();
    }

    // ---------------- 단어 행: 클릭 = 발음 + 뜻, 드래그 = 복사 ----------------

    static std::string lowerAscii(std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    void copyText(const std::string& text) {
        if (text.empty()) return;
        ImGui::SetClipboardText(text.c_str());
        message = "복사됨: " + (text.size() > 60 ? text.substr(0, 60) + "..." : text);
        wordDrag.copiedAt = Clock::now();
    }

    // 마우스 위치에서 가장 가까운 단어 (같은 줄 우선)
    static int nearestWord(const std::vector<std::pair<ImVec2, ImVec2>>& rects, ImVec2 p) {
        int best = -1;
        float bestD = 1e30f;
        for (size_t i = 0; i < rects.size(); ++i) {
            const auto& [a, b] = rects[i];
            float dx = p.x < a.x ? a.x - p.x : p.x > b.x ? p.x - b.x : 0.0f;
            float dy = p.y < a.y ? a.y - p.y : p.y > b.y ? p.y - b.y : 0.0f;
            float d = dy * 1000.0f + dx;
            if (d < bestD) { bestD = d; best = (int)i; }
        }
        return best;
    }

    // 단어 뜻 찾기: 캐시 → AI(키가 있으면) → 무료 영어 사전
    void lookupWord(const std::string& word, const std::string& sentence) {
        wordQuery = word;
        wordKey = lowerAscii(word);
        wordSentence = sentence;
        wordErr.clear();
        wordInfo = WordMeaning();
        wordLoading = false;
        std::string cached = db.getWordMeaning(wordKey, sentence);
        if (!cached.empty()) {
            WordMeaning c = WordMeaning::fromJson(cached);
            // 사전 결과만 있는데 지금은 AI 를 쓸 수 있으면 다시 찾는다
            if (!c.empty() && !(c.provider == "사전" && llm.ready())) { wordInfo = c; return; }
        }
        wordLoading = true;
        if (wordJob.running) { wordJob.hasPending = true; wordJob.pendingWord = wordKey; wordJob.pendingSentence = sentence; return; }
        startWordJob(wordKey, sentence);
    }

    void startWordJob(const std::string& key, const std::string& sentence) {
        if (wordJob.th.joinable()) wordJob.th.join();
        wordJob.running = true; wordJob.done = false; wordJob.hasPending = false;
        wordJob.word = key; wordJob.sentence = sentence;
        LlmConfig cfg = llm;
        wordJob.th = std::thread([this, cfg, key, sentence] {
            std::string e;
            WordMeaning r;
            if (cfg.ready()) {
                r = explainWord(cfg, key, sentence, &e);
                if (r.empty()) {  // AI 실패 → 사전으로 대체 (오류는 함께 보여 준다)
                    std::string e2;
                    WordMeaning d = lookupDictionary(key, &e2);
                    if (!d.empty()) r = d;
                }
            } else {
                r = lookupDictionary(key, &e);
            }
            std::lock_guard<std::mutex> lock(wordJob.m);
            wordJob.result = r; wordJob.err = e; wordJob.done = true;
        });
    }

    void drawWordRow() {
        if (quizHidden()) { ImGui::TextDisabled("단어: 문장 학습 문제 진행 중이라 단어를 숨깁니다"); return; }
        if (!valid(current)) { ImGui::TextDisabled("단어: 문장을 선택하면 단어를 클릭해 발음과 뜻을 보고, 드래그해서 복사할 수 있습니다"); return; }
        const std::string sentence = seg(current).text;

        ImGui::TextDisabled("단어:");
        ImGui::SameLine();
        ImGui::BeginDisabled(!tts.available());
        if (ImGui::Checkbox("천천히", &ttsSlow)) db.setSetting("tts.slow", ttsSlow ? "1" : "0");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (tts.available()) ImGui::SetTooltip("음성: %s", tts.voiceName().c_str());
            else ImGui::SetTooltip("이 PC 에 영어 음성 합성 엔진이 없어 발음을 들려줄 수 없습니다");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("문장 복사")) copyText(sentence);
        ImGui::SameLine();
        ImGui::TextDisabled("단어 클릭: 발음과 뜻  |  드래그: 복사");

        std::vector<std::string> words;
        { std::istringstream ss(sentence); std::string w; while (ss >> w) words.push_back(w); }
        std::vector<std::pair<ImVec2, ImVec2>> rects(words.size());

        // 드래그 판정: 누른 뒤 조금 움직이면 드래그 (버튼 클릭은 무시)
        const bool mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (wordDrag.anchor >= 0 && mouseDown && !wordDrag.dragging) {
            ImVec2 mp = ImGui::GetMousePos();
            if (std::fabs(mp.x - wordDrag.pressPos.x) + std::fabs(mp.y - wordDrag.pressPos.y) > 6.0f * uiScale) wordDrag.dragging = true;
        }

        const float lineRight = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x - ImGui::GetStyle().WindowPadding.x;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        int clicked = -1;
        for (size_t k = 0; k < words.size(); ++k) {
            ImGui::PushID((int)k);
            float bw = ImGui::CalcTextSize(words[k].c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
            if (ImGui::GetItemRectMax().x + spacing + bw <= lineRight) ImGui::SameLine();
            bool pressed = ImGui::SmallButton(words[k].c_str());
            rects[k] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                wordDrag.anchor = (int)k;
                wordDrag.dragging = false;
                wordDrag.pressPos = ImGui::GetMousePos();
                wordDrag.a = wordDrag.b = -1;
            }
            if (pressed && !wordDrag.dragging) clicked = (int)k;
            ImGui::PopID();
        }

        // 드래그 범위 갱신 / 놓으면 복사
        if (wordDrag.anchor >= 0) {
            if (wordDrag.anchor >= (int)words.size()) { wordDrag.anchor = -1; wordDrag.a = wordDrag.b = -1; }
            else {
                if (wordDrag.dragging) {
                    int cur = nearestWord(rects, ImGui::GetMousePos());
                    wordDrag.a = std::min(wordDrag.anchor, cur);
                    wordDrag.b = std::max(wordDrag.anchor, cur);
                }
                if (!mouseDown) {
                    if (wordDrag.dragging && wordDrag.a >= 0) {
                        std::string sel;
                        for (int i = wordDrag.a; i <= wordDrag.b; ++i) { if (i > wordDrag.a) sel += ' '; sel += words[i]; }
                        copyText(sel);
                    } else {
                        wordDrag.a = wordDrag.b = -1;
                    }
                    wordDrag.anchor = -1;
                    wordDrag.dragging = false;
                }
            }
        }
        // 선택 강조: 드래그 중이거나 복사 직후 잠깐
        const bool recentlyCopied = std::chrono::duration<float>(Clock::now() - wordDrag.copiedAt).count() < 1.2f;
        if (wordDrag.a >= 0 && wordDrag.b < (int)words.size() && (wordDrag.anchor >= 0 || recentlyCopied)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 col = wordDrag.anchor >= 0 ? IM_COL32(90, 170, 255, 90) : IM_COL32(120, 230, 120, 90);
            for (int i = wordDrag.a; i <= wordDrag.b; ++i) dl->AddRectFilled(rects[i].first, rects[i].second, col, 3.0f);
        } else if (wordDrag.a >= 0 && wordDrag.anchor < 0 && !recentlyCopied) {
            wordDrag.a = wordDrag.b = -1;
        }

        if (scriptWordClick >= 0 && scriptWordClick < (int)words.size()) { clicked = scriptWordClick; scriptWordClick = -1; }
        if (clicked >= 0) openWord(words[clicked], sentence, ImVec2(rects[clicked].first.x, rects[clicked].second.y + 4 * uiScale));

        // 단어 뜻 팝업
        ImGui::SetNextWindowPos(wordPopupPos, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(460 * uiScale, 0), ImGuiCond_Appearing);
        if (ImGui::BeginPopup("word_popup")) {
            drawWordPopup();
            ImGui::EndPopup();
        }
    }

    // 단어 클릭 처리 (스크립트에서도 사용)
    void openWord(const std::string& rawWord, const std::string& sentence, ImVec2 pos) {
        std::string b = bareWord(rawWord);
        if (b.empty()) return;
        if (tts.available()) speak(b);
        lookupWord(b, sentence);
        wordPopupPos = pos;
        ImGui::OpenPopup("word_popup");
    }

    void drawWordPopup() {
        // 화면 아래/오른쪽으로 넘치면 안쪽으로 밀어 넣는다 (크기는 자동이라 한 프레임 뒤에 맞춰진다)
        {
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
            float maxX = vp->WorkPos.x + vp->WorkSize.x - 8, maxY = vp->WorkPos.y + vp->WorkSize.y - 8;
            ImVec2 np(std::min(wp.x, maxX - ws.x), std::min(wp.y, maxY - ws.y));
            if (np.x != wp.x || np.y != wp.y) ImGui::SetWindowPos(ImVec2(std::max(np.x, vp->WorkPos.x), std::max(np.y, vp->WorkPos.y)));
        }
        const WordMeaning& w = wordInfo;
        const std::string head = w.word.empty() ? wordQuery : w.word;
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 1, 1), "%s", head.c_str());
        if (!w.ipa.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", w.ipa.c_str()); }
        if (!w.pos.empty()) { ImGui::SameLine(); ImGui::TextDisabled("[%s]", w.pos.c_str()); }
        if (!w.word.empty() && lowerAscii(w.word) != wordKey) { ImGui::SameLine(); ImGui::TextDisabled("(문장에서는 \"%s\")", wordQuery.c_str()); }
        ImGui::Separator();

        if (wordLoading) {
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", llm.ready() ? "AI 에게 뜻을 묻는 중..." : "사전에서 찾는 중...");
        } else {
            if (!w.meaning.empty()) ImGui::TextWrapped("%s", w.meaning.c_str());
            if (!w.contextMeaning.empty()) {
                ImGui::Spacing();
                ImGui::TextDisabled("이 문장에서");
                ImGui::TextWrapped("%s", w.contextMeaning.c_str());
            }
            if (!w.example.empty()) {
                ImGui::Spacing();
                ImGui::TextDisabled("예문");
                ImGui::TextWrapped("%s", w.example.c_str());
            }
            if (!wordErr.empty()) {
                ImGui::Spacing();
                ImGui::PushTextWrapPos(0);
                ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "%s", wordErr.c_str());
                ImGui::PopTextWrapPos();
            }
            if (!llm.ready()) {
                ImGui::Spacing();
                ImGui::TextDisabled("AI 설정에서 API 키를 넣으면 한국어 뜻과 문맥 설명을 볼 수 있습니다.");
            }
        }

        ImGui::Spacing();
        ImGui::BeginDisabled(!tts.available());
        if (ImGui::SmallButton("발음")) speak(wordKey);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("복사")) copyText(head);
        ImGui::SameLine();
        ImGui::BeginDisabled(wordLoading || w.empty() || !loaded || !valid(current));
        if (ImGui::SmallButton("표현 노트에 저장")) {
            Expression e;
            e.text = head;
            e.meaning = w.meaning;
            e.note = w.contextMeaning;
            e.example = w.example;
            saveExpression(current, e);
        }
        ImGui::EndDisabled();
        if (!llm.ready()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("AI 설정 열기")) { showSettings = true; ImGui::CloseCurrentPopup(); }
        } else if (!wordLoading && w.provider == "사전") {
            ImGui::SameLine();
            if (ImGui::SmallButton("AI 로 다시 찾기")) { db.setWordMeaning(wordKey, wordSentence, ""); lookupWord(wordQuery, wordSentence); }
        }
        if (!w.provider.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s · %s", w.provider.c_str(), w.model.c_str());
        }
    }

    void drawScoreLine() {
        if (quizHidden()) { ImGui::TextDisabled("문장 학습 문제 진행 중이라 채점 결과를 숨깁니다."); return; }
        if (scoring) { ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "채점 중..."); return; }
        if (!haveScore || scoreSeg != current) {
            if (!stt.loaded()) ImGui::TextDisabled("STT 모델을 받으면 녹음을 자동으로 채점합니다.");
            else ImGui::TextDisabled("쉐도잉 / 따라말하기 후 여기에 채점 결과가 표시됩니다.");
            return;
        }
        const auto& r = lastScore;
        drawScoreMarks(r);
        ImGui::TextDisabled("초록: 맞음  빨강: 빠짐  주황: 다르게 들림(들린 단어)  회색: 추가로 들린 단어  |  들린 문장: %s", r.heard.c_str());
    }

    // 정확도 + 단어별 채점 표시 (발음 채점과 문장 학습에서 공용)
    void drawScoreMarks(const ScoreResult& r) {
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
                ImGui::TextDisabled("%s%s", local::isLocalId(v.id) ? "[내 파일] " : "", v.title.c_str());
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
        int dueSeg = db.dueCount(), dueExpr = db.dueExpressionCount();
        int due = dueSeg + dueExpr;
        if (due > 0) ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "복습할 항목 %d개 (문장 %d · 표현 %d)", due, dueSeg, dueExpr);
        else ImGui::TextDisabled("복습할 항목이 없습니다");
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
            ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "복습 세션 %d / %d   (어려움 %d · 보통 %d · 쉬움 %d)   아래 [어려움/보통/쉬움] 을 누르면 다음으로 넘어갑니다",
                               (int)session.pos + 1, (int)session.queue.size(), session.hard, session.good, session.easy);
            ImGui::SameLine(0, 16);
            if (ImGui::SmallButton("건너뛰기")) sessionSkip();
            ImGui::SameLine();
            if (ImGui::SmallButton("세션 종료")) endSession();
            // 표현 카드: 앞면(표현) → [뜻 보기] → 뒷면(뜻, 예문)
            if (session.pos < session.queue.size() && session.queue[session.pos].expressionId > 0) {
                const auto& it = session.queue[session.pos];
                ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.5f, 1.0f), "표현 카드:  %s", it.text.c_str());
                ImGui::SameLine();
                if (tts.available() && ImGui::SmallButton("듣기")) speak(it.text);
                ImGui::SameLine();
                if (!cardRevealed) { if (ImGui::SmallButton("뜻 보기")) cardRevealed = true; }
                else {
                    ImGui::TextWrapped("뜻: %s   %s", it.meaning.c_str(), it.note.c_str());
                    if (!it.example.empty()) ImGui::TextWrapped("예: %s", it.example.c_str());
                }
            }
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
        ImGui::SetNextItemWidth(120 * uiScale);
        if (ImGui::SliderInt("영상 음량", &videoVolume, 0, 100, "%d%%")) { mpv.setVolume(videoVolume); db.setSetting("audio.videoVolume", std::to_string(videoVolume)); }
        ImGui::SameLine(0, 12);
        ImGui::SetNextItemWidth(120 * uiScale);
        if (ImGui::SliderInt("내 녹음 음량", &recVolume, 0, 200, "%d%%")) { recPlayer.setGain(recVolume / 100.0f); db.setSetting("audio.recVolume", std::to_string(recVolume)); }
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

        // 4행: 단어 발음 (클릭하면 읽어 준다)
        drawWordRow();

        // 5행: 채점 결과 + 난이도 평가
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
        if (cmd == "load") { std::string rest; std::getline(ss, rest); snprintf(urlBuf, sizeof urlBuf, "%s", (arg + rest).c_str()); requestLoad(arg + rest); }
        else if (cmd == "wait") scriptWaitUntil = Clock::now() + std::chrono::milliseconds((int)(std::stod(arg) * 1000));
        else if (cmd == "play") playSegment(std::stoi(arg), loopsSetting);
        else if (cmd == "echo") startEcho(std::stoi(arg));
        else if (cmd == "record") startRecordOnly(std::max(0, current));
        else if (cmd == "stop") stopAll();
        else if (cmd == "home") goHome();
        else if (cmd == "explain" && valid(current)) requestExplain({current});
        else if (cmd == "explain_all") requestExplainAll();
        else if (cmd == "settings") showSettings = true;
        else if (cmd == "say") { std::string rest; std::getline(ss, rest); speak(arg + rest); fprintf(stderr, "[tts] voice=%s available=%d\n", tts.voiceName().c_str(), (int)tts.available()); }
        else if (cmd == "tab") forceTab = arg == "explain" ? Tab::Explain : arg == "cards" ? Tab::Cards : arg == "review" ? Tab::Review : arg == "history" ? Tab::History : arg == "quiz" ? Tab::Quiz : Tab::Sentences;
        else if (cmd == "quiz") startQuiz(std::max(0, current), arg == "compose" ? 1 : 0);
        else if (cmd == "quiz_answer") { std::string rest; std::getline(ss, rest); snprintf(quizBuf, sizeof quizBuf, "%s", (arg + rest).c_str()); checkQuiz(); }
        else if (cmd == "quiz_reveal") quizRevealed = true;
        else if (cmd == "word" && valid(current)) {  // word <k>: 현재 문장의 k 번째 단어를 클릭한 것처럼
            std::vector<std::string> ws; std::istringstream ws_(seg(current).text); for (std::string w; ws_ >> w;) ws.push_back(w);
            int k = std::clamp(std::atoi(arg.c_str()), 0, (int)ws.size() - 1);
            scriptWordClick = k;
        }
        else if (cmd == "copy" && valid(current)) {  // copy <a> <b>: 단어 a~b 를 드래그해 복사한 것처럼
            std::vector<std::string> ws; std::istringstream ws_(seg(current).text); for (std::string w; ws_ >> w;) ws.push_back(w);
            int a = std::clamp(std::atoi(arg.c_str()), 0, (int)ws.size() - 1), b = a; ss >> b; b = std::clamp(b, a, (int)ws.size() - 1);
            std::string sel; for (int i = a; i <= b; ++i) { if (i > a) sel += ' '; sel += ws[i]; }
            wordDrag.a = a; wordDrag.b = b; copyText(sel);
        }
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
        bool enter = ImGui::InputTextWithHint("##url", "유튜브 URL / 영상 ID / 내 영상 파일 경로 (파일을 창에 끌어다 놓아도 됩니다)", urlBuf, sizeof urlBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("불러오기") || enter) requestLoad(urlBuf);
#ifdef _WIN32
        ImGui::SameLine();
        if (ImGui::Button("파일 열기")) openFileDialog();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("내 PC 의 영상 파일(mkv, mp4 …)을 엽니다.\n같은 폴더에 같은 이름의 .smi/.srt/.vtt 자막이 있으면 함께 읽고,\n없으면 영상 안의 영어 자막 트랙을 찾고, 그것도 없으면 whisper 로 대본을 만듭니다.");
#endif
        ImGui::EndDisabled();
        ImGui::SameLine(0, 16);
        if (ImGui::Button(llm.ready() ? "AI 설정" : "AI 설정 (키 없음)")) showSettings = true;
        ImGui::SameLine(0, 16);
        if (!stt.loaded()) {
            if (sttBusy) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", sttStatus.c_str());
            else if (ImGui::Button("STT 모델 받기 (148MB, 채점/자막 생성용)")) sttLoader.start(&stt, true);
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "STT 준비됨");
        }
        ImGui::SameLine(0, 16);
        if (busy) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", status.c_str());
        else if (!message.empty()) {
            const bool info = message.rfind("복사됨", 0) == 0 || message.rfind("표현 노트에 저장", 0) == 0;
            ImGui::TextColored(info ? ImVec4(0.5f, 0.9f, 0.5f, 1) : ImVec4(1, 0.4f, 0.4f, 1), "%s", message.c_str());
        }
        else if (loaded) ImGui::TextDisabled("%s", video.title.c_str());
        else ImGui::TextDisabled(" ");  // SameLine 뒤에 항목이 없으면 다음 줄이 옆으로 붙는다

        // 본문 레이아웃
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (showHome || !loaded) {
            mpv.render(0, 0);  // 이벤트 큐만 비운다 (FILE_LOADED 감지)
            drawHome(avail);
        } else {
            const float rightW = std::clamp(avail.x * 0.34f, 280.0f * uiScale, 520.0f * uiScale);
            const float bottomH = 330.0f * uiScale;
            drawVideoPanel(ImVec2(avail.x - rightW - 8, avail.y - bottomH - 8));
            ImGui::SameLine();
            drawRightPanel(ImVec2(rightW, avail.y - bottomH - 8));
            drawControls(ImVec2(avail.x, bottomH));
        }

        drawSettings();

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

// 초기화 실패 등 치명적 오류를 사용자에게 알린다
void fatalBox(const std::string& msg) {
#ifdef _WIN32
    MessageBoxA(nullptr, msg.c_str(), "YouShadow", MB_ICONERROR);
#else
    fprintf(stderr, "YouShadow: %s\n", msg.c_str());
    CFStringRef text = CFStringCreateWithCString(nullptr, msg.c_str(), kCFStringEncodingUTF8);
    if (text) {
        CFUserNotificationDisplayNotice(0, kCFUserNotificationStopAlertLevel, nullptr, nullptr, nullptr,
                                        CFSTR("YouShadow"), text, CFSTR("확인"));
        CFRelease(text);
    }
#endif
}

#ifdef _WIN32
// 처리되지 않은 예외(크래시)를 logs\crash.log 에 남기고 안내창을 띄운다
LONG WINAPI crashHandler(EXCEPTION_POINTERS* ep) {
    char buf[512];
    snprintf(buf, sizeof buf, "YouShadow v%s crashed: exception 0x%08lX at %p\r\n", YS_VERSION,
             ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0,
             ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr);
    std::string path = paths::logDir() + "/crash.log";
    if (FILE* f = fopen(path.c_str(), "ab")) { fputs(buf, f); fclose(f); }
    MessageBoxA(nullptr, (std::string("프로그램에 문제가 생겨 종료합니다.\n\n") + buf + "\n로그: " + path).c_str(), "YouShadow", MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

void installCrashHandler() { SetUnhandledExceptionFilter(crashHandler); }
#else
// 치명적 시그널을 logs/crash.log 에 남긴다 (시그널 핸들러라 async-safe 함수만 사용)
char g_crashLogPath[1024];

void crashSignal(int sig) {
    char buf[160];
    int n = snprintf(buf, sizeof buf, "YouShadow v%s crashed: signal %d\n", YS_VERSION, sig);
    int fd = open(g_crashLogPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        (void)!write(fd, buf, (size_t)n);
        close(fd);
    }
    _exit(128 + sig);
}

void installCrashHandler() {
    snprintf(g_crashLogPath, sizeof g_crashLogPath, "%s/crash.log", paths::logDir().c_str());
    for (int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS}) signal(sig, crashSignal);
}
#endif

}  // namespace

#ifndef YS_VERSION
#define YS_VERSION "dev"
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    paths::setup();
    installCrashHandler();

    glfwSetErrorCallback(glfwError);
    if (!glfwInit()) return 1;
#ifdef __APPLE__
    // macOS 는 3.2+ 코어 프로파일만 지원한다
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
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
#ifdef __APPLE__
    // macOS 는 창 좌표가 이미 포인트 단위라 레티나 배율을 UI 크기에 더하지 않는다
    const float uiScale = 1.0f;
#else
    const float uiScale = std::max(1.0f, scaleX);
#endif
    ImGui::GetStyle().ScaleAllSizes(uiScale);
#ifdef _WIN32
    const char* fontPath = "C:/Windows/Fonts/malgun.ttf";
#else
    const char* fontPath = "/System/Library/Fonts/AppleSDGothicNeo.ttc";  // 한글 지원 기본 폰트
#endif
    if (fs::exists(fontPath)) io.Fonts->AddFontFromFileTTF(fontPath, 18.0f * uiScale);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
#ifdef __APPLE__
    ImGui_ImplOpenGL3_Init("#version 150");
#else
    ImGui_ImplOpenGL3_Init("#version 130");
#endif

    App app;
    app.uiScale = uiScale;
    app.window = window;
    glfwSetWindowUserPointer(window, &app);
    glfwSetDropCallback(window, [](GLFWwindow* w, int count, const char** paths) {
        auto* a = static_cast<App*>(glfwGetWindowUserPointer(w));
        if (!a) return;
        std::vector<std::string> files;
        for (int i = 0; i < count; ++i) files.emplace_back(paths[i]);  // GLFW 는 UTF-8 로 준다
        a->onDropFiles(files);
    });
    std::string err;
    if (!app.mpv.init([](const char* n) { return (void*)glfwGetProcAddress(n); }, &err)) {
        fatalBox("영상 재생기(libmpv) 초기화 실패: " + err);
        return 1;
    }
    if (!app.db.open(paths::dataDir() + "/youshadow.db", &err)) {
        fatalBox(err);
        return 1;
    }
    app.db.importTsv(paths::dataDir() + "/practice.tsv");
    app.loadSettings();
    app.tts.init();
    app.applyAudioSettings();
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

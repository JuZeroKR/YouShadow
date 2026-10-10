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
#include <cfloat>
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
#include "endict.h"
#include "jadict.h"
#include "llm.h"
#include "local.h"
#include "paths.h"
#include "subtitle.h"
#include "player.h"
#include "prosody.h"
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
    Lang lang = Lang::En;
    std::vector<Segment> segs;
    std::vector<std::string> ko;  // 문장별 한국어 자막 (내 영상 파일에 한국어 자막이 있을 때만, 없으면 비어 있음)
    std::vector<float> peaks;  // 파형용 [min,max] per 10ms
};

struct Loader {
    std::thread th;
    std::mutex m;
    std::string status;
    bool busy = false, done = false, ok = false;
    LoadedVideo result;
    Stt* stt = nullptr;
    std::function<bool(const std::string&)> isWord;  // 영어 사전 (붙은 단어 떼기용). 사전이 아직 없으면 비어 있다

    void setStatus(const std::string& s) {
        std::lock_guard<std::mutex> lock(m);
        status = s;
    }

    // 로컬 영상을 처음 등록할 때만 채운다 (비어 있으면 data/<id>/source.txt 로 다시 연다)
    std::string localVideo, localSub;

    void start(const std::string& id, Lang lang) {
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
        th = std::thread([this, id, lang, isLocal, lv, ls, isWord = isWord] {
            LoadedVideo v;
            std::string err;
            try {
                v.id = id;
                v.lang = lang;
                v.dir = paths::dataDir() + "/" + id;
                std::string subtitlePath;
                bool json3 = false;
                if (isLocal) {
                    local::Prepared p = lv.empty() ? local::reopen(v.dir, lang) : local::prepare(lv, ls, v.dir, lang);
                    v.title = p.title;
                    v.videoPath = p.videoPath;
                    v.audioPath = p.audioPath;
                    subtitlePath = p.subtitlePath;
                } else {
                    auto dl = yt::download(id, v.dir, lang);
                    v.title = dl.title.empty() ? id : dl.title;
                    v.videoPath = fs::absolute(dl.videoPath).string();
                    v.audioPath = dl.audioPath;
                    subtitlePath = dl.subtitlePath;
                    json3 = true;
                }

                const std::string segPath = v.dir + "/segments.json";
                v.segs = transcript::load(segPath);
                bool dirty = false;
                if (v.segs.empty()) {
                    dirty = true;
                    if (!subtitlePath.empty()) {
                        // 자막 파일은 장면(대사) 단위가 이미 깔끔하므로 그대로 문장으로 쓴다
                        v.segs = json3 ? transcript::parseJson3(subtitlePath, lang)
                                       : subtitle::cuesToLines(subtitle::parseCues(subtitlePath, lang));
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
                        v.segs = transcript::splitWords(std::move(words), lang);
                        if (v.segs.empty()) throw std::runtime_error("음성에서 문장을 찾지 못했습니다");
                    }
                }
                // 자막 제작 실수로 붙은 단어("beenafter") 떼기. 사전이 있을 때만, 이미 저장한 문장도 한 번 고쳐 다시 저장한다
                if (lang == Lang::En && isWord) {
                    for (auto& s : v.segs) {
                        std::string t = subtitle::splitGlued(s.text, isWord);
                        if (t != s.text) { s.text = std::move(t); dirty = true; }
                    }
                }
                if (dirty) transcript::save(v.segs, segPath);
                if (isLocal) {
                    setStatus("한국어 자막 찾는 중...");
                    auto ko = local::koreanCues(v.videoPath, v.dir);
                    if (!ko.empty()) v.ko = subtitle::alignLines(v.segs, ko);
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

    void start(Stt* stt, bool download, Lang lang = Lang::En) {
        if (th.joinable()) th.join();
        { std::lock_guard<std::mutex> lock(m); busy = true; status = download ? std::string(langName(lang)) + " STT 모델 다운로드 중 (약 " + std::to_string(Stt::modelSizeMB(lang)) + "MB)..." : std::string(langName(lang)) + " STT 모델 로딩 중..."; }
        th = std::thread([this, stt, download, lang] {
            std::string err;
            const std::string path = Stt::modelPath(lang);
            try {
                if (download && !fs::exists(path)) {
                    fs::create_directories(fs::path(path).parent_path());
                    std::string part = path + ".part";
                    std::string cmd = "curl -L --fail -o \"" + part + "\" \"" + Stt::modelUrl(lang) + "\"";
                    if (paths::runCommand(cmd, yt::logPath()) != 0 || !fs::exists(part)) {
                        throw std::runtime_error("모델 다운로드 실패. 로그: " + yt::logPath());
                    }
                    fs::rename(part, path);
                }
                { std::lock_guard<std::mutex> lock(m); status = std::string(langName(lang)) + " STT 모델 로딩 중..."; }
                if (!stt->load(path, &err, lang)) throw std::runtime_error(err);
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

// 녹음 채점 (백그라운드): 단어 정확도 + 억양 · 리듬 · 강세
struct ScoreJob {
    std::thread th;
    std::mutex m;
    bool running = false, done = false;
    long long practiceId = 0;
    std::string videoId;              // 채점한 영상 · 문장 (결과를 받을 때 아직 같은 것인지 확인용)
    int segIdx = -1;
    int segStartMs = 0, segEndMs = 0;
    ScoreResult result;
    std::string err;
    // 억양 · 리듬 · 강세
    prosody::Result prosodyResult;
    prosody::Track origTrack, userTrack;  // 곡선 표시용 (원음 / 내 녹음)
    bool haveProsodyTracks = false;       // 두 트랙을 구했다
    bool origWordsFresh = false;          // 원음 단어 시각을 이번에 새로 인식했다 → DB 에 캐시
    std::string origWordsJsonOut;
    // 자막 시각은 실제 말소리와 수백 ms 어긋나기도 하고 앞뒤 대사의 꼬리가 섞이기도 한다. 그래서 원음은 앞뒤로 kOrigPadMs 넓게 읽어
    // whisper 가 이웃 단어까지 인식하게 한 뒤, 기준 문장과 맞는 단어만 쓴다. 원음 트랙 · 단어 시각은 이 넓힌 클립 기준이다
    static constexpr int kOrigPadMs = 300;
    int origPadMs = 0;                    // 실제로 앞에 붙은 여유 (영상 맨 앞이면 300 보다 작다)

    // dict: 일본어면 원문과 인식 결과를 모두 읽기(히라가나)로 바꿔 비교한다 (한자/가나 표기 차이를 없앤다)
    // origAudioPath 가 비어 있으면 억양 비교를 건너뛴다. origWordsJson 은 DB 에 캐시된 원음 단어 시각 (비어 있으면 원음도 이번에 인식한다)
    void start(Stt* stt, long long pid, std::string reference, std::vector<float> pcm48k, Lang lang, const JaDict* dict,
               std::string origAudioPath, int segStart, int segEnd, std::string origWordsJson,
               std::string vid, int seg) {
        if (th.joinable()) th.join();
        {
            std::lock_guard<std::mutex> lock(m);
            running = true; done = false; practiceId = pid; err.clear();
            videoId = std::move(vid); segIdx = seg; segStartMs = segStart; segEndMs = segEnd;
        }
        th = std::thread([this, stt, reference, lang, dict, origAudioPath, segStart, segEnd, origWordsJson, pcm = std::move(pcm48k)] {
            ScoreResult r;
            prosody::Result pr;
            prosody::Track oTrack, uTrack;
            bool haveTracks = false, fresh = false;
            std::string origJson;
            std::string e;
            try {
                auto pcm16 = AudioEngine::resample(pcm, AudioEngine::kSampleRate, Stt::kRate);
                std::vector<Word> userWords = stt->transcribe(pcm16, {}, &e);
                // transcribeText 와 같은 방식으로 단어를 이어 붙인다
                std::string heard;
                for (const auto& w : userWords) { if (!heard.empty()) heard += ' '; heard += w.text; }
                if (e.empty()) {
                    if (lang == Lang::Ja && dict && dict->loaded()) {
                        r = scoreTranscript(dict->toReading(reference, true), dict->toReading(heard, true), lang);
                        r.heard = heard;
                    } else {
                        r = scoreTranscript(reference, heard, lang);
                    }
                }
                if (e.empty() && !origAudioPath.empty()) {
                    std::vector<float> origPcm;
                    // 일본어는 whisper 가 단어를 나누지 못해 이웃 대사를 걸러 낼 수 없으므로 자막 구간 그대로 읽는다
                    const int pad = lang == Lang::Ja ? 0 : kOrigPadMs;
                    const int padFront = std::min(pad, segStart);
                    try {
                        origPcm = AudioEngine::loadWavSlice(origAudioPath, segStart - padFront, segEnd + pad, Stt::kRate);
                    } catch (const std::exception&) {
                        origPcm.clear();
                    }
                    if (origPcm.empty()) {
                        pr.note = "원음 구간을 읽지 못해 억양 비교 생략";
                    } else {
                        std::vector<Word> origWords;
                        if (!origWordsJson.empty()) origWords = prosody::wordsFromJson(origWordsJson);
                        if (origWords.empty()) {
                            // 캐시가 없으면 원음을 한 번 인식한다 (문장당 한 번, 결과는 DB 에 저장). 빈 결과는 저장하지 않는다
                            std::string e2;
                            origWords = stt->transcribe(origPcm, {}, &e2);
                            fresh = e2.empty() && !origWords.empty();
                            if (fresh) origJson = prosody::wordsToJson(origWords);
                        }
                        // 넓힌 구간에 들어온 이웃 대사의 단어는 빼고, 기준 문장과 맞는 단어만으로 말소리 구간을 잡는다
                        std::vector<Word> matched;
                        if (lang != Lang::Ja) {
                            std::vector<std::string> texts;
                            for (const auto& w : origWords) texts.push_back(w.text);
                            std::vector<char> used(origWords.size(), 0);
                            for (int j : alignTokens(reference, texts, lang)) if (j >= 0) used[j] = 1;
                            for (size_t i = 0; i < origWords.size(); ++i) if (used[i]) matched.push_back(origWords[i]);
                        }
                        oTrack = prosody::analyze(origPcm, matched.size() >= 2 ? matched : origWords);
                        uTrack = prosody::analyze(pcm16, userWords);
                        haveTracks = true;
                        // 단어를 많이 틀려도 억양 · 속도 · 강세는 소리로 비교한다 (맞은 단어가 적으면 리듬은 속도만, 강세는 DTW 로)
                        pr = prosody::compare(oTrack, origWords, uTrack, userWords, reference, lang);
                    }
                }
            } catch (const std::exception& ex) {
                e = ex.what();
            }
            std::lock_guard<std::mutex> lock(m);
            result = std::move(r);
            prosodyResult = std::move(pr);
            origTrack = std::move(oTrack);
            userTrack = std::move(uTrack);
            haveProsodyTracks = haveTracks;
            origWordsFresh = fresh;
            origWordsJsonOut = std::move(origJson);
            origPadMs = std::min(lang == Lang::Ja ? 0 : kOrigPadMs, segStart);
            err = e;
            done = true;
        });
    }
    ~ScoreJob() { if (th.joinable()) th.join(); }
};

// 원음 문장의 단어 시각만 인식한다 (파형 위에 단어를 적기 위해). 채점 때와 같은 넓힌 클립 · 같은 JSON 이라 결과는 같은 DB 캐시에 들어간다
struct OrigWordsJob {
    std::thread th;
    std::mutex m;
    bool running = false, done = false;
    std::string videoId;
    int segIdx = -1;
    std::string jsonOut;  // 비어 있으면 실패

    void start(Stt* stt, std::string audioPath, int segStart, int segEnd, Lang lang, std::string vid, int seg) {
        if (th.joinable()) th.join();
        {
            std::lock_guard<std::mutex> lock(m);
            running = true; done = false; videoId = std::move(vid); segIdx = seg; jsonOut.clear();
        }
        th = std::thread([this, stt, audioPath, segStart, segEnd, lang] {
            std::string json;
            try {
                const int pad = lang == Lang::Ja ? 0 : ScoreJob::kOrigPadMs;
                auto pcm = AudioEngine::loadWavSlice(audioPath, segStart - std::min(pad, segStart), segEnd + pad, Stt::kRate);
                std::string e;
                auto words = stt->transcribe(pcm, {}, &e);
                if (e.empty() && !words.empty()) json = prosody::wordsToJson(words);
            } catch (const std::exception&) {}
            std::lock_guard<std::mutex> lock(m);
            jsonOut = std::move(json);
            done = true;
        });
    }
    ~OrigWordsJob() { if (th.joinable()) th.join(); }
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
    // 단어 비교 듣기: 원음의 그 단어 → 잠깐 쉼 → 내 녹음의 그 단어 (큐에 넣고 update 에서 차례로 재생)
    Player wordPlayer;
    struct WordClip { std::vector<float> pcm48k; float gain; };
    std::vector<WordClip> wordQueue;
    Db db;
    Stt sttEn, sttJa;              // 언어별 whisper (영어 base.en / 일본어 small)
    Loader loader;
    SttLoader sttLoaderEn, sttLoaderJa;
    Lang uiLang = Lang::En;        // 새 영상을 불러올 때 쓰는 학습 언어 (홈에서 선택, 설정에 저장)

    // ---- 일본어 오프라인 사전 (IPADIC + JMdict) ----
    JaDict jaDict;
    std::atomic<bool> jaReady{false};
    struct DictLoader {
        std::thread th;
        std::mutex m;
        bool busy = false, done = false, ok = false;
        std::string status;
        ~DictLoader() { if (th.joinable()) th.join(); }
    } dictLoader;

    void startDictLoad(bool download) {
        if (dictLoader.busy) return;
        if (dictLoader.th.joinable()) dictLoader.th.join();
        { std::lock_guard<std::mutex> lock(dictLoader.m); dictLoader.busy = true; dictLoader.done = false; dictLoader.status = download ? "일본어 사전 다운로드 중 (약 " + std::to_string(JaDict::packSizeMB()) + "MB)..." : "일본어 사전 로딩 중..."; }
        dictLoader.th = std::thread([this, download] {
            std::string err;
            try {
                const std::string dir = JaDict::dir();
                if (download && !JaDict::installed()) {
                    fs::create_directories(fs::u8path(dir));
                    const std::string zip = dir + "/pack.zip";
                    std::string cmd = "curl -L --fail -o \"" + zip + "\" \"" + JaDict::packUrl() + "\"";
                    if (paths::runCommand(cmd, yt::logPath()) != 0 || !fs::exists(fs::u8path(zip))) throw std::runtime_error("사전 다운로드 실패. 로그: " + yt::logPath());
                    { std::lock_guard<std::mutex> lock(dictLoader.m); dictLoader.status = "일본어 사전 압축 해제 중..."; }
                    if (paths::runCommand("tar -xf \"" + zip + "\" -C \"" + dir + "\"", yt::logPath()) != 0 || !JaDict::installed()) throw std::runtime_error("사전 압축 해제 실패. 로그: " + yt::logPath());
                    std::error_code ec;
                    fs::remove(fs::u8path(zip), ec);
                }
                { std::lock_guard<std::mutex> lock(dictLoader.m); dictLoader.status = "일본어 사전 로딩 중..."; }
                if (!jaDict.load(&err)) throw std::runtime_error(err);
            } catch (const std::exception& e) {
                err = e.what();
            }
            std::lock_guard<std::mutex> lock(dictLoader.m);
            dictLoader.busy = false;
            dictLoader.done = true;
            dictLoader.ok = err.empty();
            dictLoader.status = dictLoader.ok ? "" : "일본어 사전 오류: " + err;
        });
    }

    // ---- 영어 오프라인 사전 (위키낱말사전) ----
    EnDict enDict;
    std::atomic<bool> enReady{false};
    bool enDictAnnounce = false;  // 사용자가 버튼으로 받았을 때만 완료 메시지
    DictLoader enDictLoader;

    void startEnDictLoad(bool download) {
        if (enDictLoader.busy) return;
        if (enDictLoader.th.joinable()) enDictLoader.th.join();
        { std::lock_guard<std::mutex> lock(enDictLoader.m); enDictLoader.busy = true; enDictLoader.done = false; enDictLoader.status = download ? "영어 사전 다운로드 중 (약 " + std::to_string(EnDict::packSizeMB()) + "MB)..." : "영어 사전 로딩 중..."; }
        enDictLoader.th = std::thread([this, download] {
            std::string err;
            try {
                const std::string dir = EnDict::dir();
                if (download && !EnDict::installed()) {
                    fs::create_directories(fs::u8path(dir));
                    const std::string zip = dir + "/pack.zip";
                    std::string cmd = "curl -L --fail -o \"" + zip + "\" \"" + EnDict::packUrl() + "\"";
                    if (paths::runCommand(cmd, yt::logPath()) != 0 || !fs::exists(fs::u8path(zip))) throw std::runtime_error("사전 다운로드 실패. 로그: " + yt::logPath());
                    { std::lock_guard<std::mutex> lock(enDictLoader.m); enDictLoader.status = "영어 사전 압축 해제 중..."; }
                    if (paths::runCommand("tar -xf \"" + zip + "\" -C \"" + dir + "\"", yt::logPath()) != 0 || !EnDict::installed()) throw std::runtime_error("사전 압축 해제 실패. 로그: " + yt::logPath());
                    std::error_code ec;
                    fs::remove(fs::u8path(zip), ec);
                }
                { std::lock_guard<std::mutex> lock(enDictLoader.m); enDictLoader.status = "영어 사전 로딩 중..."; }
                if (!enDict.load(&err)) throw std::runtime_error(err);
            } catch (const std::exception& e) {
                err = e.what();
            }
            std::lock_guard<std::mutex> lock(enDictLoader.m);
            enDictLoader.busy = false;
            enDictLoader.done = true;
            enDictLoader.ok = err.empty();
            enDictLoader.status = enDictLoader.ok ? "" : "영어 사전 오류: " + err;
        });
    }

    Lang curLang() const { return loaded ? video.lang : uiLang; }
    Stt& sttFor(Lang l) { return l == Lang::Ja ? sttJa : sttEn; }
    SttLoader& sttLoaderFor(Lang l) { return l == Lang::Ja ? sttLoaderJa : sttLoaderEn; }
    Stt& sttCur() { return sttFor(curLang()); }
    ScoreJob scoreJob;
    OrigWordsJob origWordsJob;
    // 문장 번호 → 원음 단어 (기준 문장과 맞는 것만, 문장 시작 기준 ms). 영상을 열 때 비운다
    std::map<int, std::vector<Word>> origWordsCache;
    std::set<int> origWordsTried;  // 인식을 시도한 문장 (실패해도 다시 돌리지 않는다)

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
    int loopStartMs = -1;  // 반복할 때 되돌아갈 시각 (파형을 클릭해 중간부터 들을 때 그 지점, 아니면 문장 시작)
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
    // 억양 · 리듬 · 강세 (채점 결과와 함께 들어온다). have 가 true 면 곡선(orig / user) 을 그릴 수 있다
    struct ProsodyView { prosody::Result result; prosody::Track orig, user; bool have = false; int origPadMs = 0; } lastProsody;
    std::string scoreRecPath;   // 채점한 녹음 파일. 기록 탭에서 다른 녹음을 들을 땐 그 파형에 곡선을 겹치지 않는다

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
        bool ai = false;                     // 진행 중인 요청이 AI 인지 (아니면 온라인 위키낱말사전)
        bool hasPending = false;             // 진행 중에 다른 단어를 클릭하면 끝난 뒤 이어서 찾는다
        bool pendingAi = false;
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
    std::string wordHintReading, wordHintKorean;  // 클릭한 일본어 토큰의 읽기/발음 (AI 응답 전에도 보여 준다)

    // ---- 일본어 읽기 (토큰 · 후리가나 · 한국어 발음) ----
    struct ReadingJob {
        std::thread th;
        std::mutex m;
        bool running = false;
        std::string videoId;
        std::vector<int> queue;
        int done = 0;
        std::vector<std::pair<int, JaReading>> ready;
        std::string err;
        std::atomic<bool> cancel{false};
        ~ReadingJob() { cancel = true; if (th.joinable()) th.join(); }
    } readingJob;
    std::map<int, JaReading> readings;       // AI 가 만든 읽기 (DB 캐시)
    std::map<int, JaReading> roughReadings;  // AI 없을 때의 간이 읽기 (가나만 변환)
    std::set<int> readingRequested;          // 자동 요청 중복 방지
    std::vector<int> readingPending;         // 작업 중에 들어온 요청
    bool showPron = true;                    // 자막 아래 한국어 발음 표시
    bool showKo = true;                      // 한국어 자막 표시 (내 영상 파일에 한국어 자막이 있을 때)
    bool showText = true;                    // 원문 자막 표시. 끄면 영상 자막 · 문장 목록 · 단어 줄 · 파형 단어를 숨겨 소리만 듣고 따라 할 수 있다
    bool alignWave = true;                   // 내 녹음 파형 · 곡선을 원음 시간축에 맞춰 그린다 (늦게 시작하거나 느리게 말해도 나란히)

    // 문장 i 의 한국어 자막 (끄거나 없으면 빈 문자열)
    const std::string& koFor(int i) const {
        static const std::string none;
        return showKo && i >= 0 && i < (int)video.ko.size() ? video.ko[i] : none;
    }
    void setShowKo(bool on) {
        showKo = on;
        db.setSetting("sub.showKo", on ? "1" : "0");
    }
    void setShowText(bool on) {
        showText = on;
        db.setSetting("sub.showText", on ? "1" : "0");
    }
    // 원문을 숨겨야 하는가 (문장 학습 문제 진행 중이거나 [원문 자막] 을 껐을 때)
    bool textHidden() const { return !showText || quizHidden(); }

    // 읽기 우선순위: AI 가 만든 것(DB) → 오프라인 사전 → 가나만 변환한 간이 읽기. AI 는 자동으로 부르지 않는다 (토큰 비용).
    const JaReading& readingFor(int i) {
        auto it = readings.find(i);
        if (it != readings.end()) return it->second;
        auto& r = roughReadings[i];
        if (r.empty()) r = jaReady ? dictReading(seg(i).text) : roughJapaneseReading(seg(i).text);
        return r;
    }

    bool isFunctionWord(const std::string& pos) const {
        return pos.rfind("助詞", 0) == 0 || pos.rfind("助動詞", 0) == 0 || pos.rfind("記号", 0) == 0;
    }

    // 오프라인 사전으로 만든 읽기: 형태소 단위 토큰 + 발음 + 짧은 영어 뜻
    JaReading dictReading(const std::string& text) {
        JaReading r;
        for (const auto& m : jaDict.tokenize(text)) {
            if (m.pos.rfind("記号", 0) == 0) continue;  // 구두점은 단어 줄에서 뺀다
            JaToken t;
            t.surface = m.surface;
            t.reading = m.reading;
            t.korean = JaDict::korean(m);
            if (isFunctionWord(m.pos)) {
                t.meaning = JaDict::posKorean(m.pos);
            } else {
                auto g = jaDict.lookup(m.base, m.base == m.surface ? m.reading : "", m.pos, 1);
                if (!g.empty() && !g[0].senses.empty()) t.meaning = g[0].senses[0];
            }
            if (!r.reading.empty()) { r.reading += ' '; r.pronunciation += ' '; }
            r.reading += t.reading.empty() ? t.surface : t.reading;
            r.pronunciation += t.korean.empty() ? t.surface : t.korean;
            r.tokens.push_back(std::move(t));
        }
        r.provider = "사전";
        r.model = "IPADIC · JMdict";
        return r;
    }

    bool readingInProgress(int i) {
        std::lock_guard<std::mutex> lock(readingJob.m);
        if (readingJob.running && std::find(readingJob.queue.begin(), readingJob.queue.end(), i) != readingJob.queue.end()) return true;
        return std::find(readingPending.begin(), readingPending.end(), i) != readingPending.end();
    }

    void requestReadings(std::vector<int> idxs) {
        if (!loaded || !llm.ready() || idxs.empty()) return;
        if (readingJob.running) { readingPending.insert(readingPending.end(), idxs.begin(), idxs.end()); return; }
        if (readingJob.th.joinable()) readingJob.th.join();
        readingJob.running = true;
        readingJob.cancel = false;
        readingJob.videoId = video.id;
        readingJob.queue = idxs;
        readingJob.done = 0;
        readingJob.err.clear();
        readingJob.ready.clear();
        LlmConfig cfg = llm;
        std::vector<Segment> segs = video.segs;
        readingJob.th = std::thread([this, cfg, segs, idxs] {
            for (int i : idxs) {
                if (readingJob.cancel) break;
                if (i < 0 || i >= (int)segs.size()) continue;
                std::string e;
                JaReading r = readJapanese(cfg, segs[i].text, &e);
                std::lock_guard<std::mutex> lock(readingJob.m);
                readingJob.done++;
                if (e.empty()) readingJob.ready.push_back({i, r});
                else { readingJob.err = e; if (idxs.size() == 1) break; }
            }
            std::lock_guard<std::mutex> lock(readingJob.m);
            readingJob.running = false;
        });
    }

    void requestAllReadings() {
        std::vector<int> idxs;
        for (int i = 0; i < (int)video.segs.size(); ++i) if (!readings.count(i)) idxs.push_back(i);
        if (idxs.empty()) { message = "모든 문장에 읽기가 있습니다"; return; }
        for (int i : idxs) readingRequested.insert(i);
        requestReadings(idxs);
    }

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

    void speak(const std::string& text) { speak(text, curLang()); }

    // 일본어 텍스트의 한국어 발음 표기 (단어 팝업 → 현재 문장의 토큰 → 문장 전체 → 가나 변환 순으로 찾는다)
    std::string koreanPronunciationFor(const std::string& text) {
        if (text == wordKey && !wordInfo.korean.empty()) return wordInfo.korean;
        if (text == wordKey && !wordHintKorean.empty()) return wordHintKorean;
        if (loaded && valid(current) && video.lang == Lang::Ja) {
            const JaReading& rd = readingFor(current);
            if (text == seg(current).text && !rd.pronunciation.empty()) return rd.pronunciation;
            for (const auto& t : rd.tokens) if (t.surface == text && !t.korean.empty()) return t.korean;
        }
        bool kanaOnly = true;
        for (const auto& ch : jp::splitChars(text)) { unsigned cp = jp::decodeFirst(ch); if (jp::isKanji(cp)) { kanaOnly = false; break; } }
        if (kanaOnly) return jp::kanaToKorean(text);
        // 한자가 섞여 있으면 아는 부분(가나)만이라도 읽는다 — 한자는 건너뛴다
        std::string partial;
        for (const auto& ch : jp::splitChars(text)) { unsigned cp = jp::decodeFirst(ch); if (!jp::isKanji(cp)) partial += ch; }
        std::string ko = jp::kanaToKorean(partial);
        bool hasHangul = false;
        for (const auto& ch : jp::splitChars(ko)) { unsigned cp = jp::decodeFirst(ch); if (cp >= 0xAC00 && cp <= 0xD7A3) { hasHangul = true; break; } }
        return hasHangul ? ko : "";
    }
    void speak(const std::string& text, Lang lang) {
        if (!tts.available()) { message = "이 PC 에 음성 합성 엔진이 없습니다"; return; }
        if (lang == Lang::Ja && !tts.hasVoice(Lang::Ja)) {
            // 일본어 음성이 없으면 영어 음성은 가나/한자를 읽지 못해 소리가 나지 않는다.
            // 대신 한국어 발음 표기를 한국어 음성으로 읽어 준다 (근사치).
            const std::string ko = koreanPronunciationFor(text);
            if (!ko.empty() && tts.hasVoice(Lang::Ko)) {
                tts.speak(ko, ttsSlow ? -4 : 0, Lang::Ko);
                message = "일본어 음성이 없어 한국어 음성으로 발음 표기를 읽습니다. 원어 발음은 Windows 설정 > 시간 및 언어 > 음성 > 음성 추가 > 일본어";
            } else {
                message = ko.empty() ? "이 단어의 발음 표기가 아직 없습니다 ([이 문장 읽기 만들기] 로 만들면 들을 수 있습니다). 원어 발음은 Windows 에 일본어 음성을 추가하세요"
                                     : "일본어 음성이 없습니다. Windows 설정 > 시간 및 언어 > 음성 > 음성 추가 > 일본어";
            }
            return;
        }
        tts.speak(text, ttsSlow ? -4 : 0, lang);
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
        uiLang = langFromCode(db.getSetting("ui.lang", "en"));
        showPron = db.getSetting("ja.showPron", "1") == "1";
        showKo = db.getSetting("sub.showKo", "1") == "1";
        showText = db.getSetting("sub.showText", "1") == "1";
        alignWave = db.getSetting("wave.align", "1") == "1";
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
        const Lang lang = video.lang;
        explainJob.th = std::thread([this, cfg, segs, queue, lang] {
            for (int i : queue) {
                if (explainJob.cancel) break;
                std::string before = i > 0 ? segs[i - 1].text : "";
                std::string after = i + 1 < (int)segs.size() ? segs[i + 1].text : "";
                std::string e;
                Explanation ex = explainSentence(cfg, segs[i].text, before, after, &e, lang);
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
        wordQueue.clear();
        wordPlayer.stop();
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

    // 현재 문장 · myRec 으로 채점 작업 시작 (단어 정확도 + 억양 · 리듬 · 강세). 원음 단어 시각 캐시가 있으면 함께 넘긴다
    void startScoreJob(long long pid) {
        const auto& s = seg(current);
        scoreRecPath = myRecPath;
        scoreJob.start(&sttCur(), pid, s.text, myRec, video.lang, jaReady ? &jaDict : nullptr,
                       video.audioPath, s.startMs, s.endMs, db.getSegWords(video.id, current), video.id, current);
    }

    // 녹음 종료 → 저장, 기록, 채점 시작
    long long finishRecording(const char* modeName) {
        auto rec = recorder.stop();
        std::string path = saveRecording(current, rec);
        setMyRec(std::move(rec), path);
        long long pid = log(current, modeName, path);
        haveScore = false;
        lastProsody = ProsodyView{};
        scoreSeg = current;
        if (sttCur().loaded()) {
            scoring = true;
            startScoreJob(pid);
        }
        return pid;
    }

    // ---- 동작 ----
    // fromMs 가 있으면 그 시각부터 문장 끝까지 (반복 때도 거기로 돌아간다)
    void playSegment(int i, int loops, int fromMs = -1) {
        if (!valid(i)) return;
        stopAll();
        current = i;
        loopsLeft = std::max(1, loops);
        loopStartMs = fromMs >= 0 ? fromMs : seg(i).startMs;
        seekTo(loopStartMs / 1000.0);
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
        if (video.lang == Lang::Ja && jaReady) {
            // 한자/가나 표기 차이가 틀림으로 잡히지 않게 읽기로 비교한다
            quizScore = scoreTranscript(jaDict.toReading(seg(quizSeg).text, true), jaDict.toReading(quizBuf, true), video.lang);
            quizScore.heard = quizBuf;
        } else {
            quizScore = scoreTranscript(seg(quizSeg).text, quizBuf, video.lang);
        }
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
        if (!wordQueue.empty() && !wordPlayer.playing()) {
            wordPlayer.setGain(wordQueue.front().gain);
            wordPlayer.play(std::move(wordQueue.front().pcm48k));
            wordQueue.erase(wordQueue.begin());
        }
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
                        seekTo(loopStartMs / 1000.0);
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
            for (SttLoader* sl : {&sttLoaderEn, &sttLoaderJa}) {
                std::lock_guard<std::mutex> lock(sl->m);
                if (!sl->busy && !sl->status.empty()) { message = sl->status; sl->status.clear(); }
            }
        }
        // 일본어 사전 로더
        {
            std::lock_guard<std::mutex> lock(dictLoader.m);
            if (dictLoader.done) {
                dictLoader.done = false;
                if (dictLoader.ok) {
                    jaReady = true;
                    roughReadings.clear();  // 간이 읽기를 사전 읽기로 바꾼다
                    message = "일본어 사전 준비 완료: 단어 분할 · 읽기 · 뜻을 오프라인으로 표시합니다";
                } else {
                    message = dictLoader.status;
                    dictLoader.status.clear();
                }
            }
        }
        // 영어 사전 로더 (시작할 때 조용히 로드한다. 직접 받았을 때만 알린다)
        {
            std::lock_guard<std::mutex> lock(enDictLoader.m);
            if (enDictLoader.done) {
                enDictLoader.done = false;
                if (enDictLoader.ok) {
                    enReady = true;
                    if (enDictAnnounce) message = "영어 사전 준비 완료: 단어 뜻을 AI 없이 오프라인으로 표시합니다";
                } else {
                    message = enDictLoader.status;
                    enDictLoader.status.clear();
                }
            }
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
        // 일본어 읽기 결과 수거
        {
            std::vector<int> toStart;
            {
                std::lock_guard<std::mutex> lock(readingJob.m);
                if (!readingJob.ready.empty()) {
                    for (auto& [i, r] : readingJob.ready) {
                        db.setReading(readingJob.videoId, i, r.toJson());
                        if (loaded && video.id == readingJob.videoId) readings[i] = r;
                    }
                    readingJob.ready.clear();
                }
                if (!readingJob.running && !readingJob.err.empty()) { message = "읽기 생성 실패: " + readingJob.err; readingJob.err.clear(); }
                if (!readingJob.running && !readingPending.empty()) toStart.swap(readingPending);
            }
            if (!toStart.empty()) requestReadings(toStart);
        }
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
                if (wordJob.hasPending) startWordJob(wordJob.pendingWord, wordJob.pendingSentence, wordJob.pendingAi);
            }
        }
        {
            std::lock_guard<std::mutex> lock(scoreJob.m);
            if (scoreJob.done) {
                scoreJob.done = false;
                scoreJob.running = false;
                scoring = false;
                if (scoreJob.err.empty()) {
                    const prosody::Result& p = scoreJob.prosodyResult;
                    if (scoreJob.practiceId > 0) {
                        db.setPracticeScore(scoreJob.practiceId, scoreJob.result.accuracy);
                        db.setPracticeProsody(scoreJob.practiceId, p.haveIntonation ? p.intonation : -1,
                                              p.haveRhythm ? p.rhythm : -1, p.haveStress ? p.stress : -1);
                        historyDirty = true;
                    }
                    // 원음 단어 시각을 새로 인식했으면 캐시한다 (그 사이 다른 영상을 열었으면 버린다)
                    const bool sameVideo = loaded && scoreJob.videoId == video.id;
                    if (scoreJob.origWordsFresh && sameVideo && valid(scoreJob.segIdx)) {
                        db.setSegWords(video.id, scoreJob.segIdx, scoreJob.origWordsJsonOut);
                        origWordsCache.erase(scoreJob.segIdx);  // 다음 프레임에 DB 에서 다시 읽는다
                    }
                    // 화면에는 지금 보고 있는 영상 · 문장의 결과일 때만 반영한다
                    if (sameVideo && scoreJob.segIdx == scoreSeg) {
                        lastScore = scoreJob.result;
                        haveScore = true;
                        lastProsody.result = std::move(scoreJob.prosodyResult);
                        lastProsody.orig = std::move(scoreJob.origTrack);
                        lastProsody.user = std::move(scoreJob.userTrack);
                        lastProsody.have = scoreJob.haveProsodyTracks;
                        lastProsody.origPadMs = scoreJob.origPadMs;
                    }
                    if (loaded) bestScores = db.bestScoresFor(video.id);
                } else {
                    message = "채점 실패: " + scoreJob.err;
                }
            }
        }
        // 원음 단어 시각 (파형 단어 표시용) 수거
        {
            std::lock_guard<std::mutex> lock(origWordsJob.m);
            if (origWordsJob.done) {
                origWordsJob.done = false;
                origWordsJob.running = false;
                if (!origWordsJob.jsonOut.empty() && loaded && origWordsJob.videoId == video.id && valid(origWordsJob.segIdx)) {
                    db.setSegWords(video.id, origWordsJob.segIdx, origWordsJob.jsonOut);
                    origWordsCache.erase(origWordsJob.segIdx);
                }
            }
        }
    }

    // 문장 i 의 원음 단어 시각 (문장 시작 기준 ms, 기준 문장과 맞는 단어만). DB 캐시가 없으면 STT 로 한 번 인식을 걸어 두고 nullptr
    const std::vector<Word>* origWordsFor(int i) {
        if (!loaded || !valid(i)) return nullptr;
        if (auto it = origWordsCache.find(i); it != origWordsCache.end()) return &it->second;
        const auto& s = seg(i);
        const std::string json = db.getSegWords(video.id, i);
        if (json.empty()) {
            bool busy; { std::lock_guard<std::mutex> lock(origWordsJob.m); busy = origWordsJob.running; }
            if (!busy && !scoring && sttCur().loaded() && !origWordsTried.count(i) && !video.audioPath.empty()) {
                origWordsTried.insert(i);
                origWordsJob.start(&sttCur(), video.audioPath, s.startMs, s.endMs, video.lang, video.id, i);
            }
            return nullptr;
        }
        // 캐시는 앞에 여유를 붙인 클립 기준 시각이다. 이웃 대사의 단어는 빼고 문장 시작 기준으로 옮긴다
        std::vector<Word> all = prosody::wordsFromJson(json), out;
        const int padFront = std::min(video.lang == Lang::Ja ? 0 : ScoreJob::kOrigPadMs, s.startMs);
        if (video.lang != Lang::Ja) {
            std::vector<std::string> texts;
            for (const auto& w : all) texts.push_back(w.text);
            std::vector<char> used(all.size(), 0);
            for (int j : alignTokens(s.text, texts, video.lang)) if (j >= 0) used[j] = 1;
            for (size_t k = 0; k < all.size(); ++k) if (used[k]) out.push_back(all[k]);
        } else {
            out = all;
        }
        for (auto& w : out) { w.startMs -= padFront; w.endMs -= padFront; }
        return &(origWordsCache[i] = std::move(out));
    }

    // 원음 파형 위에 단어 경계와 단어를 적는다 (채점 전에도). words 는 문장 시작 기준 ms
    void drawOrigWordLabels(ImVec2 pos, ImVec2 size, float durationMs, const std::vector<Word>& words) {
        if (durationMs <= 0 || size.x <= 0) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), true);
        ImFont* font = ImGui::GetFont();
        const float fs = 13.0f * uiScale;
        for (const auto& w : words) {
            const float x0 = pos.x + w.startMs / durationMs * size.x, x1 = pos.x + w.endMs / durationMs * size.x;
            dl->AddLine(ImVec2(x0, pos.y), ImVec2(x0, pos.y + size.y), IM_COL32(255, 255, 255, 50), 1.0f);
            const float tw = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, w.text.c_str()).x;
            if (x1 - x0 > tw + 4) dl->AddText(font, fs, ImVec2(x0 + 2, pos.y + 2), IM_COL32(255, 255, 255, 170), w.text.c_str());
        }
        dl->PopClipRect();
    }

    void onLoaded(LoadedVideo&& v) {
        stopAll();
        video = std::move(v);
        loaded = true;
        current = -1;
        origWordsCache.clear();
        origWordsTried.clear();
        myRec.clear();
        myRecPeaks.clear();
        haveScore = false;
        scoring = false;   // 이전 영상의 채점이 진행 중이어도 새 영상에서는 "채점 중" 을 보이지 않는다 (결과는 pollJobs 가 버린다)
        scoreSeg = -1;
        lastProsody = ProsodyView{};
        scoreRecPath.clear();
        int durMs = (int)(video.peaks.size() / 2) * kPeakBinMs;
        db.upsertVideo(video.id, video.title, durMs, video.lang);
        readings.clear();
        if (video.lang == Lang::Ja) {
            for (int i = 0; i < (int)video.segs.size(); ++i) {
                std::string j = db.getReading(video.id, i);
                if (!j.empty()) { JaReading r = JaReading::fromJson(j); if (!r.empty()) readings[i] = r; }
            }
        }
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
        if (local::isLocalId(in)) { startLoad(in, db.hasVideo(in) ? db.videoLang(in) : uiLang); return; }
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
        // 이미 등록된 영상은 그때의 학습 언어로, 새 영상은 홈에서 고른 언어로 연다
        startLoad(*id, db.hasVideo(*id) ? db.videoLang(*id) : uiLang);
    }

    void startLoad(const std::string& id, Lang lang) {
        loader.stt = &sttFor(lang);
        // 영어 사전이 준비돼 있으면 붙은 단어를 떼는 데 쓴다 (사전은 시작할 때 한 번 로드되고 그 뒤로는 읽기만 한다)
        if (enReady) loader.isWord = [this](const std::string& w) { return enDict.isKnownWord(w); };
        else loader.isWord = nullptr;
        loader.start(id, lang);
    }

    void setUiLang(Lang l) {
        uiLang = l;
        db.setSetting("ui.lang", langCode(l));
    }

    // ---- 내 영상 파일 ----
    void requestLoadFile(const std::string& videoPath, const std::string& subPath) {
        if (loader.busy) { message = "다른 영상을 불러오는 중입니다"; return; }
        const std::string id = local::makeId(videoPath);
        const std::string dir = paths::dataDir() + "/" + id;
        std::error_code ec;
        const bool registered = fs::exists(fs::u8path(dir + "/segments.json"), ec);
        const Lang lang = registered && db.hasVideo(id) ? db.videoLang(id) : uiLang;
        std::string sub = subPath;
        if (registered) {
            // 이미 등록된 영상: 자막을 새로 지정했으면 교체 (학습 기록이 있으면 문장 번호가 어긋나므로 거부)
            if (!sub.empty()) {
                if (db.countsFor(id).empty()) local::replaceSubtitle(sub, dir);
                else message = "이미 학습 기록이 있는 영상이라 자막은 바꾸지 않고 그대로 엽니다";
            }
            sub.clear();
        } else if (sub.empty()) {
            sub = local::findSiblingSubtitle(videoPath, lang);
        }
        loader.localVideo = videoPath;
        loader.localSub = sub;
        snprintf(urlBuf, sizeof urlBuf, "%s", videoPath.c_str());
        startLoad(id, lang);
    }

    // 자막 파일만 들어왔을 때: 열려 있는 로컬 영상의 자막으로 바꾼다
    void attachSubtitle(const std::string& subPath) {
        if (!loaded || !local::isLocalId(video.id)) { message = "자막을 적용할 영상 파일을 먼저 여세요 (영상과 자막을 함께 끌어다 놓아도 됩니다)"; return; }
        if (loader.busy) { message = "다른 영상을 불러오는 중입니다"; return; }
        if (!db.countsFor(video.id).empty()) { message = "이미 학습 기록이 있는 영상이라 자막을 바꿀 수 없습니다 (문장 번호가 어긋납니다)"; return; }
        local::replaceSubtitle(subPath, video.dir);
        startLoad(video.id, video.lang);
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

    // ---- 음높이 곡선 (파형 위에 겹쳐 그림) ----
    // 프레임 k 의 중심 시각 (ms). 16 kHz 에서 프레임 k 는 샘플 [160k, 160k+587) 이므로 중심 ≈ (160k + 293) / 16 = k*10 + 18 ms
    static float frameCenterMs(int k) { return k * (float)prosody::kHopMs + 18.0f; }

    // 반음 곡선을 사각형(pos, size) 안에 그린다. 세로는 두 파형 공통으로 ±12 반음, NaN(무성음) 에서 선을 끊는다.
    // durationMs: 이 사각형이 나타내는 클립 길이. ghost 는 원음 곡선을 내 녹음 시간축으로 옮긴 것 (점선, 내 곡선 아래에)
    // offsetMs: 곡선의 클립이 사각형의 0 ms 보다 이만큼 앞에서 시작한다 (원음은 앞에 여유를 붙여 읽는다)
    // scale · shiftMs: 곡선 시각을 (t − offsetMs) * scale + shiftMs 로 옮겨 다른 클립의 시간축에 맞출 때 쓴다
    void drawPitchCurve(ImVec2 pos, ImVec2 size, const std::vector<float>& st, const std::vector<float>* ghost, float durationMs, ImU32 color,
                        float offsetMs = 0.0f, float scale = 1.0f, float shiftMs = 0.0f) {
        if (durationMs <= 0 || size.x <= 0 || size.y <= 0) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), true);
        const float midY = pos.y + size.y / 2;
        const float yScale = (size.y / 2 - 2) / 12.0f;
        auto xOf = [&](int k) { return pos.x + ((frameCenterMs(k) - offsetMs) * scale + shiftMs) / durationMs * size.x; };
        auto yOf = [&](float v) { return midY - std::clamp(v, -12.0f, 12.0f) * yScale; };
        if (ghost) {
            // 점선: 6 px 칸을 하나 걸러 그린다
            const float dash = 6.0f * uiScale;
            const ImU32 gcol = IM_COL32(90, 170, 255, 160);
            for (int k = 0; k + 1 < (int)ghost->size(); ++k) {
                const float a = (*ghost)[k], b = (*ghost)[k + 1];
                if (std::isnan(a) || std::isnan(b)) continue;
                const float x0 = xOf(k);
                if (((int)((x0 - pos.x) / dash)) % 2) continue;
                dl->AddLine(ImVec2(x0, yOf(a)), ImVec2(xOf(k + 1), yOf(b)), gcol, 1.5f * uiScale);
            }
        }
        std::vector<ImVec2> pts;
        pts.reserve(st.size());
        const float thick = 2.0f * uiScale;
        auto flush = [&] {
            if (pts.size() >= 2) dl->AddPolyline(pts.data(), (int)pts.size(), color, thick);
            else if (pts.size() == 1) dl->AddCircleFilled(pts[0], thick * 0.75f, color);  // 유성음 한 프레임짜리도 보이게
            pts.clear();
        };
        for (int k = 0; k < (int)st.size(); ++k) {
            if (std::isnan(st[k])) { flush(); continue; }
            pts.push_back(ImVec2(xOf(k), yOf(st[k])));
        }
        flush();
        dl->PopClipRect();
    }

    // 단어 눈금: 기준 문장의 단어가 시작하는 곳에 세로선, 칸이 넓으면 단어도 적는다. user 면 내 녹음 시간축
    void drawWordTicks(ImVec2 pos, ImVec2 size, float durationMs, bool user, float offsetMs = 0.0f, float scale = 1.0f, float shiftMs = 0.0f) {
        if (durationMs <= 0 || size.x <= 0) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), true);
        ImFont* font = ImGui::GetFont();
        const float fs = 12.0f * uiScale;
        for (const auto& wp : lastProsody.result.words) {
            const float s = user ? (wp.userStartMs - offsetMs) * scale + shiftMs : (float)(wp.origStartMs - lastProsody.origPadMs);
            const float e = user ? (wp.userEndMs - offsetMs) * scale + shiftMs : (float)(wp.origEndMs - lastProsody.origPadMs);
            const float x0 = pos.x + s / durationMs * size.x, x1 = pos.x + e / durationMs * size.x;
            dl->AddLine(ImVec2(x0, pos.y), ImVec2(x0, pos.y + size.y), IM_COL32(255, 255, 255, 50), 1.0f);
            const float tw = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, wp.text.c_str()).x;
            if (x1 - x0 > tw + 4) dl->AddText(font, fs, ImVec2(x0 + 2, pos.y + 2), IM_COL32(255, 255, 255, 150), wp.text.c_str());
        }
        dl->PopClipRect();
    }

    // 내 녹음 파형 위에 마우스를 올리면 가장 가까운 단어의 차이를 보여 준다 (바로 앞 항목이 그 파형이어야 한다)
    void drawWordPairTooltip(ImVec2 pos, ImVec2 size, float durationMs, float offsetMs = 0.0f, float scale = 1.0f, float shiftMs = 0.0f) {
        if (durationMs <= 0 || size.x <= 0 || lastProsody.result.words.empty() || !ImGui::IsItemHovered()) return;
        // 마우스 위치의 시각 (사각형 축) → 내 녹음 시각
        const float tMs = ((ImGui::GetMousePos().x - pos.x) / size.x * durationMs - shiftMs) / scale + offsetMs;
        const prosody::WordPair* best = nullptr;
        float bestD = FLT_MAX;
        for (const auto& wp : lastProsody.result.words) {
            const float d = tMs < wp.userStartMs ? wp.userStartMs - tMs : tMs > wp.userEndMs ? tMs - wp.userEndMs : 0.0f;
            if (d < bestD) { bestD = d; best = &wp; }
        }
        if (!best) return;
        if (best->hasPitch) ImGui::SetTooltip("'%s'  길이 ×%.1f  크기 %+.2f  음높이 %+.1f 반음", best->text.c_str(), best->durRatio, best->loudDiff, best->pitchDiffSt);
        else ImGui::SetTooltip("'%s'  길이 ×%.1f  크기 %+.2f", best->text.c_str(), best->durRatio, best->loudDiff);
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
        // 자막 오버레이 (문장 학습 문제 중에는 정답이라 숨긴다. [원문 자막] 을 끄면 원문 · 발음은 숨기고 한국어 자막만 남긴다)
        if (valid(current) && !quizHidden()) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const std::string text = showText ? seg(current).text : std::string();
            float wrap = avail.x * 0.9f;
            ImFont* font = ImGui::GetFont();
            float fsize = ImGui::GetFontSize() * 1.35f;
            // 일본어: 자막 아래에 한국어 발음을 한 줄 더 (읽지 못하는 한자가 있어도 따라 말할 수 있게)
            std::string pron;
            if (video.lang == Lang::Ja && showPron && showText) pron = readingFor(current).pronunciation;
            // 한국어 자막은 그 아래 한 줄 더
            const std::string& ko = koFor(current);
            if (!text.empty() || !pron.empty() || !ko.empty()) {
                const float psize = ImGui::GetFontSize() * 1.05f;
                ImVec2 ts = text.empty() ? ImVec2(0, 0) : font->CalcTextSizeA(fsize, FLT_MAX, wrap, text.c_str());
                ImVec2 ps = pron.empty() ? ImVec2(0, 0) : font->CalcTextSizeA(psize, FLT_MAX, wrap, pron.c_str());
                ImVec2 ks = ko.empty() ? ImVec2(0, 0) : font->CalcTextSizeA(psize, FLT_MAX, wrap, ko.c_str());
                const float gap = pron.empty() || text.empty() ? 0.0f : 4.0f, kgap = ko.empty() || (text.empty() && pron.empty()) ? 0.0f : 4.0f;
                const float boxW = std::max({ts.x, ps.x, ks.x}), boxH = ts.y + gap + ps.y + kgap + ks.y;
                ImVec2 pos(origin.x + (avail.x - boxW) / 2, origin.y + avail.y - boxH - 24);
                dl->AddRectFilled(ImVec2(pos.x - 10, pos.y - 6), ImVec2(pos.x + boxW + 10, pos.y + boxH + 6), IM_COL32(0, 0, 0, 170), 6.0f);
                if (!text.empty())
                    dl->AddText(font, fsize, ImVec2(pos.x + (boxW - ts.x) / 2, pos.y), IM_COL32(255, 255, 255, 255), text.c_str(), nullptr, wrap);
                if (!pron.empty())
                    dl->AddText(font, psize, ImVec2(pos.x + (boxW - ps.x) / 2, pos.y + ts.y + gap), IM_COL32(255, 230, 140, 255), pron.c_str(), nullptr, wrap);
                if (!ko.empty())
                    dl->AddText(font, psize, ImVec2(pos.x + (boxW - ks.x) / 2, pos.y + ts.y + gap + ps.y + kgap), IM_COL32(170, 215, 255, 255), ko.c_str(), nullptr, wrap);
            }
        }
        ImGui::EndChild();
    }

    void drawSentenceList() {
        ImGui::TextDisabled("%s", video.title.c_str());
        ImGui::TextDisabled("문장 %d개  |  클릭: 재생  |  ★ 북마크", (int)video.segs.size());
        if (video.lang == Lang::Ja) {
            bool running; int done, total;
            { std::lock_guard<std::mutex> lock(readingJob.m); running = readingJob.running; done = readingJob.done; total = (int)readingJob.queue.size(); }
            ImGui::SameLine(0, 12);
            if (running) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "읽기 생성 중 %d/%d", done, total);
            else if (jaReady) ImGui::TextDisabled("읽기 · 발음: 오프라인 사전");
            else if ((int)readings.size() < (int)video.segs.size()) {
                if (ImGui::SmallButton(JaDict::installed() ? "일본어 사전 로드" : "일본어 사전 받기")) startDictLoad(!JaDict::installed());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("오프라인 사전(IPADIC + JMdict, 약 %dMB 한 번 다운로드)을 받으면\nAI 없이 모든 문장의 단어 분할 · 읽기 · 발음 · 영어 뜻이 나옵니다.", JaDict::packSizeMB());
                ImGui::SameLine();
                ImGui::BeginDisabled(!llm.ready());
                if (ImGui::SmallButton("모든 문장 읽기 만들기 (AI)")) requestAllReadings();
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("아직 읽기가 없는 %d개 문장마다 AI 를 한 번씩 호출합니다 (토큰이 그만큼 듭니다).\n만든 읽기는 저장되어 다시 호출하지 않습니다.%s", (int)video.segs.size() - (int)readings.size(), llm.ready() ? "" : "\nAI 설정에서 API 키를 먼저 넣으세요.");
            } else ImGui::TextDisabled("읽기 %d/%d", (int)readings.size(), (int)video.segs.size());
        }
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
            // 학습 문제 진행 중인 문장은 정답이라 통째로, [원문 자막] 을 끄면 원문 · 발음만 숨긴다 (한국어 자막은 그대로)
            const bool rowQuiz = i == quizSeg && valid(quizSeg) && !quizRevealed;
            const char* rowText = rowQuiz ? "(학습 문제 진행 중 — 정답 공개 전까지 숨김)" : showText ? s.text.c_str() : "(원문 숨김 — 소리만 듣고 따라 말하기)";
            ImVec2 textSize = ImGui::CalcTextSize(rowText, nullptr, false, wrap);
            // 일본어: AI 읽기가 있으면 한국어 발음을 한 줄 더 보여 준다
            const JaReading* rowRead = nullptr;
            if (video.lang == Lang::Ja && showPron && showText && !rowQuiz && (jaReady || readings.count(i))) { const JaReading& rr = readingFor(i); if (!rr.pronunciation.empty()) rowRead = &rr; }
            ImVec2 pronSize = rowRead ? ImGui::CalcTextSize(rowRead->pronunciation.c_str(), nullptr, false, wrap) : ImVec2(0, 0);
            const std::string& rowKo = rowQuiz ? std::string() : koFor(i);
            ImVec2 koSize = rowKo.empty() ? ImVec2(0, 0) : ImGui::CalcTextSize(rowKo.c_str(), nullptr, false, wrap);
            float h = ImGui::GetTextLineHeight() + textSize.y + pronSize.y + koSize.y + 6;
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
            if (rowQuiz || !showText) col = ImGui::GetColorU32(ImGuiCol_TextDisabled);
            dl->AddText(font, fs, ImVec2(top.x + 4, top.y + 2 + ImGui::GetTextLineHeight()), col, rowText, nullptr, wrap);
            if (rowRead)
                dl->AddText(font, fs, ImVec2(top.x + 4, top.y + 2 + ImGui::GetTextLineHeight() + textSize.y), IM_COL32(255, 230, 140, 200), rowRead->pronunciation.c_str(), nullptr, wrap);
            if (!rowKo.empty())
                dl->AddText(font, fs, ImVec2(top.x + 4, top.y + 2 + ImGui::GetTextLineHeight() + textSize.y + pronSize.y), IM_COL32(170, 215, 255, 200), rowKo.c_str(), nullptr, wrap);
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
        // 점수 칸: 없으면 "-", 있으면 85 / 60 기준 색
        auto scoreCell = [](double v, bool percent) {
            if (v < 0) { ImGui::TextDisabled("-"); return; }
            ImVec4 col = v >= 85 ? ImVec4(0.4f, 1, 0.5f, 1) : v >= 60 ? ImVec4(1, 0.85f, 0.3f, 1) : ImVec4(1, 0.5f, 0.5f, 1);
            if (percent) ImGui::TextColored(col, "%d%%", (int)std::lround(v));
            else ImGui::TextColored(col, "%d", (int)std::lround(v));
        };
        if (ImGui::BeginTable("hist", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("시각", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("모드", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("정확도", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("억양", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("리듬", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("강세", ImGuiTableColumnFlags_WidthStretch, 0.8f);
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
                scoreCell(h.score, true);
                ImGui::TableNextColumn();
                scoreCell(h.intonation, false);
                ImGui::TableNextColumn();
                scoreCell(h.rhythm, false);
                ImGui::TableNextColumn();
                scoreCell(h.stress, false);
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

        ImGui::TextDisabled("자막 없이 듣고 받아쓰거나, 한국어 번역만 보고 %s로 써 보세요. %s", langName(video.lang),
                            video.lang == Lang::Ja ? "구두점과 띄어쓰기는 채점에서 무시하고 글자 단위로 비교합니다." : "대소문자와 문장 부호는 채점에서 무시합니다.");
        ImGui::RadioButton("리스닝 (받아쓰기)", &quizKind, 0);
        ImGui::SameLine();
        char composeLabel[64];
        snprintf(composeLabel, sizeof composeLabel, "작문 (한국어 → %s)", langName(video.lang));
        ImGui::RadioButton(composeLabel, &quizKind, 1);
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
        if (!ex.reading.empty() || !ex.pronunciation.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "읽기 · 발음");
            if (!ex.reading.empty()) ImGui::TextWrapped("%s", ex.reading.c_str());
            if (!ex.pronunciation.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.9f, 0.55f, 1.0f));
                ImGui::TextWrapped("%s", ex.pronunciation.c_str());
                ImGui::PopStyleColor();
            }
        }
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
            if (tts.available() && ImGui::SmallButton("듣기")) speak(c.text, db.videoLang(c.videoId));
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
        wordKey = curLang() == Lang::Ja ? word : lowerAscii(word);
        wordSentence = sentence;
        wordErr.clear();
        wordInfo = WordMeaning();
        wordLoading = false;
        // 순서: 예전에 받아 둔 AI 결과(이미 토큰을 쓴 것) → 오프라인 사전 → 온라인 위키낱말사전.
        // AI 는 자동으로 부르지 않는다 (팝업의 버튼으로만).
        WordMeaning cached;
        if (std::string j = db.getWordMeaning(wordKey, sentence); !j.empty()) cached = WordMeaning::fromJson(j);
        if (!cached.empty() && cached.provider != "사전") { wordInfo = cached; return; }
        if (curLang() == Lang::Ja && jaReady) {
            wordInfo = dictWord(word, sentence);
            if (!wordInfo.empty() || !wordInfo.reading.empty()) return;
        }
        if (curLang() == Lang::En && enReady) {
            wordInfo = enDictWord(word);
            if (!wordInfo.empty()) return;
        }
        if (!cached.empty()) { wordInfo = cached; return; }
        wordLoading = true;
        if (wordJob.running) { wordJob.hasPending = true; wordJob.pendingAi = false; wordJob.pendingWord = wordKey; wordJob.pendingSentence = sentence; return; }
        startWordJob(wordKey, sentence, false);
    }

    // 오프라인 영어 사전으로 단어 정보 만들기 (품사별 한국어 뜻, 없으면 영어 정의)
    WordMeaning enDictWord(const std::string& word) {
        WordMeaning w;
        EnEntry e = enDict.lookup(word);
        if (e.empty()) return w;
        w.word = e.word;
        if (!e.ipa.empty()) w.ipa = "/" + e.ipa + "/";
        int shown = 0;
        for (const auto& s : e.senses) {
            if (shown == 4) break;
            std::string line = s.pos.empty() ? "" : "[" + s.pos + "] ";
            if (!s.korean.empty()) {
                for (size_t i = 0; i < s.korean.size(); ++i) line += (i ? ", " : "") + s.korean[i];
                if (!s.english.empty()) line += "\n      " + s.english[0];
            } else {
                for (size_t i = 0; i < s.english.size() && i < 2; ++i) line += (i ? "\n      " : "") + s.english[i];
            }
            if (!w.meaning.empty()) w.meaning += "\n";
            w.meaning += line;
            ++shown;
        }
        w.contextMeaning = e.formNote;
        w.provider = "사전";
        w.model = "위키낱말사전 (오프라인)";
        return w;
    }

    // 오프라인 사전으로 단어 정보 만들기 (문장 속 위치의 형태소를 찾아 기본형 · 읽기 · 품사 · 영어 뜻)
    WordMeaning dictWord(const std::string& word, const std::string& sentence) {
        WordMeaning w;
        JaMorph found;
        bool have = false;
        for (const auto& m : jaDict.tokenize(sentence)) if (m.surface == word) { found = m; have = true; break; }
        if (!have) {
            auto ms = jaDict.tokenize(word);
            if (ms.size() == 1) { found = ms[0]; have = true; }
        }
        std::string base = have ? found.base : word;
        std::string readingHint = have && found.base == found.surface ? found.reading : "";
        auto glosses = jaDict.lookup(base, readingHint, have ? found.pos : "", 3);
        w.word = base;
        if (have) {
            w.reading = found.reading;
            w.korean = JaDict::korean(found);
            w.pos = JaDict::posKorean(found.pos);
        }
        if (!glosses.empty()) {
            const JaGloss& g = glosses[0];
            if (w.reading.empty()) { w.reading = g.reading; w.korean = jp::kanaToKorean(g.reading); }
            std::string jm = JaDict::jmPosKorean(g.pos);
            if (!jm.empty() && w.pos.find(jm) == std::string::npos) w.pos = w.pos.empty() ? jm : w.pos + " · " + jm;
            for (size_t i = 0; i < g.senses.size() && i < 3; ++i) { if (!w.meaning.empty()) w.meaning += "\n"; w.meaning += std::to_string(i + 1) + ". " + g.senses[i]; }
            if (have && found.base != found.surface) w.contextMeaning = "문장에서는 \"" + found.surface + "\" 로 활용된 형태 (" + w.pos + ")";
        } else if (have && isFunctionWord(found.pos)) {
            w.meaning = JaDict::posKorean(found.pos);
        }
        w.provider = "사전";
        w.model = "IPADIC · JMdict";
        return w;
    }

    void requestWordAi() {
        if (!llm.ready() || wordKey.empty()) return;
        wordLoading = true;
        wordErr.clear();
        if (wordJob.running) { wordJob.hasPending = true; wordJob.pendingAi = true; wordJob.pendingWord = wordKey; wordJob.pendingSentence = wordSentence; return; }
        startWordJob(wordKey, wordSentence, true);
    }

    // ai: 팝업의 AI 버튼을 눌렀을 때만 true. 아니면 온라인 위키낱말사전 (오프라인 사전이 없을 때)
    void startWordJob(const std::string& key, const std::string& sentence, bool ai) {
        if (wordJob.th.joinable()) wordJob.th.join();
        wordJob.running = true; wordJob.done = false; wordJob.hasPending = false;
        wordJob.word = key; wordJob.sentence = sentence; wordJob.ai = ai && llm.ready();
        LlmConfig cfg = llm;
        const Lang lang = curLang();
        const bool useAi = wordJob.ai;
        wordJob.th = std::thread([this, cfg, key, sentence, lang, useAi] {
            std::string e;
            WordMeaning r;
            if (useAi) {
                r = explainWord(cfg, key, sentence, &e, lang);
                if (r.empty()) {  // AI 실패 → 사전으로 대체 (오류는 함께 보여 준다)
                    std::string e2;
                    WordMeaning d = lookupDictionary(key, &e2, lang);
                    if (!d.empty()) r = d;
                }
            } else {
                r = lookupDictionary(key, &e, lang);
            }
            // 일본어: 가나만으로 된 단어는 AI 없이도 읽기/발음을 채울 수 있다
            if (lang == Lang::Ja && r.reading.empty()) {
                bool kanaOnly = true;
                for (const auto& ch : jp::splitChars(key)) if (jp::isKanji(jp::decodeFirst(ch))) { kanaOnly = false; break; }
                if (kanaOnly) { r.reading = jp::katakanaToHiragana(key); r.korean = jp::kanaToKorean(key); }
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
        if (video.lang == Lang::Ja) {
            // 직접 읽는 연습을 할 수 있게 읽기 · 발음을 통째로 숨길 수 있다 (단어 아래 발음, 읽기/발음 줄, 자막 아래 발음, 문장 목록)
            if (ImGui::Checkbox("읽기·발음 표시 (P)", &showPron)) db.setSetting("ja.showPron", showPron ? "1" : "0");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("끄면 한자를 직접 읽는 연습을 할 수 있습니다. 단어를 클릭하면 팝업에서는 읽기와 발음을 볼 수 있습니다.");
            ImGui::SameLine();
        }
        {
            bool on = showText;
            if (ImGui::Checkbox("원문 자막 (H)", &on)) setShowText(on);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("끄면 영상 자막 · 문장 목록 · 단어 줄 · 파형의 원문을 모두 숨깁니다.\n소리만 듣고 따라 말하는 연습을 할 수 있습니다. 채점 결과와 해설은 그대로 보입니다.");
            ImGui::SameLine();
        }
        if (!video.ko.empty()) {
            bool on = showKo;
            if (ImGui::Checkbox("한국어 자막 (K)", &on)) setShowKo(on);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("영상 자막 아래와 문장 목록에 한국어 자막을 보여 줍니다.\n끄면 뜻을 보지 않고 듣고 따라 하는 연습을 할 수 있습니다.");
            ImGui::SameLine();
        }
        if (!showText) {
            ImGui::TextDisabled("원문을 숨겼습니다. 소리만 듣고 따라 말해 보세요 (다시 보려면 [원문 자막] 켜기 또는 H)");
            return;
        }
        ImGui::TextDisabled("단어 클릭: 발음과 뜻  |  드래그: 복사  |  아래 파형 클릭: 그 지점부터 다시 듣기");

        // 영어: 띄어쓰기 단위. 일본어: AI(또는 간이) 토큰 단위, 버튼 아래 줄에 한국어 발음.
        const bool ja = video.lang == Lang::Ja;
        std::vector<std::string> words, labels;
        const JaReading* rd = nullptr;
        // 채점 뒤에는 단어마다 원음과의 차이를 아래 줄에 적는다 ("길게 세게" 처럼). pairOf[k] = 그 단어의 WordPair (없으면 nullptr)
        std::vector<const prosody::WordPair*> pairOf;
        bool anyMark = false;
        if (ja) {
            rd = &readingFor(current);
            for (const auto& t : rd->tokens) {
                words.push_back(t.surface);
                labels.push_back((t.korean.empty() || !showPron) ? t.surface : t.surface + "\n" + t.korean);
            }
        } else {
            std::istringstream ss(sentence);
            std::string w;
            while (ss >> w) { words.push_back(w); labels.push_back(w); }
            const bool showPairs = haveScore && scoreSeg == current && !scoring && lastProsody.have && !lastProsody.result.words.empty();
            pairOf.assign(words.size(), nullptr);
            if (showPairs) {
                // 띄어쓰기 단어 → 채점 토큰 번호 (채점은 추임새 · 괄호를 뺀다). 표기가 같은 것을 차례로 맞춘다
                const auto toks = scoringTokens(sentence, video.lang);
                size_t j = 0;
                std::vector<int> tokIdx(words.size(), -1);
                for (size_t k = 0; k < words.size(); ++k)
                    if (j < toks.size() && toks[j] == words[k]) tokIdx[k] = (int)j++;
                for (size_t k = 0; k < words.size(); ++k) {
                    if (tokIdx[k] < 0) continue;
                    for (const auto& wp : lastProsody.result.words) if (wp.refIdx == tokIdx[k]) { pairOf[k] = &wp; break; }
                }
                for (size_t k = 0; k < words.size(); ++k) {
                    std::string mark = wordMark(pairOf[k]);
                    if (!mark.empty()) anyMark = true;
                    labels[k] = words[k] + "\n" + (mark.empty() ? "·" : mark);
                }
                if (!anyMark) for (size_t k = 0; k < words.size(); ++k) labels[k] = words[k];
            }
        }
        const bool twoLine = ja || anyMark;
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
            float bw = ImGui::CalcTextSize(labels[k].c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
            if (ImGui::GetItemRectMax().x + spacing + bw <= lineRight) ImGui::SameLine();
            bool pressed = twoLine ? ImGui::Button((labels[k] + "##tok").c_str()) : ImGui::SmallButton(words[k].c_str());
            rects[k] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
            if (!ja && k < pairOf.size() && pairOf[k]) {
                const auto& wp = *pairOf[k];
                if (ImGui::IsItemHovered()) {
                    if (wp.hasPitch) ImGui::SetTooltip("'%s'  길이 ×%.1f  크기 %+.2f  음높이 %+.1f 반음\n우클릭: 원음 → 내 소리 이어 듣기", wp.text.c_str(), wp.durRatio, wp.loudDiff, wp.pitchDiffSt);
                    else ImGui::SetTooltip("'%s'  길이 ×%.1f  크기 %+.2f\n우클릭: 원음 → 내 소리 이어 듣기", wp.text.c_str(), wp.durRatio, wp.loudDiff);
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) playWordPair(wp);
            }
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
                        for (int i = wordDrag.a; i <= wordDrag.b; ++i) { if (i > wordDrag.a && !ja) sel += ' '; sel += words[i]; }
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
        if (clicked >= 0) {
            wordHintReading.clear();
            wordHintKorean.clear();
            if (ja && rd && clicked < (int)rd->tokens.size()) { wordHintReading = rd->tokens[clicked].reading; wordHintKorean = rd->tokens[clicked].korean; }
            openWord(words[clicked], sentence, ImVec2(rects[clicked].first.x, rects[clicked].second.y + 4 * uiScale));
        }

        // 일본어: 문장 전체 읽기와 한국어 발음 (읽기 연습 중에는 숨긴다)
        if (ja && rd && showPron) {
            if (!rd->reading.empty()) {
                ImGui::TextDisabled("읽기:");
                ImGui::SameLine();
                ImGui::TextWrapped("%s", rd->reading.c_str());
            }
            if (!rd->pronunciation.empty()) {
                ImGui::TextDisabled("발음:");
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.9f, 0.55f, 1.0f));
                ImGui::TextWrapped("%s", rd->pronunciation.c_str());
                ImGui::PopStyleColor();
            }
            if (rd->provider == "간이") {
                if (readingInProgress(current)) {
                    ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "AI 로 읽기를 만드는 중...");
                } else {
                    bool dictBusy; { std::lock_guard<std::mutex> lock(dictLoader.m); dictBusy = dictLoader.busy; }
                    if (dictBusy) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "일본어 사전 준비 중...");
                    else if (ImGui::SmallButton(JaDict::installed() ? "일본어 사전 로드" : "일본어 사전 받기 (오프라인, 권장)")) startDictLoad(!JaDict::installed());
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!llm.ready());
                    if (ImGui::SmallButton("이 문장만 AI 로")) { readingRequested.insert(current); requestReadings({current}); }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextDisabled("지금은 가나만 변환한 상태입니다. 사전(약 %dMB, 한 번만)을 받으면 AI 없이 단어 분할 · 한자 읽기 · 뜻이 나옵니다", JaDict::packSizeMB());
                }
            } else {
                ImGui::SameLine(0, 12);
                ImGui::TextDisabled("%s · %s", rd->provider.c_str(), rd->model.c_str());
            }
        }
        if (ja && rd && !showPron) {
            ImGui::TextDisabled("읽기 연습 중: 읽기와 발음을 숨겼습니다. 확인하려면 [읽기·발음 표시] 를 켜거나 P 키, 또는 단어를 클릭하세요.");
        }

        // 단어 뜻 팝업
        ImGui::SetNextWindowPos(wordPopupPos, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(460 * uiScale, 0), ImGuiCond_Appearing);
        if (ImGui::BeginPopup("word_popup")) {
            drawWordPopup();
            ImGui::EndPopup();
        }
    }

    // 단어 아래 줄에 적는 차이: 길이 · 세기 · 높낮이 중 문턱을 넘은 것 (없으면 빈 문자열)
    static std::string wordMark(const prosody::WordPair* wp) {
        if (!wp) return "";
        std::string m;
        auto add = [&](const char* s) { if (!m.empty()) m += " "; m += s; };
        const bool longEnough = wp->origEndMs - wp->origStartMs >= 80 && wp->userEndMs - wp->userStartMs >= 80;
        const float ld = std::log2(std::max(wp->durRatio, 1e-3f));
        if (longEnough && std::fabs(ld) > 0.5f) add(ld < 0 ? "길게" : "짧게");
        if (std::fabs(wp->loudDiff) > 0.2f) add(wp->loudDiff > 0 ? "약하게" : "세게");
        if (wp->hasPitch && std::fabs(wp->pitchDiffSt) > 3.f) add(wp->pitchDiffSt > 0 ? "낮게" : "높게");
        return m;
    }

    // 원음의 그 단어와 내 녹음의 그 단어를 이어서 들려 준다 (앞뒤 40 ms 여유, 사이 250 ms 쉼)
    void playWordPair(const prosody::WordPair& wp) {
        if (!loaded || !valid(current) || myRec.empty()) return;
        if (!mpv.paused()) togglePause();
        wordPlayer.stop();
        wordQueue.clear();
        const auto& s = seg(current);
        try {
            const int base = s.startMs - lastProsody.origPadMs;  // 원음 단어 시각은 앞에 여유를 붙인 클립 기준
            auto orig = AudioEngine::loadWavSlice(video.audioPath, base + wp.origStartMs - 40, base + wp.origEndMs + 40, AudioEngine::kSampleRate);
            orig.resize(orig.size() + (size_t)AudioEngine::kSampleRate / 4, 0.0f);  // 250 ms 쉼
            if (!orig.empty()) wordQueue.push_back({std::move(orig), videoVolume / 100.0f});
        } catch (const std::exception&) {}
        const size_t a = (size_t)std::max(0, wp.userStartMs - 40) * AudioEngine::kSampleRate / 1000;
        const size_t b = std::min(myRec.size(), (size_t)(wp.userEndMs + 40) * AudioEngine::kSampleRate / 1000);
        if (b > a) wordQueue.push_back({std::vector<float>(myRec.begin() + a, myRec.begin() + b), recVolume / 100.0f});
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
        {
            const std::string& reading = w.reading.empty() ? wordHintReading : w.reading;
            const std::string& korean = w.korean.empty() ? wordHintKorean : w.korean;
            if (!reading.empty() || !korean.empty()) {
                ImGui::TextDisabled("읽기");
                ImGui::SameLine();
                ImGui::TextUnformatted(reading.c_str());
                ImGui::SameLine(0, 14);
                ImGui::TextDisabled("발음");
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.55f, 1.0f), "%s", korean.c_str());
            }
        }
        ImGui::Separator();

        if (wordLoading) {
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", wordJob.ai ? "AI 에게 뜻을 묻는 중..." : "사전에서 찾는 중...");
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
            // 영어: 오프라인 사전이 아직 없으면 받기 권유 (한 번만 받으면 AI 없이 한국어 뜻)
            if (curLang() == Lang::En && !enReady) {
                ImGui::Spacing();
                bool busy; { std::lock_guard<std::mutex> lock(enDictLoader.m); busy = enDictLoader.busy; }
                if (busy) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "영어 사전 준비 중...");
                else {
                    char lbl[96];
                    if (EnDict::installed()) snprintf(lbl, sizeof lbl, "영어 사전 로드");
                    else snprintf(lbl, sizeof lbl, "영어 사전 받기 (오프라인, 약 %dMB)", EnDict::packSizeMB());
                    if (ImGui::SmallButton(lbl)) { enDictAnnounce = true; startEnDictLoad(!EnDict::installed()); }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("위키낱말사전 기반 오프라인 영어 사전을 한 번 받으면\nAI 없이 단어의 한국어 뜻 · 발음 기호 · 품사가 바로 나옵니다.");
                }
            } else if (!llm.ready() && w.provider == "사전") {
                ImGui::Spacing();
                ImGui::TextDisabled("AI 설정에서 API 키를 넣으면 문맥에 맞는 설명도 볼 수 있습니다 (버튼을 누를 때만 호출).");
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
            if (ImGui::SmallButton(curLang() == Lang::Ja ? "AI 로 한국어 설명" : "AI 로 문맥 설명")) requestWordAi();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("AI 를 한 번 호출합니다 (토큰 사용). 결과는 저장되어 같은 단어를 다시 누르면 재사용합니다.");
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
            if (!sttCur().loaded()) ImGui::TextDisabled("%s STT 모델을 받으면 녹음을 자동으로 채점합니다.", langName(curLang()));
            else ImGui::TextDisabled("쉐도잉 / 따라말하기 후 여기에 채점 결과가 표시됩니다.");
            return;
        }
        const auto& r = lastScore;
        drawScoreMarks(r, &lastProsody);
        ImGui::TextDisabled("초록: 맞음  빨강: 빠짐  주황: 다르게 들림(들린 단어)  회색: 추가로 들린 단어  |  들린 문장: %s", r.heard.c_str());
    }

    // 정확도 + 단어별 채점 표시 (발음 채점과 문장 학습에서 공용). pv 가 있으면 억양 · 리듬 · 강세 점수와 힌트도 같이 보인다
    void drawScoreMarks(const ScoreResult& r, const ProsodyView* pv = nullptr) {
        auto band = [](float v) { return v >= 85 ? ImVec4(0.4f, 1, 0.5f, 1) : v >= 60 ? ImVec4(1, 0.85f, 0.3f, 1) : ImVec4(1, 0.5f, 0.5f, 1); };
        ImGui::TextColored(band(r.accuracy), "정확도 %d%% (%d/%d)", (int)r.accuracy, r.matched, r.total);
        if (pv) {
            // 측정한 항목만 같은 줄에 덧붙인다. 측정 못 한 항목은 0 으로 보이지 않고 숨긴다
            const auto& p = pv->result;
            if (p.haveIntonation) {
                ImGui::SameLine(0, 14);
                ImGui::TextColored(band(p.intonation), "억양 %d", (int)std::lround(p.intonation));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("억양: 원음 화자와 음높이 곡선 차이, 반음 평균 %.1f — 목소리 높낮이 차이는 빼고 비교", p.pitchDiffSt);
            }
            if (p.haveRhythm) {
                ImGui::SameLine(0, 14);
                if (p.rhythmSpeedOnly) ImGui::TextColored(band(p.rhythm), "리듬 %d (속도만)", (int)std::lround(p.rhythm));
                else ImGui::TextColored(band(p.rhythm), "리듬 %d", (int)std::lround(p.rhythm));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("리듬: 단어별 길이 비율과 쉼, 속도 %.2f배", p.speedRatio);
            }
            if (p.haveStress) {
                ImGui::SameLine(0, 14);
                ImGui::TextColored(band(p.stress), "강세 %d", (int)std::lround(p.stress));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("강세: 단어별 소리 크기 차이");
            }
            if (!p.note.empty()) { ImGui::SameLine(0, 14); ImGui::TextDisabled("(%s)", p.note.c_str()); }
        }
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
        if (pv && !pv->result.hints.empty()) {
            std::string s = "힌트: ";
            for (size_t i = 0; i < pv->result.hints.size(); ++i) { if (i) s += " · "; s += pv->result.hints[i].text; }
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", s.c_str());
            ImGui::PopStyleColor();
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
        // 학습 언어: 새로 불러오는 영상에 적용된다 (이미 등록된 영상은 그때의 언어를 유지)
        ImGui::TextDisabled("학습 언어:");
        ImGui::SameLine();
        if (ImGui::RadioButton("영어", uiLang == Lang::En)) setUiLang(Lang::En);
        ImGui::SameLine();
        if (ImGui::RadioButton("일본어", uiLang == Lang::Ja)) setUiLang(Lang::Ja);
        ImGui::SameLine(0, 12);
        ImGui::TextDisabled(uiLang == Lang::Ja ? "일본어 자막을 받고, 단어마다 읽기와 한국어 발음을 보여 줍니다" : "영어 자막을 받고, 단어 발음과 뜻을 보여 줍니다");
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
            const std::string cardTitle = std::string(v.lang == Lang::Ja ? "[일본어] " : "") + (local::isLocalId(v.id) ? "[내 파일] " : "") + v.title;
            dl->AddText(font, fs, ImVec2(top.x + 6, top.y + 4), IM_COL32(255, 255, 255, 255), cardTitle.c_str(), nullptr, wrap);
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
        ImGui::TextDisabled("단축키: Space 재생/정지, ← → 문장 이동, S 쉐도잉, E 따라말하기, T 녹음, B 북마크, H 원문 자막 숨기기, K 한국어 자막, P 읽기·발음 숨기기(일본어)");
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
                if (tts.available() && ImGui::SmallButton("듣기")) speak(it.text, it.lang);
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
            if (ImGui::Checkbox("파형 맞추기", &alignWave)) db.setSetting("wave.align", alignWave ? "1" : "0");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("채점 뒤 내 녹음 파형과 음높이 곡선을 원음의 시간축에 맞춰 보여 줍니다.\n늦게 시작하거나 느리게 말해도 같은 단어가 위아래로 나란히 놓여 비교하기 쉽습니다.");
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

        // 6행: 파형 (원본 문장 / 내 녹음). 채점이 끝나면 그 위에 음높이 곡선과 단어 눈금을 겹친다
        float w = ImGui::GetContentRegionAvail().x;
        float h = std::max(24.0f, (ImGui::GetContentRegionAvail().y - 8) / 2);
        const size_t totalBins = video.peaks.size() / 2;
        const bool curves = haveScore && scoreSeg == current && !scoring && !quizHidden() && lastProsody.have && valid(current);
        if (valid(current)) {
            const auto& s = seg(current);
            size_t a = std::min(totalBins, (size_t)s.startMs / kPeakBinMs);
            size_t b = std::min(totalBins, (size_t)s.endMs / kPeakBinMs);
            float ph = (t * 1000 - s.startMs) / (float)(s.endMs - s.startMs);
            const ImVec2 rectPos = ImGui::GetCursorScreenPos();
            drawWaveform(video.peaks.data() + 2 * a, b - a, ImVec2(w, h), IM_COL32(90, 170, 255, 255), mpv.paused() ? -1.0f : ph);
            const float segDur = (float)(s.endMs - s.startMs);
            // 단어 경계 · 단어: 채점 뒤에는 채점 결과의 단어 쌍을, 그 전에는 원음 단어 시각 캐시(없으면 뒤에서 인식)를 쓴다
            const std::vector<Word>* ow = textHidden() ? nullptr : origWordsFor(current);
            if (curves) {
                drawWordTicks(rectPos, ImVec2(w, h), segDur, false);
                drawPitchCurve(rectPos, ImVec2(w, h), lastProsody.orig.semitone, nullptr, segDur, IM_COL32(255, 210, 80, 230), (float)lastProsody.origPadMs);
            } else if (ow) {
                drawOrigWordLabels(rectPos, ImVec2(w, h), segDur, *ow);
            }
            // 원음 파형 클릭: 그 지점부터 문장 끝까지 다시 듣는다 (긴 문장을 중간부터 따라잡을 때). 마우스를 올리면 안내선과 시각 · 단어
            if (ImGui::IsItemHovered() && !recording()) {
                const float fx = std::clamp((ImGui::GetMousePos().x - rectPos.x) / std::max(w, 1.0f), 0.0f, 1.0f);
                const int ms = s.startMs + (int)(fx * segDur);
                const float x = rectPos.x + fx * w;
                ImGui::GetWindowDrawList()->AddLine(ImVec2(x, rectPos.y), ImVec2(x, rectPos.y + h), IM_COL32(255, 255, 255, 140), 1.0f);
                const Word* under = nullptr;
                if (ow) for (const auto& wd : *ow) if (ms - s.startMs >= wd.startMs && ms - s.startMs < wd.endMs) { under = &wd; break; }
                if (under) ImGui::SetTooltip("'%s'  %s 부터 다시 듣기", under->text.c_str(), transcript::formatTime(ms).c_str());
                else ImGui::SetTooltip("%s 부터 다시 듣기", transcript::formatTime(ms).c_str());
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) playSegment(current, loopsSetting, ms);
            }
        } else {
            drawWaveform(nullptr, 0, ImVec2(w, h), 0, -1.0f);
        }
        const float recDur = (float)AudioEngine::durationMs(myRec);
        const bool mine = curves && !myRec.empty() && myRecPath == scoreRecPath;
        const ImVec2 rectPos2 = ImGui::GetCursorScreenPos();
        // 맞춤 모드: 내 말소리 구간 [uS, uE] 을 원음 말소리 구간 [oS, oE] 에 겹치도록 시간축을 옮기고 늘인다
        float oS = 0, uS = 0, scale = 1;
        bool aligned = false;
        if (mine && alignWave && valid(current)) {
            const auto& o = lastProsody.orig;
            const auto& u = lastProsody.user;
            oS = frameCenterMs(o.speechStart) - lastProsody.origPadMs;
            const float oE = frameCenterMs(o.speechEnd) - lastProsody.origPadMs;
            uS = frameCenterMs(u.speechStart);
            const float uE = frameCenterMs(u.speechEnd);
            if (oE - oS > 100 && uE - uS > 100) { scale = (oE - oS) / (uE - uS); aligned = true; }
        }
        if (aligned) {
            const auto& s = seg(current);
            const float segDur = (float)(s.endMs - s.startMs);
            // 원음 시간축의 10 ms 칸마다 그 시각에 해당하는 내 녹음 피크를 가져온다
            const size_t nBins = (size_t)std::max(1.0f, segDur / kPeakBinMs), userBins = myRecPeaks.size() / 2;
            std::vector<float> mapped(nBins * 2, 0.0f);
            for (size_t i = 0; i < nBins; ++i) {
                const float tUser = ((float)i * kPeakBinMs + kPeakBinMs / 2 - oS) / scale + uS;
                const long j = (long)(tUser / kPeakBinMs);
                if (tUser >= 0 && j >= 0 && (size_t)j < userBins) { mapped[2 * i] = myRecPeaks[2 * j]; mapped[2 * i + 1] = myRecPeaks[2 * j + 1]; }
            }
            float ph2 = recPlayer.playing() ? ((recPlayer.positionMs() - uS) * scale + oS) / segDur : -1.0f;
            drawWaveform(mapped.data(), nBins, ImVec2(w, h), IM_COL32(120, 230, 120, 255), ph2);
            drawWordTicks(rectPos2, ImVec2(w, h), segDur, true, uS, scale, oS);
            // 원음 곡선은 점선으로 같은 자리에, 내 곡선은 옮긴 시간축으로
            static const std::vector<float> none;
            drawPitchCurve(rectPos2, ImVec2(w, h), none, &lastProsody.orig.semitone, segDur, 0, (float)lastProsody.origPadMs);
            drawPitchCurve(rectPos2, ImVec2(w, h), lastProsody.user.semitone, nullptr, segDur, IM_COL32(255, 210, 80, 255), uS, scale, oS);
            drawWordPairTooltip(rectPos2, ImVec2(w, h), segDur, uS, scale, oS);
        } else {
            float ph2 = recPlayer.playing() && !myRec.empty() ? recPlayer.positionMs() / recDur : -1.0f;
            drawWaveform(myRecPeaks.data(), myRecPeaks.size() / 2, ImVec2(w, h), IM_COL32(120, 230, 120, 255), ph2);
            if (mine) {
                drawWordTicks(rectPos2, ImVec2(w, h), recDur, true);
                drawPitchCurve(rectPos2, ImVec2(w, h), lastProsody.user.semitone,
                               lastProsody.result.haveAlignment ? &lastProsody.result.origOnUser : nullptr, recDur, IM_COL32(255, 210, 80, 255));
                drawWordPairTooltip(rectPos2, ImVec2(w, h), recDur);
            }
        }

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
        if (ImGui::IsKeyPressed(ImGuiKey_K, false) && !video.ko.empty()) setShowKo(!showKo);
        if (ImGui::IsKeyPressed(ImGuiKey_H, false)) setShowText(!showText);
        if (ImGui::IsKeyPressed(ImGuiKey_P, false) && loaded && video.lang == Lang::Ja) { showPron = !showPron; db.setSetting("ja.showPron", showPron ? "1" : "0"); }
    }

    // --script 파일의 명령을 한 줄씩 실행 (테스트용): load <id> / wait <sec> / play <n> / echo <n> / record / stop / rescore / prosody_dump / quit
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
        else if (cmd == "say") { std::string rest; std::getline(ss, rest); speak(arg + rest); fprintf(stderr, "[tts] en=%s ja=%s ko=%s available=%d msg=%s\n", tts.voiceName(Lang::En).c_str(), tts.voiceName(Lang::Ja).c_str(), tts.voiceName(Lang::Ko).c_str(), (int)tts.available(), message.c_str()); }
        else if (cmd == "tab") forceTab = arg == "explain" ? Tab::Explain : arg == "cards" ? Tab::Cards : arg == "review" ? Tab::Review : arg == "history" ? Tab::History : arg == "quiz" ? Tab::Quiz : Tab::Sentences;
        else if (cmd == "quiz") startQuiz(std::max(0, current), arg == "compose" ? 1 : 0);
        else if (cmd == "quiz_answer") { std::string rest; std::getline(ss, rest); snprintf(quizBuf, sizeof quizBuf, "%s", (arg + rest).c_str()); checkQuiz(); }
        else if (cmd == "quiz_reveal") quizRevealed = true;
        else if (cmd == "lang") setUiLang(langFromCode(arg));
        else if (cmd == "dict") startDictLoad(!JaDict::installed());  // 일본어 사전 받기/로드
        else if (cmd == "ko") setShowKo(arg != "0");
        else if (cmd == "text") setShowText(arg != "0");
        else if (cmd == "pron") { showPron = arg != "0"; db.setSetting("ja.showPron", showPron ? "1" : "0"); }
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
        else if (cmd == "rescore" && valid(current) && sttCur().loaded()) {
            // 현재 문장의 마지막 녹음을 다시 채점 (마이크 없이 채점 화면 확인용)
            std::string path = db.lastRecording(video.id, current);
            if (!path.empty() && fs::exists(path)) {
                setMyRec(AudioEngine::loadWav(path), path);
                scoring = true;
                haveScore = false;
                lastProsody = ProsodyView{};
                scoreSeg = current;
                startScoreJob(0);
            }
        }
        else if (cmd == "prosody_dump") {  // 억양 · 리듬 · 강세 결과를 stderr 로 (자동 테스트용)
            const auto& p = lastProsody.result;
            auto sc = [](bool have, float v) { char b[16]; if (!have) return std::string("-"); snprintf(b, sizeof b, "%.0f", v); return std::string(b); };
            fprintf(stderr, "[prosody] haveScore=%d scoring=%d tracks=%d acc=%.1f intonation=%s rhythm=%s%s stress=%s speed=%.2f pitchDiff=%.2f words=%d align=%d origFrames=%d userFrames=%d note=%s\n",
                    (int)haveScore, (int)scoring, (int)lastProsody.have, lastScore.accuracy,
                    sc(p.haveIntonation, p.intonation).c_str(), sc(p.haveRhythm, p.rhythm).c_str(), p.haveRhythm && p.rhythmSpeedOnly ? "(speedOnly)" : "",
                    sc(p.haveStress, p.stress).c_str(), p.speedRatio, p.pitchDiffSt, (int)p.words.size(), (int)p.haveAlignment,
                    lastProsody.orig.frames(), lastProsody.user.frames(), p.note.c_str());
            for (const auto& hnt : p.hints) fprintf(stderr, "[prosody] hint[%d]: %s\n", hnt.refIdx, hnt.text.c_str());
            for (const auto& wp : p.words)
                fprintf(stderr, "[prosody] word '%s' orig %d-%d user %d-%d dur x%.2f loud %+.2f pitch %+.1f%s\n", wp.text.c_str(),
                        wp.origStartMs, wp.origEndMs, wp.userStartMs, wp.userEndMs, wp.durRatio, wp.loudDiff, wp.pitchDiffSt, wp.hasPitch ? "" : " (no pitch)");
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
        const Lang sl = curLang();
        { SttLoader& ld = sttLoaderFor(sl); std::lock_guard<std::mutex> lock(ld.m); sttBusy = ld.busy; sttStatus = ld.status; }

        if (showHome) ImGui::BeginDisabled(!loaded);
        if (ImGui::Button(showHome ? "학습 화면" : "홈")) { if (showHome) showHome = false; else goHome(); }
        if (showHome) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(420 * uiScale);
        const std::string hint = std::string("유튜브 URL / 영상 ID / 내 영상 파일 경로  (새 영상은 ") + langName(uiLang) + " 로 불러옵니다 · 홈에서 변경)";
        bool enter = ImGui::InputTextWithHint("##url", hint.c_str(), urlBuf, sizeof urlBuf, ImGuiInputTextFlags_EnterReturnsTrue);
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
        if (!sttFor(sl).loaded()) {
            if (sttBusy) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", sttStatus.c_str());
            else {
                char b[128];
                snprintf(b, sizeof b, "%s STT 모델 받기 (%dMB, 채점/자막 생성용)", langName(sl), Stt::modelSizeMB(sl));
                if (ImGui::Button(b)) sttLoaderFor(sl).start(&sttFor(sl), true, sl);
            }
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "%s STT 준비됨", langName(sl));
        }
        if (sl == Lang::Ja) {
            ImGui::SameLine(0, 16);
            bool dictBusy; std::string dictStatus;
            { std::lock_guard<std::mutex> lock(dictLoader.m); dictBusy = dictLoader.busy; dictStatus = dictLoader.status; }
            if (jaReady) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "일본어 사전 준비됨");
            else if (dictBusy) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", dictStatus.c_str());
            else {
                char b[128];
                snprintf(b, sizeof b, JaDict::installed() ? "일본어 사전 로드" : "일본어 사전 받기 (%dMB, 단어 분할/읽기/뜻)", JaDict::packSizeMB());
                if (ImGui::Button(b)) startDictLoad(!JaDict::installed());
            }
        }
        ImGui::SameLine(0, 16);
        if (busy) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", status.c_str());
        else if (!message.empty()) {
            const bool info = message.rfind("복사됨", 0) == 0 || message.rfind("표현 노트에 저장", 0) == 0 || message.rfind("일본어 사전 준비 완료", 0) == 0;
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
            const float bottomH = 360.0f * uiScale;  // 파형 두 줄에 음높이 곡선과 단어 눈금이 들어갈 높이
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
    // 일본어(한자 · 가나) 글리프는 기본 폰트에 없어 일본어 폰트를 합친다 (기본 폰트에 없는 글자만 가져온다)
    {
#ifdef _WIN32
        const char* jpFonts[] = {"C:/Windows/Fonts/YuGothM.ttc", "C:/Windows/Fonts/meiryo.ttc", "C:/Windows/Fonts/msgothic.ttc", "C:/Windows/Fonts/YuGothR.ttc"};
#else
        const char* jpFonts[] = {"/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc", "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
                                 "/System/Library/Fonts/Hiragino Sans GB.ttc", "/Library/Fonts/Arial Unicode.ttf"};
#endif
        ImFontConfig merge;
        merge.MergeMode = true;
        for (const char* f : jpFonts) {
            if (fs::exists(fs::u8path(f))) { io.Fonts->AddFontFromFileTTF(f, 18.0f * uiScale, &merge); break; }
        }
    }
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
    for (Lang l : {Lang::En, Lang::Ja})
        if (fs::exists(Stt::modelPath(l))) app.sttLoaderFor(l).start(&app.sttFor(l), false, l);
    if (JaDict::installed()) app.startDictLoad(false);
    if (EnDict::installed()) app.startEnDictLoad(false);
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

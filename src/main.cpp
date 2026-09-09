#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "audio.h"
#include "db.h"
#include "paths.h"
#include "transcript.h"
#include "youtube.h"

namespace fs = std::filesystem;

namespace {

const char* kHelp = R"(
명령어:
  l [N]        N번부터 문장 목록 (기본: 현재 위치)
  p N          N번 문장 재생
  r N [K]      N번 문장 K번 반복 (기본 3)
  s N          쉐도잉: 원본 재생과 동시에 녹음 → 내 녹음 재생
  e N          따라말하기: 원본 재생 → 녹음 → 원본/내 녹음 비교 재생
  b N          N번 문장의 마지막 녹음 듣기
  n / .        다음 문장 / 현재 문장 다시
  h            도움말
  q            종료
)";

struct App {
    std::string videoId;
    std::string title;
    std::string dir;
    std::vector<float> pcm;
    std::vector<Segment> segs;
    Db* db = nullptr;
    int current = 0;

    bool valid(int n) const { return n >= 0 && n < (int)segs.size(); }

    void showSegment(int n, int count) {
        const auto& s = segs[n];
        std::cout << "[" << n << "] " << transcript::formatTime(s.startMs) << "  "
                  << (count > 0 ? "(" + std::to_string(count) + "회) " : "") << s.text << "\n";
    }

    void list(int from) {
        auto counts = db->countsFor(videoId);
        int to = std::min((int)segs.size(), from + 15);
        for (int i = from; i < to; ++i) showSegment(i, counts[i]);
        if (to < (int)segs.size()) std::cout << "  ... (" << segs.size() - to << "개 더 있음, l " << to << ")\n";
    }

    void log(int n, const std::string& mode, const std::string& rec = "") {
        db->addPractice({videoId, n, mode, Db::now(), rec, -1});
    }

    std::string saveRecording(int n, const std::vector<float>& rec) {
        fs::create_directories(dir + "/rec");
        std::string ts = Db::now();
        for (auto& c : ts) if (c == ':') c = '-';
        std::string path = dir + "/rec/" + std::to_string(n) + "_" + ts + ".wav";
        AudioEngine::saveWav(path, rec);
        return path;
    }

    void play(int n, int loops) {
        current = n;
        showSegment(n, 0);
        AudioEngine::play(pcm, segs[n].startMs, segs[n].endMs, loops);
        log(n, loops > 1 ? "repeat" : "play");
    }

    void shadow(int n) {
        current = n;
        showSegment(n, 0);
        std::cout << "  >> 원본과 함께 따라 말하세요 (녹음 중)\n";
        auto rec = AudioEngine::playAndRecord(pcm, segs[n].startMs, segs[n].endMs, 1500);
        std::string path = saveRecording(n, rec);
        std::cout << "  >> 내 녹음 재생\n";
        AudioEngine::play(rec, 0, AudioEngine::durationMs(rec));
        log(n, "shadow", path);
    }

    void echo(int n) {
        current = n;
        showSegment(n, 0);
        std::cout << "  >> 원본 듣기\n";
        AudioEngine::play(pcm, segs[n].startMs, segs[n].endMs);
        int dur = (segs[n].endMs - segs[n].startMs) * 2 + 2000;
        std::cout << "  >> 이제 말하세요 (" << dur / 1000.0 << "초 녹음)\n";
        auto rec = AudioEngine::record(dur);
        std::string path = saveRecording(n, rec);
        std::cout << "  >> 비교: 원본 → 내 녹음\n";
        AudioEngine::play(pcm, segs[n].startMs, segs[n].endMs);
        AudioEngine::play(rec, 0, AudioEngine::durationMs(rec));
        log(n, "echo", path);
    }

    void playback(int n) {
        std::string path = db->lastRecording(videoId, n);
        if (path.empty() || !fs::exists(path)) {
            std::cout << "  녹음이 없습니다.\n";
            return;
        }
        auto rec = AudioEngine::loadWav(path);
        std::cout << "  >> " << path << "\n";
        AudioEngine::play(rec, 0, AudioEngine::durationMs(rec));
    }

    void run() {
        std::cout << "\n== " << title << " ==\n문장 " << segs.size() << "개, 길이 "
                  << transcript::formatTime(AudioEngine::durationMs(pcm)) << "\n" << kHelp << "\n";
        list(0);

        std::string line;
        while (std::cout << "\n> ", std::getline(std::cin, line)) {
            std::istringstream ss(line);
            std::string cmd;
            ss >> cmd;
            if (cmd.empty()) continue;
            int a = -1, b = -1;
            ss >> a >> b;

            try {
                if (cmd == "q") break;
                else if (cmd == "h") std::cout << kHelp;
                else if (cmd == "l") list(a >= 0 ? a : current);
                else if (cmd == "n") { if (valid(current + 1)) play(current + 1, 1); else std::cout << "  마지막 문장입니다.\n"; }
                else if (cmd == ".") play(current, 1);
                else if (!valid(a)) std::cout << "  문장 번호를 확인하세요 (0 ~ " << segs.size() - 1 << ")\n";
                else if (cmd == "p") play(a, 1);
                else if (cmd == "r") play(a, b > 0 ? b : 3);
                else if (cmd == "s") shadow(a);
                else if (cmd == "e") echo(a);
                else if (cmd == "b") playback(a);
                else std::cout << "  알 수 없는 명령. h 로 도움말.\n";
            } catch (const std::exception& e) {
                std::cout << "  오류: " << e.what() << "\n";
            }
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    if (argc < 2) {
        std::cout << "사용법: youshadow-cli <유튜브 URL 또는 영상 ID>\n";
        return 1;
    }

    auto id = yt::extractVideoId(argv[1]);
    if (!id) {
        std::cout << "유튜브 영상 ID를 찾을 수 없습니다: " << argv[1] << "\n";
        return 1;
    }

    try {
        paths::setup();
        App app;
        app.videoId = *id;
        app.dir = paths::dataDir() + "\\" + *id;

        auto dl = yt::download(*id, app.dir);
        app.title = dl.title.empty() ? *id : dl.title;

        const std::string segPath = app.dir + "/segments.json";
        app.segs = transcript::load(segPath);
        if (app.segs.empty()) {
            if (dl.subtitlePath.empty()) {
                std::cout << "영어 자막이 없는 영상입니다. GUI 에서 whisper 로 대본을 생성하세요.\n";
                return 1;
            }
            app.segs = transcript::parseJson3(dl.subtitlePath);
            transcript::save(app.segs, segPath);
        }

        std::cout << "[오디오] 로딩 중...\n";
        app.pcm = AudioEngine::loadWav(dl.audioPath);

        Db db;
        std::string err;
        if (!db.open(paths::dataDir() + "\\youshadow.db", &err)) throw std::runtime_error(err);
        db.importTsv(paths::dataDir() + "\\practice.tsv");
        db.upsertVideo(app.videoId, app.title, AudioEngine::durationMs(app.pcm));
        db.upsertSegments(app.videoId, app.segs);
        app.db = &db;
        app.run();
    } catch (const std::exception& e) {
        std::cout << "오류: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

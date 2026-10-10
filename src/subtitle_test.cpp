// 자막 붙은 단어 떼기(subtitle::splitGlued) 검증용 콘솔 도구. 영어 오프라인 사전이 설치돼 있어야 한다.
//  subtitle_test                      자체 테스트 (종료 코드 = 실패 수)
//  subtitle_test <segments.json>      그 파일의 문장 중 바뀌는 것만 "전 → 후" 로 출력 (파일은 고치지 않는다)
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "endict.h"
#include "subtitle.h"
#include "transcript.h"

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    EnDict dict;
    std::string err;
    if (!dict.load(&err)) { std::printf("영어 사전 로드 실패: %s\n", err.c_str()); return 1; }
    auto isWord = [&](const std::string& w) { return dict.isKnownWord(w); };

    if (argc >= 3 && std::string(argv[1]) == "--word") {
        for (int i = 2; i < argc; ++i) {
            EnEntry e = dict.lookup(argv[i]);
            std::printf("%-12s known=%d  lookup=%s%s\n", argv[i], dict.isKnownWord(argv[i]) ? 1 : 0, e.empty() ? "없음" : e.word.c_str(),
                        e.formNote.empty() ? "" : ("  (" + e.formNote + ")").c_str());
        }
        return 0;
    }
    if (argc >= 2) {
        auto segs = transcript::load(argv[1]);
        int changed = 0;
        for (const auto& s : segs) {
            std::string t = subtitle::splitGlued(s.text, isWord);
            if (t != s.text) { ++changed; std::printf("[%d] %s\n  → %s\n", s.idx, s.text.c_str(), t.c_str()); }
        }
        std::printf("%zu 문장 중 %d 개 바뀜\n", segs.size(), changed);
        return 0;
    }

    struct Case { const char* in; const char* want; };
    const std::vector<Case> cases = {
        // 떼야 하는 것
        {"You've beenafter the dutchman almost as longas you were after me.", "You've been after the dutchman almost as long as you were after me."},
        {"And I know the second you'reout, you'll take off after kate.", "And I know the second you're out, you'll take off after kate."},
        {"How do you know anythingabout him?", "How do you know anything about him?"},
        {"I need your helpwith this.", "I need your help with this."},
        {"Is this the informationon hagen?", "Is this the information on hagen?"},
        {"This isyour wife's visa bill.", "This is your wife's visa bill."},
        {"You canget me out of here.", "You can get me out of here."},
        {"There's alwaysa first time.", "There's always a first time."},
        {"Getthe birthday cards?", "Get the birthday cards?"},
        {"This is a warehousedown by the docks.", "This is a warehouse down by the docks."},
        {"I need recording equipmentdown here.", "I need recording equipment down here."},
        {"I wasn'tseeing anybody else.", "I wasn't seeing anybody else."},
        {"Forgive me if I don'tshake hands.", "Forgive me if I don't shake hands."},
        {"Where elseam I gonna go?", "Where else am I gonna go?"},
        {"A lotof holdings, and you blow upmy evidence to youat prison.", "A lot of holdings, and you blow up my evidence to you at prison."},
        {"I can be releasedinto your custody.", "I can be released into your custody."},
        {"Diana's on her waywith that.", "Diana's on her way with that."},
        {"Can he help youfind him? Help youdust for prints? Stop byon our way. I've seen iton the news.", "Can he help you find him? Help you dust for prints? Stop by on our way. I've seen it on the news."},
        {"The age of the bondat six days, a listof her bids, the best mindof my generation, one weekto go.", "The age of the bond at six days, a list of her bids, the best mind of my generation, one week to go."},
        // 이름의 소유격은 사전에 없어 못 뗀다 (hagen 이 표제어가 아니다) — 이름을 잘못 가르는 것보다 낫다
        {"Like alexander pushkin's tale. Hagen'simpressive as hell.", "Like alexander pushkin's tale. Hagen'simpressive as hell."},
        // 두면 안 되는 것 (이름 · 사전에 없는 합성어 · 멀쩡한 단어)
        {"Neal Caffrey and Elizabeth Burke went together.", "Neal Caffrey and Elizabeth Burke went together."},
        {"neal caffrey, kate moreau, sy devore, mozzie, ginsberg, pushkin, perdue", "neal caffrey, kate moreau, sy devore, mozzie, ginsberg, pushkin, perdue"},
        {"Nothing happened whatsoever on the webpage.", "Nothing happened whatsoever on the webpage."},
        {"It's a well-known tamper-proof anklet at the supermax.", "It's a well-known tamper-proof anklet at the supermax."},
        {"Peter...I am not gonna run.", "Peter...I am not gonna run."},
        {"Hagen said so. Diana and Jones agreed.", "Hagen said so. Diana and Jones agreed."},
        {"He restriped a utility card months agoand left.", "He restriped a utility card months agoand left."},
        {"Even the freakin' coffee is a surefire hit using roadblocks.", "Even the freakin' coffee is a surefire hit using roadblocks."},
    };
    int fail = 0;
    for (const auto& c : cases) {
        std::string got = subtitle::splitGlued(c.in, isWord);
        const bool ok = got == c.want;
        if (!ok) ++fail;
        std::printf("%s  %s\n", ok ? "OK  " : "FAIL", c.in);
        if (!ok) std::printf("      기대: %s\n      결과: %s\n", c.want, got.c_str());
    }
    std::printf("%zu 중 %d 실패\n", cases.size(), fail);
    return fail;
}

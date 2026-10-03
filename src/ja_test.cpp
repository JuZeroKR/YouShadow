// 일본어 오프라인 사전 검증용 콘솔 도구.
// 사용법: ja_test <문장> [<문장> ...]  → 형태소 분할 · 읽기 · 한국어 발음 · 영어 뜻을 출력한다.
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "jadict.h"
#include "lang.h"
#include "paths.h"

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    paths::setup();
    JaDict dict;
    std::string err;
    auto t0 = std::chrono::steady_clock::now();
    if (!dict.load(&err)) { std::cout << err << "\n"; return 1; }
    auto t1 = std::chrono::steady_clock::now();
    std::cout << "사전 로드 " << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() << "ms, 뜻 사전 " << (dict.hasGlosses() ? "있음" : "없음") << "\n";

    // Windows 콘솔은 argv 를 ANSI 로 주므로 UTF-8 파일(--file) 로 받는 쪽이 안전하다
    std::vector<std::string> inputs;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--file" && i + 1 < argc) {
            std::ifstream in(argv[++i], std::ios::binary);
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line.erase(0, 3);
                if (!line.empty()) inputs.push_back(line);
            }
        } else {
            inputs.push_back(a);
        }
    }
    for (const std::string& s : inputs) {
        auto a = std::chrono::steady_clock::now();
        auto toks = dict.tokenize(s);
        auto b = std::chrono::steady_clock::now();
        std::cout << "\n" << s << "   (" << std::chrono::duration_cast<std::chrono::microseconds>(b - a).count() << "us)\n";
        std::cout << "  읽기: " << dict.toReading(s, true) << "\n";
        std::cout << "  발음: " << jp::kanaToKorean(dict.toReading(s, true, true)) << "\n";
        for (const auto& m : toks) {
            std::cout << "  " << m.surface << "\t" << m.reading << "\t" << JaDict::korean(m) << "\t" << m.base << "\t" << JaDict::posKorean(m.pos) << (m.unknown ? " (미지어)" : "");
            // 기본형의 읽기는 모르므로 활용형이면 표기로만 찾는다 (읽기는 활용형과 다를 수 있어 힌트로만)
            auto g = dict.lookup(m.base, m.base == m.surface ? m.reading : "", m.pos, 1);
            if (!g.empty()) std::cout << "\t[" << JaDict::jmPosKorean(g[0].pos) << "] " << (g[0].senses.empty() ? "" : g[0].senses[0]);
            std::cout << "\n";
        }
    }
    return 0;
}

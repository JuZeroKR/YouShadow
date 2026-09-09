#pragma once
#include <string>

// 실행 환경에 따른 경로. 저장소에서 개발 중이면(현재 폴더에 CMakeLists.txt) ./data, ./models 를 쓰고,
// 설치/포터블 실행이면 %LOCALAPPDATA%\YouShadow 아래를 쓴다.
namespace paths {

std::string exeDir();
bool devMode();
std::string dataDir();    // 영상 캐시, 녹음, DB
std::string modelsDir();  // whisper 모델
std::string logDir();     // 외부 도구 실행 로그

// exe 옆 bin/ 을 PATH 앞에 붙이고 필요한 폴더를 만든다. 프로그램 시작 시 한 번 호출.
void setup();

// 명령을 콘솔 창 없이 실행하고 stdout/stderr 를 logPath 에 덧붙인다. 종료 코드 반환.
int runCommand(const std::string& cmd, const std::string& logPath);

}  // namespace paths

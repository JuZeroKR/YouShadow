# YouShadow

유튜브 영상으로 영어 쉐도잉을 연습하는 Windows 데스크톱 앱 (C++).

링크 하나만 넣으면 영상과 영어 자막을 받아 문장 단위로 나눠 주고, 문장별로 반복 재생 · 쉐도잉 · 따라말하기 녹음을 한 뒤 whisper 음성 인식으로 발음을 채점합니다. 연습 기록은 SQLite에 쌓이고, 간격 반복 방식으로 복습할 문장을 골라 줍니다.

![시연](docs/demo.gif)

## 화면

| 학습 화면 | 홈 (라이브러리) |
|---|---|
| ![학습 화면](docs/study.png) | ![홈](docs/home.png) |

![채점](docs/scoring.png)

## 기능

- **영상 불러오기**: 유튜브 URL → yt-dlp 로 720p 영상과 영어 자막(json3) 다운로드. 자막이 없거나 유튜브가 자막 요청을 막으면 whisper.cpp 로 대본을 만든다
- **문장 분할**: 단어 타임스탬프를 문장 부호 · 침묵 · 길이 기준으로 나눠 문장 목록을 만든다
- **재생**: libmpv 로 영상 재생. 문장 클릭 재생, 반복, 속도 0.5x~1.5x (피치 유지), 자유 재생 중 현재 문장 하이라이트
- **연습 모드**
  - 쉐도잉: 원본과 동시에 말하기 (영상이 끝나도 말을 마칠 때까지 녹음)
  - 따라말하기: 듣고 → 말하고 → 원본과 내 녹음 이어서 비교
  - 녹음만: 원본 듣기 없이 바로 녹음 (같은 문장 반복 연습)
  - 무음 감지 자동 종료 (배경 소음을 재서 감도 자동 조절), Space 로 수동 종료
- **채점**: 내 녹음을 whisper 로 텍스트화해서 원문과 단어 단위로 정렬. 정확도 % 와 맞음 / 빠짐 / 다르게 들림 / 추가로 들림 표시. 추임새("uh", "um")와 효과음 표기는 제외
- **기록과 복습** (SQLite)
  - 홈: 영상별 진행률, 연습 횟수, 평균 점수, 마지막 연습, 최근 30일 그래프, 연속 학습일
  - 문장별 연습 이력과 예전 녹음 다시 듣기, 북마크
  - 간격 반복: 연습한 문장은 다음 날 복습 목록에 나타나고, 어려움 / 보통 / 쉬움 평가로 다음 복습일이 정해진다 (SM-2 단순화)
  - 복습 세션: [복습 시작] 한 번으로 복습할 문장을 순서대로 열어 준다
- **파형 표시**: 원본 문장과 내 녹음의 파형을 나란히 표시

## 빌드

요구 사항: Windows 10/11, Visual Studio 2022 (MSVC), CMake 3.20+, git

```powershell
# 1. 의존성 받기 (third_party/ 와 models/ 에 설치, 저장소에는 포함되지 않음)
powershell -ExecutionPolicy Bypass -File scripts\setup-deps.ps1

# 2. 빌드
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# 3. 실행에 필요한 외부 도구
winget install yt-dlp.yt-dlp     # yt-dlp + ffmpeg 가 PATH 에 들어간다
```

`scripts/setup-deps.ps1` 이 받는 것:

| 라이브러리 | 용도 |
|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) + [GLFW](https://www.glfw.org/) | GUI |
| [libmpv](https://mpv.io/) ([zhongfly/mpv-winbuild](https://github.com/zhongfly/mpv-winbuild)) | 영상 재생 (OpenGL 텍스처로 렌더) |
| [whisper.cpp](https://github.com/ggml-org/whisper.cpp) + ggml-base.en 모델 | 음성 인식 (채점, 대본 생성) |
| [miniaudio](https://miniaud.io/) | 마이크 녹음, 녹음 재생 |
| [SQLite](https://www.sqlite.org/) | 학습 기록 |
| [nlohmann/json](https://github.com/nlohmann/json) | 자막 파싱 |

## 실행

```
run-gui.cmd
run-gui.cmd https://www.youtube.com/watch?v=VIDEO_ID
```

실행하면 홈 화면이 뜹니다. 상단 입력창에 URL 을 넣고 [불러오기]를 누르면 다운로드 후 학습 화면으로 넘어갑니다. 처음 한 번만 받고 그 뒤로는 캐시를 씁니다.

### 단축키

| 키 | 동작 |
|---|---|
| Space | 재생 / 일시정지, 녹음 중이면 녹음 끝내기 |
| ← / → | 이전 / 다음 문장 |
| R | 현재 문장 다시 |
| S | 쉐도잉 |
| E | 따라말하기 |
| T | 녹음만 |
| B | 북마크 |

### 데이터 위치

```
data/
  youshadow.db               SQLite (영상, 문장, 연습 이력+점수, 복습 상태, 북마크)
  <VIDEO_ID>/
    video.mp4, audio.wav      원본 영상, 오디오 (mono 16kHz)
    video.en.json3            유튜브 자막
    segments.json             문장 분할 결과 (고정)
    rec/                      내 녹음 (wav)
models/ggml-base.en.bin      whisper 모델 (148MB)
```

`data/`, `models/`, `third_party/`, `build/` 는 저장소에 올라가지 않습니다.

## 구조

```
src/
  gui_main.cpp    ImGui 앱: 레이아웃, 연습 모드 상태 머신, 홈/복습 세션, 백그라운드 작업
  player.cpp      libmpv → OpenGL FBO 텍스처
  youtube.cpp     URL 파싱, yt-dlp / ffmpeg 호출
  transcript.cpp  단어 목록 → 문장 세그먼트, json3 자막 파싱
  stt.cpp         whisper.cpp 래퍼 (단어 타임스탬프 / 텍스트)
  scoring.cpp     원문 vs 인식 결과 편집 거리 정렬, 정확도
  audio.cpp       miniaudio 녹음/재생, 파형 피크, 리샘플
  db.cpp          SQLite 저장소, 간격 반복, 통계
  main.cpp        CLI 버전 (초기 프로토타입)
  stt_test.cpp    STT + 채점 검증 도구
scripts/setup-deps.ps1   의존성 설치
```

테스트용으로 `youshadow-gui.exe --script cmds.txt` 를 주면 `load <id>` / `wait <초>` / `play <n>` / `echo <n>` / `record` / `home` / `review` / `rate hard|good|easy` / `quit` 명령을 순서대로 실행합니다. 시연 GIF 도 이 방식으로 찍었습니다.

## 알아 둘 것

- 유튜브가 자막 요청을 일시적으로 막는 경우(HTTP 429)가 있습니다. 그때는 whisper 로 대본을 만들며, 몇 시간 뒤 다시 불러오면 자막을 받습니다.
- 채점은 CPU 에서 whisper base.en 모델로 돌립니다. 10초 녹음에 약 0.4초 걸립니다.
- 개인 학습 용도로 만든 프로그램입니다. 영상 다운로드는 유튜브 약관을 확인하고 본인 책임으로 사용하세요.

## AI 사용 표기

이 프로젝트의 설계와 코드는 Anthropic 의 **Claude (Claude Code)** 와 대화하며 작성했습니다. 아키텍처 제안, 코드 작성, 디버깅, 문서 작성에 AI 를 사용했고, 기능 방향 결정과 실제 사용 테스트는 사람이 했습니다.

## 다음 단계 후보

- 문장 편집 (분할 / 병합)
- 더 큰 whisper 모델 선택 (small.en), GPU 가속
- 억양 · 속도 비교, 음소 단위 발음 피드백

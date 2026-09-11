#!/bin/bash
# YouShadow 의존성 설치 스크립트 (macOS)
# third_party/ 아래에 빌드에 필요한 라이브러리를 받아 놓는다. 저장소에는 포함하지 않는다.
#
#   bash scripts/setup-deps.sh            # 전체 (whisper 모델 포함)
#   bash scripts/setup-deps.sh --no-model # whisper 모델 다운로드 생략 (CI 용)
#
# 필요: git, curl, Homebrew (glfw, mpv 설치용)

set -euo pipefail
NO_MODEL=0
[[ "${1:-}" == "--no-model" ]] && NO_MODEL=1

root="$(cd "$(dirname "$0")/.." && pwd)"
tp="$root/third_party"
mkdir -p "$tp"
cd "$tp"

step() { printf '\033[36m==> %s\033[0m\n' "$1"; }

# 1. 단일 헤더 라이브러리
if [[ ! -f miniaudio.h ]]; then
    step "miniaudio.h"
    curl -sL -o miniaudio.h "https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h"
fi
if [[ ! -f json.hpp ]]; then
    step "nlohmann/json.hpp"
    curl -sL -o json.hpp "https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp"
fi

# 2. SQLite amalgamation
if [[ ! -f sqlite/sqlite3.c ]]; then
    step "SQLite amalgamation"
    curl -sL -o sqlite.zip "https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip"
    unzip -q -o sqlite.zip
    rm sqlite.zip
    rm -rf sqlite
    mv sqlite-amalgamation-3530400 sqlite
fi

# 3. Dear ImGui (검증된 커밋에 고정, Windows 와 동일)
if [[ ! -f imgui/imgui.h ]]; then
    step "Dear ImGui"
    git clone -q https://github.com/ocornut/imgui.git imgui
    git -C imgui checkout -q ea6d21687bec144dd7aee0f4db37f7c61a8799bb
fi

# 4. whisper.cpp (검증된 커밋에 고정, Windows 와 동일)
if [[ ! -f whisper.cpp/CMakeLists.txt ]]; then
    step "whisper.cpp"
    git clone -q https://github.com/ggml-org/whisper.cpp.git whisper.cpp
    git -C whisper.cpp checkout -q c44b60b8053bbf2a5c1e014f11323fb3f2485177
fi

# 5. GLFW / libmpv / 실행 도구 (Homebrew)
if command -v brew >/dev/null; then
    missing=()
    brew list --versions glfw >/dev/null 2>&1 || missing+=(glfw)
    brew list --versions mpv >/dev/null 2>&1 || missing+=(mpv)
    command -v yt-dlp >/dev/null 2>&1 || missing+=(yt-dlp)
    command -v ffmpeg >/dev/null 2>&1 || missing+=(ffmpeg)
    if (( ${#missing[@]} )); then
        step "brew install ${missing[*]}"
        brew install "${missing[@]}"
    fi
else
    echo "경고: Homebrew 가 없습니다. glfw, mpv, yt-dlp, ffmpeg 를 직접 설치하세요." >&2
fi

# 6. whisper 모델 (선택, 148MB). 앱에서도 받을 수 있다.
model="$root/models/ggml-base.en.bin"
if [[ $NO_MODEL -eq 0 && ! -f "$model" ]]; then
    step "whisper 모델 ggml-base.en.bin (148MB)"
    mkdir -p "$(dirname "$model")"
    curl -L -o "$model" "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin"
fi

echo
printf '\033[32m완료. 이제 빌드하세요:\033[0m\n'
echo '  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release'
echo '  cmake --build build -j'

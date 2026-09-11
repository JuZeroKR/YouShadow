#!/bin/bash
# YouShadow macOS 배포 패키지 만들기
#   bash scripts/package.sh [--skip-build]
#
# 결과: dist/YouShadow.app, dist/YouShadow-v<버전>-macos-<arch>.zip
# 동봉: libmpv/glfw 와 그 의존 dylib (Frameworks/), bin/yt-dlp, bin/ffmpeg(+libs), bin/deno
# whisper 모델은 앱에서 첫 실행 때 받는다 (148MB 라 패키지에 넣지 않음).
#
# 필요: Homebrew (mpv, glfw, ffmpeg, dylibbundler)

set -euo pipefail
SKIP_BUILD=0
[[ "${1:-}" == "--skip-build" ]] && SKIP_BUILD=1

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
step() { printf '\033[36m==> %s\033[0m\n' "$1"; }

version=$(sed -n 's/^project(YouShadow VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
arch=$(uname -m)
step "YouShadow v$version ($arch)"

command -v dylibbundler >/dev/null || { echo "dylibbundler 가 필요합니다: brew install dylibbundler" >&2; exit 1; }
brew_prefix=$(brew --prefix)

# ---- 빌드 ----
if [[ $SKIP_BUILD -eq 0 ]]; then
    step "빌드"
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j --target youshadow-gui
fi

# ---- .app 뼈대 ----
dist="$root/dist"
cache="$dist/cache"
app="$dist/YouShadow.app"
contents="$app/Contents"
mkdir -p "$cache"
rm -rf "$app"
mkdir -p "$contents/MacOS/bin" "$contents/Resources" "$contents/Frameworks"

cp build/youshadow-gui "$contents/MacOS/YouShadow"

cat > "$contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>YouShadow</string>
    <key>CFBundleDisplayName</key><string>YouShadow</string>
    <key>CFBundleIdentifier</key><string>com.juzerokr.youshadow</string>
    <key>CFBundleVersion</key><string>$version</string>
    <key>CFBundleShortVersionString</key><string>$version</string>
    <key>CFBundleExecutable</key><string>YouShadow</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleIconFile</key><string>youshadow</string>
    <key>LSMinimumSystemVersion</key><string>12.0</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.education</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSMicrophoneUsageDescription</key>
    <string>쉐도잉 녹음과 발음 채점을 위해 마이크를 사용합니다.</string>
</dict>
</plist>
EOF

# ---- 아이콘 (png → icns) ----
step "아이콘"
iconset="$dist/youshadow.iconset"
rm -rf "$iconset"
mkdir -p "$iconset"
for s in 16 32 128 256 512; do
    sips -z $s $s assets/youshadow.png --out "$iconset/icon_${s}x${s}.png" >/dev/null
    sips -z $((s*2)) $((s*2)) assets/youshadow.png --out "$iconset/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$iconset" -o "$contents/Resources/youshadow.icns"
rm -rf "$iconset"

# ---- 외부 도구 다운로드 (캐시) ----
fetch() { # fetch <파일명> <URL> → 경로를 stdout 으로 반환
    local f="$cache/$1"
    if [[ ! -f "$f" ]]; then
        step "다운로드 $1" >&2
        curl -sL --fail -o "$f" "$2" >&2
    fi
    echo "$f"
}

ytdlp=$(fetch yt-dlp "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_macos")
cp "$ytdlp" "$contents/MacOS/bin/yt-dlp"
chmod +x "$contents/MacOS/bin/yt-dlp"

if [[ "$arch" == "arm64" ]]; then deno_zip="deno-aarch64-apple-darwin.zip"; else deno_zip="deno-x86_64-apple-darwin.zip"; fi
denoz=$(fetch "$deno_zip" "https://github.com/denoland/deno/releases/latest/download/$deno_zip")
unzip -oq "$denoz" -d "$contents/MacOS/bin"
chmod +x "$contents/MacOS/bin/deno"

# ffmpeg 는 Homebrew 것을 복사하고 의존 dylib 을 bin/libs 에 함께 넣는다.
# (@executable_path 는 실행 중인 프로그램 기준이라 앱의 Frameworks/ 를 같이 쓸 수 없다)
cp "$brew_prefix/bin/ffmpeg" "$contents/MacOS/bin/ffmpeg"

# ---- dylib 동봉 + 경로 재작성 ----
step "dylib 동봉 (libmpv, glfw 등)"
dylibbundler -ns -od -b -x "$contents/MacOS/YouShadow" \
    -d "$contents/Frameworks" -p @executable_path/../Frameworks/ >/dev/null
step "dylib 동봉 (ffmpeg)"
dylibbundler -ns -od -b -x "$contents/MacOS/bin/ffmpeg" \
    -d "$contents/MacOS/bin/libs" -p @executable_path/libs/ >/dev/null

# dylibbundler 가 brew 의 rpath 여러 개를 같은 경로로 바꿔 중복 LC_RPATH 가 생길 수 있다.
# 최신 macOS 의 dyld 는 중복 LC_RPATH 가 있는 이미지를 거부하므로 하나만 남긴다.
dedupe_rpaths() {
    local f="$1" rp
    otool -l "$f" | awk '/LC_RPATH/{p=1} p && /path /{print $2; p=0}' | sort | uniq -d |
    while read -r rp; do
        while [[ $(otool -l "$f" | grep -c "path $rp ") -gt 1 ]]; do
            install_name_tool -delete_rpath "$rp" "$f" 2>/dev/null || break
        done
    done
}
step "중복 rpath 정리"
dedupe_rpaths "$contents/MacOS/YouShadow"
dedupe_rpaths "$contents/MacOS/bin/ffmpeg"
for f in "$contents"/Frameworks/*.dylib "$contents"/MacOS/bin/libs/*.dylib; do dedupe_rpaths "$f"; done

# 재작성으로 깨진 서명을 ad-hoc 으로 다시 붙인다 (arm64 는 서명 필수)
step "ad-hoc 서명"
find "$contents/Frameworks" "$contents/MacOS/bin/libs" -name '*.dylib' -exec codesign --force -s - {} \; 2>/dev/null
codesign --force -s - "$contents/MacOS/bin/ffmpeg"
codesign --force -s - "$contents/MacOS/YouShadow"
codesign --force -s - "$app"

# Homebrew 경로가 남아 있으면 다른 컴퓨터에서 실행되지 않으므로 검사한다
leftover=$( { otool -L "$contents/MacOS/YouShadow" "$contents/MacOS/bin/ffmpeg" "$contents"/Frameworks/*.dylib "$contents"/MacOS/bin/libs/*.dylib | grep -o '/opt/homebrew[^ ]*\|/usr/local/opt[^ ]*'; } | sort -u || true)
if [[ -n "$leftover" ]]; then
    echo "오류: Homebrew 경로가 남아 있습니다:" >&2
    echo "$leftover" >&2
    exit 1
fi

# ---- 안내문 + zip ----
stage="$dist/stage-mac"
rm -rf "$stage"
mkdir -p "$stage"
cp -R "$app" "$stage/"
cat > "$stage/READ_ME_FIRST.txt" <<EOF
YouShadow v$version (macOS $arch)
====================
YouShadow.app 을 응용 프로그램 폴더로 옮기고 실행하세요.
코드 서명이 없어서 처음에는 우클릭 → [열기] 로 실행해야 할 수 있습니다.
그래도 막히면 터미널에서: xattr -dr com.apple.quarantine /Applications/YouShadow.app

학습 데이터는 ~/Library/Application Support/YouShadow 에 저장됩니다.
발음 채점과 자막 없는 영상의 대본 생성에는 whisper 모델(148MB)이 필요하며,
앱 상단의 [STT 모델 받기] 버튼으로 받을 수 있습니다.

동봉된 도구: yt-dlp, ffmpeg, deno (유튜브 다운로드용)
EOF
cp LICENSE "$stage/LICENSE.txt"

zip_path="$dist/YouShadow-v$version-macos-$arch.zip"
rm -f "$zip_path"
step "zip"
ditto -c -k "$stage" "$zip_path"
rm -rf "$stage"

echo
ls -lh "$dist" | awk '{print $9, $5}'

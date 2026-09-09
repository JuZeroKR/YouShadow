# YouShadow 배포 패키지 만들기
#   powershell -ExecutionPolicy Bypass -File scripts\package.ps1 [-SkipBuild] [-SkipInstaller]
#
# 결과: dist\YouShadow-v<버전>-win64.zip (포터블), dist\YouShadow-Setup-v<버전>.exe (Inno Setup 설치 프로그램)
# 동봉: youshadow-gui.exe → YouShadow.exe, libmpv-2.dll, bin\yt-dlp.exe, bin\ffmpeg.exe, bin\deno.exe
# whisper 모델은 앱에서 첫 실행 때 받는다 (148MB 라 패키지에 넣지 않음).

param([switch]$SkipBuild, [switch]$SkipInstaller)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

# 버전은 CMakeLists.txt 의 project(... VERSION x.y.z) 에서 읽는다
$version = (Select-String -Path CMakeLists.txt -Pattern 'project\(YouShadow VERSION ([0-9.]+)').Matches[0].Groups[1].Value
Write-Host "==> YouShadow v$version" -ForegroundColor Cyan

if (-not $SkipBuild) {
    Write-Host "==> 빌드" -ForegroundColor Cyan
    # 생성기를 지정하지 않으면 설치된 최신 Visual Studio 를 쓴다 (2022 / 2026 모두 동작)
    cmake -S . -B build -A x64
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
    cmake --build build --config Release --target youshadow-gui
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

$dist = Join-Path $root "dist"
$cache = Join-Path $dist "cache"
$stage = Join-Path $dist "YouShadow"
New-Item -ItemType Directory -Force $cache | Out-Null
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\bin" | Out-Null

# ---- 외부 도구 다운로드 (캐시) ----
function Fetch($name, $url) {
    $f = Join-Path $cache $name
    if (-not (Test-Path $f)) {
        Write-Host "==> 다운로드 $name" -ForegroundColor Cyan
        curl.exe -L --fail -o $f $url
    }
    return $f
}

$ytdlp = Fetch "yt-dlp.exe" "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe"

$ffzip = Fetch "ffmpeg.zip" "https://github.com/yt-dlp/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip"
$ffdir = Join-Path $cache "ffmpeg"
if (-not (Test-Path "$ffdir\ffmpeg.exe")) {
    Write-Host "==> ffmpeg 추출" -ForegroundColor Cyan
    $tmp = Join-Path $cache "ffmpeg-tmp"
    if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
    Expand-Archive -Path $ffzip -DestinationPath $tmp -Force
    New-Item -ItemType Directory -Force $ffdir | Out-Null
    Copy-Item (Get-ChildItem $tmp -Recurse -Filter ffmpeg.exe | Select-Object -First 1).FullName "$ffdir\ffmpeg.exe"
    Remove-Item $tmp -Recurse -Force
}

$denozip = Fetch "deno.zip" "https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip"
$denodir = Join-Path $cache "deno"
if (-not (Test-Path "$denodir\deno.exe")) {
    Write-Host "==> deno 추출" -ForegroundColor Cyan
    Expand-Archive -Path $denozip -DestinationPath $denodir -Force
}

# ---- 스테이징 ----
Write-Host "==> 파일 모으기" -ForegroundColor Cyan
Copy-Item "build\Release\youshadow-gui.exe" "$stage\YouShadow.exe"
Copy-Item "build\Release\libmpv-2.dll" "$stage\libmpv-2.dll"
Copy-Item $ytdlp "$stage\bin\yt-dlp.exe"
Copy-Item "$ffdir\ffmpeg.exe" "$stage\bin\ffmpeg.exe"
Copy-Item "$denodir\deno.exe" "$stage\bin\deno.exe"
Copy-Item "LICENSE" "$stage\LICENSE.txt"
Copy-Item "README.md" "$stage\README.md"
@"
YouShadow v$version
====================
YouShadow.exe 를 실행하세요. 학습 데이터는 %LOCALAPPDATA%\YouShadow 에 저장됩니다.
발음 채점과 자막 없는 영상의 대본 생성에는 whisper 모델(148MB)이 필요하며,
앱 상단의 [STT 모델 받기] 버튼으로 받을 수 있습니다.

bin\ 에 동봉된 도구: yt-dlp, ffmpeg, deno (유튜브 다운로드용)
"@ | Out-File -Encoding utf8 "$stage\READ_ME_FIRST.txt"

# ---- zip ----
$zip = Join-Path $dist "YouShadow-v$version-win64.zip"
if (Test-Path $zip) { Remove-Item $zip }
Write-Host "==> zip" -ForegroundColor Cyan
Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal

# ---- Inno Setup ----
if (-not $SkipInstaller) {
    $iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($iscc) {
        Write-Host "==> Inno Setup" -ForegroundColor Cyan
        & $iscc /Q "/DAppVersion=$version" "/DStageDir=$stage" "/DOutDir=$dist" "installer\youshadow.iss"
        if ($LASTEXITCODE -ne 0) { throw "ISCC failed" }
    } else {
        Write-Host "Inno Setup 이 없어 설치 프로그램은 건너뜁니다 (winget install JRSoftware.InnoSetup)" -ForegroundColor Yellow
    }
}

Write-Host ""
Get-ChildItem $dist -File | Select-Object Name, @{n='MB';e={[math]::Round($_.Length/1MB,1)}} | Format-Table -AutoSize

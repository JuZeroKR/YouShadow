@echo off
rem YouShadow GUI 실행. 사용법: run-gui.cmd [유튜브 URL]
cd /d "%~dp0"
chcp 65001 >nul
if not exist build\Release\youshadow-gui.exe (
    echo 먼저 빌드하세요: cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^&^& cmake --build build --config Release
    pause
    exit /b 1
)
build\Release\youshadow-gui.exe %*

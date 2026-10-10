cmake_minimum_required(VERSION 3.20)
project(YouShadow VERSION 0.6.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    enable_language(OBJCXX)  # tts_mac.mm (AVSpeechSynthesizer)
endif()

# 배포판이 VC 런타임 없이 동작하도록 CRT 를 정적으로 링크한다 (하위 프로젝트 포함)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(TP ${CMAKE_SOURCE_DIR}/third_party)

# ---- 공통 옵션 ----
add_library(ys_options INTERFACE)
target_compile_definitions(ys_options INTERFACE YS_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_compile_definitions(ys_options INTERFACE
        _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(APPLE)
    target_compile_definitions(ys_options INTERFACE GL_SILENCE_DEPRECATION)
endif()
if(MSVC)
    target_compile_options(ys_options INTERFACE /utf-8 /W3)
endif()

# ---- whisper.cpp (음성 인식, 정적 링크) ----
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
if(APPLE)
    # Windows 와 같은 CPU 경로를 쓴다 (base.en 모델은 CPU 로 충분)
    set(GGML_METAL OFF CACHE BOOL "" FORCE)
else()
    # 빌드한 PC 의 CPU 기능(GGML_NATIVE)을 쓰면 CI 러너에 AVX-512 가 있을 때 AVX-512 가 없는 PC(예: Core Ultra)에서
    # 시작하자마자 0xC000001D 로 죽는다. 배포판은 AVX2 기준으로 고정한다 (2013년 이후 대부분의 x86 CPU)
    set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
    set(GGML_AVX ON CACHE BOOL "" FORCE)
    set(GGML_AVX2 ON CACHE BOOL "" FORCE)
    set(GGML_FMA ON CACHE BOOL "" FORCE)
    set(GGML_F16C ON CACHE BOOL "" FORCE)
    set(GGML_AVX512 OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory(${TP}/whisper.cpp EXCLUDE_FROM_ALL)

# ---- SQLite ----
add_library(sqlite3 STATIC ${TP}/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${TP}/sqlite)
target_compile_definitions(sqlite3 PRIVATE SQLITE_OMIT_LOAD_EXTENSION SQLITE_THREADSAFE=1)
if(MSVC)
    target_compile_options(sqlite3 PRIVATE /w)
endif()

# ---- 핵심 로직 (CLI/GUI 공용) ----
add_library(ys_core STATIC
    src/lang.cpp
    src/paths.cpp
    src/youtube.cpp
    src/transcript.cpp
    src/audio.cpp
    src/db.cpp
    src/stt.cpp
    src/scoring.cpp
    src/http.cpp
    src/secret.cpp
    src/llm.cpp
    src/tts.cpp
    src/subtitle.cpp
    src/local.cpp
    src/jadict.cpp
    src/endict.cpp
    src/prosody.cpp
)
if(APPLE)
    target_sources(ys_core PRIVATE src/tts_mac.mm)
endif()
target_include_directories(ys_core PUBLIC ${TP} src)
target_link_libraries(ys_core PUBLIC ys_options sqlite3 whisper)
if(WIN32)
    target_link_libraries(ys_core PUBLIC winhttp crypt32 ole32)
elseif(APPLE)
    target_link_libraries(ys_core PUBLIC
        curl
        iconv   # subtitle.cpp: CP949 자막 변환
        "-framework Security"
        "-framework CoreFoundation"
        "-framework Foundation"
        "-framework AVFoundation"
        "-framework CoreAudio"
        "-framework AudioToolbox"
    )
endif()

# ---- Dear ImGui + GLFW 백엔드 ----
set(IMGUI ${TP}/imgui)
add_library(imgui STATIC
    ${IMGUI}/imgui.cpp
    ${IMGUI}/imgui_draw.cpp
    ${IMGUI}/imgui_tables.cpp
    ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_glfw.cpp
    ${IMGUI}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI} ${IMGUI}/backends)
target_link_libraries(imgui PUBLIC ys_options)
if(WIN32)
    target_include_directories(imgui PUBLIC ${TP}/glfw/include)
else()
    find_package(glfw3 3.3 REQUIRED)   # brew install glfw
    target_link_libraries(imgui PUBLIC glfw)
endif()

# ---- CLI ----
add_executable(youshadow-cli src/main.cpp)
target_link_libraries(youshadow-cli PRIVATE ys_core)

# ---- STT/채점 검증 도구 ----
add_executable(stt_test src/stt_test.cpp)
target_link_libraries(stt_test PRIVATE ys_core)

# ---- LLM 연동 검증 도구 ----
add_executable(llm_test src/llm_test.cpp)
target_link_libraries(llm_test PRIVATE ys_core)
add_executable(ja_test src/ja_test.cpp)
target_link_libraries(ja_test PRIVATE ys_core)
# 억양 · 리듬 · 강세 비교 검증 도구 (합성 신호 자체 테스트 + 실제 녹음 비교)
add_executable(prosody_test src/prosody_test.cpp)
target_link_libraries(prosody_test PRIVATE ys_core)
# 자막 붙은 단어 떼기 검증 도구 (자체 테스트 + segments.json 미리보기)
add_executable(subtitle_test src/subtitle_test.cpp)
target_link_libraries(subtitle_test PRIVATE ys_core)

# 일본어 오프라인 사전 팩 빌드 도구 (개발용, 배포본에는 들어가지 않는다)
add_executable(ja_dict_build tools/ja_dict_build.cpp)
target_link_libraries(ja_dict_build PRIVATE ys_options)
if(APPLE)
    target_link_libraries(ja_dict_build PRIVATE iconv)
endif()

# ---- GUI ----
if(WIN32)
    # 콘솔 창 없는 Windows 앱, 진입점은 main
    add_executable(youshadow-gui WIN32
        src/gui_main.cpp
        src/player.cpp
        assets/youshadow.rc
    )
    target_include_directories(youshadow-gui PRIVATE ${TP}/mpv/include)
    target_link_libraries(youshadow-gui PRIVATE
        ys_core imgui
        ${TP}/glfw/lib-vc2022/glfw3_mt.lib
        ${TP}/mpv/libmpv.lib
        opengl32
        comdlg32
    )
    target_link_options(youshadow-gui PRIVATE /ENTRY:mainCRTStartup)
    add_custom_command(TARGET youshadow-gui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${TP}/mpv/libmpv-2.dll $<TARGET_FILE_DIR:youshadow-gui>
    )
else()
    # macOS: libmpv 는 Homebrew (brew install mpv)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
    add_executable(youshadow-gui
        src/gui_main.cpp
        src/player.cpp
    )
    target_link_libraries(youshadow-gui PRIVATE
        ys_core imgui
        PkgConfig::MPV
        "-framework OpenGL"
    )
endif()

cmake_minimum_required(VERSION 3.20)
project(YouShadow VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 배포판이 VC 런타임 없이 동작하도록 CRT 를 정적으로 링크한다 (하위 프로젝트 포함)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(TP ${CMAKE_SOURCE_DIR}/third_party)

# ---- 공통 옵션 ----
add_library(ys_options INTERFACE)
target_compile_definitions(ys_options INTERFACE
    _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN
    YS_VERSION="${PROJECT_VERSION}")
if(MSVC)
    target_compile_options(ys_options INTERFACE /utf-8 /W3)
endif()

# ---- whisper.cpp (음성 인식, 정적 링크) ----
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
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
)
target_include_directories(ys_core PUBLIC ${TP} src)
target_link_libraries(ys_core PUBLIC ys_options sqlite3 whisper winhttp crypt32)

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
target_include_directories(imgui PUBLIC ${IMGUI} ${IMGUI}/backends ${TP}/glfw/include)
target_link_libraries(imgui PUBLIC ys_options)

# ---- CLI ----
add_executable(youshadow-cli src/main.cpp)
target_link_libraries(youshadow-cli PRIVATE ys_core)

# ---- STT/채점 검증 도구 ----
add_executable(stt_test src/stt_test.cpp)
target_link_libraries(stt_test PRIVATE ys_core)

# ---- LLM 연동 검증 도구 ----
add_executable(llm_test src/llm_test.cpp)
target_link_libraries(llm_test PRIVATE ys_core)

# ---- GUI (콘솔 창 없는 Windows 앱, 진입점은 main) ----
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
)
target_link_options(youshadow-gui PRIVATE /ENTRY:mainCRTStartup)
add_custom_command(TARGET youshadow-gui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${TP}/mpv/libmpv-2.dll $<TARGET_FILE_DIR:youshadow-gui>
)

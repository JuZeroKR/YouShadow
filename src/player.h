#pragma once
#include <atomic>
#include <string>

struct mpv_handle;
struct mpv_render_context;

// libmpv 를 OpenGL 텍스처로 렌더링하는 래퍼. 모든 호출은 GL 컨텍스트가 있는 메인 스레드에서.
class MpvPlayer {
public:
    using ProcAddressFn = void* (*)(const char* name);

    MpvPlayer() = default;
    ~MpvPlayer();

    bool init(ProcAddressFn getProcAddress, std::string* error);
    void shutdown();

    void load(const std::string& path);
    bool hasFile() const { return hasFile_; }

    void seek(double sec);
    void setPaused(bool paused);
    bool paused() const;
    bool seeking() const;
    void setSpeed(double speed);
    void setVolume(double percent);  // 0~100
    double timePos() const;
    double duration() const;

    // 이벤트 큐를 비우고 (필요하면) 새 프레임을 텍스처에 그린다. 매 프레임 호출.
    // 반환값은 GL 텍스처 ID (0이면 아직 없음).
    unsigned render(int width, int height);

private:
    static void* procAdapter(void* ctx, const char* name);
    static void onUpdate(void* ctx);
    void ensureTarget(int w, int h);
    double getDouble(const char* prop) const;
    int getFlag(const char* prop) const;

    mpv_handle* mpv_ = nullptr;
    mpv_render_context* ctx_ = nullptr;
    ProcAddressFn proc_ = nullptr;
    unsigned fbo_ = 0, tex_ = 0;
    int w_ = 0, h_ = 0;
    bool hasFile_ = false;
    bool clearPending_ = false;   // 새 파일을 열 때 이전 프레임을 지운다
    int forceFrames_ = 0;         // 파일 로드 직후 몇 프레임은 무조건 다시 그린다
    std::atomic<bool> redraw_{false};
};

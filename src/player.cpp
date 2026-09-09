#include "player.h"

#include <windows.h>
#include <GL/gl.h>

#include <cstdio>
#include <mpv/client.h>
#include <mpv/render_gl.h>

namespace {

// 필요한 GL 3.0 FBO 함수만 직접 로드한다.
constexpr GLenum kFramebuffer = 0x8D40;
constexpr GLenum kColorAttachment0 = 0x8CE0;
constexpr GLint kRGBA8 = 0x8058;

typedef void(APIENTRY* PFN_GenFramebuffers)(GLsizei, GLuint*);
typedef void(APIENTRY* PFN_BindFramebuffer)(GLenum, GLuint);
typedef void(APIENTRY* PFN_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void(APIENTRY* PFN_DeleteFramebuffers)(GLsizei, const GLuint*);

PFN_GenFramebuffers glGenFramebuffers_;
PFN_BindFramebuffer glBindFramebuffer_;
PFN_FramebufferTexture2D glFramebufferTexture2D_;
PFN_DeleteFramebuffers glDeleteFramebuffers_;

}  // namespace

MpvPlayer::~MpvPlayer() { shutdown(); }

void* MpvPlayer::procAdapter(void* ctx, const char* name) {
    return static_cast<MpvPlayer*>(ctx)->proc_(name);
}

void MpvPlayer::onUpdate(void* ctx) { static_cast<MpvPlayer*>(ctx)->redraw_ = true; }

bool MpvPlayer::init(ProcAddressFn getProcAddress, std::string* error) {
    proc_ = getProcAddress;
    glGenFramebuffers_ = (PFN_GenFramebuffers)proc_("glGenFramebuffers");
    glBindFramebuffer_ = (PFN_BindFramebuffer)proc_("glBindFramebuffer");
    glFramebufferTexture2D_ = (PFN_FramebufferTexture2D)proc_("glFramebufferTexture2D");
    glDeleteFramebuffers_ = (PFN_DeleteFramebuffers)proc_("glDeleteFramebuffers");
    if (!glGenFramebuffers_ || !glBindFramebuffer_ || !glFramebufferTexture2D_ || !glDeleteFramebuffers_) {
        if (error) *error = "OpenGL 3.0 FBO 함수를 찾을 수 없습니다";
        return false;
    }

    mpv_ = mpv_create();
    if (!mpv_) {
        if (error) *error = "mpv_create 실패";
        return false;
    }
    mpv_set_option_string(mpv_, "vo", "libmpv");
    mpv_set_option_string(mpv_, "hwdec", "auto-safe");
    mpv_set_option_string(mpv_, "keep-open", "yes");
    mpv_set_option_string(mpv_, "pause", "yes");
    mpv_set_option_string(mpv_, "hr-seek", "yes");
    mpv_set_option_string(mpv_, "terminal", "no");
    mpv_set_option_string(mpv_, "ytdl", "no");
    mpv_set_option_string(mpv_, "sub", "no");  // 자막은 우리가 직접 그린다
    if (mpv_initialize(mpv_) < 0) {
        if (error) *error = "mpv_initialize 실패";
        return false;
    }

    mpv_opengl_init_params gl{procAdapter, this};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpv_render_context_create(&ctx_, mpv_, params) < 0) {
        if (error) *error = "mpv_render_context_create 실패";
        return false;
    }
    mpv_render_context_set_update_callback(ctx_, onUpdate, this);
    return true;
}

void MpvPlayer::shutdown() {
    if (ctx_) {
        mpv_render_context_free(ctx_);
        ctx_ = nullptr;
    }
    if (mpv_) {
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
    }
    if (fbo_) glDeleteFramebuffers_(1, &fbo_);
    if (tex_) glDeleteTextures(1, &tex_);
    fbo_ = tex_ = 0;
    w_ = h_ = 0;
    hasFile_ = false;
}

void MpvPlayer::load(const std::string& path) {
    if (!mpv_) return;
    const char* cmd[] = {"loadfile", path.c_str(), nullptr};
    mpv_command(mpv_, cmd);
    hasFile_ = false;  // FILE_LOADED 이벤트에서 true 로
    clearPending_ = true;
}

void MpvPlayer::seek(double sec) {
    if (!mpv_) return;
    char buf[64];
    snprintf(buf, sizeof buf, "%.3f", sec);
    const char* cmd[] = {"seek", buf, "absolute+exact", nullptr};
    mpv_command(mpv_, cmd);
}

void MpvPlayer::setPaused(bool paused) {
    if (!mpv_) return;
    int flag = paused ? 1 : 0;
    mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &flag);
}

void MpvPlayer::setSpeed(double speed) {
    if (!mpv_) return;
    mpv_set_property(mpv_, "speed", MPV_FORMAT_DOUBLE, &speed);
}

double MpvPlayer::getDouble(const char* prop) const {
    double v = 0.0;
    if (mpv_) mpv_get_property(mpv_, prop, MPV_FORMAT_DOUBLE, &v);
    return v;
}

int MpvPlayer::getFlag(const char* prop) const {
    int v = 0;
    if (mpv_) mpv_get_property(mpv_, prop, MPV_FORMAT_FLAG, &v);
    return v;
}

bool MpvPlayer::paused() const { return getFlag("pause") != 0; }
bool MpvPlayer::seeking() const { return getFlag("seeking") != 0; }
double MpvPlayer::timePos() const { return getDouble("time-pos"); }
double MpvPlayer::duration() const { return getDouble("duration"); }

void MpvPlayer::ensureTarget(int w, int h) {
    if (w == w_ && h == h_ && tex_) return;
    if (!tex_) glGenTextures(1, &tex_);
    if (!fbo_) glGenFramebuffers_(1, &fbo_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, kRGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer_(kFramebuffer, fbo_);
    glFramebufferTexture2D_(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, tex_, 0);
    glBindFramebuffer_(kFramebuffer, 0);
    w_ = w;
    h_ = h;
    redraw_ = true;
}

unsigned MpvPlayer::render(int width, int height) {
    if (!mpv_ || !ctx_) return 0;

    // 이벤트 큐 비우기
    for (;;) {
        mpv_event* ev = mpv_wait_event(mpv_, 0);
        if (!ev || ev->event_id == MPV_EVENT_NONE) break;
        if (ev->event_id == MPV_EVENT_FILE_LOADED) {
            hasFile_ = true;
            // 일시정지 상태로 열면 첫 프레임이 표시되지 않으므로 한 프레임 진행시켜 그린다
            if (paused()) {
                const char* step[] = {"frame-step", nullptr};
                mpv_command_async(mpv_, 0, step);
            }
            forceFrames_ = 60;
        }
        if (ev->event_id == MPV_EVENT_END_FILE) hasFile_ = false;
    }

    if (width < 16 || height < 16) return tex_;
    const bool resized = (width != w_ || height != h_);
    ensureTarget(width, height);

    if (clearPending_) {
        clearPending_ = false;
        glBindFramebuffer_(kFramebuffer, fbo_);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer_(kFramebuffer, 0);
    }

    bool need = resized;
    if (redraw_.exchange(false)) {
        need |= (mpv_render_context_update(ctx_) & MPV_RENDER_UPDATE_FRAME) != 0;
    }
    if (forceFrames_ > 0 && hasFile_) {
        --forceFrames_;
        need = true;
    }
    if (need) {
        mpv_opengl_fbo fbo{(int)fbo_, w_, h_, 0};
        int flipY = 0;  // ImGui 텍스처 좌표계(위→아래)와 맞춤
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flipY},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        mpv_render_context_render(ctx_, params);
        glBindFramebuffer_(kFramebuffer, 0);
    }
    return tex_;
}

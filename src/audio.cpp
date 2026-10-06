#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "audio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {

constexpr int kSR = AudioEngine::kSampleRate;

size_t msToFrames(int ms) { return (size_t)ms * kSR / 1000; }

ma_device_config makeConfig(ma_device_type type, ma_device_data_proc cb, void* user) {
    ma_device_config cfg = ma_device_config_init(type);
    cfg.sampleRate = kSR;
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 1;
    cfg.capture.format = ma_format_f32;
    cfg.capture.channels = 1;
    cfg.dataCallback = cb;
    cfg.pUserData = user;
    return cfg;
}

void openAndStart(ma_device& dev, const ma_device_config& cfg) {
    if (ma_device_init(nullptr, &cfg, &dev) != MA_SUCCESS) {
        throw std::runtime_error("오디오 장치를 열 수 없습니다 (마이크/스피커 확인)");
    }
    if (ma_device_start(&dev) != MA_SUCCESS) {
        ma_device_uninit(&dev);
        throw std::runtime_error("오디오 장치를 시작할 수 없습니다");
    }
}

// ---------------- 블로킹 세션 (CLI) ----------------

struct Session {
    const float* src = nullptr;
    size_t srcBegin = 0, srcEnd = 0, cursor = 0;
    int loopsLeft = 0;
    size_t gapFrames = msToFrames(500);  // 반복 사이 무음
    size_t gapLeft = 0;

    std::vector<float>* rec = nullptr;
    size_t recTarget = 0;    // 녹음 목표 프레임 수 (0이면 재생 종료 기준)
    size_t tailFrames = 0;   // 재생 종료 후 추가 녹음
    size_t tailLeft = 0;
    bool playbackDone = false;

    std::atomic<bool> done{false};

    bool hasPlayback() const { return src != nullptr; }
};

void sessionCallback(ma_device* dev, void* out, const void* in, ma_uint32 frames) {
    auto* s = static_cast<Session*>(dev->pUserData);
    float* o = static_cast<float*>(out);
    const float* i = static_cast<const float*>(in);

    if (o) std::fill(o, o + frames, 0.0f);

    if (s->hasPlayback() && !s->playbackDone && o) {
        ma_uint32 written = 0;
        while (written < frames) {
            if (s->gapLeft > 0) {
                size_t n = std::min<size_t>(s->gapLeft, frames - written);
                s->gapLeft -= n;
                written += (ma_uint32)n;
                continue;
            }
            if (s->cursor >= s->srcEnd) {
                if (--s->loopsLeft > 0) {
                    s->cursor = s->srcBegin;
                    s->gapLeft = s->gapFrames;
                    continue;
                }
                s->playbackDone = true;
                s->tailLeft = s->tailFrames;
                break;
            }
            size_t n = std::min<size_t>(s->srcEnd - s->cursor, frames - written);
            std::copy(s->src + s->cursor, s->src + s->cursor + n, o + written);
            s->cursor += n;
            written += (ma_uint32)n;
        }
    }

    if (s->rec && i) s->rec->insert(s->rec->end(), i, i + frames);

    if (s->hasPlayback()) {
        if (s->playbackDone) {
            if (!s->rec || s->tailLeft == 0) s->done = true;
            else s->tailLeft -= std::min<size_t>(s->tailLeft, frames);
        }
    } else if (s->rec && s->rec->size() >= s->recTarget) {
        s->done = true;
    }
}

void runSession(Session& s) {
    ma_device_type type;
    if (s.hasPlayback() && s.rec) type = ma_device_type_duplex;
    else if (s.rec) type = ma_device_type_capture;
    else type = ma_device_type_playback;

    ma_device dev;
    openAndStart(dev, makeConfig(type, sessionCallback, &s));
    while (!s.done) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ma_device_uninit(&dev);
}

}  // namespace

// ---------------- AudioEngine ----------------

std::vector<float> AudioEngine::resample(const std::vector<float>& pcm, int fromRate, int toRate) {
    if (fromRate == toRate || pcm.empty()) return pcm;
    const double ratio = (double)fromRate / toRate;
    std::vector<float> out((size_t)(pcm.size() / ratio));
    for (size_t i = 0; i < out.size(); ++i) {
        double pos = i * ratio;
        size_t k = (size_t)pos;
        double f = pos - k;
        float a = pcm[std::min(k, pcm.size() - 1)];
        float b = pcm[std::min(k + 1, pcm.size() - 1)];
        out[i] = (float)(a + (b - a) * f);
    }
    return out;
}

std::vector<float> AudioEngine::loadWav(const std::string& path, int sampleRate) {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, (ma_uint32)sampleRate);
    ma_decoder dec;
    if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS) {
        throw std::runtime_error("오디오 파일을 열 수 없음: " + path);
    }
    std::vector<float> pcm;
    std::vector<float> chunk((size_t)sampleRate);
    for (;;) {
        ma_uint64 got = 0;
        ma_result r = ma_decoder_read_pcm_frames(&dec, chunk.data(), chunk.size(), &got);
        pcm.insert(pcm.end(), chunk.begin(), chunk.begin() + (size_t)got);
        if (r != MA_SUCCESS || got < chunk.size()) break;
    }
    ma_decoder_uninit(&dec);
    return pcm;
}

std::vector<float> AudioEngine::loadWavSlice(const std::string& path, int startMs, int endMs, int sampleRate) {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, (ma_uint32)sampleRate);
    ma_decoder dec;
    if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS) {
        throw std::runtime_error("오디오 파일을 열 수 없음: " + path);
    }
    // seek 의 프레임 번호는 출력 샘플레이트 기준이라 파일이 48 kHz 든 16 kHz 든 같은 식이 된다
    const ma_uint64 start = (ma_uint64)std::max(0, startMs) * (ma_uint64)sampleRate / 1000;
    const ma_uint64 want = endMs > startMs ? (ma_uint64)(endMs - startMs) * (ma_uint64)sampleRate / 1000 : 0;
    std::vector<float> pcm;
    if (want > 0 && ma_decoder_seek_to_pcm_frame(&dec, start) == MA_SUCCESS) {
        pcm.resize((size_t)want);
        ma_uint64 got = 0;
        ma_decoder_read_pcm_frames(&dec, pcm.data(), want, &got);
        pcm.resize((size_t)got);
    }
    ma_decoder_uninit(&dec);
    return pcm;
}

namespace {
// 프레임을 binFrames 단위로 묶어 [min,max] 를 out 에 누적한다. carry 는 bin 경계를 넘는 상태.
struct PeakAccum {
    size_t binFrames;
    size_t count = 0;
    float lo = 0, hi = 0;
    std::vector<float> out;
    void feed(const float* p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            lo = std::min(lo, p[i]);
            hi = std::max(hi, p[i]);
            if (++count == binFrames) flush();
        }
    }
    void flush() {
        if (count == 0) return;
        out.push_back(lo);
        out.push_back(hi);
        count = 0;
        lo = hi = 0;
    }
};
}  // namespace

std::vector<float> AudioEngine::loadPeaks(const std::string& path, int binMs) {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, kSR);
    ma_decoder dec;
    if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS) {
        throw std::runtime_error("오디오 파일을 열 수 없음: " + path);
    }
    PeakAccum acc{msToFrames(binMs)};
    std::vector<float> chunk(kSR);
    for (;;) {
        ma_uint64 got = 0;
        ma_result r = ma_decoder_read_pcm_frames(&dec, chunk.data(), chunk.size(), &got);
        acc.feed(chunk.data(), (size_t)got);
        if (r != MA_SUCCESS || got < chunk.size()) break;
    }
    ma_decoder_uninit(&dec);
    acc.flush();
    return std::move(acc.out);
}

std::vector<float> AudioEngine::peaksFromPcm(const std::vector<float>& pcm, int binMs) {
    PeakAccum acc{msToFrames(binMs)};
    acc.feed(pcm.data(), pcm.size());
    acc.flush();
    return std::move(acc.out);
}

void AudioEngine::saveWav(const std::string& path, const std::vector<float>& pcm) {
    ma_encoder_config cfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 1, kSR);
    ma_encoder enc;
    if (ma_encoder_init_file(path.c_str(), &cfg, &enc) != MA_SUCCESS) {
        throw std::runtime_error("녹음 파일을 저장할 수 없음: " + path);
    }
    ma_uint64 written = 0;
    ma_encoder_write_pcm_frames(&enc, pcm.data(), pcm.size(), &written);
    ma_encoder_uninit(&enc);
}

void AudioEngine::play(const std::vector<float>& pcm, int startMs, int endMs, int loops) {
    Session s;
    s.src = pcm.data();
    s.srcBegin = std::min(msToFrames(startMs), pcm.size());
    s.srcEnd = std::min(msToFrames(endMs), pcm.size());
    s.cursor = s.srcBegin;
    s.loopsLeft = std::max(1, loops);
    if (s.srcBegin >= s.srcEnd) return;
    runSession(s);
}

std::vector<float> AudioEngine::playAndRecord(const std::vector<float>& pcm, int startMs, int endMs,
                                              int tailMs) {
    std::vector<float> rec;
    Session s;
    s.src = pcm.data();
    s.srcBegin = std::min(msToFrames(startMs), pcm.size());
    s.srcEnd = std::min(msToFrames(endMs), pcm.size());
    s.cursor = s.srcBegin;
    s.loopsLeft = 1;
    s.rec = &rec;
    s.tailFrames = msToFrames(tailMs);
    rec.reserve(s.srcEnd - s.srcBegin + s.tailFrames);
    runSession(s);
    return rec;
}

std::vector<float> AudioEngine::record(int durationMs) {
    std::vector<float> rec;
    Session s;
    s.rec = &rec;
    s.recTarget = msToFrames(durationMs);
    rec.reserve(s.recTarget);
    runSession(s);
    rec.resize(std::min(rec.size(), s.recTarget));
    return rec;
}

// ---------------- Recorder (비동기) ----------------

struct Recorder::Impl {
    ma_device dev{};
    bool open = false;
    std::mutex m;
    std::vector<float> buf;
    std::atomic<float> level{0.0f};
    std::atomic<size_t> frames{0};

    static void cb(ma_device* d, void*, const void* in, ma_uint32 n) {
        auto* self = static_cast<Impl*>(d->pUserData);
        const float* i = static_cast<const float*>(in);
        if (!i) return;
        float peak = 0.0f;
        for (ma_uint32 k = 0; k < n; ++k) peak = std::max(peak, std::abs(i[k]));
        // 콜백마다 튀는 피크 대신 천천히 감쇠하는 포락선을 레벨로 쓴다 (약 100ms 유지)
        self->level = std::max(peak, self->level.load() * 0.85f);
        std::lock_guard<std::mutex> lock(self->m);
        self->buf.insert(self->buf.end(), i, i + n);
        self->frames = self->buf.size();
    }
};

Recorder::Recorder() : impl_(new Impl) {}
Recorder::~Recorder() { if (impl_->open) ma_device_uninit(&impl_->dev); }

void Recorder::start() {
    if (impl_->open) stop();
    {
        std::lock_guard<std::mutex> lock(impl_->m);
        impl_->buf.clear();
        impl_->frames = 0;
    }
    openAndStart(impl_->dev, makeConfig(ma_device_type_capture, Impl::cb, impl_.get()));
    impl_->open = true;
}

std::vector<float> Recorder::stop() {
    if (!impl_->open) return {};
    ma_device_uninit(&impl_->dev);
    impl_->open = false;
    impl_->level = 0.0f;
    std::lock_guard<std::mutex> lock(impl_->m);
    return std::move(impl_->buf);
}

bool Recorder::active() const { return impl_->open; }
float Recorder::level() const { return impl_->level; }
int Recorder::recordedMs() const { return (int)(impl_->frames * 1000 / kSR); }

// ---------------- Player (비동기) ----------------

struct Player::Impl {
    ma_device dev{};
    bool open = false;
    std::vector<float> pcm;
    std::atomic<size_t> cursor{0};
    std::atomic<bool> done{true};
    std::atomic<float> gain{1.0f};

    static void cb(ma_device* d, void* out, const void*, ma_uint32 n) {
        auto* self = static_cast<Impl*>(d->pUserData);
        float* o = static_cast<float*>(out);
        size_t c = self->cursor;
        size_t avail = c < self->pcm.size() ? self->pcm.size() - c : 0;
        size_t k = std::min<size_t>(avail, n);
        const float g = self->gain.load();
        for (size_t i = 0; i < k; ++i) o[i] = std::clamp(self->pcm[c + i] * g, -1.0f, 1.0f);
        std::fill(o + k, o + n, 0.0f);
        self->cursor = c + k;
        if (k < n) self->done = true;
    }
};

Player::Player() : impl_(new Impl) {}
Player::~Player() { stop(); }

void Player::play(std::vector<float> pcm) {
    stop();
    impl_->pcm = std::move(pcm);
    impl_->cursor = 0;
    impl_->done = impl_->pcm.empty();
    if (impl_->done) return;
    openAndStart(impl_->dev, makeConfig(ma_device_type_playback, Impl::cb, impl_.get()));
    impl_->open = true;
}

void Player::stop() {
    if (impl_->open) {
        ma_device_uninit(&impl_->dev);
        impl_->open = false;
    }
    impl_->done = true;
}

bool Player::playing() const { return impl_->open && !impl_->done; }
void Player::setGain(float gain) { impl_->gain = std::max(0.0f, gain); }
int Player::positionMs() const { return (int)(impl_->cursor * 1000 / kSR); }

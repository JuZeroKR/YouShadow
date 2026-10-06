#pragma once
#include <memory>
#include <string>
#include <vector>

// 모든 오디오는 mono / 48kHz / float32 로 다룬다.
class AudioEngine {
public:
    static constexpr int kSampleRate = 48000;

    // wav 파일을 메모리에 mono float 로 디코딩한다 (sampleRate 로 리샘플).
    static std::vector<float> loadWav(const std::string& path, int sampleRate = kSampleRate);
    // wav 의 [startMs, endMs) 구간만 읽는다 (seek, 파일 전체를 읽지 않는다). 긴 영상의 한 문장 원음을 꺼낼 때 쓴다.
    static std::vector<float> loadWavSlice(const std::string& path, int startMs, int endMs, int sampleRate = kSampleRate);

    // 선형 보간 리샘플 (STT 입력용 48k → 16k 등)
    static std::vector<float> resample(const std::vector<float>& pcm, int fromRate, int toRate);
    static void saveWav(const std::string& path, const std::vector<float>& pcm);

    // 파형 표시용 피크. binMs 마다 [min, max] 두 값이 interleave 된다.
    // loadPeaks 는 파일을 스트리밍으로 읽어 긴 영상도 메모리를 적게 쓴다.
    static std::vector<float> loadPeaks(const std::string& path, int binMs = 10);
    static std::vector<float> peaksFromPcm(const std::vector<float>& pcm, int binMs = 10);

    // ---- 블로킹 API (CLI 용) ----

    // 구간을 loops 번 재생한다 (반복 사이에 짧은 무음).
    static void play(const std::vector<float>& pcm, int startMs, int endMs, int loops = 1);

    // 원본 구간을 재생하면서 동시에 마이크를 녹음한다 (쉐도잉).
    static std::vector<float> playAndRecord(const std::vector<float>& pcm, int startMs, int endMs,
                                            int tailMs = 700);

    // 마이크만 durationMs 동안 녹음한다.
    static std::vector<float> record(int durationMs);

    static int durationMs(const std::vector<float>& pcm) {
        return (int)(pcm.size() * 1000 / kSampleRate);
    }
};

// ---- 비동기 API (GUI 용) ----

// 마이크 녹음기. start() 후 stop() 까지 계속 녹음한다.
class Recorder {
public:
    Recorder();
    ~Recorder();
    void start();                 // 실패 시 예외
    std::vector<float> stop();    // 녹음된 pcm 반환
    bool active() const;
    float level() const;          // 최근 입력 피크 (0~1), 미터 표시용
    int recordedMs() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// pcm 버퍼를 비동기로 재생한다.
class Player {
public:
    Player();
    ~Player();
    void play(std::vector<float> pcm);  // 이전 재생은 중단
    void stop();
    bool playing() const;
    int positionMs() const;
    void setGain(float gain);           // 1.0 = 원음, 2.0 = 두 배

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

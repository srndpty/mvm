#ifndef MVM_AUDIO_PREVIEW_WASAPI_AUDIO_SINK_H
#define MVM_AUDIO_PREVIEW_WASAPI_AUDIO_SINK_H

#include "media/audio_preview/audio_clock.h"
#include "media/audio_preview/audio_frame_queue.h"
#include "media/audio_preview/runtime_attribution.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct IAudioClient;
struct IAudioClock;
struct IAudioRenderClient;
struct IMMDevice;
struct IMMDeviceEnumerator;
struct SwrContext;
typedef struct tWAVEFORMATEX WAVEFORMATEX;

namespace mvm::audio {

struct WasapiSnapshot {
    bool open = false;
    bool running = false;
    bool joined = true;
    AudioFormatInfo deviceFormat;
    std::uint64_t audioRenderedSamples = 0;
    std::int64_t firstConsumedSample = -1;
    std::int64_t playStartFirstConsumedSample = -1;
    std::int64_t lastConsumedSampleExclusive = -1;
    std::uint64_t endpointPrefillFrames = 0;
    unsigned int endpointBufferFrames = 0;
    std::int64_t endpointFirstMediaSample = -1;
    std::uint64_t endpointStartDevicePosition = 0;
    std::int64_t clockAnchorMediaSample = -1;
    std::uint64_t clockAnchorDevicePosition = 0;
    std::uint64_t deviceFailureCount = 0;
    std::uint64_t audioRenderThreadJoinLeak = 0;
    std::uint64_t audioDeviceReleaseBeforeJoin = 0;
    std::uint64_t audioLifecycleViolation = 0;
    float sessionVolume = 1.0F;
    // 直近に endpoint へ送った PCM の channel ごと peak (linear, 0..1)。
    // UI の polling 間隔で peak を取りこぼさないよう、減衰は render 側で行う。
    float meterPeakLeft = 0.0F;
    float meterPeakRight = 0.0F;
    std::string lastError;
};

// render block ごとの peak 減衰係数。endpoint の 1 block は概ね 10ms なので、
// この係数だと約 -0.9 dB/block で落ちる。値の根拠を呼び出し側へ散らさない。
inline constexpr float kMeterPeakDecayPerBlock = 0.90F;

class WasapiAudioSink final {
public:
    WasapiAudioSink(AudioFrameQueue& queue, AudioMasterClock& clock);
    ~WasapiAudioSink();
    WasapiAudioSink(const WasapiAudioSink&) = delete;
    WasapiAudioSink& operator=(const WasapiAudioSink&) = delete;

    // sessionVolume は Windows の per-process endpoint session volume である。
    // 既定は unity で、その場合は一切設定しない (既存の挙動を変えない)。
    // 非 unity を要求して適用できなかった場合は、黙って全音量で鳴らさず失敗する。
    bool open(std::string& error, float sessionVolume = 1.0F);
    bool play(std::int64_t mediaStartSample, SourceGeneration generation, std::string& error);
    bool pause(std::string& error);
    bool resetForSeek(std::string& error);
    void stop();
    WasapiSnapshot snapshot() const;

    // P3 ATTR-Q1診断専用。呼び出し側がsinkより長く所有する固定POD stateを渡す。
    void setRuntimeAttribution(RuntimeAttributionState* attribution) { attribution_ = attribution; }

    // render loop に実際の device failure を起こさせる test seam。
    // 完成した error を外から渡すのではなく、通常の recordFailure() 経路を通す。
    void injectRenderFaultForTest();
    // pause() を実際に失敗させる test seam。product側が「止められないまま
    // ReadyPaused を公開しない」ことを検査するために使う。
    void injectPauseFaultForTest();
    void injectPlayFaultForTest();
    // play() の pre-roll 直前で決定論的に停止させる test barrier。
    // shutdown/resume の interleaving を再現可能に固定するために使う。
    void armPlayBarrierForTest();
    bool waitPlayBarrierEnteredForTest(int timeoutMs);
    void releasePlayBarrierForTest();

private:
    void renderLoop();
    bool renderAvailable();
    // 直近 block の peak を減衰付きで更新する。sourceScratch_ の先頭から frames 分を見る。
    void updateMeterPeaks(std::int64_t consumedFrames);
    // render loop が止まる経路で peak を確定的に 0 へ落とす。
    void clearMeterPeaks();
    bool prefillEndpoint(std::int64_t mediaStartSample, SourceGeneration generation,
                         std::string& error);
    bool resetClient(std::string& error);
    void recordFailure(const std::string& error);
    void releaseDevice();
    // mutex_ を呼び出し側が保持している前提。open() の失敗経路から使う。
    void releaseDeviceLocked();

    AudioFrameQueue& queue_;
    AudioMasterClock& clock_;
    mutable std::mutex mutex_;
    std::mutex clientMutex_;
    WasapiSnapshot metrics_;
    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* client_ = nullptr;
    IAudioRenderClient* renderClient_ = nullptr;
    IAudioClock* deviceClock_ = nullptr;
    WAVEFORMATEX* mixFormat_ = nullptr;
    SwrContext* outputResampler_ = nullptr;
    void* audioEvent_ = nullptr;
    void* stopEvent_ = nullptr;
    unsigned int bufferFrames_ = 0;
    int sourceScratchSamples_ = 0;
    std::vector<float> sourceScratch_;
    std::thread thread_;
    std::atomic<bool> acceptingCommands_{true};
    std::atomic<bool> threadRunning_{false};
    std::atomic<bool> playing_{false};
    std::atomic<float> meterPeakLeft_{0.0F};
    std::atomic<float> meterPeakRight_{0.0F};
    std::atomic<bool> renderFaultInjected_{false};
    std::atomic<bool> pauseFaultInjected_{false};
    std::atomic<bool> playFaultInjected_{false};
    std::mutex barrierMutex_;
    std::condition_variable barrierChanged_;
    bool playBarrierArmed_ = false;
    bool playBarrierEntered_ = false;
    std::atomic<std::uint64_t> generation_{0};
    bool comInitialized_ = false;
    bool endpointPrefillRequired_ = true;
    std::int64_t nextRequestedSample_ = -1;
    RuntimeAttributionState* attribution_ = nullptr;
    std::array<AudioConsumeTraceEntry, kAudioConsumeTraceCapacity> recentConsumeTrace_{};
    std::size_t recentConsumeTraceCount_ = 0;
    std::size_t recentConsumeTraceNext_ = 0;
};

} // namespace mvm::audio
#endif

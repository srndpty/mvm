#ifndef MVM_AUDIO_PREVIEW_WASAPI_AUDIO_SINK_H
#define MVM_AUDIO_PREVIEW_WASAPI_AUDIO_SINK_H

#include "media/audio_preview/audio_clock.h"
#include "media/audio_preview/audio_frame_queue.h"

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
    std::int64_t endpointFirstMediaSample = -1;
    std::uint64_t endpointStartDevicePosition = 0;
    std::int64_t clockAnchorMediaSample = -1;
    std::uint64_t clockAnchorDevicePosition = 0;
    std::uint64_t deviceFailureCount = 0;
    std::uint64_t audioRenderThreadJoinLeak = 0;
    std::uint64_t audioDeviceReleaseBeforeJoin = 0;
    std::uint64_t audioLifecycleViolation = 0;
    std::string lastError;
};

class WasapiAudioSink final {
public:
    WasapiAudioSink(AudioFrameQueue& queue, AudioMasterClock& clock);
    ~WasapiAudioSink();
    WasapiAudioSink(const WasapiAudioSink&) = delete;
    WasapiAudioSink& operator=(const WasapiAudioSink&) = delete;

    bool open(std::string& error);
    bool play(std::int64_t mediaStartSample, SourceGeneration generation, std::string& error);
    bool pause(std::string& error);
    bool resetForSeek(std::string& error);
    void stop();
    WasapiSnapshot snapshot() const;

private:
    void renderLoop();
    bool renderAvailable();
    bool prefillEndpoint(std::int64_t mediaStartSample, SourceGeneration generation,
                         std::string& error);
    bool resetClient(std::string& error);
    void recordFailure(const std::string& error);
    void releaseDevice();

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
    std::atomic<std::uint64_t> generation_{0};
    bool comInitialized_ = false;
    bool endpointPrefillRequired_ = true;
};

} // namespace mvm::audio
#endif

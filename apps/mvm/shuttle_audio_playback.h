#ifndef MVM_APPS_MVM_SHUTTLE_AUDIO_PLAYBACK_H
#define MVM_APPS_MVM_SHUTTLE_AUDIO_PLAYBACK_H

#include "clip_sample_reader.h"
#include "media/audio_preview/audio_clock.h"
#include "media/audio_preview/wasapi_audio_sink.h"
#include "project/project.h"
#include "shuttle_audio_mix.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mvm::app {

class ShuttleAudioPlayback final {
public:
    ShuttleAudioPlayback();
    ~ShuttleAudioPlayback();
    ShuttleAudioPlayback(const ShuttleAudioPlayback&) = delete;
    ShuttleAudioPlayback& operator=(const ShuttleAudioPlayback&) = delete;

    // 鳴らす clip が 0 件なら失敗する。音声が無いのに WASAPI を開かない。
    bool start(const project::Project& project, int rate, std::int64_t baseFrame, float volume,
               std::string& error);
    void stop();
    std::int64_t elapsedSamples() const;

    std::uint64_t nonSilentSamples() const { return nonSilentSamples_.load(); }

    bool setVolume(float volume, std::string& error) {
        return sink_.setSessionVolume(volume, error);
    }

    audio::WasapiSnapshot sinkSnapshot() const { return sink_.snapshot(); }

    std::string error() const;

private:
    void produce();

    audio::AudioFrameQueue queue_;
    audio::AudioMasterClock clock_;
    audio::WasapiAudioSink sink_;
    ShuttleAudioPlan plan_;
    ClipSampleReaders readers_;
    std::thread producer_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> nonSilentSamples_{0};
    mutable std::mutex errorMutex_;
    std::string error_;
};

} // namespace mvm::app

#endif

#ifndef MVM_APPS_MVM_SHUTTLE_AUDIO_PLAYBACK_H
#define MVM_APPS_MVM_SHUTTLE_AUDIO_PLAYBACK_H

#include "media/audio_preview/audio_clock.h"
#include "media/audio_preview/audio_decode_worker.h"
#include "media/audio_preview/wasapi_audio_sink.h"
#include "project/project.h"
#include "shuttle_audio_mix.h"

#include <atomic>
#include <cstdint>
#include <memory>
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
    // plan_.clips と同じ添字で並ぶ decode 状態。
    struct ClipReader {
        std::unique_ptr<audio::AudioDecodeWorker> worker;
        std::int64_t nextSample = -1;
    };

    bool readSamples(std::size_t clipIndex, std::int64_t first, std::int64_t count,
                     std::vector<float>& pcm, std::string& error);
    void produce();

    audio::AudioFrameQueue queue_;
    audio::AudioMasterClock clock_;
    audio::WasapiAudioSink sink_;
    ShuttleAudioPlan plan_;
    std::vector<ClipReader> readers_;
    std::thread producer_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> nonSilentSamples_{0};
    mutable std::mutex errorMutex_;
    std::string error_;
};

} // namespace mvm::app

#endif

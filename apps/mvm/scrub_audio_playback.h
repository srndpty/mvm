#ifndef MVM_APPS_MVM_SCRUB_AUDIO_PLAYBACK_H
#define MVM_APPS_MVM_SCRUB_AUDIO_PLAYBACK_H

#include "clip_sample_reader.h"
#include "media/audio_preview/audio_clock.h"
#include "media/audio_preview/wasapi_audio_sink.h"
#include "project/project.h"
#include "scrub_audio_grain.h"
#include "shuttle_audio_mix.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace mvm::app {

// ルーラー drag 中の scrub 音声。
//
// AudioFrameQueue は要求位置と chunk の開始 sample が一致しないと読まないので、
// grain を任意の時刻に push すると、一度 starvation しただけで以後ずっと鳴らなくなる。
// そこで pusher thread は無音も含めた連続 sample を浅い queue へ流し続け、
// decode (seek 待ちを含む) は別の grain thread で行って pusher へ受け渡す。
class ScrubAudioPlayback final {
public:
    ScrubAudioPlayback();
    ~ScrubAudioPlayback();
    ScrubAudioPlayback(const ScrubAudioPlayback&) = delete;
    ScrubAudioPlayback& operator=(const ScrubAudioPlayback&) = delete;

    // 鳴らす clip が 0 件なら失敗する。音声が無いのに WASAPI を開かない。
    bool start(const project::Project& project, float volume, std::string& error);
    void stop();
    // drag 位置 (timeline frame)。前回と同じ位置なら新しい grain は鳴らさない。
    void setTarget(std::int64_t frame);

    // endpoint へ渡す前の PCM で数えた値。実際に render されたかは sinkSnapshot で見る。
    std::uint64_t nonSilentSamples() const { return nonSilentSamples_.load(); }

    std::uint64_t grainCount() const { return grains_.publishedCount(); }

    std::uint64_t seekWaitCount() const { return readers_.seekWaitCount(); }

    audio::WasapiSnapshot sinkSnapshot() const { return sink_.snapshot(); }

    void clearMeterClip() { sink_.clearMeterClip(); }

    std::string error() const;

private:
    void pushLoop();
    bool makeGrain(std::int64_t frame, std::vector<float>& pcm, std::string& error);
    void fail(const std::string& error);

    audio::AudioFrameQueue queue_;
    audio::AudioMasterClock clock_;
    audio::WasapiAudioSink sink_;
    ShuttleAudioPlan plan_;
    ClipSampleReaders readers_;
    std::thread pusher_;
    std::atomic<bool> running_{false};
    // play() の pre-roll を満たすまでは深く、再生が始まったら浅く保つ。
    std::atomic<std::int64_t> queueTargetSamples_{0};

    ScrubGrainScheduler grains_;

    std::atomic<std::uint64_t> nonSilentSamples_{0};
    mutable std::mutex errorMutex_;
    std::string error_;
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_SCRUB_AUDIO_PLAYBACK_H

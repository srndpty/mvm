#include "scrub_audio_playback.h"

#include "core/checked_output_timebase.h"

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace mvm::app {
namespace {
constexpr audio::SourceId kOutputSource{1};
constexpr audio::SourceGeneration kOutputGeneration{1};
constexpr audio::AudioResourceEpoch kOutputEpoch{1};
// 10 ms ずつ流す。grain の差し替えがこの粒度で出力へ反映される。
constexpr std::int64_t kBlockSamples = 480;
// 再生開始後に queue へ貯めておく量。scrub の反応の遅れはこれと endpoint buffer で決まる。
constexpr std::int64_t kSteadyQueueSamples = 2880; // 60 ms
// play() は pre-roll を待ち、続く endpoint prefill でさらに消費するので、開始時だけ深く貯める。
constexpr std::int64_t kStartQueueSamples = audio::kAudioPrerollSamples * 2;
// drag を前へ進める間は、この範囲なら seek せずに続きから decode する。
constexpr std::int64_t kForwardSkipSamples = audio::kInternalSampleRate / 2;
} // namespace

ScrubAudioPlayback::ScrubAudioPlayback()
    : queue_(kOutputSource, kOutputGeneration), sink_(queue_, clock_),
      grains_([this](std::int64_t frame, std::vector<float>& pcm, std::string& error) {
          return makeGrain(frame, pcm, error);
      }) {}

ScrubAudioPlayback::~ScrubAudioPlayback() {
    stop();
}

bool ScrubAudioPlayback::start(const project::Project& project, float volume, std::string& error) {
    if (running_) {
        error = "scrub音声はすでに再生中です";
        return false;
    }
    // scrub の grain は等速で鳴らすので rate = 1 で plan を作り、grain
    // ごとに基準位置だけ差し替える。
    if (!planShuttleAudio(project, 1, 0, plan_, error))
        return false;
    if (plan_.clips.empty()) {
        error = "scrub音声で鳴らすaudio clipがありません";
        return false;
    }
    readers_.reset(plan_.clips.size(), kForwardSkipSamples);
    if (!sink_.open(error, volume))
        return false;
    queueTargetSamples_ = kStartQueueSamples;
    running_ = true;
    pusher_ = std::thread(&ScrubAudioPlayback::pushLoop, this);
    grains_.start();
    if (!sink_.play(0, kOutputGeneration, error)) {
        stop();
        return false;
    }
    queueTargetSamples_ = kSteadyQueueSamples;
    return true;
}

void ScrubAudioPlayback::stop() {
    running_ = false;
    grains_.requestStop();
    queue_.stop();
    // mouse を離したら音はすぐ止める。decode 待ちの thread を join するより先に endpoint を止める。
    sink_.stop();
    if (pusher_.joinable())
        pusher_.join();
    grains_.join();
    readers_.clear();
}

void ScrubAudioPlayback::setTarget(std::int64_t frame) {
    grains_.setTarget(frame);
}

std::string ScrubAudioPlayback::error() const {
    {
        std::lock_guard lock(errorMutex_);
        if (!error_.empty())
            return error_;
    }
    return grains_.error();
}

void ScrubAudioPlayback::fail(const std::string& error) {
    std::lock_guard lock(errorMutex_);
    if (error_.empty())
        error_ = error.empty() ? "scrub音声を生成できません" : error;
}

bool ScrubAudioPlayback::makeGrain(std::int64_t frame, std::vector<float>& pcm,
                                   std::string& error) {
    const auto timebase = core::CheckedOutputTimebase::create(
        plan_.timelineFpsNum, plan_.timelineFpsDen, audio::kInternalSampleRate);
    if (!timebase) {
        error = "scrub音声のtimebaseを作成できません";
        return false;
    }
    const auto base = timebase.value().seekTargetSample(frame);
    if (!base || base.value() < 0) {
        error = "scrub位置をsample位置へ換算できません";
        return false;
    }
    ShuttleAudioPlan plan = plan_;
    plan.baseSample = base.value();
    const auto read = [this](std::size_t clipIndex, std::int64_t first, std::int64_t count,
                             std::vector<float>& source, std::string& readError) {
        return readers_.read(plan_.clips[clipIndex], clipIndex, first, count, source, running_,
                             readError);
    };
    if (!mixShuttleBlock(plan, 0, kScrubGrainSamples, read, pcm, error))
        return false;
    applyScrubGrainFade(pcm, kScrubGrainFadeSamples);
    return true;
}

void ScrubAudioPlayback::pushLoop() {
    std::int64_t next = 0;
    while (running_) {
        if (!queue_.waitUntilBelow(queueTargetSamples_.load(), 20))
            continue;
        // 一度でも途切れると sink の要求位置が先へ進み、以後の chunk と一致しなくなる。
        // 黙って無音で続けず、error として controller へ知らせる。
        if (queue_.snapshot().underflowCount > 0) {
            fail("scrub音声の供給が途切れました");
            return;
        }
        auto pcm = std::make_shared<std::vector<float>>(static_cast<std::size_t>(kBlockSamples) *
                                                        audio::kInternalChannels);
        grains_.fill(pcm->data(), kBlockSamples);
        std::uint64_t audible = 0;
        for (const float sample : *pcm) {
            if (std::abs(sample) > 0.00001F)
                ++audible;
        }
        nonSilentSamples_.fetch_add(audible, std::memory_order_relaxed);
        audio::AudioChunk chunk;
        chunk.sourceId = kOutputSource;
        chunk.sourceGeneration = kOutputGeneration;
        chunk.resourceEpoch = kOutputEpoch;
        chunk.startSample = next;
        chunk.sampleCount = kBlockSamples;
        chunk.sampleRate = audio::kInternalSampleRate;
        chunk.channels = audio::kInternalChannels;
        chunk.pcm = std::move(pcm);
        if (queue_.push(std::move(chunk)) != audio::AudioQueuePushResult::Accepted) {
            if (running_)
                fail("scrub音声をqueueへ渡せません");
            return;
        }
        next += kBlockSamples;
    }
}

} // namespace mvm::app

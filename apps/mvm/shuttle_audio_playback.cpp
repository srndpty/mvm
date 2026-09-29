#include "shuttle_audio_playback.h"

#include <algorithm>
#include <cmath>

namespace mvm::app {
namespace {
constexpr audio::SourceId kOutputSource{1};
constexpr audio::SourceGeneration kOutputGeneration{1};
constexpr audio::AudioResourceEpoch kOutputEpoch{1};
constexpr std::int64_t kBlockSamples = 12000;
} // namespace

ShuttleAudioPlayback::ShuttleAudioPlayback()
    : queue_(kOutputSource, kOutputGeneration), sink_(queue_, clock_) {}

ShuttleAudioPlayback::~ShuttleAudioPlayback() {
    stop();
}

bool ShuttleAudioPlayback::start(const project::Project& project, int rate, std::int64_t baseFrame,
                                 float volume, std::string& error) {
    if (running_) {
        error = "シャトル音声はすでに再生中です";
        return false;
    }
    if (!planShuttleAudio(project, rate, baseFrame, plan_, error))
        return false;
    if (plan_.clips.empty()) {
        error = "シャトル音声で鳴らすaudio clipがありません";
        return false;
    }
    // 前進シャトルでは前 block の続きから rate 未満しか離れていないので、seek せずに読む。
    readers_.reset(plan_.clips.size(), std::max(0, plan_.rate));
    if (!sink_.open(error, volume))
        return false;
    running_ = true;
    producer_ = std::thread(&ShuttleAudioPlayback::produce, this);
    if (!sink_.play(0, kOutputGeneration, error)) {
        stop();
        return false;
    }
    return true;
}

void ShuttleAudioPlayback::stop() {
    running_ = false;
    queue_.stop();
    if (producer_.joinable())
        producer_.join();
    sink_.stop();
    readers_.clear();
}

std::int64_t ShuttleAudioPlayback::elapsedSamples() const {
    return clock_.snapshot().mediaSamplePosition;
}

std::string ShuttleAudioPlayback::error() const {
    std::lock_guard lock(errorMutex_);
    return error_;
}

void ShuttleAudioPlayback::produce() {
    std::int64_t next = 0;
    while (running_) {
        if (!queue_.waitUntilBelow(audio::kQueueTargetSamples, 50) ||
            !queue_.waitForSpace(kBlockSamples, 50))
            continue;
        std::vector<float> pcm;
        std::string error;
        const auto read = [this](std::size_t clipIndex, std::int64_t first, std::int64_t count,
                                 std::vector<float>& source, std::string& readError) {
            return readers_.read(plan_.clips[clipIndex], clipIndex, first, count, source,
                                 running_, readError);
        };
        if (!mixShuttleBlock(plan_, next, kBlockSamples, read, pcm, error)) {
            if (running_) {
                std::lock_guard lock(errorMutex_);
                error_ = error.empty() ? "シャトル音声を生成できません" : error;
            }
            break;
        }
        std::uint64_t audible = 0;
        for (const float sample : pcm) {
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
        chunk.pcm = std::make_shared<std::vector<float>>(std::move(pcm));
        if (queue_.push(std::move(chunk)) != audio::AudioQueuePushResult::Accepted)
            break;
        next += kBlockSamples;
    }
}

} // namespace mvm::app

#include "media/audio_preview/audio_frame_queue.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace mvm::audio {

namespace {
double toMs(std::int64_t samples) {
    return static_cast<double>(samples) * 1000.0 / kInternalSampleRate;
}
} // namespace

AudioFrameQueue::AudioFrameQueue(SourceId source, SourceGeneration generation,
                                 std::int64_t hardMaxSamples)
    : source_(source), generation_(generation),
      hardMaxSamples_(std::max<std::int64_t>(1, hardMaxSamples)) {}

AudioQueuePushResult AudioFrameQueue::push(AudioChunk chunk) {
    std::lock_guard lock(mutex_);
    if (stopped_)
        return AudioQueuePushResult::RejectedStopped;
    if (!chunk.valid() || chunk.sourceId != source_) {
        ++metrics_.invalidRejectCount;
        return AudioQueuePushResult::RejectedInvalid;
    }
    if (chunk.sourceGeneration < generation_) {
        ++metrics_.staleGenerationRejectCount;
        return AudioQueuePushResult::RejectedStaleGeneration;
    }
    if (generation_ < chunk.sourceGeneration) {
        ++metrics_.futureGenerationRejectCount;
        return AudioQueuePushResult::RejectedFutureGeneration;
    }
    if (chunk.sampleCount > hardMaxSamples_ - metrics_.queuedSamples) {
        ++metrics_.overflowRejectCount;
        return AudioQueuePushResult::RejectedOverflow;
    }
    metrics_.queuedSamples += chunk.sampleCount;
    ++metrics_.pushCount;
    metrics_.queuedDurationMs = toMs(metrics_.queuedSamples);
    metrics_.highWatermarkMs = std::max(metrics_.highWatermarkMs, metrics_.queuedDurationMs);
    chunks_.push_back(std::move(chunk));
    changed_.notify_all();
    return AudioQueuePushResult::Accepted;
}

bool AudioFrameQueue::waitForSpace(std::int64_t requiredSamples, int timeoutMs) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        return stopped_ || requiredSamples <= hardMaxSamples_ - metrics_.queuedSamples;
    }) && !stopped_;
}

bool AudioFrameQueue::waitUntilBelow(std::int64_t samples, int timeoutMs) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        return stopped_ || metrics_.queuedSamples < samples;
    }) && !stopped_;
}

bool AudioFrameQueue::waitForSamples(std::int64_t requiredSamples, int timeoutMs) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(
               lock, std::chrono::milliseconds(timeoutMs),
               [&] { return stopped_ || metrics_.queuedSamples >= requiredSamples; }) &&
           !stopped_ && metrics_.queuedSamples >= requiredSamples;
}

AudioConsumeResult AudioFrameQueue::consume(float* destination, std::int64_t samples,
                                            SourceGeneration expectedGeneration) {
    AudioConsumeResult result{samples, 0, -1, -1};
    if (!destination || samples <= 0)
        return result;
    std::lock_guard lock(mutex_);
    if (expectedGeneration != generation_)
        return result;
    while (result.audioSamples < samples && !chunks_.empty()) {
        AudioChunk& chunk = chunks_.front();
        if (chunk.sourceGeneration != generation_) {
            metrics_.queuedSamples -= chunk.sampleCount;
            ++metrics_.staleGenerationRejectCount;
            chunks_.pop_front();
            continue;
        }
        const std::int64_t take = std::min(samples - result.audioSamples, chunk.sampleCount);
        if (result.firstSample < 0)
            result.firstSample = chunk.startSample;
        const float* source = chunk.pcm->data() + chunk.offsetSamples * kInternalChannels;
        std::memcpy(destination + result.audioSamples * kInternalChannels, source,
                    static_cast<std::size_t>(take) * kInternalChannels * sizeof(float));
        result.audioSamples += take;
        result.lastSampleExclusive = chunk.startSample + take;
        chunk.startSample += take;
        chunk.offsetSamples += static_cast<std::size_t>(take);
        chunk.sampleCount -= take;
        metrics_.queuedSamples -= take;
        if (chunk.sampleCount == 0) {
            chunks_.pop_front();
            ++metrics_.popCount;
        }
    }
    metrics_.queuedDurationMs = toMs(metrics_.queuedSamples);
    changed_.notify_all();
    return result;
}

bool AudioFrameQueue::setGeneration(SourceGeneration generation) {
    std::lock_guard lock(mutex_);
    if (generation < generation_)
        return false;
    if (generation == generation_)
        return true;
    generation_ = generation;
    chunks_.clear();
    metrics_.queuedSamples = 0;
    metrics_.queuedDurationMs = 0.0;
    changed_.notify_all();
    return true;
}

void AudioFrameQueue::noteUnderflow(std::int64_t samples) {
    if (samples <= 0)
        return;
    std::lock_guard lock(mutex_);
    ++metrics_.underflowCount;
    metrics_.underflowSamples += static_cast<std::uint64_t>(samples);
}

void AudioFrameQueue::stop() {
    std::lock_guard lock(mutex_);
    stopped_ = true;
    chunks_.clear();
    metrics_.queuedSamples = 0;
    metrics_.queuedDurationMs = 0.0;
    changed_.notify_all();
}

void AudioFrameQueue::restart() {
    std::lock_guard lock(mutex_);
    stopped_ = false;
    chunks_.clear();
    metrics_.queuedSamples = 0;
    metrics_.queuedDurationMs = 0.0;
    changed_.notify_all();
}

AudioQueueSnapshot AudioFrameQueue::snapshot() const {
    std::lock_guard lock(mutex_);
    return metrics_;
}

SourceGeneration AudioFrameQueue::generation() const {
    std::lock_guard lock(mutex_);
    return generation_;
}

} // namespace mvm::audio

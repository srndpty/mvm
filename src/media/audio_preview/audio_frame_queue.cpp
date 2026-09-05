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
    if (endOfStreamKnown_) {
        ++metrics_.invalidRejectCount;
        return AudioQueuePushResult::RejectedInvalid;
    }
    if (chunk.sampleCount > hardMaxSamples_ - metrics_.queuedSamples) {
        ++metrics_.overflowRejectCount;
        return AudioQueuePushResult::RejectedOverflow;
    }
    metrics_.queuedSamples += chunk.sampleCount;
    ++metrics_.pushCount;
    metrics_.observedSampleRate = chunk.sampleRate;
    metrics_.observedChannels = chunk.channels;
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

AudioConsumeResult AudioFrameQueue::consume(float* destination, std::int64_t requestedSampleStart,
                                            std::int64_t samples,
                                            SourceGeneration expectedGeneration) {
    AudioConsumeResult result;
    result.requestedSamples = samples;
    if (!destination || requestedSampleStart < 0 || samples <= 0)
        return result;
    std::lock_guard lock(mutex_);
    result.queuedSamplesBeforeConsume = metrics_.queuedSamples;
    result.queuedSamplesAfterConsume = metrics_.queuedSamples;
    if (expectedGeneration != generation_) {
        result.silenceSamples = samples;
        result.shortageKind = AudioShortageKind::Starvation;
        return result;
    }
    for (auto chunk = chunks_.rbegin(); chunk != chunks_.rend(); ++chunk) {
        if (chunk->sourceGeneration == generation_) {
            result.queueLastAvailableSampleExclusive = chunk->startSample + chunk->sampleCount;
            break;
        }
    }
    bool continuousFromRequestedStart = true;
    while (result.audioSamples < samples && !chunks_.empty()) {
        AudioChunk& chunk = chunks_.front();
        if (chunk.sourceGeneration != generation_) {
            metrics_.queuedSamples -= chunk.sampleCount;
            ++metrics_.staleGenerationRejectCount;
            chunks_.pop_front();
            continue;
        }
        if (chunk.startSample != requestedSampleStart + result.audioSamples) {
            continuousFromRequestedStart = false;
            break;
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
    result.queuedSamplesAfterConsume = metrics_.queuedSamples;
    result.silenceSamples = samples - result.audioSamples;
    if (result.silenceSamples > 0) {
        const std::int64_t requestedEnd = requestedSampleStart + samples;
        const bool beginsAtOrAfterEnd = endOfStreamKnown_ &&
                                        requestedSampleStart >= endOfStreamSampleExclusive_ &&
                                        result.audioSamples == 0 && metrics_.queuedSamples == 0;
        const bool continuouslyReachesEnd =
            endOfStreamKnown_ && requestedSampleStart < endOfStreamSampleExclusive_ &&
            requestedEnd > endOfStreamSampleExclusive_ && continuousFromRequestedStart &&
            result.firstSample == requestedSampleStart &&
            result.lastSampleExclusive == endOfStreamSampleExclusive_ &&
            result.audioSamples == endOfStreamSampleExclusive_ - requestedSampleStart;
        if (beginsAtOrAfterEnd || continuouslyReachesEnd) {
            result.shortageKind = AudioShortageKind::TerminalEof;
            result.terminalEndSampleExclusive = endOfStreamSampleExclusive_;
            ++metrics_.terminalEofSilenceCallbackCount;
            metrics_.terminalEofSilenceSamples += static_cast<std::uint64_t>(result.silenceSamples);
            if (metrics_.firstTerminalEofRequestedStart < 0) {
                metrics_.firstTerminalEofRequestedStart = requestedSampleStart;
                metrics_.firstTerminalEofRequestedCount = samples;
                metrics_.firstTerminalEofAudioSamples = result.audioSamples;
                metrics_.firstTerminalEofSilenceSamples = result.silenceSamples;
                metrics_.firstTerminalEofEndSampleExclusive = endOfStreamSampleExclusive_;
                metrics_.firstTerminalEofGeneration = generation_.value;
            }
        } else {
            result.shortageKind = AudioShortageKind::Starvation;
        }
    }
    metrics_.queuedDurationMs = toMs(metrics_.queuedSamples);
    changed_.notify_all();
    return result;
}

bool AudioFrameQueue::markEndOfStream(SourceGeneration generation,
                                      std::int64_t endSampleExclusive) {
    std::lock_guard lock(mutex_);
    if (generation != generation_ || endSampleExclusive < 0)
        return false;
    for (const auto& chunk : chunks_) {
        if (chunk.sourceGeneration != generation_ ||
            chunk.startSample + chunk.sampleCount > endSampleExclusive)
            return false;
    }
    if (endOfStreamKnown_)
        return endOfStreamSampleExclusive_ == endSampleExclusive;
    endOfStreamKnown_ = true;
    endOfStreamSampleExclusive_ = endSampleExclusive;
    metrics_.endOfStreamKnown = true;
    metrics_.endOfStreamSampleExclusive = endSampleExclusive;
    metrics_.endOfStreamGeneration = generation.value;
    changed_.notify_all();
    return true;
}

bool AudioFrameQueue::setGeneration(SourceGeneration generation) {
    std::lock_guard lock(mutex_);
    if (generation < generation_)
        return false;
    if (generation == generation_)
        return true;
    generation_ = generation;
    endOfStreamKnown_ = false;
    endOfStreamSampleExclusive_ = -1;
    metrics_.endOfStreamKnown = false;
    metrics_.endOfStreamSampleExclusive = -1;
    metrics_.endOfStreamGeneration = 0;
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

void AudioFrameQueue::noteSampleAddressOverflow() {
    std::lock_guard lock(mutex_);
    ++metrics_.sampleAddressOverflowCount;
}

void AudioFrameQueue::stop() {
    std::lock_guard lock(mutex_);
    stopped_ = true;
    chunks_.clear();
    metrics_.queuedSamples = 0;
    metrics_.queuedDurationMs = 0.0;
    endOfStreamKnown_ = false;
    endOfStreamSampleExclusive_ = -1;
    metrics_.endOfStreamKnown = false;
    metrics_.endOfStreamSampleExclusive = -1;
    metrics_.endOfStreamGeneration = 0;
    changed_.notify_all();
}

void AudioFrameQueue::restart() {
    std::lock_guard lock(mutex_);
    stopped_ = false;
    chunks_.clear();
    metrics_.queuedSamples = 0;
    metrics_.queuedDurationMs = 0.0;
    endOfStreamKnown_ = false;
    endOfStreamSampleExclusive_ = -1;
    metrics_.endOfStreamKnown = false;
    metrics_.endOfStreamSampleExclusive = -1;
    metrics_.endOfStreamGeneration = 0;
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

#ifndef MVM_AUDIO_PREVIEW_AUDIO_FRAME_QUEUE_H
#define MVM_AUDIO_PREVIEW_AUDIO_FRAME_QUEUE_H

#include "media/audio_preview/audio_types.h"

#include <condition_variable>
#include <deque>
#include <mutex>

namespace mvm::audio {

enum class AudioQueuePushResult {
    Accepted,
    RejectedInvalid,
    RejectedOverflow,
    RejectedStaleGeneration,
    RejectedFutureGeneration,
    RejectedStopped
};

struct AudioQueueSnapshot {
    std::uint64_t pushCount = 0;
    std::uint64_t popCount = 0;
    std::int64_t queuedSamples = 0;
    double queuedDurationMs = 0.0;
    double highWatermarkMs = 0.0;
    std::uint64_t underflowCount = 0;
    std::uint64_t underflowSamples = 0;
    std::uint64_t overflowRejectCount = 0;
    std::uint64_t staleGenerationRejectCount = 0;
    std::uint64_t futureGenerationRejectCount = 0;
    std::uint64_t invalidRejectCount = 0;
};

struct AudioConsumeResult {
    std::int64_t requestedSamples = 0;
    std::int64_t audioSamples = 0;
    std::int64_t firstSample = -1;
    std::int64_t lastSampleExclusive = -1;
};

class AudioFrameQueue final {
public:
    explicit AudioFrameQueue(SourceId source, SourceGeneration generation,
                             std::int64_t hardMaxSamples = kQueueHardMaxSamples);

    AudioQueuePushResult push(AudioChunk chunk);
    bool waitForSpace(std::int64_t requiredSamples, int timeoutMs);
    bool waitUntilBelow(std::int64_t samples, int timeoutMs);
    bool waitForSamples(std::int64_t requiredSamples, int timeoutMs);
    AudioConsumeResult consume(float* destination, std::int64_t samples,
                               SourceGeneration expectedGeneration);
    bool setGeneration(SourceGeneration generation);
    void noteUnderflow(std::int64_t samples);
    void stop();
    void restart();
    AudioQueueSnapshot snapshot() const;
    SourceGeneration generation() const;

private:
    SourceId source_{};
    SourceGeneration generation_{};
    std::int64_t hardMaxSamples_ = kQueueHardMaxSamples;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<AudioChunk> chunks_;
    AudioQueueSnapshot metrics_;
    bool stopped_ = false;
};

} // namespace mvm::audio
#endif

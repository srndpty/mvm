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

enum class AudioShortageKind { None, Starvation, TerminalEof };

struct AudioQueueSnapshot {
    std::uint64_t pushCount = 0;
    std::uint64_t popCount = 0;
    std::int64_t queuedSamples = 0;
    double queuedDurationMs = 0.0;
    double highWatermarkMs = 0.0;
    std::uint64_t underflowCount = 0;
    std::uint64_t underflowSamples = 0;
    // queue starvation とは別枠。sample address が int64 で表現できず consume を諦めた回数。
    std::uint64_t sampleAddressOverflowCount = 0;
    std::uint64_t terminalEofSilenceCallbackCount = 0;
    std::uint64_t terminalEofSilenceSamples = 0;
    bool endOfStreamKnown = false;
    std::int64_t endOfStreamSampleExclusive = -1;
    std::uint64_t endOfStreamGeneration = 0;
    std::int64_t firstTerminalEofRequestedStart = -1;
    std::int64_t firstTerminalEofRequestedCount = 0;
    std::int64_t firstTerminalEofAudioSamples = 0;
    std::int64_t firstTerminalEofSilenceSamples = 0;
    std::int64_t firstTerminalEofEndSampleExclusive = -1;
    std::uint64_t firstTerminalEofGeneration = 0;
    std::uint64_t overflowRejectCount = 0;
    std::uint64_t staleGenerationRejectCount = 0;
    std::uint64_t futureGenerationRejectCount = 0;
    std::uint64_t invalidRejectCount = 0;
    // 実際に受理した chunk の PCM domain (未受理なら 0)。期待値ではなく観測値。
    int observedSampleRate = 0;
    int observedChannels = 0;
};

struct AudioConsumeResult {
    std::int64_t requestedSamples = 0;
    std::int64_t audioSamples = 0;
    std::int64_t firstSample = -1;
    std::int64_t lastSampleExclusive = -1;
    std::int64_t queuedSamplesBeforeConsume = 0;
    std::int64_t queuedSamplesAfterConsume = 0;
    std::int64_t queueLastAvailableSampleExclusive = -1;
    std::int64_t silenceSamples = 0;
    AudioShortageKind shortageKind = AudioShortageKind::None;
    std::int64_t terminalEndSampleExclusive = -1;
};

class AudioFrameQueue final {
public:
    explicit AudioFrameQueue(SourceId source, SourceGeneration generation,
                             std::int64_t hardMaxSamples = kQueueHardMaxSamples);

    AudioQueuePushResult push(AudioChunk chunk);
    bool waitForSpace(std::int64_t requiredSamples, int timeoutMs);
    bool waitUntilBelow(std::int64_t samples, int timeoutMs);
    bool waitForSamples(std::int64_t requiredSamples, int timeoutMs);
    AudioConsumeResult consume(float* destination, std::int64_t requestedSampleStart,
                               std::int64_t samples, SourceGeneration expectedGeneration);
    bool markEndOfStream(SourceGeneration generation, std::int64_t endSampleExclusive);
    bool setGeneration(SourceGeneration generation);
    void noteUnderflow(std::int64_t samples);
    void noteSampleAddressOverflow();
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
    bool endOfStreamKnown_ = false;
    std::int64_t endOfStreamSampleExclusive_ = -1;
    bool stopped_ = false;
};

} // namespace mvm::audio
#endif

#ifndef MVM_AUDIO_PREVIEW_RUNTIME_ATTRIBUTION_H
#define MVM_AUDIO_PREVIEW_RUNTIME_ATTRIBUTION_H

#include <atomic>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace mvm::audio {

enum class RuntimeAttributionMode : int { Unknown = 0, Playback = 1, Seek = 2, PauseResume = 3 };

enum class ClockRegressionSite : int {
    Unknown = 0,
    SchedulerProjectionInvalid = 1,
    SchedulerDecision = 2,
    DisplayProjectionInvalid = 3
};

struct RuntimeAttributionContextSnapshot {
    RuntimeAttributionMode mode = RuntimeAttributionMode::Unknown;
    int engineState = 0;
    std::int64_t seekOrdinal = 0;
    std::int64_t currentSeekTarget = -1;
    bool seekPending = false;
    bool decodeReady = false;
    bool requestedFramePresented = false;
    std::int64_t pauseResumeCycleOrdinal = 0;
    std::int64_t resumeStartQpc = 0;
};

class RuntimeAttributionContext final {
public:
    RuntimeAttributionContextSnapshot snapshot() const {
        return {static_cast<RuntimeAttributionMode>(mode.load(std::memory_order_acquire)),
                engineState.load(std::memory_order_acquire),
                seekOrdinal.load(std::memory_order_acquire),
                currentSeekTarget.load(std::memory_order_acquire),
                seekPending.load(std::memory_order_acquire),
                decodeReady.load(std::memory_order_acquire),
                requestedFramePresented.load(std::memory_order_acquire),
                pauseResumeCycleOrdinal.load(std::memory_order_acquire),
                resumeStartQpc.load(std::memory_order_acquire)};
    }

    std::atomic<int> mode{static_cast<int>(RuntimeAttributionMode::Unknown)};
    std::atomic<int> engineState{0};
    std::atomic<std::int64_t> seekOrdinal{0};
    std::atomic<std::int64_t> currentSeekTarget{-1};
    std::atomic<bool> seekPending{false};
    std::atomic<bool> decodeReady{false};
    std::atomic<bool> requestedFramePresented{false};
    std::atomic<std::int64_t> pauseResumeCycleOrdinal{0};
    std::atomic<std::int64_t> resumeStartQpc{0};
};

struct AudioUnderflowFirstSnapshot {
    std::int64_t occurrenceQpc = 0;
    RuntimeAttributionContextSnapshot context;
    std::int64_t requestedSampleStart = -1;
    std::int64_t requestedSampleCount = 0;
    std::int64_t queuedSamplesBeforeConsume = 0;
    std::int64_t actuallyConsumedSamples = 0;
    std::int64_t queuedSamplesAfterConsume = 0;
    std::uint64_t sourceGeneration = 0;
    std::int64_t audioMasterSamplePosition = -1;
};

struct ClockRegressionFirstSnapshot {
    ClockRegressionSite site = ClockRegressionSite::Unknown;
    std::int64_t occurrenceQpc = 0;
    RuntimeAttributionContextSnapshot context;
    std::int64_t previousFrame = -1;
    std::int64_t candidateFrame = -1;
    std::int64_t rawAudioMasterSamplePosition = -1;
    std::int64_t schedulerTargetFrame = -1;
    std::int64_t currentDisplayedFrame = -1;
    std::uint64_t sourceGeneration = 0;
};

template<class Snapshot>
class FirstFailureCapture final {
    static_assert(std::is_trivially_copyable_v<Snapshot>);

public:
    bool capture(const Snapshot& value) {
        int expected = 0;
        if (!state_.compare_exchange_strong(expected, 1, std::memory_order_acq_rel,
                                            std::memory_order_acquire))
            return false;
        value_ = value;
        state_.store(2, std::memory_order_release);
        return true;
    }

    std::optional<Snapshot> snapshot() const {
        if (state_.load(std::memory_order_acquire) != 2)
            return std::nullopt;
        return value_;
    }

private:
    // 0=未記録、1=writerがPODをコピー中、2=publish済み。
    std::atomic<int> state_{0};
    Snapshot value_{};
};

struct RuntimeAttributionState {
    RuntimeAttributionContext context;
    FirstFailureCapture<AudioUnderflowFirstSnapshot> firstAudioUnderflow;
    FirstFailureCapture<ClockRegressionFirstSnapshot> firstClockRegression;
};

static_assert(std::is_trivially_copyable_v<RuntimeAttributionContextSnapshot>);
static_assert(std::is_trivially_copyable_v<AudioUnderflowFirstSnapshot>);
static_assert(std::is_trivially_copyable_v<ClockRegressionFirstSnapshot>);

} // namespace mvm::audio
#endif

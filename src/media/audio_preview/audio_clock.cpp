#include "media/audio_preview/audio_clock.h"

#include <limits>

namespace mvm::audio {

bool AudioMasterClock::start(const ClockAnchor& anchor) {
    std::lock_guard lock(mutex_);
    if (anchor.deviceFrequency == 0 || anchor.generation.value == 0 || anchor.mediaStartSample < 0)
        return false;
    anchor_ = anchor;
    snapshot_.devicePosition = anchor.deviceStartPosition;
    snapshot_.deviceFrequency = anchor.deviceFrequency;
    snapshot_.qpcAtSample = anchor.qpcStart;
    snapshot_.mediaSamplePosition = anchor.mediaStartSample;
    snapshot_.mediaTimeSeconds = static_cast<double>(anchor.mediaStartSample) / kInternalSampleRate;
    snapshot_.running = true;
    snapshot_.generation = anchor.generation;
    return true;
}

bool AudioMasterClock::update(std::uint64_t devicePosition, std::int64_t qpcAtSample,
                              SourceGeneration generation) {
    std::lock_guard lock(mutex_);
    if (!snapshot_.running)
        return false;
    if (generation != anchor_.generation) {
        ++snapshot_.clockGenerationMismatchCount;
        return false;
    }
    if (devicePosition < anchor_.deviceStartPosition) {
        ++snapshot_.clockRegressionCount;
        return false;
    }
    const std::uint64_t delta = devicePosition - anchor_.deviceStartPosition;
    // quotient/remainder に分け、delta * sampleRate の overflow を避ける。
    const std::uint64_t whole = delta / anchor_.deviceFrequency;
    const std::uint64_t remainder = delta % anchor_.deviceFrequency;
    if (whole > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() /
                                           kInternalSampleRate)) {
        ++snapshot_.clockRegressionCount;
        return false;
    }
    const std::int64_t mapped =
        anchor_.mediaStartSample + static_cast<std::int64_t>(whole * kInternalSampleRate) +
        static_cast<std::int64_t>((remainder * kInternalSampleRate) / anchor_.deviceFrequency);
    if (mapped < snapshot_.mediaSamplePosition) {
        ++snapshot_.clockRegressionCount;
        return false;
    }
    snapshot_.devicePosition = devicePosition;
    snapshot_.qpcAtSample = qpcAtSample;
    snapshot_.mediaSamplePosition = mapped;
    snapshot_.mediaTimeSeconds = static_cast<double>(mapped) / kInternalSampleRate;
    return true;
}

void AudioMasterClock::pause() {
    std::lock_guard lock(mutex_);
    snapshot_.running = false;
}

void AudioMasterClock::stop() {
    std::lock_guard lock(mutex_);
    snapshot_.running = false;
}

void AudioMasterClock::noteQueryFailure() {
    std::lock_guard lock(mutex_);
    ++snapshot_.clockQueryFailureCount;
}

AudioClockSnapshot AudioMasterClock::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

} // namespace mvm::audio

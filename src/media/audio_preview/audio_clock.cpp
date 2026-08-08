#include "media/audio_preview/audio_clock.h"

#include <limits>

namespace mvm::audio {

namespace {
constexpr std::uint64_t kHundredNsPerSecond = 10'000'000;

// value < divisor を前提に floor(value * multiplier / divisor) を求める。
// residue を divisor 未満に保ったまま加算するため、中間積は overflow しない。
std::uint64_t multiplyDivideFloor(std::uint64_t value, std::uint64_t multiplier,
                                  std::uint64_t divisor) {
    std::uint64_t quotient = 0;
    std::uint64_t remainder = 0;
    std::uint64_t termQuotient = value / divisor;
    std::uint64_t termRemainder = value % divisor;
    while (multiplier != 0) {
        if ((multiplier & 1U) != 0) {
            quotient += termQuotient;
            if (remainder >= divisor - termRemainder) {
                remainder -= divisor - termRemainder;
                ++quotient;
            } else {
                remainder += termRemainder;
            }
        }
        multiplier >>= 1U;
        if (multiplier == 0)
            break;
        termQuotient *= 2;
        if (termRemainder >= divisor - termRemainder) {
            termRemainder -= divisor - termRemainder;
            ++termQuotient;
        } else {
            termRemainder *= 2;
        }
    }
    return quotient;
}
} // namespace

bool qpcTicksTo100ns(QpcTicks ticks, std::uint64_t frequency, Qpc100ns& converted) {
    if (frequency == 0)
        return false;
    const std::uint64_t whole = ticks.value / frequency;
    const std::uint64_t remainder = ticks.value % frequency;
    if (whole > std::numeric_limits<std::uint64_t>::max() / kHundredNsPerSecond)
        return false;
    const std::uint64_t base = whole * kHundredNsPerSecond;
    const std::uint64_t fraction = multiplyDivideFloor(remainder, kHundredNsPerSecond, frequency);
    if (base > std::numeric_limits<std::uint64_t>::max() - fraction)
        return false;
    converted.value = base + fraction;
    return true;
}

AudioClockProjection projectAtQpc100ns(const AudioClockSnapshot& snapshot, Qpc100ns now,
                                       SourceGeneration expectedGeneration) {
    AudioClockProjection result;
    result.generation = snapshot.generation;
    if (!snapshot.running || snapshot.deviceFrequency == 0 || expectedGeneration.value == 0 ||
        snapshot.generation != expectedGeneration || now.value < snapshot.qpcPosition100ns.value ||
        snapshot.mediaSamplePosition < 0)
        return result;
    const std::uint64_t elapsed100ns = now.value - snapshot.qpcPosition100ns.value;
    const std::uint64_t whole = elapsed100ns / kHundredNsPerSecond;
    const std::uint64_t remainder = elapsed100ns % kHundredNsPerSecond;
    if (whole >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / kInternalSampleRate))
        return result;
    const std::uint64_t extrapolated =
        whole * kInternalSampleRate + (remainder * kInternalSampleRate) / kHundredNsPerSecond;
    if (extrapolated > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        snapshot.mediaSamplePosition >
            std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(extrapolated))
        return result;
    result.valid = true;
    result.extrapolatedSamples = static_cast<std::int64_t>(extrapolated);
    result.mediaSample = snapshot.mediaSamplePosition + result.extrapolatedSamples;
    result.mediaTimeSeconds = static_cast<double>(result.mediaSample) / kInternalSampleRate;
    return result;
}

bool AudioMasterClock::start(const ClockAnchor& anchor) {
    std::lock_guard lock(mutex_);
    if (anchor.deviceFrequency == 0 || anchor.generation.value == 0 || anchor.mediaStartSample < 0)
        return false;
    anchor_ = anchor;
    snapshot_.devicePosition = anchor.deviceStartPosition;
    snapshot_.deviceFrequency = anchor.deviceFrequency;
    snapshot_.qpcPosition100ns = anchor.qpcPosition100ns;
    snapshot_.mediaSamplePosition = anchor.mediaStartSample;
    snapshot_.mediaTimeSeconds = static_cast<double>(anchor.mediaStartSample) / kInternalSampleRate;
    snapshot_.running = true;
    snapshot_.generation = anchor.generation;
    return true;
}

bool AudioMasterClock::update(std::uint64_t devicePosition, Qpc100ns qpcPosition100ns,
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
    snapshot_.qpcPosition100ns = qpcPosition100ns;
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

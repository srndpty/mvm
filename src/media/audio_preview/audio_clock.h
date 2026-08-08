#ifndef MVM_AUDIO_PREVIEW_AUDIO_CLOCK_H
#define MVM_AUDIO_PREVIEW_AUDIO_CLOCK_H

#include "media/audio_preview/audio_types.h"

#include <mutex>

namespace mvm::audio {

struct ClockAnchor {
    std::int64_t mediaStartSample = 0;
    std::uint64_t deviceStartPosition = 0;
    std::int64_t qpcStart = 0;
    std::uint64_t deviceFrequency = 0;
    SourceGeneration generation{};
};

struct AudioClockSnapshot {
    std::uint64_t devicePosition = 0;
    std::uint64_t deviceFrequency = 0;
    std::int64_t qpcAtSample = 0;
    std::int64_t mediaSamplePosition = 0;
    double mediaTimeSeconds = 0.0;
    bool running = false;
    SourceGeneration generation{};
    std::uint64_t clockRegressionCount = 0;
    std::uint64_t clockGenerationMismatchCount = 0;
    std::uint64_t clockQueryFailureCount = 0;
};

class AudioMasterClock final {
public:
    bool start(const ClockAnchor& anchor);
    bool update(std::uint64_t devicePosition, std::int64_t qpcAtSample,
                SourceGeneration generation);
    void pause();
    void stop();
    void noteQueryFailure();
    AudioClockSnapshot snapshot() const;

private:
    mutable std::mutex mutex_;
    ClockAnchor anchor_{};
    AudioClockSnapshot snapshot_{};
};

} // namespace mvm::audio
#endif

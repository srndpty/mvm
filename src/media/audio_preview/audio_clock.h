#ifndef MVM_AUDIO_PREVIEW_AUDIO_CLOCK_H
#define MVM_AUDIO_PREVIEW_AUDIO_CLOCK_H

#include "media/audio_preview/audio_types.h"

#include <cstdint>
#include <mutex>

namespace mvm::audio {

struct QpcTicks {
    std::uint64_t value = 0;
};

struct Qpc100ns {
    std::uint64_t value = 0;
};

bool qpcTicksTo100ns(QpcTicks ticks, std::uint64_t frequency, Qpc100ns& converted);

struct ClockAnchor {
    std::int64_t mediaStartSample = 0;
    std::uint64_t deviceStartPosition = 0;
    Qpc100ns qpcPosition100ns{};
    std::uint64_t deviceFrequency = 0;
    SourceGeneration generation{};
};

struct AudioClockSnapshot {
    std::uint64_t devicePosition = 0;
    std::uint64_t deviceFrequency = 0;
    Qpc100ns qpcPosition100ns{};
    std::int64_t mediaSamplePosition = 0;
    double mediaTimeSeconds = 0.0;
    bool running = false;
    SourceGeneration generation{};
    std::uint64_t clockRegressionCount = 0;
    std::uint64_t clockGenerationMismatchCount = 0;
    std::uint64_t clockQueryFailureCount = 0;
};

struct AudioClockProjection {
    bool valid = false;
    SourceGeneration generation{};
    std::int64_t mediaSample = -1;
    double mediaTimeSeconds = 0.0;
    std::int64_t extrapolatedSamples = 0;
};

AudioClockProjection projectAtQpc100ns(const AudioClockSnapshot& snapshot, Qpc100ns now,
                                       SourceGeneration expectedGeneration);

class AudioMasterClock final {
public:
    bool start(const ClockAnchor& anchor);
    bool update(std::uint64_t devicePosition, Qpc100ns qpcPosition100ns,
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

#include "media/gpu_preview/composition_schedule.h"

#include <algorithm>

namespace mvm::gpu {

std::optional<CompositionSchedule>
CompositionSchedule::create(std::vector<CompositionScheduleEntry> entries) {
    if (entries.empty() || entries.front().boundaryOutputFrame != 0)
        return std::nullopt;

    long long previous = -1;
    for (const auto& entry : entries) {
        if (entry.boundaryOutputFrame < 0 || entry.boundaryOutputFrame <= previous ||
            !entry.state.valid())
            return std::nullopt;
        previous = entry.boundaryOutputFrame;
    }
    return CompositionSchedule(std::move(entries));
}

std::optional<CompositionStateId> CompositionSchedule::resolve(long long outputFrameNumber) const {
    if (outputFrameNumber < 0 || entries_.empty())
        return std::nullopt;
    const auto after = std::upper_bound(entries_.begin(), entries_.end(), outputFrameNumber,
                                        [](long long frame, const CompositionScheduleEntry& entry) {
                                            return frame < entry.boundaryOutputFrame;
                                        });
    if (after == entries_.begin())
        return std::nullopt;
    return std::prev(after)->state;
}

} // namespace mvm::gpu

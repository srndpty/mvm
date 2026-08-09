#ifndef MVM_GPU_PREVIEW_COMPOSITION_SCHEDULE_H
#define MVM_GPU_PREVIEW_COMPOSITION_SCHEDULE_H

#include "media/gpu_preview/gpu_frame.h"

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace mvm::gpu {

struct CompositionScheduleEntry {
    long long boundaryOutputFrame = -1;
    CompositionStateId state{};
};

// playback 前に検証済みの entry だけを保持する immutable value。
// 測定区間の終端は controller の責務なので、末尾以後は最後の state を返す。
class CompositionSchedule {
public:
    static std::optional<CompositionSchedule> create(std::vector<CompositionScheduleEntry> entries);

    std::optional<CompositionStateId> resolve(long long outputFrameNumber) const;

    size_t size() const { return entries_.size(); }

private:
    explicit CompositionSchedule(std::vector<CompositionScheduleEntry> entries)
        : entries_(std::move(entries)) {}

    std::vector<CompositionScheduleEntry> entries_;
};

} // namespace mvm::gpu

#endif

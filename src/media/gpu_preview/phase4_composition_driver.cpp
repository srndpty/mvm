#include "media/gpu_preview/phase4_composition_driver.h"

#include "media/gpu_preview/phase4_composition_catalog.h"

#include <utility>

namespace mvm::gpu {

Phase4CompositionDriver::Phase4CompositionDriver(CompositionSchedule schedule,
                                                 std::vector<SourceId> watchedSources)
    : schedule_(std::move(schedule)), watchedSources_(std::move(watchedSources)) {}

Phase4DriveResult Phase4CompositionDriver::onTargetFrame(CompositorCoordinator& coordinator,
                                                         long long outputFrame) {
    const auto resolved = schedule_.resolve(outputFrame);
    if (!resolved) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++counters_.unresolvedFrameCount;
        return Phase4DriveResult::Unresolved;
    }

    // scheduleとlayoutはworkload開始前にimmutable publish済みであり、呼び出しも
    // render threadだけである。同一segmentの各frameでlayout生成とcoordinatorの
    // 複数mutex取得を繰り返さず、resolve/noop会計だけを進める。
    if (lastResolvedState_ && *lastResolvedState_ == *resolved) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++counters_.resolveCount;
        ++counters_.noopCount;
        return Phase4DriveResult::NoOp;
    }

    auto layout = phase4CanonicalLayout(*resolved);
    if (layout.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++counters_.resolveCount;
        ++counters_.rejectCount;
        return Phase4DriveResult::Rejected;
    }

    std::vector<SourceGeneration> before;
    before.reserve(watchedSources_.size());
    for (const auto source : watchedSources_)
        before.push_back(coordinator.sourceGeneration(source));

    const auto epochBefore = coordinator.compositionEpoch();
    const auto adoption = coordinator.adoptCompositionSnapshot(*resolved, std::move(layout));
    const auto epochAfter = coordinator.compositionEpoch();

    long long generationChanges = 0;
    for (size_t i = 0; i < watchedSources_.size(); ++i)
        if (coordinator.sourceGeneration(watchedSources_[i]) != before[i])
            ++generationChanges;

    std::lock_guard<std::mutex> lock(mutex_);
    ++counters_.resolveCount;
    if (epochAfter.value > epochBefore.value)
        counters_.epochIncrementCount +=
            static_cast<long long>(epochAfter.value - epochBefore.value);
    counters_.sourceGenerationChangeDueToLayoutCount += generationChanges;

    switch (adoption) {
    case CompositionStateAdoptionResult::Adopted:
        lastResolvedState_ = *resolved;
        ++counters_.adoptionCount;
        return Phase4DriveResult::Adopted;
    case CompositionStateAdoptionResult::NoOp:
        lastResolvedState_ = *resolved;
        ++counters_.noopCount;
        return Phase4DriveResult::NoOp;
    case CompositionStateAdoptionResult::Rejected:
        break;
    }
    ++counters_.rejectCount;
    return Phase4DriveResult::Rejected;
}

Phase4DriverCounters Phase4CompositionDriver::counters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return counters_;
}

} // namespace mvm::gpu

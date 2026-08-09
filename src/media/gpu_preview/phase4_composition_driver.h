/*
 * Phase 4 / B: audio-master が決めた target output frame に対する
 * schedule resolve -> atomic composition snapshot adoption。
 *
 * 呼ぶのは exact pair を取る**前**だけである。compose 後や display 後に
 * state を変えると、既に表示した frame へ後付けの state が付く。
 *
 * updateLayout() + adoptCompositionState() の 2 段階は使わない。
 * 途中状態で compose されると layout と state が食い違う。
 *
 * Qt / audio へは依存しない。immutable schedule と counter だけを持つ。
 */
#ifndef MVM_GPU_PREVIEW_PHASE4_COMPOSITION_DRIVER_H
#define MVM_GPU_PREVIEW_PHASE4_COMPOSITION_DRIVER_H

#include "media/gpu_preview/composition_schedule.h"
#include "media/gpu_preview/compositor_coordinator.h"

#include <mutex>
#include <vector>

namespace mvm::gpu {

struct Phase4DriverCounters {
    // resolve == adoption + noop + reject が常に成立する (§6 の自己整合)。
    long long resolveCount = 0;
    long long adoptionCount = 0;
    long long noopCount = 0;
    long long rejectCount = 0;
    long long epochIncrementCount = 0;
    // resolve 自体に失敗した target frame。0 でなければ integration failure。
    long long unresolvedFrameCount = 0;
    // adoption 前後で SourceGeneration が変化した件数。layout transition は
    // decoder generation を触らないので 0 でなければならない。
    long long sourceGenerationChangeDueToLayoutCount = 0;
};

enum class Phase4DriveResult {
    Adopted = 0,
    NoOp,
    Rejected,
    Unresolved,
};

class Phase4CompositionDriver {
public:
    // watchedSources は generation 不変を確認する source。catalog の A/B を渡す。
    Phase4CompositionDriver(CompositionSchedule schedule, std::vector<SourceId> watchedSources);

    // measurement 中の render thread からだけ呼ぶ。measurement 前の initial S0
    // adoption は controller が coordinator へ直接行い、この counter へ含めない。
    Phase4DriveResult onTargetFrame(CompositorCoordinator& coordinator, long long outputFrame);

    Phase4DriverCounters counters() const;

private:
    const CompositionSchedule schedule_;
    const std::vector<SourceId> watchedSources_;
    mutable std::mutex mutex_;
    Phase4DriverCounters counters_;
};

} // namespace mvm::gpu

#endif

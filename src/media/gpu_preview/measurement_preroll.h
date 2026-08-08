#ifndef MVM_GPU_PREVIEW_MEASUREMENT_PREROLL_H
#define MVM_GPU_PREVIEW_MEASUREMENT_PREROLL_H

#include "media/gpu_preview/composed_frame.h"

#include <cstddef>

namespace mvm::gpu {

constexpr size_t kMeasurementPrerollFrames = 8;
constexpr int kMeasurementPrerollTimeoutMs = 2000;

struct MeasurementPrerollSourceState {
    size_t depth = 0;
    bool hasFront = false;
    SourceFrameIdentity front;
    SourceGeneration generation{};
    bool eof = false;
    bool fatal = false;
};

enum class MeasurementPrerollResult {
    Waiting,
    Ready,
    RejectedSchedulerStarted,
    RejectedFatal,
    RejectedEof,
    RejectedFront,
    TimedOut
};

inline MeasurementPrerollResult evaluateMeasurementPreroll(const MeasurementPrerollSourceState& a,
                                                           const MeasurementPrerollSourceState& b,
                                                           bool schedulerEnabled, int elapsedMs) {
    if (schedulerEnabled)
        return MeasurementPrerollResult::RejectedSchedulerStarted;
    if (a.fatal || b.fatal)
        return MeasurementPrerollResult::RejectedFatal;
    if (a.eof || b.eof)
        return MeasurementPrerollResult::RejectedEof;
    if (!a.hasFront || !b.hasFront || a.front.frameNumber != 0 || b.front.frameNumber != 0 ||
        a.front.sourceGeneration != a.generation || b.front.sourceGeneration != b.generation)
        return MeasurementPrerollResult::RejectedFront;
    if (a.depth >= kMeasurementPrerollFrames && b.depth >= kMeasurementPrerollFrames)
        return MeasurementPrerollResult::Ready;
    if (elapsedMs >= kMeasurementPrerollTimeoutMs)
        return MeasurementPrerollResult::TimedOut;
    return MeasurementPrerollResult::Waiting;
}

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_MEASUREMENT_PREROLL_H

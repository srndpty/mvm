#ifndef MVM_GPU_PREVIEW_OUTPUT_SCHEDULER_H
#define MVM_GPU_PREVIEW_OUTPUT_SCHEDULER_H

#include "media/gpu_preview/exact_frame_pairer.h"

namespace mvm::gpu {

enum class OutputDropReason {
    None = 0,
    MissingSourceA,
    MissingSourceB,
    MissingBoth,
    StaleGeneration,
    FutureGeneration,
    StaleCompositionEpoch,
    RenderFailure,
    SchedulerDeadline,
};

const char* toString(OutputDropReason reason);

struct ScheduledOutput {
    long long outputFrameNumber = -1;
    long long deadlineQpc = 0;
};

struct OutputScheduleDecision {
    bool due = false;
    ScheduledOutput output;
    long long skippedDeadlineCount = 0;
};

struct OutputScheduleState {
    long long nextFrame = -1;
    long long nextDeadlineQpc = 0;
    long long nextNextDeadlineQpc = 0;
};

class OutputScheduler60Hz {
public:
    void start(long long startQpc, long long qpcFrequency);
    ScheduledOutput next();
    OutputScheduleDecision takeDue(long long nowQpc);
    OutputScheduleDecision takeDueBefore(long long nowQpc, long long endQpcExclusive);
    long long closeBefore(long long endQpcExclusive);
    OutputScheduleState state() const;
    static OutputDropReason classifyDeadline(PairResult pairResult,
                                             CompositionResult compositionResult);

private:
    long long startQpc_ = 0;
    long long frequency_ = 0;
    long long nextFrame_ = 0;
};

} // namespace mvm::gpu
#endif

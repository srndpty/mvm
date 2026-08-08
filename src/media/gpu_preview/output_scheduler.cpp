#include "media/gpu_preview/output_scheduler.h"

namespace mvm::gpu {

const char* toString(OutputDropReason reason) {
    switch (reason) {
    case OutputDropReason::None:
        return "none";
    case OutputDropReason::MissingSourceA:
        return "missing_source_a";
    case OutputDropReason::MissingSourceB:
        return "missing_source_b";
    case OutputDropReason::MissingBoth:
        return "missing_both";
    case OutputDropReason::StaleGeneration:
        return "stale_generation";
    case OutputDropReason::FutureGeneration:
        return "future_generation";
    case OutputDropReason::StaleCompositionEpoch:
        return "stale_composition_epoch";
    case OutputDropReason::RenderFailure:
        return "render_failure";
    case OutputDropReason::SchedulerDeadline:
        return "scheduler_deadline";
    }
    return "unknown";
}

void OutputScheduler60Hz::start(long long startQpc, long long qpcFrequency) {
    startQpc_ = startQpc;
    frequency_ = qpcFrequency;
    nextFrame_ = 0;
}

ScheduledOutput OutputScheduler60Hz::next() {
    const long long frame = nextFrame_++;
    // 除算を最後にし、60Hz の端数を各frameへ均等に配る。
    return {frame, startQpc_ + (frame * frequency_) / 60};
}

OutputScheduleDecision OutputScheduler60Hz::takeDue(long long nowQpc) {
    OutputScheduleDecision decision;
    if (frequency_ <= 0)
        return decision;
    auto deadlineFor = [this](long long frame) { return startQpc_ + (frame * frequency_) / 60; };
    if (nowQpc < deadlineFor(nextFrame_))
        return decision;
    while (nowQpc >= deadlineFor(nextFrame_ + 1)) {
        ++nextFrame_;
        ++decision.skippedDeadlineCount;
    }
    decision.due = true;
    decision.output = next();
    return decision;
}

OutputScheduleDecision OutputScheduler60Hz::takeDueBefore(long long nowQpc,
                                                          long long endQpcExclusive) {
    OutputScheduleDecision decision;
    if (frequency_ <= 0 || nowQpc >= endQpcExclusive)
        return decision;
    auto deadlineFor = [this](long long frame) { return startQpc_ + (frame * frequency_) / 60; };
    if (deadlineFor(nextFrame_) >= endQpcExclusive || nowQpc < deadlineFor(nextFrame_))
        return decision;
    while (deadlineFor(nextFrame_ + 1) < endQpcExclusive && nowQpc >= deadlineFor(nextFrame_ + 1)) {
        ++nextFrame_;
        ++decision.skippedDeadlineCount;
    }
    decision.due = true;
    decision.output = next();
    return decision;
}

long long OutputScheduler60Hz::closeBefore(long long endQpcExclusive) {
    if (frequency_ <= 0)
        return 0;
    long long closed = 0;
    while (startQpc_ + (nextFrame_ * frequency_) / 60 < endQpcExclusive) {
        ++nextFrame_;
        ++closed;
    }
    return closed;
}

OutputDropReason OutputScheduler60Hz::classifyDeadline(PairResult pairResult,
                                                       CompositionResult compositionResult) {
    if (compositionResult == CompositionResult::StaleEpoch)
        return OutputDropReason::StaleCompositionEpoch;
    if (compositionResult == CompositionResult::StaleGeneration ||
        pairResult == PairResult::StaleGeneration)
        return OutputDropReason::StaleGeneration;
    if (compositionResult == CompositionResult::FutureGeneration ||
        pairResult == PairResult::FutureGeneration)
        return OutputDropReason::FutureGeneration;
    if (pairResult == PairResult::MissingA)
        return OutputDropReason::MissingSourceA;
    if (pairResult == PairResult::MissingB)
        return OutputDropReason::MissingSourceB;
    if (pairResult == PairResult::MissingBoth)
        return OutputDropReason::MissingBoth;
    return OutputDropReason::SchedulerDeadline;
}

} // namespace mvm::gpu

#include "media/gpu_preview/scheduler_phase_attribution.h"

#include <algorithm>

namespace mvm::gpu {

void SchedulerPhaseRing::reset() {
    overflowCount_.store(0, std::memory_order_relaxed);
    publishedCount_.store(0, std::memory_order_release);
}

void SchedulerPhaseRing::capture(const SchedulerPhaseRecord& record) {
    const std::size_t index = publishedCount_.load(std::memory_order_relaxed);
    if (index >= records_.size()) {
        overflowCount_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    records_[index] = record;
    publishedCount_.store(index + 1, std::memory_order_release);
}

std::vector<SchedulerPhaseRecord> SchedulerPhaseRing::snapshot() const {
    const std::size_t count = publishedCount();
    return {records_.begin(), records_.begin() + static_cast<std::ptrdiff_t>(count)};
}

std::size_t SchedulerPhaseRing::publishedCount() const {
    return std::min(publishedCount_.load(std::memory_order_acquire), records_.size());
}

long long SchedulerPhaseRing::overflowCount() const {
    return overflowCount_.load(std::memory_order_acquire);
}

const char* toString(SchedulerPhaseClassification value) {
    switch (value) {
    case SchedulerPhaseClassification::None:
        return "NONE";
    case SchedulerPhaseClassification::PhasePair:
        return "PHASE_PAIR";
    case SchedulerPhaseClassification::LongCallbackGap:
        return "LONG_CALLBACK_GAP";
    case SchedulerPhaseClassification::UnpairedSkip:
        return "UNPAIRED_SKIP";
    }
    return "UNKNOWN";
}

SchedulerPhaseAttributionSummary
classifySchedulerPhase(const std::vector<SchedulerPhaseRecord>& records, long long qpcFrequency) {
    SchedulerPhaseAttributionSummary result;
    result.classifications.resize(records.size(), SchedulerPhaseClassification::None);
    if (qpcFrequency <= 0)
        return result;
    // 2 nominal slot以上なら、callback phaseにかかわらず少なくとも1 slotを
    // callback不在だけで失ったと判定できる。任意の時間閾値は導入しない。
    const long long twoSlotsQpc = (2 * qpcFrequency + 59) / 60;
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto& current = records[index];
        if (current.decisionSkippedDeadlineCount <= 0)
            continue;
        ++result.skipEventCount;
        result.classifiedDeadlineCount += current.decisionSkippedDeadlineCount;
        if (index > 0 && !records[index - 1].decisionDue) {
            const auto& previous = records[index - 1];
            result.classifications[index] = SchedulerPhaseClassification::PhasePair;
            result.phasePairDeadlineCount += current.decisionSkippedDeadlineCount;
            result.phasePairs.push_back(
                {index - 1, index, current.decisionSkippedDeadlineCount,
                 static_cast<double>(previous.nextDeadlineQpc - previous.schedulerNowQpc) *
                     1000000.0 / static_cast<double>(qpcFrequency),
                 static_cast<double>(current.schedulerNowQpc - current.nextNextDeadlineQpc) *
                     1000000.0 / static_cast<double>(qpcFrequency),
                 static_cast<double>(current.callbackQpc - previous.callbackQpc) * 1000000.0 /
                     static_cast<double>(qpcFrequency)});
        } else if (current.previousCallbackQpc > 0 &&
                   current.callbackQpc - current.previousCallbackQpc >= twoSlotsQpc) {
            result.classifications[index] = SchedulerPhaseClassification::LongCallbackGap;
            result.longCallbackGapDeadlineCount += current.decisionSkippedDeadlineCount;
        } else {
            result.classifications[index] = SchedulerPhaseClassification::UnpairedSkip;
            result.unpairedSkipDeadlineCount += current.decisionSkippedDeadlineCount;
        }
    }
    return result;
}

} // namespace mvm::gpu

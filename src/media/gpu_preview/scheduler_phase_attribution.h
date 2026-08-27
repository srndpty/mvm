#ifndef MVM_GPU_PREVIEW_SCHEDULER_PHASE_ATTRIBUTION_H
#define MVM_GPU_PREVIEW_SCHEDULER_PHASE_ATTRIBUTION_H

#include <array>
#include <atomic>
#include <cstddef>
#include <vector>

namespace mvm::gpu {

constexpr std::size_t kSchedulerPhaseRingCapacity = 8192;

struct SchedulerPhaseRecord {
    long long callbackQpc = 0;
    long long previousCallbackQpc = 0;
    long long schedulerNowQpc = 0;
    long long nextFrameBefore = -1;
    long long nextDeadlineQpc = 0;
    long long nextNextDeadlineQpc = 0;
    long long nowMinusNextDeadlineQpc = 0;
    bool decisionDue = false;
    long long decisionSkippedDeadlineCount = 0;
    long long decisionOutputFrame = -1;
    bool repeatedThisCallback = false;
};

// measurement中はrender threadだけが書き、停止後にcontrollerがsnapshotする。
// writer pathではallocation、lock、file I/Oを行わない。
class SchedulerPhaseRing {
public:
    void reset();
    void capture(const SchedulerPhaseRecord& record);
    std::vector<SchedulerPhaseRecord> snapshot() const;
    std::size_t publishedCount() const;
    long long overflowCount() const;

private:
    std::array<SchedulerPhaseRecord, kSchedulerPhaseRingCapacity> records_{};
    std::atomic<std::size_t> publishedCount_{0};
    std::atomic<long long> overflowCount_{0};
};

enum class SchedulerPhaseClassification { None, PhasePair, LongCallbackGap, UnpairedSkip };

const char* toString(SchedulerPhaseClassification value);

struct SchedulerPhasePairSample {
    std::size_t previousRecordIndex = 0;
    std::size_t currentRecordIndex = 0;
    long long skippedDeadlineCount = 0;
    double previousEarlyUs = 0.0;
    double currentLateUs = 0.0;
    double callbackIntervalUs = 0.0;
};

struct SchedulerPhaseAttributionSummary {
    long long skipEventCount = 0;
    long long classifiedDeadlineCount = 0;
    long long phasePairDeadlineCount = 0;
    long long longCallbackGapDeadlineCount = 0;
    long long unpairedSkipDeadlineCount = 0;
    std::vector<SchedulerPhaseClassification> classifications;
    std::vector<SchedulerPhasePairSample> phasePairs;
};

SchedulerPhaseAttributionSummary
classifySchedulerPhase(const std::vector<SchedulerPhaseRecord>& records, long long qpcFrequency);

} // namespace mvm::gpu

#endif

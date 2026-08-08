#include "media/gpu_preview/composition_display_ledger.h"

#include <algorithm>
#include <chrono>

namespace mvm::gpu {

bool matches(const CompositionDisplayRecord& record,
             const CompositionDisplayExpectation& expectation) {
    if (record.outputFrameNumber != expectation.outputFrameNumber ||
        record.compositionEpoch != expectation.compositionEpoch ||
        record.sources.size() != expectation.sources.size())
        return false;
    for (size_t i = 0; i < record.sources.size(); ++i) {
        const auto& a = record.sources[i];
        const auto& b = expectation.sources[i];
        if (a.sourceId != b.sourceId || a.sourceGeneration != b.sourceGeneration ||
            a.resourceEpoch != b.resourceEpoch || a.frameNumber != b.frameNumber)
            return false;
    }
    return true;
}

CompositionDisplayLedger::CompositionDisplayLedger(size_t capacity)
    : capacity_(std::max<size_t>(1, capacity)) {}

unsigned long long CompositionDisplayLedger::record(const ComposedFrame& frame,
                                                    long long displayedQpc, long long pairReadyQpc,
                                                    long long submissionQpc) {
    CompositionDisplayRecord record;
    record.pairReadyQpc = pairReadyQpc;
    record.submissionQpc = submissionQpc;
    record.displayedQpc = displayedQpc;
    record.outputFrameNumber = frame.outputFrameNumber;
    record.compositionEpoch = frame.compositionEpoch;
    record.sources.reserve(frame.layers.size());
    for (const auto& layer : frame.layers)
        record.sources.push_back(identityOf(layer.frame));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        record.displaySequence = ++sequence_;
        history_.push_back(record);
        while (history_.size() > capacity_)
            history_.pop_front();
    }
    changed_.notify_all();
    return record.displaySequence;
}

unsigned long long CompositionDisplayLedger::baseline() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sequence_;
}

bool CompositionDisplayLedger::findAfter(unsigned long long baselineValue,
                                         const CompositionDisplayExpectation& expectation,
                                         CompositionDisplayRecord& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& record : history_)
        if (record.displaySequence > baselineValue && matches(record, expectation)) {
            out = record;
            return true;
        }
    return false;
}

bool CompositionDisplayLedger::findEpochAfter(unsigned long long baselineValue,
                                              CompositionEpoch epoch,
                                              CompositionDisplayRecord& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& record : history_)
        if (record.displaySequence > baselineValue && record.compositionEpoch == epoch) {
            out = record;
            return true;
        }
    return false;
}

bool CompositionDisplayLedger::waitAfter(unsigned long long baselineValue,
                                         const CompositionDisplayExpectation& expectation,
                                         int timeoutMs, CompositionDisplayRecord& out) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto predicate = [&] {
        for (const auto& record : history_)
            if (record.displaySequence > baselineValue && matches(record, expectation)) {
                out = record;
                return true;
            }
        return aborted_;
    };
    if (!changed_.wait_for(lock, std::chrono::milliseconds(std::max(0, timeoutMs)), predicate))
        return false;
    return !aborted_ && out.displaySequence > baselineValue;
}

void CompositionDisplayLedger::abort() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        aborted_ = true;
    }
    changed_.notify_all();
}

size_t CompositionDisplayLedger::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return history_.size();
}

} // namespace mvm::gpu

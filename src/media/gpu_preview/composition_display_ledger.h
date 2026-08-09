#ifndef MVM_GPU_PREVIEW_COMPOSITION_DISPLAY_LEDGER_H
#define MVM_GPU_PREVIEW_COMPOSITION_DISPLAY_LEDGER_H

#include "media/gpu_preview/composed_frame.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

namespace mvm::gpu {

struct CompositionDisplayRecord {
    unsigned long long displaySequence = 0;
    long long pairReadyQpc = 0;
    long long submissionQpc = 0;
    long long displayedQpc = 0;
    long long displayRecordQpc = 0;
    long long outputFrameNumber = -1;
    long long frameNumber = -1;
    bool applicationAvProjectionValid = false;
    double applicationAvDeltaMs = 0.0;
    CompositionEpoch compositionEpoch{};
    std::vector<SourceFrameIdentity> sources;
    CompositionStateId compositionState{};
};

struct CompositionDisplayExpectation {
    long long outputFrameNumber = -1;
    CompositionEpoch compositionEpoch{};
    std::vector<SourceFrameIdentity> sources;
    CompositionStateId compositionState{};
};

bool matches(const CompositionDisplayRecord& record,
             const CompositionDisplayExpectation& expectation);

class CompositionDisplayLedger {
public:
    explicit CompositionDisplayLedger(size_t capacity = 512);

    unsigned long long record(const ComposedFrame& frame, long long displayedQpc,
                              long long pairReadyQpc = 0, long long submissionQpc = 0,
                              bool applicationAvProjectionValid = false,
                              double applicationAvDeltaMs = 0.0);
    unsigned long long baseline() const;
    bool findAfter(unsigned long long baseline, const CompositionDisplayExpectation& expectation,
                   CompositionDisplayRecord& out) const;
    bool findEpochAfter(unsigned long long baseline, CompositionEpoch epoch,
                        CompositionDisplayRecord& out) const;
    bool waitAfter(unsigned long long baseline, const CompositionDisplayExpectation& expectation,
                   int timeoutMs, CompositionDisplayRecord& out);
    void abort();
    size_t size() const;

    size_t capacity() const { return capacity_; }

private:
    const size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<CompositionDisplayRecord> history_;
    unsigned long long sequence_ = 0;
    bool aborted_ = false;
};

} // namespace mvm::gpu
#endif

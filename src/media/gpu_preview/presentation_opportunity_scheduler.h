#ifndef MVM_GPU_PREVIEW_PRESENTATION_OPPORTUNITY_SCHEDULER_H
#define MVM_GPU_PREVIEW_PRESENTATION_OPPORTUNITY_SCHEDULER_H

#include <vector>

namespace mvm::gpu {

enum class PresentationOpportunityError {
    None,
    InvalidConfiguration,
    ArithmeticOverflow,
    AmbiguousOpportunity,
    OpportunityRegression,
    RenderWithoutSwap,
    SwapWithoutRender,
    RenderSwapMismatch,
    PresentedFrameMismatch,
};

struct PresentationOpportunityConfig {
    long long requiredFrameCount = 0;
    long long sourceFpsNumerator = 0;
    long long sourceFpsDenominator = 0;
    long long refreshNumerator = 0;
    long long refreshDenominator = 0;
    long long qpcFrequency = 0;
};

struct PresentationOpportunityDecision {
    bool valid = false;
    bool duplicateCallback = false;
    bool repeat = false;
    bool pastSourceDomain = false;
    long long opportunityOrdinal = -1;
    long long targetFrame = -1;
    long long trueDropBefore = 0;
};

struct PresentationOpportunityLedgerRecord {
    long long opportunityOrdinal = -1;
    long long swapQpc = 0;
    long long refreshNumerator = 0;
    long long refreshDenominator = 0;
    long long expectedSourceFrame = -1;
    long long presentedSourceFrame = -1;
    bool repeat = false;
    long long trueDropBefore = 0;
};

struct PresentationOpportunitySnapshot {
    bool valid = false;
    bool closed = false;
    PresentationOpportunityError error = PresentationOpportunityError::None;
    long long displayedUnique = 0;
    long long repeated = 0;
    long long gapTrueDrop = 0;
    long long tailTrueDrop = 0;
    long long trueDrop = 0;
    long long lastOpportunityOrdinal = -1;
    long long lastUniqueFrame = -1;
    std::vector<PresentationOpportunityLedgerRecord> records;
};

// P2-D5-2 formal Playback専用。render時点で直前の完了swapだけを参照して
// 次のpresentation opportunityを決め、frameSwappedで対応と序数を検証する。
// OutputScheduler60HzやP3 audio-master schedulerには接続しない。
class PresentationOpportunityScheduler {
public:
    bool start(const PresentationOpportunityConfig& config);
    PresentationOpportunityDecision selectForRender(long long callbackQpc);
    bool commitSwap(long long swapQpc, long long presentedSourceFrame);
    bool close();
    PresentationOpportunitySnapshot snapshot() const;

    bool hasPendingRender() const { return pending_; }

    bool pastSourceDomain() const { return pastSourceDomain_; }

    PresentationOpportunityError error() const { return error_; }

private:
    bool fail(PresentationOpportunityError error);
    bool targetFor(long long ordinal, long long& target) const;
    bool roundedRefreshIntervals(long long deltaQpc, long long& intervals) const;

    PresentationOpportunityConfig config_{};
    PresentationOpportunityError error_ = PresentationOpportunityError::None;
    bool started_ = false;
    bool closed_ = false;
    bool pending_ = false;
    bool pastSourceDomain_ = false;
    long long lastSwapQpc_ = 0;
    long long lastOpportunityOrdinal_ = -1;
    long long lastUniqueFrame_ = -1;
    PresentationOpportunityDecision pendingDecision_{};
    long long displayedUnique_ = 0;
    long long repeated_ = 0;
    long long gapTrueDrop_ = 0;
    long long tailTrueDrop_ = 0;
    std::vector<PresentationOpportunityLedgerRecord> records_;
};

const char* presentationOpportunityErrorName(PresentationOpportunityError error);

} // namespace mvm::gpu

#endif

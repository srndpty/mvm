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
    AuthorityDiscontinuity,
    RenderWithoutSwap,
    SwapWithoutRender,
    RenderNotCompleted,
    RenderOrdinalMismatch,
    SwapOrdinalMismatch,
    PresentedFrameMismatch,
};

enum class PresentationOpportunityClassification {
    None,
    Exact,
    ForwardOpportunityLoss,
    Regression,
    AuthorityDiscontinuity,
    PairingDefect,
};

struct PresentationAuthoritySample {
    bool available = false;
    unsigned long long refreshCount = 0;
    long long qpcVBlank = 0;
    long long refreshNumerator = 0;
    long long refreshDenominator = 0;
};

struct PresentationOpportunityConfig {
    long long requiredFrameCount = 0;
    long long sourceFpsNumerator = 0;
    long long sourceFpsDenominator = 0;
    long long refreshNumerator = 0;
    long long refreshDenominator = 0;
    long long qpcFrequency = 0;
    bool requireAuthoritySamples = false;
};

struct PresentationOpportunityDecision {
    bool valid = false;
    bool duplicateCallback = false;
    bool repeat = false;
    bool pastSourceDomain = false;
    long long opportunityOrdinal = -1;
    long long targetFrame = -1;
    long long trueDropBefore = 0;
    long long lastCommittedOpportunityOrdinal = -1;
    long long renderBeginQpc = 0;
    long long renderOrdinal = -1;
    PresentationAuthoritySample preRenderAuthority;
};

struct PresentationOpportunityLedgerRecord {
    long long lastCommittedOpportunityOrdinal = -1;
    long long predictedOpportunityOrdinal = -1;
    long long actualOpportunityOrdinal = -1;
    long long renderBeginQpc = 0;
    long long renderEndQpc = 0;
    long long swapQpc = 0;
    long long renderOrdinal = -1;
    long long swapOrdinal = -1;
    long long refreshNumerator = 0;
    long long refreshDenominator = 0;
    PresentationAuthoritySample preRenderAuthority;
    PresentationAuthoritySample postSwapAuthority;
    bool authorityContinuous = false;
    long long predictedSourceFrame = -1;
    long long expectedSourceFrame = -1;
    long long presentedSourceFrame = -1;
    bool repeat = false;
    long long trueDropBefore = 0;
    long long lostOpportunityCount = 0;
    PresentationOpportunityClassification classification =
        PresentationOpportunityClassification::None;
};

struct PresentationOpportunityFirstEvent {
    bool captured = false;
    PresentationOpportunityClassification classification =
        PresentationOpportunityClassification::None;
    long long lastCommittedOpportunityOrdinal = -1;
    long long predictedOpportunityOrdinal = -1;
    long long actualOpportunityOrdinal = -1;
    long long renderBeginQpc = 0;
    long long renderEndQpc = 0;
    long long swapQpc = 0;
    PresentationAuthoritySample preRenderAuthority;
    PresentationAuthoritySample postSwapAuthority;
    long long predictedSourceFrame = -1;
    long long actualTargetFrame = -1;
    long long renderedSourceFrame = -1;
    long long renderOrdinal = -1;
    long long swapOrdinal = -1;
    bool authorityContinuous = false;
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
    long long forwardReconciliationCount = 0;
    long long lostOpportunityCount = 0;
    PresentationOpportunityFirstEvent firstEvent;
    std::vector<PresentationOpportunityLedgerRecord> records;
};

// P2-D5-2 formal Playback専用。render時点で直前の完了swapだけを参照して
// 次のpresentation opportunityを決め、frameSwappedで対応と序数を検証する。
// OutputScheduler60HzやP3 audio-master schedulerには接続しない。
class PresentationOpportunityScheduler {
public:
    bool start(const PresentationOpportunityConfig& config);
    PresentationOpportunityDecision
    selectForRender(long long callbackQpc,
                    const PresentationAuthoritySample& preRenderAuthority = {},
                    long long renderOrdinal = -1);
    bool markRenderComplete(long long renderEndQpc, long long renderedSourceFrame,
                            long long renderOrdinal);
    bool commitSwap(long long swapQpc, const PresentationAuthoritySample& postSwapAuthority = {},
                    long long swapOrdinal = -1);
    bool close();
    PresentationOpportunitySnapshot snapshot() const;

    bool hasPendingRender() const { return pending_; }

    bool pastSourceDomain() const { return pastSourceDomain_; }

    PresentationOpportunityError error() const { return error_; }

private:
    bool fail(PresentationOpportunityError error);
    bool targetFor(long long ordinal, long long& target) const;
    bool roundedRefreshIntervals(long long deltaQpc, long long& intervals) const;
    bool authorityContinuous(const PresentationAuthoritySample& pre,
                             const PresentationAuthoritySample& post) const;
    void captureFirstEvent(PresentationOpportunityClassification classification,
                           long long actualOrdinal, long long actualTarget, long long swapQpc,
                           const PresentationAuthoritySample& post, long long swapOrdinal,
                           bool continuous);

    PresentationOpportunityConfig config_{};
    PresentationOpportunityError error_ = PresentationOpportunityError::None;
    bool started_ = false;
    bool closed_ = false;
    bool pending_ = false;
    bool pastSourceDomain_ = false;
    long long lastSwapQpc_ = 0;
    long long lastOpportunityOrdinal_ = -1;
    long long lastUniqueFrame_ = -1;
    long long lastRenderOrdinal_ = -1;
    long long lastSwapOrdinal_ = -1;
    PresentationAuthoritySample lastPostSwapAuthority_{};
    PresentationOpportunityDecision pendingDecision_{};
    bool pendingRenderCompleted_ = false;
    long long pendingRenderEndQpc_ = 0;
    long long pendingRenderedSourceFrame_ = -1;
    long long displayedUnique_ = 0;
    long long repeated_ = 0;
    long long gapTrueDrop_ = 0;
    long long tailTrueDrop_ = 0;
    long long forwardReconciliationCount_ = 0;
    long long lostOpportunityCount_ = 0;
    PresentationOpportunityFirstEvent firstEvent_{};
    std::vector<PresentationOpportunityLedgerRecord> records_;
};

const char* presentationOpportunityErrorName(PresentationOpportunityError error);
const char*
presentationOpportunityClassificationName(PresentationOpportunityClassification classification);

} // namespace mvm::gpu

#endif

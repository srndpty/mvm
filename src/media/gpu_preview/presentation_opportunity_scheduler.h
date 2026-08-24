#ifndef MVM_GPU_PREVIEW_PRESENTATION_OPPORTUNITY_SCHEDULER_H
#define MVM_GPU_PREVIEW_PRESENTATION_OPPORTUNITY_SCHEDULER_H

#include "media/gpu_preview/presentation_refresh_authority.h"

#include <vector>

namespace mvm::gpu {

enum class PresentationOpportunityError {
    None,
    InvalidConfiguration,
    ArithmeticOverflow,
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

struct PresentationOpportunityConfig {
    long long requiredFrameCount = 0;
    // acquisition prerollなど、既存frame domain内の途中をscheduler target 0として
    // 扱う場合の加算offset。formal measurementは常に0。
    long long sourceFrameOffset = 0;
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
    long long lastFinalizedOpportunityOrdinal = -1;
    long long renderBeginQpc = 0;
    long long renderOrdinal = -1;
    PresentationAuthoritySample preRenderAuthority;
};

// finalizeされた1 presentation opportunity。同一opportunity内で複数swapが起きた
// 場合に記録されるのはlatest candidateで、置き換えられたcandidate数は
// supersededCandidateCountへ残る。
struct PresentationOpportunityLedgerRecord {
    long long lastFinalizedOpportunityOrdinal = -1;
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
    long long supersededCandidateCount = 0;
    bool forwardReconciliation = false;
    PresentationOpportunityClassification classification =
        PresentationOpportunityClassification::None;
};

struct PresentationOpportunityFirstEvent {
    bool captured = false;
    PresentationOpportunityClassification classification =
        PresentationOpportunityClassification::None;
    long long lastFinalizedOpportunityOrdinal = -1;
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
    bool anchored = false;
    unsigned long long originRefreshCount = 0;
    long long displayedUnique = 0;
    long long repeated = 0;
    long long gapTrueDrop = 0;
    long long tailTrueDrop = 0;
    long long trueDrop = 0;
    long long lastFinalizedOpportunityOrdinal = -1;
    long long lastUniqueFrame = -1;
    long long forwardReconciliationCount = 0;
    long long lostOpportunityCount = 0;
    long long supersededCandidateCount = 0;
    long long swappedCompositionCount = 0;
    PresentationOpportunityFirstEvent firstEvent;
    std::vector<PresentationOpportunityLedgerRecord> records;
};

// P2-D5-2/F2 formal Playback専用。presentation opportunityの序数はDWM refresh
// count authorityだけから決め、QPC差分はcontinuityのcross-checkに留める。
// frameSwappedごとに即finalizeせず、refresh opportunityごとにlatest candidateを
// 保持し、opportunityが前進した時点で直前pendingをfinalizeする。
// OutputScheduler60HzやP3 audio-master schedulerには接続しない。
class PresentationOpportunityScheduler {
public:
    bool start(const PresentationOpportunityConfig& config);
    PresentationOpportunityDecision selectForRender(long long callbackQpc,
                                                    const PresentationAuthoritySample& preRender,
                                                    long long renderOrdinal);
    bool markRenderComplete(long long renderEndQpc, long long renderedSourceFrame,
                            long long renderOrdinal);
    bool commitSwap(long long swapQpc, const PresentationAuthoritySample& postSwapAuthority,
                    long long swapOrdinal);
    bool close();
    PresentationOpportunitySnapshot snapshot() const;

    bool hasPendingRender() const { return pendingRender_; }

    bool pastSourceDomain() const { return pastSourceDomain_; }

    PresentationOpportunityError error() const { return error_; }

private:
    // finalize待ちopportunityのlatest render/swap candidate。
    struct Candidate {
        long long predictedOpportunityOrdinal = -1;
        long long predictedSourceFrame = -1;
        long long presentedSourceFrame = -1;
        long long renderBeginQpc = 0;
        long long renderEndQpc = 0;
        long long swapQpc = 0;
        long long renderOrdinal = -1;
        long long swapOrdinal = -1;
        PresentationAuthoritySample preRenderAuthority;
        PresentationAuthoritySample postSwapAuthority;
    };

    bool fail(PresentationOpportunityError error);
    bool targetFor(long long ordinal, long long& target) const;
    bool finalizePendingOpportunity();
    void captureFirstEvent(PresentationOpportunityClassification classification,
                           long long actualOrdinal, long long actualTarget, long long swapQpc,
                           const PresentationAuthoritySample& post, long long swapOrdinal,
                           bool continuous);

    PresentationOpportunityConfig config_{};
    PresentationOpportunityError error_ = PresentationOpportunityError::None;
    bool started_ = false;
    bool closed_ = false;
    bool pastSourceDomain_ = false;
    bool anchored_ = false;
    unsigned long long originRefreshCount_ = 0;

    bool pendingRender_ = false;
    PresentationOpportunityDecision pendingDecision_{};
    bool pendingRenderCompleted_ = false;
    long long pendingRenderEndQpc_ = 0;
    long long pendingRenderedSourceFrame_ = -1;

    bool pendingOpportunity_ = false;
    long long pendingOpportunityOrdinal_ = -1;
    long long pendingSupersededCount_ = 0;
    Candidate pendingCandidate_{};

    long long lastSwapQpc_ = 0;
    long long lastFinalizedOrdinal_ = -1;
    long long lastUniqueFrame_ = -1;
    long long lastRenderOrdinal_ = -1;
    long long lastSwapOrdinal_ = -1;
    PresentationAuthoritySample lastAuthority_{};
    long long displayedUnique_ = 0;
    long long repeated_ = 0;
    long long gapTrueDrop_ = 0;
    long long tailTrueDrop_ = 0;
    long long forwardReconciliationCount_ = 0;
    long long lostOpportunityCount_ = 0;
    long long supersededCandidateCount_ = 0;
    long long swappedCompositionCount_ = 0;
    PresentationOpportunityFirstEvent firstEvent_{};
    std::vector<PresentationOpportunityLedgerRecord> records_;
};

const char* presentationOpportunityErrorName(PresentationOpportunityError error);
const char*
presentationOpportunityClassificationName(PresentationOpportunityClassification classification);

} // namespace mvm::gpu

#endif

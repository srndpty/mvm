#ifndef MVM_GPU_PREVIEW_PRESENTATION_OPPORTUNITY_SCHEDULER_H
#define MVM_GPU_PREVIEW_PRESENTATION_OPPORTUNITY_SCHEDULER_H

#include "media/gpu_preview/presentation_refresh_authority.h"
#include "media/gpu_preview/required_intent_queue.h"

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
    RequiredQueueFailure,
    SourceCoverageInsufficient,
    QualifiedCommitMissing,
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
    // W4-C2 diagnostic capture専用。canonical performance authorityでは使わない。
    bool invocationLedgerEnabled = false;
};

struct PresentationOpportunityDecision {
    bool valid = false;
    bool duplicateCallback = false;
    bool repeat = false;
    bool pastSourceDomain = false;
    bool requiredIntentMembership = false;
    bool requiredIntentMembershipExact = false;
    long long opportunityOrdinal = -1;
    long long targetFrame = -1;
    long long lastFinalizedOpportunityOrdinal = -1;
    long long renderBeginQpc = 0;
    long long renderOrdinal = -1;
    // B3-I0。primary pending renderごとのlocal reservation identity。
    // intent ordinalのauthorityではなく、token/render/Present/swap join専用。
    unsigned long long reservationId = 0;
    PresentationAuthoritySample preRenderAuthority;
    unsigned long long invocationSerial = 0;
};

enum class FormalIntentTransportDisposition {
    Transport = 0,
    SuppressDuplicateCallback,
    SuppressOutsideRequiredSet,
    InvalidMembershipProvenance,
};

inline const char*
formalIntentTransportDispositionName(FormalIntentTransportDisposition disposition) {
    switch (disposition) {
    case FormalIntentTransportDisposition::Transport:
        return "TRANSPORT";
    case FormalIntentTransportDisposition::SuppressDuplicateCallback:
        return "SUPPRESS_DUPLICATE_CALLBACK";
    case FormalIntentTransportDisposition::SuppressOutsideRequiredSet:
        return "SUPPRESS_OUTSIDE_REQUIRED_SET";
    case FormalIntentTransportDisposition::InvalidMembershipProvenance:
        return "INVALID_MEMBERSHIP_PROVENANCE";
    }
    return "UNKNOWN";
}

inline FormalIntentTransportDisposition
formalIntentTransportDisposition(bool foreignPreMeasurement,
                                 const PresentationOpportunityDecision& decision) {
    if (!decision.requiredIntentMembershipExact)
        return FormalIntentTransportDisposition::InvalidMembershipProvenance;
    if (decision.duplicateCallback)
        return FormalIntentTransportDisposition::SuppressDuplicateCallback;
    if (!foreignPreMeasurement && !decision.requiredIntentMembership)
        return FormalIntentTransportDisposition::SuppressOutsideRequiredSet;
    return FormalIntentTransportDisposition::Transport;
}

enum class PresentationSchedulerInvocationResult {
    PrimaryDecision = 0,
    DuplicateDecision,
    OutsideSourceDomainDecision,
    RequiredQueueExhaustedDecision,
    InvalidFatal,
};

enum class PresentationSchedulerInvocationReason {
    Primary = 0,
    PendingRender,
    PastSourceDomain,
    RequiredQueueExhausted,
    InvalidConfiguration,
    AuthorityUnusable,
    CallbackQpcRegression,
    CompletedOrdinalUnavailable,
    CompletedOrdinalOverflow,
    TargetArithmeticOverflow,
};

struct PresentationSchedulerInvocationState {
    bool started = false;
    bool closed = false;
    bool anchored = false;
    unsigned long long originRefreshCount = 0;
    long long lastFinalizedOpportunityOrdinal = -1;
    bool pendingRender = false;
    bool pastSourceDomain = false;
};

struct PresentationSchedulerInvocationRecord {
    unsigned long long invocationSerial = 0;
    long long invocationQpc = 0;
    PresentationAuthoritySample inputAuthority;
    PresentationSchedulerInvocationState pre;
    PresentationSchedulerInvocationResult result =
        PresentationSchedulerInvocationResult::InvalidFatal;
    PresentationSchedulerInvocationReason reason =
        PresentationSchedulerInvocationReason::InvalidConfiguration;
    PresentationOpportunityDecision decision;
    FormalIntentTransportDisposition transportDisposition =
        FormalIntentTransportDisposition::InvalidMembershipProvenance;
    bool transportDispositionExact = false;
    PresentationSchedulerInvocationState post;
    bool stateTransitionExact = false;
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
    bool requiredIntentSetExact = false;
    std::vector<long long> requiredIntentOrdinals;
    bool invocationLedgerEnabled = false;
    std::vector<PresentationSchedulerInvocationRecord> invocationRecords;
    RequiredIntentQueueSnapshot requiredIntentQueue;
    // W4-C3 exact causal replayの入力。scheduler instanceが実際に使用したconfigを
    // そのまま渡す。artifact側で別fieldから再構成してはならない。
    PresentationOpportunityConfig config;
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
    bool commitQualifiedPresent(unsigned long long reservationId, long long intentOrdinal);
    bool commitSwap(long long swapQpc, const PresentationAuthoritySample& postSwapAuthority,
                    long long swapOrdinal);
    // B3-I5B。preroll drainのexact finalize point。closeより前にpending opportunityを
    // 確定させるだけで、queue semanticsとaccept/reject判定は変更しない。
    bool finalizePendingOpportunityExact();
    bool closePlannedWindow();
    bool closeWithoutNormalCompletion();
    PresentationOpportunitySnapshot snapshot() const;
    bool noteInvocationTransportDisposition(unsigned long long invocationSerial,
                                            FormalIntentTransportDisposition disposition);

    bool hasPendingRender() const { return pendingRender_; }

    bool hasPendingRenderCompletion() const { return pendingRenderCompleted_; }

    bool hasPendingQualifiedEvidence() const { return pendingQualifiedEvidence_; }

    bool hasPendingOpportunity() const { return pendingOpportunity_; }

    bool pendingOpportunityExactlyFinalized() const { return pendingOpportunityExactlyFinalized_; }

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

    struct PendingOpportunityFinalization {
        PresentationOpportunityLedgerRecord record;
        PresentationOpportunityFirstEvent firstEvent;
        bool captureFirstEvent = false;
        bool repeat = false;
        bool forward = false;
        long long trueDropBefore = 0;
        long long lostOpportunities = 0;
    };

    bool fail(PresentationOpportunityError error);
    bool close(bool plannedWindowEnd);
    bool targetFor(long long ordinal, long long& target) const;
    bool finalizePendingOpportunity();
    bool preparePendingOpportunityFinalization(PendingOpportunityFinalization& prepared);
    void applyPendingOpportunityFinalization(const PendingOpportunityFinalization& prepared);
    void captureFirstEvent(PresentationOpportunityClassification classification,
                           long long actualOrdinal, long long actualTarget, long long swapQpc,
                           const PresentationAuthoritySample& post, long long swapOrdinal,
                           bool continuous);
    PresentationSchedulerInvocationState invocationState() const;
    PresentationOpportunityDecision
    finishInvocation(const PresentationSchedulerInvocationState& pre, long long invocationQpc,
                     const PresentationAuthoritySample& inputAuthority,
                     PresentationSchedulerInvocationResult result,
                     PresentationSchedulerInvocationReason reason,
                     PresentationOpportunityDecision decision);

    PresentationOpportunityConfig config_{};
    PresentationOpportunityError error_ = PresentationOpportunityError::None;
    bool started_ = false;
    bool closed_ = false;
    bool pastSourceDomain_ = false;
    bool anchored_ = false;
    unsigned long long originRefreshCount_ = 0;
    RequiredIntentQueue requiredIntentQueue_;

    bool pendingRender_ = false;
    PresentationOpportunityDecision pendingDecision_{};
    bool pendingRenderCompleted_ = false;
    bool pendingQualifiedEvidence_ = false;
    long long pendingRenderEndQpc_ = 0;
    long long pendingRenderedSourceFrame_ = -1;

    bool pendingOpportunity_ = false;
    bool pendingOpportunityExactlyFinalized_ = false;
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
    unsigned long long invocationSerial_ = 0;
    std::vector<PresentationSchedulerInvocationRecord> invocationRecords_;
};

const char* presentationOpportunityErrorName(PresentationOpportunityError error);
const char*
presentationOpportunityClassificationName(PresentationOpportunityClassification classification);
const char* presentationSchedulerInvocationResultName(PresentationSchedulerInvocationResult result);
const char* presentationSchedulerInvocationReasonName(PresentationSchedulerInvocationReason reason);

} // namespace mvm::gpu

#endif

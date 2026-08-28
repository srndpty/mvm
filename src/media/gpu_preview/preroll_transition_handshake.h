#ifndef MVM_MEDIA_GPU_PREVIEW_PREROLL_TRANSITION_HANDSHAKE_H
#define MVM_MEDIA_GPU_PREVIEW_PREROLL_TRANSITION_HANDSHAKE_H

#include <cstdint>

namespace mvm::gpu {

// B3-I5B。preroll(FOREIGN)からcurrent(CANONICAL)へのtransitionを明示state machineにする。
// phase boolやcallback位置ではなく、この列挙だけがtransitionのauthorityである。
enum class PrerollTransitionState {
    Open = 0,         // FOREIGN admission open。preroll intentを新規発行してよい
    DrainRequested,   // admission closed。新規FOREIGN reservationだけを禁止
    Draining,         // 既存active FOREIGN transactionの終端待ち
    QuiescenceCheck,  // 同一snapshotでquiescence predicateを評価中
    Quiescent,        // PREROLL_TRANSACTION_FULLY_QUIESCENT ack済み
    CurrentReady,     // current required queue初期化済み。issuanceはまだ閉じている
    MeasurementArmed, // canonical start/end authorityをfreeze済み
    CurrentRunning,   // current issuance gate open
    ProtocolFatal,    // fail-closed。performance dropへは流さない
};

// 全failureはPROTOCOL_FATALであり、表示dropやrequired setの縮小へ変換しない。
enum class PrerollTransitionError {
    None = 0,
    NotStarted,
    AlreadyStarted,
    InvalidTransition,
    InvalidArgument,
    ForeignAdmissionAfterClose,
    ForeignProgressAfterQuiescence,
    ActiveForeignTransactionRemains,
    PendingOpportunityNotFinalized,
    SchedulerCloseBeforeDrain,
    QuiescenceNotEvaluated,
    QuiescencePredicateFailed,
    CaptureEpochMismatch,
    RenderThreadMismatch,
    CurrentQueueStartBeforeQuiescence,
    MeasurementArmBeforeCurrentQueue,
    IssuanceBeforeMeasurementArm,
    CanonicalWindowNotFrozen,
    CanonicalWindowMutated,
    RetroactiveForeignOwner,
    CurrentPresentAsBoundary,
    HandshakeTimeout,
};

// quiescence checkのraw入力。1 render callback・同一capture epochで採取する。
// nearest QPC、latest Present、callback index、serial推定は入力にしない。
struct PrerollQuiescenceObservation {
    std::uint64_t captureEpoch = 0;
    std::uint32_t observerThreadId = 0;
    bool prerollAdmissionClosed = false;
    bool schedulerPendingRender = true;
    bool schedulerPendingQualifiedEvidence = true;
    bool schedulerPendingOpportunity = true;
    bool schedulerPendingOpportunityExactlyFinalized = false;
    long long queueActiveReservationCount = -1;
    bool joinActiveReservation = true;
    bool qtPendingCompositionToken = true;
    bool qtPendingFrameSwappedReceipt = true;
    long long issuedCount = -1;
    long long renderedCount = -1;
    long long qualifiedCommitCount = -1;
    long long dequeuedCount = -1;
    bool queueConservationValid = false;
    // issued prefixのうち、reservation -> token publication -> native Present ->
    // receipt -> qualified join -> swap commit/dequeueまでexactに閉じた件数。
    long long issuedPrefixExactIdentityClosedCount = -1;
    bool prerollScopeLedgerTerminalPartitionExact = false;
    long long transportFailureCounterTotal = -1;
};

// I4 PREROLL_TRANSACTION_FULLY_QUIESCENTの全predicateを個別fieldとして保存する。
// 1つでもfalseならcurrent queue startとmeasurement armを許可しない。
struct PrerollQuiescenceVerdict {
    bool evaluated = false;
    bool quiescent = false;
    std::uint64_t captureEpoch = 0;
    std::uint32_t observerThreadId = 0;
    bool sameCaptureEpoch = false;
    bool sameRenderThread = false;
    bool prerollAdmissionClosed = false;
    bool schedulerPendingRenderFalse = false;
    bool schedulerPendingQualifiedEvidenceFalse = false;
    bool schedulerPendingOpportunityFalseOrExactlyFinalized = false;
    bool queueActiveReservationCountZero = false;
    bool joinActiveReservationFalse = false;
    bool qtPendingCompositionTokenFalse = false;
    bool qtPendingFrameSwappedReceiptFalse = false;
    bool issuedEqualsRenderedEqualsQualifiedCommitEqualsDequeued = false;
    bool queueConservationValid = false;
    bool issuedPrefixExactIdentityClosed = false;
    bool prerollScopeLedgerTerminalPartitionExact = false;
    bool transportFailureCountersZero = false;
};

// exact boundary reservation候補。handshake中に実在するactive FOREIGN reservationへ
// だけ束縛する。完了済みFOREIGN Presentへの後付けもCURRENT scopeも拒否する。
struct PrerollBoundaryOwnerCandidate {
    std::uint64_t reservationId = 0;
    long long intentOrdinal = -1;
    std::uint64_t tokenSerial = 0;
    bool foreignPreMeasurementScope = false;
    bool transactionStillActive = false;
};

struct PrerollTransitionSnapshot {
    PrerollTransitionState state = PrerollTransitionState::Open;
    PrerollTransitionError error = PrerollTransitionError::None;
    bool started = false;
    std::uint64_t captureEpoch = 0;
    std::uint32_t renderThreadId = 0;
    bool foreignAdmissionOpen = false;
    long long activeForeignTransactionCount = 0;
    bool pendingOpportunityFinalized = false;
    bool foreignSchedulerClosed = false;
    bool boundaryOwnerBound = false;
    PrerollBoundaryOwnerCandidate boundaryOwner;
    bool currentIssuanceOpen = false;
    bool canonicalWindowFrozen = false;
    long long canonicalMeasurementStartQpc = 0;
    long long canonicalMeasurementEndQpc = 0;
    long long admissionCloseQpc = 0;
    long long quiescenceAckQpc = 0;
    // handshake waitはcanonical measurement windowの外である。
    long long handshakeWaitQpc = 0;
    bool waitChargedToMeasurementWindow = false;
    long long timeoutQpc = 0;
    long long quiescenceEvaluationCount = 0;
    PrerollQuiescenceVerdict verdict;
};

class PrerollTransitionHandshake {
public:
    bool begin(std::uint64_t captureEpoch, std::uint32_t renderThreadId, long long beginQpc,
               long long timeoutQpc);

    // admission closeはscheduler closeではない。新規FOREIGN reservationだけを禁止する。
    bool requestAdmissionClose(long long qpc);
    bool beginDrain(long long qpc);

    bool foreignAdmissionOpen() const;
    // 既存active FOREIGN transactionのrender/Present/receipt/join/commit/dequeue/
    // finalizeはadmission close後も許可する。
    bool foreignTransactionProgressAllowed() const;
    bool noteForeignReservationAdmitted(long long qpc);
    bool noteForeignTransactionTerminal(long long qpc);

    bool bindBoundaryOwner(const PrerollBoundaryOwnerCandidate& candidate);

    bool notePendingOpportunityFinalized();
    bool foreignSchedulerCloseAllowed() const;
    bool noteForeignSchedulerClosed();

    PrerollQuiescenceVerdict evaluateQuiescence(const PrerollQuiescenceObservation& observation,
                                                long long qpc);
    bool ackQuiescence(long long qpc);

    bool startCurrentRequiredQueue();
    bool armMeasurement(long long canonicalStartQpc, long long canonicalEndQpc);
    bool openCurrentIssuanceGate();

    bool currentIssuanceOpen() const;

    bool complete() const { return state_ == PrerollTransitionState::CurrentRunning; }

    PrerollTransitionState state() const { return state_; }

    PrerollTransitionError error() const { return error_; }

    const PrerollQuiescenceVerdict& verdict() const { return verdict_; }

    PrerollTransitionSnapshot snapshot() const;

private:
    bool fail(PrerollTransitionError error);
    bool checkTimeout(long long qpc);

    bool started_ = false;
    PrerollTransitionState state_ = PrerollTransitionState::Open;
    PrerollTransitionError error_ = PrerollTransitionError::None;
    std::uint64_t captureEpoch_ = 0;
    std::uint32_t renderThreadId_ = 0;
    bool foreignAdmissionClosed_ = false;
    long long activeForeignTransactionCount_ = 0;
    bool pendingOpportunityFinalized_ = false;
    bool foreignSchedulerClosed_ = false;
    bool boundaryOwnerBound_ = false;
    PrerollBoundaryOwnerCandidate boundaryOwner_{};
    bool currentIssuanceOpen_ = false;
    bool canonicalWindowFrozen_ = false;
    long long canonicalMeasurementStartQpc_ = 0;
    long long canonicalMeasurementEndQpc_ = 0;
    long long beginQpc_ = 0;
    long long admissionCloseQpc_ = 0;
    long long quiescenceAckQpc_ = 0;
    long long timeoutQpc_ = 0;
    long long quiescenceEvaluationCount_ = 0;
    PrerollQuiescenceVerdict verdict_{};
};

const char* prerollTransitionStateName(PrerollTransitionState state);
const char* prerollTransitionErrorName(PrerollTransitionError error);

} // namespace mvm::gpu

#endif

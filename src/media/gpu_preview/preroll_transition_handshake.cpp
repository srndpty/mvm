#include "media/gpu_preview/preroll_transition_handshake.h"

namespace mvm::gpu {
namespace {

bool inDrainPhase(PrerollTransitionState state) {
    return state == PrerollTransitionState::DrainRequested ||
           state == PrerollTransitionState::Draining ||
           state == PrerollTransitionState::QuiescenceCheck;
}

} // namespace

bool PrerollTransitionHandshake::fail(PrerollTransitionError error) {
    if (error_ == PrerollTransitionError::None)
        error_ = error;
    state_ = PrerollTransitionState::ProtocolFatal;
    return false;
}

bool PrerollTransitionHandshake::checkTimeout(long long qpc) {
    // timeoutはPROTOCOL_FATALであり、performance dropやrequired setの縮小へ変換しない。
    if (timeoutQpc_ <= 0 || admissionCloseQpc_ <= 0)
        return true;
    if (qpc - admissionCloseQpc_ > timeoutQpc_)
        return fail(PrerollTransitionError::HandshakeTimeout);
    return true;
}

bool PrerollTransitionHandshake::begin(std::uint64_t captureEpoch, std::uint32_t renderThreadId,
                                       long long beginQpc, long long timeoutQpc) {
    if (started_)
        return fail(PrerollTransitionError::AlreadyStarted);
    *this = {};
    if (captureEpoch == 0 || renderThreadId == 0 || beginQpc <= 0 || timeoutQpc <= 0)
        return fail(PrerollTransitionError::InvalidArgument);
    captureEpoch_ = captureEpoch;
    renderThreadId_ = renderThreadId;
    beginQpc_ = beginQpc;
    timeoutQpc_ = timeoutQpc;
    state_ = PrerollTransitionState::Open;
    started_ = true;
    return true;
}

bool PrerollTransitionHandshake::requestAdmissionClose(long long qpc) {
    if (!started_)
        return fail(PrerollTransitionError::NotStarted);
    if (error_ != PrerollTransitionError::None)
        return false;
    if (state_ != PrerollTransitionState::Open)
        return fail(PrerollTransitionError::InvalidTransition);
    if (qpc <= 0)
        return fail(PrerollTransitionError::InvalidArgument);
    // admission closeはscheduler closeではない。新規FOREIGN reservationだけを止める。
    foreignAdmissionClosed_ = true;
    admissionCloseQpc_ = qpc;
    state_ = PrerollTransitionState::DrainRequested;
    return true;
}

bool PrerollTransitionHandshake::beginDrain(long long qpc) {
    if (!started_)
        return fail(PrerollTransitionError::NotStarted);
    if (error_ != PrerollTransitionError::None)
        return false;
    if (state_ != PrerollTransitionState::DrainRequested)
        return fail(PrerollTransitionError::InvalidTransition);
    if (!checkTimeout(qpc))
        return false;
    state_ = PrerollTransitionState::Draining;
    return true;
}

bool PrerollTransitionHandshake::foreignAdmissionOpen() const {
    return started_ && error_ == PrerollTransitionError::None &&
           state_ == PrerollTransitionState::Open && !foreignAdmissionClosed_;
}

bool PrerollTransitionHandshake::foreignTransactionProgressAllowed() const {
    return started_ && error_ == PrerollTransitionError::None &&
           (state_ == PrerollTransitionState::Open || inDrainPhase(state_));
}

bool PrerollTransitionHandshake::noteForeignReservationAdmitted(long long qpc) {
    if (!started_)
        return fail(PrerollTransitionError::NotStarted);
    if (error_ != PrerollTransitionError::None)
        return false;
    if (!foreignAdmissionOpen())
        return fail(PrerollTransitionError::ForeignAdmissionAfterClose);
    if (qpc <= 0)
        return fail(PrerollTransitionError::InvalidArgument);
    ++activeForeignTransactionCount_;
    return true;
}

bool PrerollTransitionHandshake::noteForeignTransactionTerminal(long long qpc) {
    if (!started_)
        return fail(PrerollTransitionError::NotStarted);
    if (error_ != PrerollTransitionError::None)
        return false;
    // admission closeの後でも、既に発行済みtransactionの終端は許可する。
    if (!foreignTransactionProgressAllowed())
        return fail(PrerollTransitionError::ForeignProgressAfterQuiescence);
    if (qpc <= 0)
        return fail(PrerollTransitionError::InvalidArgument);
    if (activeForeignTransactionCount_ <= 0)
        return fail(PrerollTransitionError::InvalidTransition);
    --activeForeignTransactionCount_;
    return true;
}

bool PrerollTransitionHandshake::bindBoundaryOwner(const PrerollBoundaryOwnerCandidate& candidate) {
    if (!started_)
        return fail(PrerollTransitionError::NotStarted);
    if (error_ != PrerollTransitionError::None)
        return false;
    if (!inDrainPhase(state_) || boundaryOwnerBound_)
        return fail(PrerollTransitionError::InvalidTransition);
    if (!candidate.foreignPreMeasurementScope)
        return fail(PrerollTransitionError::CurrentPresentAsBoundary);
    // 既にcallback/commit済みのFOREIGN Presentへownerを後付けしない。
    if (!candidate.transactionStillActive || activeForeignTransactionCount_ <= 0)
        return fail(PrerollTransitionError::RetroactiveForeignOwner);
    if (candidate.reservationId == 0 || candidate.intentOrdinal < 0 || candidate.tokenSerial == 0)
        return fail(PrerollTransitionError::InvalidArgument);
    boundaryOwner_ = candidate;
    boundaryOwnerBound_ = true;
    return true;
}

bool PrerollTransitionHandshake::notePendingOpportunityFinalized() {
    if (!started_)
        return fail(PrerollTransitionError::NotStarted);
    if (error_ != PrerollTransitionError::None)
        return false;
    if (state_ != PrerollTransitionState::Draining)
        return fail(PrerollTransitionError::InvalidTransition);
    if (activeForeignTransactionCount_ != 0)
        return fail(PrerollTransitionError::ActiveForeignTransactionRemains);
    pendingOpportunityFinalized_ = true;
    return true;
}

bool PrerollTransitionHandshake::foreignSchedulerCloseAllowed() const {
    return started_ && error_ == PrerollTransitionError::None &&
           state_ == PrerollTransitionState::Draining && activeForeignTransactionCount_ == 0 &&
           pendingOpportunityFinalized_ && !foreignSchedulerClosed_;
}

bool PrerollTransitionHandshake::noteForeignSchedulerClosed() {
    if (!started_)
        return fail(PrerollTransitionError::NotStarted);
    if (error_ != PrerollTransitionError::None)
        return false;
    if (state_ != PrerollTransitionState::Draining || foreignSchedulerClosed_)
        return fail(PrerollTransitionError::InvalidTransition);
    // scheduler closeはactive transaction drainとpending opportunity finalizeの後だけ。
    if (activeForeignTransactionCount_ != 0)
        return fail(PrerollTransitionError::SchedulerCloseBeforeDrain);
    if (!pendingOpportunityFinalized_)
        return fail(PrerollTransitionError::PendingOpportunityNotFinalized);
    foreignSchedulerClosed_ = true;
    return true;
}

PrerollQuiescenceVerdict
PrerollTransitionHandshake::evaluateQuiescence(const PrerollQuiescenceObservation& observation,
                                               long long qpc) {
    if (!started_) {
        fail(PrerollTransitionError::NotStarted);
        return {};
    }
    if (error_ != PrerollTransitionError::None)
        return verdict_;
    if (state_ != PrerollTransitionState::Draining &&
        state_ != PrerollTransitionState::QuiescenceCheck) {
        fail(PrerollTransitionError::InvalidTransition);
        return {};
    }
    state_ = PrerollTransitionState::QuiescenceCheck;
    ++quiescenceEvaluationCount_;

    PrerollQuiescenceVerdict verdict;
    verdict.evaluated = true;
    verdict.captureEpoch = observation.captureEpoch;
    verdict.observerThreadId = observation.observerThreadId;
    verdict.sameCaptureEpoch =
        observation.captureEpoch != 0 && observation.captureEpoch == captureEpoch_;
    verdict.sameRenderThread =
        observation.observerThreadId != 0 && observation.observerThreadId == renderThreadId_;
    verdict.prerollAdmissionClosed = observation.prerollAdmissionClosed && foreignAdmissionClosed_;
    verdict.schedulerPendingRenderFalse = !observation.schedulerPendingRender;
    verdict.schedulerPendingQualifiedEvidenceFalse = !observation.schedulerPendingQualifiedEvidence;
    verdict.schedulerPendingOpportunityFalseOrExactlyFinalized =
        !observation.schedulerPendingOpportunity ||
        observation.schedulerPendingOpportunityExactlyFinalized;
    verdict.queueActiveReservationCountZero = observation.queueActiveReservationCount == 0;
    verdict.joinActiveReservationFalse = !observation.joinActiveReservation;
    verdict.qtPendingCompositionTokenFalse = !observation.qtPendingCompositionToken;
    verdict.qtPendingFrameSwappedReceiptFalse = !observation.qtPendingFrameSwappedReceipt;
    verdict.issuedEqualsRenderedEqualsQualifiedCommitEqualsDequeued =
        observation.issuedCount >= 0 && observation.issuedCount == observation.renderedCount &&
        observation.renderedCount == observation.qualifiedCommitCount &&
        observation.qualifiedCommitCount == observation.dequeuedCount;
    verdict.queueConservationValid = observation.queueConservationValid;
    verdict.issuedPrefixExactIdentityClosed =
        observation.issuedCount >= 0 &&
        observation.issuedPrefixExactIdentityClosedCount == observation.issuedCount;
    verdict.prerollScopeLedgerTerminalPartitionExact =
        observation.prerollScopeLedgerTerminalPartitionExact;
    verdict.transportFailureCountersZero = observation.transportFailureCounterTotal == 0;
    verdict.quiescent =
        verdict.sameCaptureEpoch && verdict.sameRenderThread && verdict.prerollAdmissionClosed &&
        verdict.schedulerPendingRenderFalse && verdict.schedulerPendingQualifiedEvidenceFalse &&
        verdict.schedulerPendingOpportunityFalseOrExactlyFinalized &&
        verdict.queueActiveReservationCountZero && verdict.joinActiveReservationFalse &&
        verdict.qtPendingCompositionTokenFalse && verdict.qtPendingFrameSwappedReceiptFalse &&
        verdict.issuedEqualsRenderedEqualsQualifiedCommitEqualsDequeued &&
        verdict.queueConservationValid && verdict.issuedPrefixExactIdentityClosed &&
        verdict.prerollScopeLedgerTerminalPartitionExact && verdict.transportFailureCountersZero;
    verdict_ = verdict;

    // epoch / render threadの不一致は待てば解消する条件ではない。即時fail-closeする。
    if (!verdict.sameCaptureEpoch) {
        fail(PrerollTransitionError::CaptureEpochMismatch);
        return verdict_;
    }
    if (!verdict.sameRenderThread) {
        fail(PrerollTransitionError::RenderThreadMismatch);
        return verdict_;
    }
    if (!verdict.quiescent) {
        state_ = PrerollTransitionState::Draining;
        checkTimeout(qpc);
    }
    return verdict_;
}

bool PrerollTransitionHandshake::ackQuiescence(long long qpc) {
    if (!started_)
        return fail(PrerollTransitionError::NotStarted);
    if (error_ != PrerollTransitionError::None)
        return false;
    // predicateが1つでもfalseならackは成立しない。stateより先に評価結果を見る。
    if (!verdict_.evaluated || !verdict_.quiescent)
        return fail(PrerollTransitionError::QuiescencePredicateFailed);
    if (state_ != PrerollTransitionState::QuiescenceCheck)
        return fail(PrerollTransitionError::InvalidTransition);
    if (!foreignSchedulerClosed_)
        return fail(PrerollTransitionError::SchedulerCloseBeforeDrain);
    if (qpc <= 0)
        return fail(PrerollTransitionError::InvalidArgument);
    quiescenceAckQpc_ = qpc;
    state_ = PrerollTransitionState::Quiescent;
    return true;
}

bool PrerollTransitionHandshake::startCurrentRequiredQueue() {
    if (!started_)
        return fail(PrerollTransitionError::NotStarted);
    if (error_ != PrerollTransitionError::None)
        return false;
    if (state_ != PrerollTransitionState::Quiescent)
        return fail(PrerollTransitionError::CurrentQueueStartBeforeQuiescence);
    // queueは初期化するだけで、issuance gateはまだ閉じたままにする。
    currentIssuanceOpen_ = false;
    state_ = PrerollTransitionState::CurrentReady;
    return true;
}

bool PrerollTransitionHandshake::armMeasurement(long long canonicalStartQpc,
                                                long long canonicalEndQpc) {
    if (!started_)
        return fail(PrerollTransitionError::NotStarted);
    if (error_ != PrerollTransitionError::None)
        return false;
    if (state_ != PrerollTransitionState::CurrentReady)
        return fail(PrerollTransitionError::MeasurementArmBeforeCurrentQueue);
    if (canonicalStartQpc <= 0 || canonicalEndQpc <= canonicalStartQpc)
        return fail(PrerollTransitionError::InvalidArgument);
    // handshake waitはcanonical measurement windowの外である。
    if (canonicalStartQpc < quiescenceAckQpc_)
        return fail(PrerollTransitionError::CanonicalWindowMutated);
    canonicalMeasurementStartQpc_ = canonicalStartQpc;
    canonicalMeasurementEndQpc_ = canonicalEndQpc;
    canonicalWindowFrozen_ = true;
    state_ = PrerollTransitionState::MeasurementArmed;
    return true;
}

bool PrerollTransitionHandshake::openCurrentIssuanceGate() {
    if (!started_)
        return fail(PrerollTransitionError::NotStarted);
    if (error_ != PrerollTransitionError::None)
        return false;
    if (state_ != PrerollTransitionState::MeasurementArmed)
        return fail(PrerollTransitionError::IssuanceBeforeMeasurementArm);
    if (!canonicalWindowFrozen_)
        return fail(PrerollTransitionError::CanonicalWindowNotFrozen);
    currentIssuanceOpen_ = true;
    state_ = PrerollTransitionState::CurrentRunning;
    return true;
}

bool PrerollTransitionHandshake::currentIssuanceOpen() const {
    return started_ && error_ == PrerollTransitionError::None &&
           state_ == PrerollTransitionState::CurrentRunning && currentIssuanceOpen_;
}

PrerollTransitionSnapshot PrerollTransitionHandshake::snapshot() const {
    PrerollTransitionSnapshot snapshot;
    snapshot.state = state_;
    snapshot.error = error_;
    snapshot.started = started_;
    snapshot.captureEpoch = captureEpoch_;
    snapshot.renderThreadId = renderThreadId_;
    snapshot.foreignAdmissionOpen = foreignAdmissionOpen();
    snapshot.activeForeignTransactionCount = activeForeignTransactionCount_;
    snapshot.pendingOpportunityFinalized = pendingOpportunityFinalized_;
    snapshot.foreignSchedulerClosed = foreignSchedulerClosed_;
    snapshot.boundaryOwnerBound = boundaryOwnerBound_;
    snapshot.boundaryOwner = boundaryOwner_;
    snapshot.currentIssuanceOpen = currentIssuanceOpen();
    snapshot.canonicalWindowFrozen = canonicalWindowFrozen_;
    snapshot.canonicalMeasurementStartQpc = canonicalMeasurementStartQpc_;
    snapshot.canonicalMeasurementEndQpc = canonicalMeasurementEndQpc_;
    snapshot.admissionCloseQpc = admissionCloseQpc_;
    snapshot.quiescenceAckQpc = quiescenceAckQpc_;
    snapshot.handshakeWaitQpc = quiescenceAckQpc_ > 0 && admissionCloseQpc_ > 0
                                    ? quiescenceAckQpc_ - admissionCloseQpc_
                                    : 0;
    snapshot.waitChargedToMeasurementWindow = false;
    snapshot.timeoutQpc = timeoutQpc_;
    snapshot.quiescenceEvaluationCount = quiescenceEvaluationCount_;
    snapshot.verdict = verdict_;
    return snapshot;
}

const char* prerollTransitionStateName(PrerollTransitionState state) {
    switch (state) {
    case PrerollTransitionState::Open:
        return "OPEN";
    case PrerollTransitionState::DrainRequested:
        return "DRAIN_REQUESTED";
    case PrerollTransitionState::Draining:
        return "DRAINING";
    case PrerollTransitionState::QuiescenceCheck:
        return "QUIESCENCE_CHECK";
    case PrerollTransitionState::Quiescent:
        return "QUIESCENT";
    case PrerollTransitionState::CurrentReady:
        return "CURRENT_READY";
    case PrerollTransitionState::MeasurementArmed:
        return "MEASUREMENT_ARMED";
    case PrerollTransitionState::CurrentRunning:
        return "CURRENT_RUNNING";
    case PrerollTransitionState::ProtocolFatal:
        return "PROTOCOL_FATAL";
    }
    return "UNKNOWN";
}

const char* prerollTransitionErrorName(PrerollTransitionError error) {
    switch (error) {
    case PrerollTransitionError::None:
        return "NONE";
    case PrerollTransitionError::NotStarted:
        return "NOT_STARTED";
    case PrerollTransitionError::AlreadyStarted:
        return "ALREADY_STARTED";
    case PrerollTransitionError::InvalidTransition:
        return "INVALID_TRANSITION";
    case PrerollTransitionError::InvalidArgument:
        return "INVALID_ARGUMENT";
    case PrerollTransitionError::ForeignAdmissionAfterClose:
        return "FOREIGN_ADMISSION_AFTER_CLOSE";
    case PrerollTransitionError::ForeignProgressAfterQuiescence:
        return "FOREIGN_PROGRESS_AFTER_QUIESCENCE";
    case PrerollTransitionError::ActiveForeignTransactionRemains:
        return "ACTIVE_FOREIGN_TRANSACTION_REMAINS";
    case PrerollTransitionError::PendingOpportunityNotFinalized:
        return "PENDING_OPPORTUNITY_NOT_FINALIZED";
    case PrerollTransitionError::SchedulerCloseBeforeDrain:
        return "SCHEDULER_CLOSE_BEFORE_DRAIN";
    case PrerollTransitionError::QuiescenceNotEvaluated:
        return "QUIESCENCE_NOT_EVALUATED";
    case PrerollTransitionError::QuiescencePredicateFailed:
        return "QUIESCENCE_PREDICATE_FAILED";
    case PrerollTransitionError::CaptureEpochMismatch:
        return "CAPTURE_EPOCH_MISMATCH";
    case PrerollTransitionError::RenderThreadMismatch:
        return "RENDER_THREAD_MISMATCH";
    case PrerollTransitionError::CurrentQueueStartBeforeQuiescence:
        return "CURRENT_QUEUE_START_BEFORE_QUIESCENCE";
    case PrerollTransitionError::MeasurementArmBeforeCurrentQueue:
        return "MEASUREMENT_ARM_BEFORE_CURRENT_QUEUE";
    case PrerollTransitionError::IssuanceBeforeMeasurementArm:
        return "ISSUANCE_BEFORE_MEASUREMENT_ARM";
    case PrerollTransitionError::CanonicalWindowNotFrozen:
        return "CANONICAL_WINDOW_NOT_FROZEN";
    case PrerollTransitionError::CanonicalWindowMutated:
        return "CANONICAL_WINDOW_MUTATED";
    case PrerollTransitionError::RetroactiveForeignOwner:
        return "RETROACTIVE_FOREIGN_OWNER";
    case PrerollTransitionError::CurrentPresentAsBoundary:
        return "CURRENT_PRESENT_AS_BOUNDARY";
    case PrerollTransitionError::HandshakeTimeout:
        return "HANDSHAKE_TIMEOUT";
    }
    return "UNKNOWN";
}

} // namespace mvm::gpu

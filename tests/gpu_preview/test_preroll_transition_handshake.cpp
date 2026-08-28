#include "media/gpu_preview/preroll_transition_handshake.h"

#include <cstdio>
#include <string>

namespace {

using Handshake = mvm::gpu::PrerollTransitionHandshake;
using State = mvm::gpu::PrerollTransitionState;
using Error = mvm::gpu::PrerollTransitionError;
using Observation = mvm::gpu::PrerollQuiescenceObservation;

constexpr std::uint64_t kEpoch = 7;
constexpr std::uint32_t kRenderThread = 9692;
constexpr long long kTimeout = 10000;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

// 全predicateがtrueになるquiescent observation。negativeは1 fieldだけを壊す。
Observation quiescentObservation() {
    Observation observation;
    observation.captureEpoch = kEpoch;
    observation.observerThreadId = kRenderThread;
    observation.prerollAdmissionClosed = true;
    observation.schedulerPendingRender = false;
    observation.schedulerPendingQualifiedEvidence = false;
    observation.schedulerPendingOpportunity = false;
    observation.schedulerPendingOpportunityExactlyFinalized = true;
    observation.queueActiveReservationCount = 0;
    observation.joinActiveReservation = false;
    observation.qtPendingCompositionToken = false;
    observation.qtPendingFrameSwappedReceipt = false;
    observation.issuedCount = 2;
    observation.renderedCount = 2;
    observation.qualifiedCommitCount = 2;
    observation.dequeuedCount = 2;
    observation.queueConservationValid = true;
    observation.issuedPrefixExactIdentityClosedCount = 2;
    observation.prerollScopeLedgerTerminalPartitionExact = true;
    observation.transportFailureCounterTotal = 0;
    return observation;
}

// admission close -> drain -> finalize -> scheduler closeまで進める。
bool drainToSchedulerClosed(Handshake& handshake, long long qpc) {
    return handshake.requestAdmissionClose(qpc) && handshake.beginDrain(qpc) &&
           handshake.notePendingOpportunityFinalized() && handshake.noteForeignSchedulerClosed();
}

void positiveZeroActiveForeignTransaction() {
    Handshake handshake;
    check(handshake.begin(kEpoch, kRenderThread, 100, kTimeout), "handshakeを開始できません");
    check(handshake.state() == State::Open && handshake.foreignAdmissionOpen(),
          "初期stateがOPEN/admission openではありません");
    check(handshake.requestAdmissionClose(200) && handshake.state() == State::DrainRequested,
          "admission closeでDRAIN_REQUESTEDへ遷移しません");
    check(!handshake.foreignAdmissionOpen(), "admission closeが新規FOREIGN発行を止めていません");
    check(handshake.foreignTransactionProgressAllowed(),
          "admission close後にexisting FOREIGN transactionの前進を禁止しました");
    check(handshake.beginDrain(210) && handshake.state() == State::Draining,
          "DRAININGへ遷移できません");
    check(!handshake.foreignSchedulerCloseAllowed(),
          "pending opportunity finalize前にscheduler closeを許可しました");
    check(handshake.notePendingOpportunityFinalized() && handshake.foreignSchedulerCloseAllowed() &&
              handshake.noteForeignSchedulerClosed(),
          "drain後のfinalize -> scheduler closeに失敗しました");
    const auto verdict = handshake.evaluateQuiescence(quiescentObservation(), 220);
    check(handshake.state() == State::QuiescenceCheck, "QUIESCENCE_CHECKへ遷移しません");
    check(verdict.evaluated && verdict.quiescent && verdict.sameCaptureEpoch &&
              verdict.sameRenderThread,
          "0件drainでquiescentになりません");
    check(!handshake.currentIssuanceOpen(), "ack前にcurrent issuanceが開いています");
    check(handshake.ackQuiescence(230) && handshake.state() == State::Quiescent,
          "quiescence ackに失敗しました");
    check(handshake.startCurrentRequiredQueue() && handshake.state() == State::CurrentReady,
          "CURRENT_READYへ遷移できません");
    check(!handshake.currentIssuanceOpen(),
          "current queue初期化だけでissuance gateが開いています");
    check(handshake.armMeasurement(240, 1240) && handshake.state() == State::MeasurementArmed,
          "measurement armに失敗しました");
    check(!handshake.currentIssuanceOpen(), "measurement arm直後にissuance gateが開いています");
    check(handshake.openCurrentIssuanceGate() && handshake.state() == State::CurrentRunning &&
              handshake.currentIssuanceOpen() && handshake.complete(),
          "CURRENT_RUNNINGへ遷移できません");
    const auto snapshot = handshake.snapshot();
    check(snapshot.canonicalWindowFrozen && snapshot.canonicalMeasurementStartQpc == 240 &&
              snapshot.canonicalMeasurementEndQpc == 1240,
          "canonical start/end authorityがfreezeされていません");
    check(snapshot.handshakeWaitQpc == 30 && !snapshot.waitChargedToMeasurementWindow,
          "handshake waitがcanonical measurement windowへ算入されています");
    check(snapshot.error == Error::None, "positive handshakeでerrorが立ちました");
}

void positiveSingleForeignTransactionDrain() {
    Handshake handshake;
    check(handshake.begin(kEpoch, kRenderThread, 100, kTimeout), "handshakeを開始できません");
    check(handshake.noteForeignReservationAdmitted(110),
          "admission open中のFOREIGN reservationを拒否しました");
    check(handshake.requestAdmissionClose(200) && handshake.beginDrain(200),
          "admission close/drain開始に失敗しました");
    check(!handshake.noteForeignReservationAdmitted(205) &&
              handshake.error() == Error::ForeignAdmissionAfterClose,
          "admission close後の新規FOREIGN reservationを許可しました");

    Handshake draining;
    check(draining.begin(kEpoch, kRenderThread, 100, kTimeout), "drain handshakeを開始できません");
    check(draining.noteForeignReservationAdmitted(110), "FOREIGN reservationを記録できません");
    check(draining.requestAdmissionClose(200) && draining.beginDrain(200),
          "drainを開始できません");
    Observation inflight = quiescentObservation();
    inflight.schedulerPendingRender = true;
    inflight.joinActiveReservation = true;
    inflight.queueActiveReservationCount = 1;
    inflight.renderedCount = 1;
    inflight.qualifiedCommitCount = 1;
    inflight.dequeuedCount = 1;
    const auto pending = draining.evaluateQuiescence(inflight, 210);
    check(pending.evaluated && !pending.quiescent && !pending.schedulerPendingRenderFalse &&
              !pending.joinActiveReservationFalse && !pending.queueActiveReservationCountZero,
          "in-flight FOREIGN transactionをquiescentと判定しました");
    check(draining.state() == State::Draining && draining.error() == Error::None,
          "未成立quiescenceがfatalになりました");
    check(!draining.startCurrentRequiredQueue() &&
              draining.error() == Error::CurrentQueueStartBeforeQuiescence,
          "quiescence未成立でcurrent queue startを許可しました");

    Handshake terminal;
    check(terminal.begin(kEpoch, kRenderThread, 100, kTimeout), "terminal handshake開始失敗");
    check(terminal.noteForeignReservationAdmitted(110), "FOREIGN reservationを記録できません");
    check(terminal.requestAdmissionClose(200) && terminal.beginDrain(200), "drain開始失敗");
    check(!terminal.notePendingOpportunityFinalized() &&
              terminal.error() == Error::ActiveForeignTransactionRemains,
          "active transaction残存中にpending opportunity finalizeを許可しました");

    Handshake drained;
    check(drained.begin(kEpoch, kRenderThread, 100, kTimeout), "drained handshake開始失敗");
    check(drained.noteForeignReservationAdmitted(110), "FOREIGN reservationを記録できません");
    check(drained.requestAdmissionClose(200) && drained.beginDrain(200), "drain開始失敗");
    // admission close後もexisting transactionのcommit/dequeue/finalizeは許可する。
    check(drained.noteForeignTransactionTerminal(215),
          "admission close後のFOREIGN transaction終端を拒否しました");
    check(drained.notePendingOpportunityFinalized() && drained.noteForeignSchedulerClosed(),
          "drain完了後のfinalize/scheduler closeに失敗しました");
    const auto verdict = drained.evaluateQuiescence(quiescentObservation(), 220);
    check(verdict.quiescent, "1件drain後にquiescentになりません");
    check(drained.ackQuiescence(230) && drained.startCurrentRequiredQueue() &&
              drained.armMeasurement(240, 1240) && drained.openCurrentIssuanceGate(),
          "1件drain後のhandshakeを完了できません");
}

// 13 predicateそれぞれのfailureがcurrent queue start / measurement armを拒否する。
void negativeQuiescencePredicateFailures() {
    struct Case {
        const char* name;
        void (*mutate)(Observation&);
        bool (*field)(const mvm::gpu::PrerollQuiescenceVerdict&);
    };
    static const Case cases[] = {
        {"PREROLL_ADMISSION_CLOSED", [](Observation& o) { o.prerollAdmissionClosed = false; },
         [](const mvm::gpu::PrerollQuiescenceVerdict& v) { return v.prerollAdmissionClosed; }},
        {"SCHEDULER_PENDING_RENDER_FALSE", [](Observation& o) { o.schedulerPendingRender = true; },
         [](const mvm::gpu::PrerollQuiescenceVerdict& v) { return v.schedulerPendingRenderFalse; }},
        {"SCHEDULER_PENDING_QUALIFIED_EVIDENCE_FALSE",
         [](Observation& o) { o.schedulerPendingQualifiedEvidence = true; },
         [](const mvm::gpu::PrerollQuiescenceVerdict& v) {
             return v.schedulerPendingQualifiedEvidenceFalse;
         }},
        {"SCHEDULER_PENDING_OPPORTUNITY_FALSE_OR_EXACTLY_FINALIZED",
         [](Observation& o) {
             o.schedulerPendingOpportunity = true;
             o.schedulerPendingOpportunityExactlyFinalized = false;
         },
         [](const mvm::gpu::PrerollQuiescenceVerdict& v) {
             return v.schedulerPendingOpportunityFalseOrExactlyFinalized;
         }},
        {"QUEUE_ACTIVE_RESERVATION_COUNT_ZERO",
         [](Observation& o) { o.queueActiveReservationCount = 1; },
         [](const mvm::gpu::PrerollQuiescenceVerdict& v) {
             return v.queueActiveReservationCountZero;
         }},
        {"JOIN_ACTIVE_RESERVATION_FALSE", [](Observation& o) { o.joinActiveReservation = true; },
         [](const mvm::gpu::PrerollQuiescenceVerdict& v) { return v.joinActiveReservationFalse; }},
        {"QT_PENDING_COMPOSITION_TOKEN_FALSE",
         [](Observation& o) { o.qtPendingCompositionToken = true; },
         [](const mvm::gpu::PrerollQuiescenceVerdict& v) {
             return v.qtPendingCompositionTokenFalse;
         }},
        {"QT_PENDING_FRAME_SWAPPED_RECEIPT_FALSE",
         [](Observation& o) { o.qtPendingFrameSwappedReceipt = true; },
         [](const mvm::gpu::PrerollQuiescenceVerdict& v) {
             return v.qtPendingFrameSwappedReceiptFalse;
         }},
        {"ISSUED_EQUALS_RENDERED_EQUALS_QUALIFIED_COMMIT_EQUALS_DEQUEUED",
         [](Observation& o) { o.dequeuedCount = 1; },
         [](const mvm::gpu::PrerollQuiescenceVerdict& v) {
             return v.issuedEqualsRenderedEqualsQualifiedCommitEqualsDequeued;
         }},
        {"QUEUE_CONSERVATION_VALID", [](Observation& o) { o.queueConservationValid = false; },
         [](const mvm::gpu::PrerollQuiescenceVerdict& v) { return v.queueConservationValid; }},
        {"ISSUED_PREFIX_EXACT_IDENTITY_CLOSED",
         [](Observation& o) { o.issuedPrefixExactIdentityClosedCount = 1; },
         [](const mvm::gpu::PrerollQuiescenceVerdict& v) {
             return v.issuedPrefixExactIdentityClosed;
         }},
        {"PREROLL_SCOPE_LEDGER_TERMINAL_PARTITION_EXACT",
         [](Observation& o) { o.prerollScopeLedgerTerminalPartitionExact = false; },
         [](const mvm::gpu::PrerollQuiescenceVerdict& v) {
             return v.prerollScopeLedgerTerminalPartitionExact;
         }},
        {"TRANSPORT_FAILURE_COUNTERS_ZERO",
         [](Observation& o) { o.transportFailureCounterTotal = 1; },
         [](const mvm::gpu::PrerollQuiescenceVerdict& v) { return v.transportFailureCountersZero; }},
    };
    for (const auto& testCase : cases) {
        Handshake handshake;
        handshake.begin(kEpoch, kRenderThread, 100, kTimeout);
        drainToSchedulerClosed(handshake, 200);
        Observation observation = quiescentObservation();
        testCase.mutate(observation);
        const auto verdict = handshake.evaluateQuiescence(observation, 210);
        const std::string label = std::string("quiescence predicate: ") + testCase.name;
        check(verdict.evaluated && !verdict.quiescent, (label + " がquiescentのままです").c_str());
        check(!testCase.field(verdict), (label + " のfieldがfalseになりません").c_str());
        check(!handshake.ackQuiescence(220) && handshake.error() == Error::QuiescencePredicateFailed,
              (label + " でackを許可しました").c_str());
        check(!handshake.startCurrentRequiredQueue(),
              (label + " でcurrent queue startを許可しました").c_str());
        check(!handshake.armMeasurement(230, 1230),
              (label + " でmeasurement armを許可しました").c_str());
    }
}

void negativeMixedEpochAndThread() {
    {
        Handshake handshake;
        handshake.begin(kEpoch, kRenderThread, 100, kTimeout);
        drainToSchedulerClosed(handshake, 200);
        Observation observation = quiescentObservation();
        observation.captureEpoch = kEpoch + 1;
        const auto verdict = handshake.evaluateQuiescence(observation, 210);
        check(!verdict.sameCaptureEpoch && !verdict.quiescent &&
                  handshake.error() == Error::CaptureEpochMismatch &&
                  handshake.state() == State::ProtocolFatal,
              "別capture epochのsnapshotをPROTOCOL_FATALにしていません");
    }
    {
        Handshake handshake;
        handshake.begin(kEpoch, kRenderThread, 100, kTimeout);
        drainToSchedulerClosed(handshake, 200);
        Observation observation = quiescentObservation();
        observation.observerThreadId = kRenderThread + 1;
        const auto verdict = handshake.evaluateQuiescence(observation, 210);
        check(!verdict.sameRenderThread && !verdict.quiescent &&
                  handshake.error() == Error::RenderThreadMismatch &&
                  handshake.state() == State::ProtocolFatal,
              "別render threadのsnapshotをPROTOCOL_FATALにしていません");
    }
}

void negativeOrderingMutations() {
    {
        Handshake handshake;
        handshake.begin(kEpoch, kRenderThread, 100, kTimeout);
        handshake.requestAdmissionClose(200);
        handshake.beginDrain(200);
        check(!handshake.startCurrentRequiredQueue() &&
                  handshake.error() == Error::CurrentQueueStartBeforeQuiescence,
              "quiescence ack前のcurrent queue startを許可しました");
    }
    {
        Handshake handshake;
        handshake.begin(kEpoch, kRenderThread, 100, kTimeout);
        drainToSchedulerClosed(handshake, 200);
        handshake.evaluateQuiescence(quiescentObservation(), 210);
        handshake.ackQuiescence(220);
        check(!handshake.armMeasurement(230, 1230) &&
                  handshake.error() == Error::MeasurementArmBeforeCurrentQueue,
              "current queue start前のmeasurement armを許可しました");
    }
    {
        Handshake handshake;
        handshake.begin(kEpoch, kRenderThread, 100, kTimeout);
        drainToSchedulerClosed(handshake, 200);
        handshake.evaluateQuiescence(quiescentObservation(), 210);
        handshake.ackQuiescence(220);
        handshake.startCurrentRequiredQueue();
        check(!handshake.openCurrentIssuanceGate() &&
                  handshake.error() == Error::IssuanceBeforeMeasurementArm,
              "measurement arm前のissuance gate openを許可しました");
    }
    {
        Handshake handshake;
        handshake.begin(kEpoch, kRenderThread, 100, kTimeout);
        handshake.requestAdmissionClose(200);
        handshake.beginDrain(200);
        handshake.noteForeignReservationAdmitted(205);
        check(handshake.error() == Error::ForeignAdmissionAfterClose,
              "admission close後の新規FOREIGN reservationを許可しました");
    }
    {
        Handshake handshake;
        handshake.begin(kEpoch, kRenderThread, 100, kTimeout);
        handshake.noteForeignReservationAdmitted(110);
        handshake.requestAdmissionClose(200);
        handshake.beginDrain(200);
        check(!handshake.noteForeignSchedulerClosed() &&
                  handshake.error() == Error::SchedulerCloseBeforeDrain,
              "active transaction drain前のscheduler closeを許可しました");
    }
    {
        Handshake handshake;
        handshake.begin(kEpoch, kRenderThread, 100, kTimeout);
        handshake.requestAdmissionClose(200);
        handshake.beginDrain(200);
        check(!handshake.noteForeignSchedulerClosed() &&
                  handshake.error() == Error::PendingOpportunityNotFinalized,
              "pending opportunity finalize前のscheduler closeを許可しました");
    }
    {
        Handshake handshake;
        handshake.begin(kEpoch, kRenderThread, 100, kTimeout);
        drainToSchedulerClosed(handshake, 200);
        handshake.evaluateQuiescence(quiescentObservation(), 210);
        handshake.ackQuiescence(230);
        handshake.startCurrentRequiredQueue();
        check(!handshake.armMeasurement(220, 1220) &&
                  handshake.error() == Error::CanonicalWindowMutated,
              "handshake wait区間をcanonical measurement windowへ算入しました");
    }
}

void negativeBoundaryOwnerMutations() {
    {
        Handshake handshake;
        handshake.begin(kEpoch, kRenderThread, 100, kTimeout);
        handshake.noteForeignReservationAdmitted(110);
        handshake.requestAdmissionClose(200);
        handshake.beginDrain(200);
        check(handshake.bindBoundaryOwner({3, 0, 84, true, true}),
              "active FOREIGN reservationへのboundary owner束縛を拒否しました");
        check(handshake.snapshot().boundaryOwnerBound, "boundary ownerが記録されていません");
    }
    {
        Handshake handshake;
        handshake.begin(kEpoch, kRenderThread, 100, kTimeout);
        handshake.noteForeignReservationAdmitted(110);
        handshake.requestAdmissionClose(200);
        handshake.beginDrain(200);
        check(!handshake.bindBoundaryOwner({3, 0, 84, false, true}) &&
                  handshake.error() == Error::CurrentPresentAsBoundary,
              "CURRENT Presentをboundaryとして受理しました");
    }
    {
        Handshake handshake;
        handshake.begin(kEpoch, kRenderThread, 100, kTimeout);
        handshake.noteForeignReservationAdmitted(110);
        handshake.requestAdmissionClose(200);
        handshake.beginDrain(200);
        handshake.noteForeignTransactionTerminal(210);
        check(!handshake.bindBoundaryOwner({3, 0, 84, true, false}) &&
                  handshake.error() == Error::RetroactiveForeignOwner,
              "完了済みFOREIGN Presentへownerを後付けしました");
    }
}

void negativeTimeoutIsProtocolFatal() {
    Handshake handshake;
    handshake.begin(kEpoch, kRenderThread, 100, 50);
    handshake.requestAdmissionClose(200);
    handshake.beginDrain(200);
    Observation observation = quiescentObservation();
    observation.schedulerPendingRender = true;
    const auto verdict = handshake.evaluateQuiescence(observation, 400);
    check(!verdict.quiescent, "timeout runでquiescentになりました");
    check(handshake.error() == Error::HandshakeTimeout && handshake.state() == State::ProtocolFatal,
          "timeoutをPROTOCOL_FATALにしていません");
    check(std::string(mvm::gpu::prerollTransitionErrorName(handshake.error())) ==
              "HANDSHAKE_TIMEOUT",
          "timeout error名が固定されていません");
    check(!handshake.startCurrentRequiredQueue() && !handshake.armMeasurement(500, 1500) &&
              !handshake.openCurrentIssuanceGate() && !handshake.currentIssuanceOpen(),
          "timeout後にcurrent issuanceへ進みました");
    const auto snapshot = handshake.snapshot();
    check(!snapshot.waitChargedToMeasurementWindow && !snapshot.currentIssuanceOpen,
          "timeout waitをmeasurement windowへ算入しました");
}

} // namespace

int main() {
    positiveZeroActiveForeignTransaction();
    positiveSingleForeignTransactionDrain();
    negativeQuiescencePredicateFailures();
    negativeMixedEpochAndThread();
    negativeOrderingMutations();
    negativeBoundaryOwnerMutations();
    negativeTimeoutIsProtocolFatal();
    std::fprintf(stderr, "P2-D5-2/B3-I5B preroll transition handshake: 失敗 %d件\n", failures);
    return failures == 0 ? 0 : 1;
}

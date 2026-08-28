// B3-I6B deferred integration-negative。
//
// production の CompositionTokenPublication / formalPresentJoinAdmission /
// PresentationOpportunityScheduler をそのまま実行する。test 側で publication logic を
// 複製しない。injection authority は「publication 直前に protocol fatal を立てる」ことだけで、
// token serial / present serial / QPC / VBlank ordinal / config override は一切偽造しない。
#include "app/preview/composition_token_publication.h"
#include "app/preview/formal_present_join_admission.h"
#include "media/gpu_preview/presentation_opportunity_scheduler.h"

#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

// 唯一の test seam。publication 直前に protocol fatal を立てる以外の注入口を持たない。
class RecordingSink final : public mvm::app::CompositionTokenPublicationSink {
public:
    bool protocolFatalLatched() const override { return fatalLatched; }

    bool terminalExitTracking() const override { return terminalTracking; }

    void noteDestructorEntered() override { ++destructorEnteredCount; }

    void noteDestructorComplete() override { ++destructorCompleteCount; }

    bool publishCompositionToken(const MvmNativePresentCompositionToken& token) override {
        ++publishCallCount;
        publishedTokenSerial = token.tokenSerial;
        return publishSucceeds;
    }

    void notePublicationAttempt(bool succeeded, const MvmNativePresentCompositionToken&) override {
        ++publicationAttemptCount;
        lastAttemptSucceeded = succeeded;
    }

    void notePublicationFailure() override { ++publicationFailureCount; }

    void noteSuppressedBeforePresent() override { ++suppressedBeforePresentCount; }

    // TEST ONLY: publication 直前の protocol fatal latch。first fatal だけを保持する。
    void injectProtocolFatal(const char* reason) {
        fatalLatched = true;
        if (fatalReason == nullptr)
            fatalReason = reason;
    }

    bool fatalLatched = false;
    bool terminalTracking = true;
    bool publishSucceeds = true;
    bool lastAttemptSucceeded = false;
    const char* fatalReason = nullptr;
    int publishCallCount = 0;
    int publicationAttemptCount = 0;
    int publicationFailureCount = 0;
    int suppressedBeforePresentCount = 0;
    int destructorEnteredCount = 0;
    int destructorCompleteCount = 0;
    std::uint64_t publishedTokenSerial = 0;
};

MvmNativePresentCompositionToken tokenWithSerial(std::uint64_t serial) {
    MvmNativePresentCompositionToken token{};
    token.tokenSerial = serial;
    token.outputFrameNumber = 41;
    return token;
}

// production の admission 判定に従って frameSwapped callback の到達点を数える。
// 判定そのものは formalPresentJoinAdmission() が持ち、ここでは複製しない。
struct FrameSwappedReach {
    int takeReceiptCalls = 0;
    int bindCalls = 0;
    int commitCalls = 0;
};

FrameSwappedReach runFrameSwappedCallback(const mvm::app::FormalPresentJoinAdmissionInput& input,
                                          mvm::gpu::PresentationOpportunityScheduler& scheduler,
                                          std::uint64_t reservationId, long long intentOrdinal) {
    const auto admission = mvm::app::formalPresentJoinAdmission(input);
    FrameSwappedReach reach;
    if (admission.formalEnvelopeActive)
        ++reach.takeReceiptCalls; // exact receipt の consume 経路
    if (admission.joinCommitAllowed) {
        ++reach.bindCalls;
        ++reach.commitCalls;
        scheduler.commitQualifiedPresent(reservationId, intentOrdinal);
    }
    return reach;
}

// positive control。fatal が無ければ production destructor は 1 回だけ publish する。
void publicationHappensWithoutFatal() {
    RecordingSink sink;
    const auto token = tokenWithSerial(3930);
    {
        mvm::app::CompositionTokenPublication publication(sink, true, token);
        publication.setTokenValid(true);
        check(publication.publicationAllowed(), "fatal無しでpublicationがgateされました");
    }
    check(sink.publishCallCount == 1 && sink.publishedTokenSerial == 3930,
          "fatal無しのpublicationが1回行われていません");
    check(sink.suppressedBeforePresentCount == 0, "fatal無しで抑止counterが増えました");
    check(sink.publicationAttemptCount == 1 && sink.lastAttemptSucceeded,
          "publication attributionが記録されていません");
    check(sink.destructorEnteredCount == 1 && sink.destructorCompleteCount == 1,
          "terminal exit stageがbracketされていません");
}

// §21.1 の 9 段 acceptance chain。
void prePublicationFatalIsAtomic() {
    // fixture: 既に reservation 済みの transaction を持つ production scheduler。
    mvm::gpu::PresentationOpportunityScheduler scheduler;
    check(scheduler.start({8, 0, 60, 1, 60, 1, 600, false, 60, 1}),
          "injection fixtureのschedulerを開始できません");
    const auto decision = scheduler.selectForRender(1, {true, 100, 1000, 60, 1}, 0);
    check(decision.valid && decision.reservationId != 0, "fixture reservationを確立できません");
    check(scheduler.markRenderComplete(5, decision.targetFrame, decision.renderOrdinal),
          "fixture render完了を記録できません");
    const auto beforeQueue = scheduler.snapshot().requiredIntentQueue;

    RecordingSink sink;
    const auto token = tokenWithSerial(3930);
    int publishCallsAtInjection = -1;
    {
        // 1. capture active / valid
        mvm::app::CompositionTokenPublication publication(sink, true, token);
        publication.setTokenValid(true);
        check(publication.publicationAllowed(), "injection前にcaptureがactive/validではありません");

        // 2. token publication 前に protocol fatal を注入する
        sink.injectProtocolFatal("SOURCE_COVERAGE_INSUFFICIENT");
        publishCallsAtInjection = sink.publishCallCount;
        check(!publication.publicationAllowed(), "fatal注入後もpublicationが許可されています");
        // 3. ここで実際の destructor が走る
    }
    check(publishCallsAtInjection == 0, "fatal注入時点で既にpublishされていました");

    // 4. setCompositionToken call count == 0
    check(sink.publishCallCount == 0, "fatal後にformal tokenがpublishされました");
    // 5. suppressed_before_present_count == 1
    check(sink.suppressedBeforePresentCount == 1, "publication抑止が1回記録されていません");
    check(sink.publicationAttemptCount == 0 && sink.publicationFailureCount == 0,
          "抑止をpublication attempt / transport failureとして数えました");
    check(sink.destructorEnteredCount == 1 && sink.destructorCompleteCount == 1,
          "抑止経路でterminal exit stageがbracketされていません");

    // 6-7. post-fatal frameSwapped は formal transaction にならない
    const auto reach =
        runFrameSwappedCallback({true, true, sink.protocolFatalLatched(), false, true}, scheduler,
                                decision.reservationId, decision.opportunityOrdinal);
    check(reach.takeReceiptCalls == 0, "post-fatalでreceipt takeへ到達しました");
    check(reach.bindCalls == 0, "post-fatalでbind native presentへ到達しました");
    check(reach.commitCalls == 0, "post-fatalでcommit frameSwappedへ到達しました");

    // 8. first fatal が authority のまま
    const char* fatalBefore = sink.fatalReason;
    sink.injectProtocolFatal("RENDER_COMPLETION_MISSING");
    check(sink.fatalReason == fatalBefore &&
              std::strcmp(sink.fatalReason, "SOURCE_COVERAGE_INSUFFICIENT") == 0,
          "post-fatal diagnosticsがfirst protocol fatalを上書きしました");

    // 9. queue は dequeue / rollback しない
    const auto afterQueue = scheduler.snapshot().requiredIntentQueue;
    check(afterQueue.issuedCount == beforeQueue.issuedCount &&
              afterQueue.renderedCount == beforeQueue.renderedCount &&
              afterQueue.qualifiedCommitCount == beforeQueue.qualifiedCommitCount &&
              afterQueue.dequeuedCount == beforeQueue.dequeuedCount,
          "post-fatalでqueue counterが前進しました");
    check(afterQueue.activeReservationCount == beforeQueue.activeReservationCount &&
              afterQueue.activeReservation.reservationId ==
                  beforeQueue.activeReservation.reservationId &&
              afterQueue.unissuedTailCount == beforeQueue.unissuedTailCount,
          "post-fatalでactive reservationまたはtailが変化しました");
    check(afterQueue.conservationValid && beforeQueue.conservationValid,
          "post-fatalでqueue conservationが崩れました");
    // dequeueより手前のtransaction stateも前進していないこと。
    check(!scheduler.hasPendingQualifiedEvidence(), "post-fatalでqualified evidenceが確定しました");
}

// seam が publication より前であること自体を固定する。publication 後の fatal は抑止しない。
void fatalAfterPublicationDoesNotSuppress() {
    RecordingSink sink;
    const auto token = tokenWithSerial(3931);
    {
        mvm::app::CompositionTokenPublication publication(sink, true, token);
        publication.setTokenValid(true);
    }
    sink.injectProtocolFatal("POST_PUBLICATION");
    check(sink.publishCallCount == 1 && sink.suppressedBeforePresentCount == 0,
          "publication後のfatalが遡って抑止扱いになりました");
}

// admission 判定の全分岐。post-fatal だけが formal transaction から外れる。
void joinAdmissionIsFatalGated() {
    using mvm::app::formalPresentJoinAdmission;
    const auto healthy = formalPresentJoinAdmission({true, true, false, false, true});
    check(healthy.formalEnvelopeActive && healthy.joinCommitAllowed,
          "healthy callbackがformal transactionになりません");
    const auto fatal = formalPresentJoinAdmission({true, true, true, false, true});
    check(!fatal.formalEnvelopeActive && !fatal.joinCommitAllowed,
          "post-fatal callbackがformal transactionのままです");
    const auto terminal = formalPresentJoinAdmission({true, true, false, true, true});
    check(terminal.formalEnvelopeActive && !terminal.joinCommitAllowed,
          "domain terminal後のcommit gateが変わりました");
    const auto captureClosed = formalPresentJoinAdmission({true, true, false, false, false});
    check(captureClosed.formalEnvelopeActive && !captureClosed.joinCommitAllowed,
          "issuance gate closed時のcommit gateが変わりました");
}

} // namespace

int main() {
    publicationHappensWithoutFatal();
    prePublicationFatalIsAtomic();
    fatalAfterPublicationDoesNotSuppress();
    joinAdmissionIsFatalGated();
    std::fprintf(stderr, "P2-D5-2/B3-I6B publication atomicity integration: 失敗 %d件\n", failures);
    return failures == 0 ? 0 : 1;
}

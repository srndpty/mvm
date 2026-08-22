#include "media/gpu_preview/presentation_opportunity_scheduler.h"

#include <cstdio>
#include <limits>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

mvm::gpu::PresentationOpportunityScheduler scheduler(long long refreshNumerator,
                                                     long long refreshDenominator,
                                                     long long qpcFrequency,
                                                     bool requireAuthority = false) {
    mvm::gpu::PresentationOpportunityScheduler value;
    check(value.start(
              {3600, 60, 1, refreshNumerator, refreshDenominator, qpcFrequency, requireAuthority}),
          "schedulerを開始できません");
    return value;
}

mvm::gpu::PresentationAuthoritySample authority(unsigned long long refreshCount,
                                                long long qpcVBlank, long long numerator = 60,
                                                long long denominator = 1) {
    return {true, refreshCount, qpcVBlank, numerator, denominator};
}

bool completeAndCommit(mvm::gpu::PresentationOpportunityScheduler& value,
                       const mvm::gpu::PresentationOpportunityDecision& decision, long long swapQpc,
                       const mvm::gpu::PresentationAuthoritySample& post = {},
                       long long swapOrdinal = -1) {
    return value.markRenderComplete(decision.renderBeginQpc, decision.targetFrame,
                                    decision.renderOrdinal) &&
           value.commitSwap(swapQpc, post, swapOrdinal);
}

void runRegular(mvm::gpu::PresentationOpportunityScheduler& value, long long count,
                long long intervalQpc) {
    for (long long index = 0; index < count; ++index) {
        const long long qpc = (index + 1) * intervalQpc;
        const auto decision = value.selectForRender(qpc);
        check(decision.valid && !decision.pastSourceDomain, "regular decisionが不正です");
        check(completeAndCommit(value, decision, qpc), "regular swapをcommitできません");
    }
    check(value.close(), "regular schedulerをcloseできません");
}

void regularCadences() {
    auto hz60 = scheduler(60, 1, 60);
    runRegular(hz60, 3600, 1);
    auto result = hz60.snapshot();
    check(result.displayedUnique == 3600 && result.trueDrop == 0 && result.repeated == 0,
          "60 Hz / 60 fpsのaccountingが違います");

    auto hz5995 = scheduler(59950, 1000, 59950);
    for (long long index = 0; index < 3597; ++index) {
        const long long qpc = (index + 1) * 1000;
        const auto decision = hz5995.selectForRender(qpc);
        check(decision.valid && !decision.pastSourceDomain,
              "59.95 Hz decisionがsource domain内ではありません");
        check(completeAndCommit(hz5995, decision, qpc), "59.95 Hz swapをcommitできません");
    }
    const auto endDecision = hz5995.selectForRender(3598000);
    check(endDecision.valid && endDecision.pastSourceDomain && endDecision.targetFrame == 3600,
          "59.95 Hzでframe 3600を非表示境界として検出できません");
    check(hz5995.close(), "59.95 Hz schedulerをcloseできません");
    result = hz5995.snapshot();
    check(result.displayedUnique == 3597 && result.trueDrop == 3 && result.repeated == 0,
          "59.95 Hz cadence/domain lossが違います");

    auto hz120 = scheduler(120, 1, 120);
    runRegular(hz120, 7200, 1);
    result = hz120.snapshot();
    check(result.displayedUnique == 3600 && result.trueDrop == 0 && result.repeated == 3600,
          "120 Hz repeat accountingが違います");

    auto hz30 = scheduler(30, 1, 30);
    runRegular(hz30, 1800, 1);
    result = hz30.snapshot();
    check(result.displayedUnique == 1800 && result.trueDrop == 1800 && result.repeated == 0,
          "30 Hz drop accountingが違います");
}

void opportunityGaps() {
    auto one = scheduler(60, 1, 60);
    for (long long ordinal = 0; ordinal < 3600; ++ordinal) {
        if (ordinal == 100)
            continue;
        const long long qpc = ordinal + 1;
        const auto decision = one.selectForRender(qpc);
        check(completeAndCommit(one, decision, qpc), "単一gapをcommitできません");
    }
    check(one.close(), "単一gapをcloseできません");
    check(one.snapshot().trueDrop == 1, "単一opportunity gapを1 dropにできません");

    auto multiple = scheduler(60, 1, 60);
    for (long long ordinal = 0; ordinal < 3600; ++ordinal) {
        if (ordinal >= 100 && ordinal < 103)
            continue;
        const long long qpc = ordinal + 1;
        const auto decision = multiple.selectForRender(qpc);
        check(completeAndCommit(multiple, decision, qpc), "連続gapをcommitできません");
    }
    check(multiple.close(), "連続gapをcloseできません");
    check(multiple.snapshot().trueDrop == 3, "連続opportunity gapを3 dropにできません");
}

void duplicateCallbackDoesNotCreateOpportunity() {
    auto value = scheduler(60, 1, 60);
    const auto first = value.selectForRender(1);
    const auto duplicate = value.selectForRender(1);
    check(duplicate.valid && duplicate.duplicateCallback &&
              duplicate.opportunityOrdinal == first.opportunityOrdinal,
          "duplicate callbackをfake opportunityとして扱いました");
    check(completeAndCommit(value, first, 1), "duplicate後のswapをcommitできません");
    check(value.close(), "duplicate callback caseをcloseできません");
    const auto result = value.snapshot();
    check(result.records.size() == 1 && result.trueDrop == 3599,
          "duplicate callbackがledger件数を増やしました");
}

void causalReconciliation() {
    auto oneLost = scheduler(60, 1, 60);
    const auto first = oneLost.selectForRender(1);
    check(completeAndCommit(oneLost, first, 1), "reconciliation初回commitに失敗しました");
    const auto delayed = oneLost.selectForRender(2);
    check(completeAndCommit(oneLost, delayed, 3), "actual=predicted+1をcommitできません");
    const auto after = oneLost.selectForRender(4);
    check(completeAndCommit(oneLost, after, 4), "rebase後のcommitに失敗しました");
    check(oneLost.close(), "actual=predicted+1をcloseできません");
    auto result = oneLost.snapshot();
    check(result.forwardReconciliationCount == 1 && result.lostOpportunityCount == 1,
          "単一lost opportunityを記録できません");
    check(result.records[1].predictedOpportunityOrdinal == 1 &&
              result.records[1].actualOpportunityOrdinal == 2 &&
              result.records[1].predictedSourceFrame == 1 &&
              result.records[1].expectedSourceFrame == 2 &&
              result.records[1].presentedSourceFrame == 1,
          "単一reconciliation ledgerが不正です");
    check(result.records[2].trueDropBefore == 1 && result.gapTrueDrop == 1,
          "actual>predictedをdrop=0で許容するmutationを殺せません");

    auto manyLost = scheduler(60, 1, 60);
    const auto manyFirst = manyLost.selectForRender(1);
    completeAndCommit(manyLost, manyFirst, 1);
    const auto manyDelayed = manyLost.selectForRender(2);
    check(completeAndCommit(manyLost, manyDelayed, 5), "actual=predicted+Nをcommitできません");
    const auto manyAfter = manyLost.selectForRender(6);
    completeAndCommit(manyLost, manyAfter, 6);
    check(manyLost.close(), "actual=predicted+Nをcloseできません");
    result = manyLost.snapshot();
    check(result.lostOpportunityCount == 3 && result.records[2].trueDropBefore == 3,
          "複数lost opportunityのsource gap accountingが違います");

    auto ordinalRegression = scheduler(60, 1, 60);
    const auto regressionFirst = ordinalRegression.selectForRender(1);
    completeAndCommit(ordinalRegression, regressionFirst, 1);
    const auto predictedAhead = ordinalRegression.selectForRender(4);
    ordinalRegression.markRenderComplete(4, predictedAhead.targetFrame,
                                         predictedAhead.renderOrdinal);
    check(!ordinalRegression.commitSwap(2) &&
              ordinalRegression.error() ==
                  mvm::gpu::PresentationOpportunityError::OpportunityRegression,
          "actual<predictedをfail-closedにできません");
    check(ordinalRegression.snapshot().firstEvent.classification ==
              mvm::gpu::PresentationOpportunityClassification::Regression,
          "actual<predictedのfirst-event分類が残りません");
}

void authorityAndPairingFailures() {
    auto discontinuity = scheduler(60, 1, 60, true);
    const auto first = discontinuity.selectForRender(1, authority(10, 100), 0);
    check(completeAndCommit(discontinuity, first, 1, authority(11, 110), 0),
          "authority対照commitに失敗しました");
    const auto second = discontinuity.selectForRender(2, authority(9, 90), 1);
    discontinuity.markRenderComplete(2, second.targetFrame, 1);
    check(!discontinuity.commitSwap(2, authority(12, 120), 1) &&
              discontinuity.error() ==
                  mvm::gpu::PresentationOpportunityError::AuthorityDiscontinuity,
          "DWM count discontinuityを拒否しません");
    check(discontinuity.snapshot().firstEvent.classification ==
              mvm::gpu::PresentationOpportunityClassification::AuthorityDiscontinuity,
          "DWM discontinuityのfirst-event分類が残りません");

    auto renderMissing = scheduler(60, 1, 60);
    renderMissing.selectForRender(1, {}, 0);
    check(!renderMissing.commitSwap(1, {}, 0) &&
              renderMissing.error() == mvm::gpu::PresentationOpportunityError::RenderNotCompleted,
          "render completion欠落を拒否しません");
    check(renderMissing.snapshot().firstEvent.classification ==
              mvm::gpu::PresentationOpportunityClassification::PairingDefect,
          "render completion欠落のfirst-event分類が残りません");

    auto swapMissing = scheduler(60, 1, 60);
    const auto selected = swapMissing.selectForRender(1, {}, 0);
    swapMissing.markRenderComplete(1, selected.targetFrame, 0);
    check(!swapMissing.close() &&
              swapMissing.error() == mvm::gpu::PresentationOpportunityError::RenderWithoutSwap,
          "swap欠落を拒否しません");
    check(swapMissing.snapshot().firstEvent.classification ==
              mvm::gpu::PresentationOpportunityClassification::PairingDefect,
          "swap欠落のfirst-event分類が残りません");
}

void failuresAreClosed() {
    auto missingRender = scheduler(60, 1, 60);
    check(!missingRender.commitSwap(1) &&
              missingRender.error() == mvm::gpu::PresentationOpportunityError::SwapWithoutRender,
          "render欠落swapを拒否しません");

    auto mismatch = scheduler(60, 1, 60);
    const auto mismatchDecision = mismatch.selectForRender(1);
    check(!mismatch.markRenderComplete(1, 1, mismatchDecision.renderOrdinal) &&
              mismatch.error() == mvm::gpu::PresentationOpportunityError::PresentedFrameMismatch,
          "presented frame mismatchを拒否しません");

    auto regression = scheduler(60, 1, 60);
    const auto first = regression.selectForRender(10);
    completeAndCommit(regression, first, 10);
    check(!regression.selectForRender(9).valid &&
              regression.error() == mvm::gpu::PresentationOpportunityError::AmbiguousOpportunity,
          "QPC regressionを拒否しません");

    mvm::gpu::PresentationOpportunityScheduler overflow;
    overflow.start({3600, std::numeric_limits<long long>::max(), 1, 60, 3, 20});
    const auto overflowFirst = overflow.selectForRender(1);
    completeAndCommit(overflow, overflowFirst, 1);
    check(!overflow.selectForRender(2).valid &&
              overflow.error() == mvm::gpu::PresentationOpportunityError::ArithmeticOverflow,
          "target arithmetic overflowを拒否しません");
}

void measurementEndIsExact() {
    auto value = scheduler(60, 1, 60);
    runRegular(value, 3590, 1);
    const auto result = value.snapshot();
    check(result.lastUniqueFrame == 3589 && result.tailTrueDrop == 10 &&
              result.displayedUnique + result.trueDrop == 3600,
          "measurement tailをsource domainへexactにcloseできません");
    for (const auto& record : result.records)
        check(record.presentedSourceFrame < 3600, "frame 3600以降をledgerへcommitしました");
}

} // namespace

int main() {
    regularCadences();
    opportunityGaps();
    duplicateCallbackDoesNotCreateOpportunity();
    causalReconciliation();
    authorityAndPairingFailures();
    failuresAreClosed();
    measurementEndIsExact();
    std::fprintf(stderr, "P2-D5-2/F1 presentation opportunity scheduler: 20分類 / 失敗 %d件\n",
                 failures);
    return failures == 0 ? 0 : 1;
}

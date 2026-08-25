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

using Scheduler = mvm::gpu::PresentationOpportunityScheduler;
using Error = mvm::gpu::PresentationOpportunityError;
using Classification = mvm::gpu::PresentationOpportunityClassification;

constexpr long long kQpcFrequency = 600;

Scheduler scheduler(long long refreshNumerator, long long refreshDenominator,
                    long long requiredFrameCount = 3600) {
    Scheduler value;
    check(value.start(
              {requiredFrameCount, 0, 60, 1, refreshNumerator, refreshDenominator, kQpcFrequency}),
          "schedulerを開始できません");
    return value;
}

mvm::gpu::PresentationAuthoritySample authority(unsigned long long refreshCount,
                                                long long refreshNumerator = 60,
                                                long long refreshDenominator = 1) {
    return {true, refreshCount, static_cast<long long>(refreshCount) * 10, refreshNumerator,
            refreshDenominator};
}

// 1回のrender/swapを駆動する。opportunity序数はrefresh countだけが決めるので、
// QPCはcontinuityのcross-checkとしてしか渡さない。
struct Driver {
    Scheduler* scheduler = nullptr;
    long long renderOrdinal = 0;
    long long swapOrdinal = 0;
    long long refreshNumerator = 60;
    long long refreshDenominator = 1;

    mvm::gpu::PresentationOpportunityDecision select(unsigned long long preCount,
                                                     long long callbackQpc) {
        return scheduler->selectForRender(
            callbackQpc, authority(preCount, refreshNumerator, refreshDenominator), renderOrdinal);
    }

    bool present(unsigned long long preCount, unsigned long long postCount, long long callbackQpc,
                 long long renderEndQpc, long long swapQpc) {
        const auto decision = select(preCount, callbackQpc);
        if (!decision.valid || decision.pastSourceDomain)
            return false;
        if (!scheduler->markRenderComplete(renderEndQpc, decision.targetFrame,
                                           decision.renderOrdinal))
            return false;
        const bool committed = scheduler->commitSwap(
            swapQpc, authority(postCount, refreshNumerator, refreshDenominator), swapOrdinal);
        ++renderOrdinal;
        ++swapOrdinal;
        return committed;
    }

    // 1 refreshずつ前進する定常cadence。render nのpre countは100+n、
    // swapのpost countは101+n。
    bool runCadence(long long count) {
        for (long long index = 0; index < count; ++index) {
            const unsigned long long pre = 100 + static_cast<unsigned long long>(index);
            if (!present(pre, pre + 1, 20 * index + 1, 20 * index + 5, 20 * index + 10))
                return false;
        }
        return true;
    }
};

void regularCadences() {
    auto hz60 = scheduler(60, 1);
    Driver driver{&hz60};
    check(driver.runCadence(3600), "60 Hz cadenceをcommitできません");
    check(hz60.close(), "60 Hz schedulerをcloseできません");
    auto result = hz60.snapshot();
    check(result.displayedUnique == 3600 && result.trueDrop == 0 && result.repeated == 0,
          "60 Hz / 60 fpsのaccountingが違います");
    check(result.records.size() == 3600 && result.supersededCandidateCount == 0 &&
              result.swappedCompositionCount == 3600,
          "60 Hzのopportunity/swap countが分離できていません");
    check(result.originRefreshCount == 101 && result.records[0].actualOpportunityOrdinal == 0,
          "最初のswapのrefresh countをoriginへ固定できません");

    auto hz5995 = scheduler(59950, 1000);
    Driver slow{&hz5995, 0, 0, 59950, 1000};
    check(slow.runCadence(3597), "59.95 Hz cadenceをcommitできません");
    const auto endDecision = slow.select(100 + 3597, 20 * 3597 + 1);
    check(endDecision.valid && endDecision.pastSourceDomain && endDecision.targetFrame == 3600,
          "59.95 Hzでframe 3600を非表示境界として検出できません");
    check(hz5995.close(), "59.95 Hz schedulerをcloseできません");
    result = hz5995.snapshot();
    check(result.displayedUnique == 3597 && result.trueDrop == 3 && result.repeated == 0,
          "59.95 Hz cadence/domain lossが違います");

    auto hz120 = scheduler(120, 1);
    Driver fast{&hz120, 0, 0, 120, 1};
    check(fast.runCadence(7200), "120 Hz cadenceをcommitできません");
    check(hz120.close(), "120 Hz schedulerをcloseできません");
    result = hz120.snapshot();
    check(result.displayedUnique == 3600 && result.trueDrop == 0 && result.repeated == 3600,
          "120 Hz repeat accountingが違います");

    auto hz30 = scheduler(30, 1);
    Driver half{&hz30, 0, 0, 30, 1};
    check(half.runCadence(1800), "30 Hz cadenceをcommitできません");
    check(hz30.close(), "30 Hz schedulerをcloseできません");
    result = hz30.snapshot();
    check(result.displayedUnique == 1800 && result.trueDrop == 1800 && result.repeated == 0,
          "30 Hz drop accountingが違います");
}

// 今回のformal Playback run 1で観測した実sequenceをdeterministic regressionにする。
// 前intervalが1.688 refresh、次intervalが0.5 refresh未満でも、refresh countが
// 連続している限りfatalにしてはいけない。
void longRenderThenShortSwapIsNotFatal() {
    auto value = scheduler(60, 1, 12);
    Driver driver{&value};
    check(driver.runCadence(5), "先行する定常cadenceをcommitできません");

    // 8 ms級のrenderがVBlankを跨ぎ、swapがopportunity 6へ着地する (1.688 refresh)。
    check(driver.present(105, 107, 101, 118, 119), "1.688 refresh swapをcommitできません");
    check(value.error() == Error::None, "1.688 refresh swapでfatalになりました");

    // 直後のswapが同一refresh count内 (0.312 refresh) で戻ってくる。
    check(driver.present(107, 107, 120, 122, 124), "0.312 refresh swapをcommitできません");
    check(value.error() == Error::None,
          "半refresh未満の追加swapをambiguous fatalにしました (旧per-interval rounding mutation)");

    // 次のopportunityへ前進した時点で、pendingがlatest candidateでfinalizeされる。
    check(driver.present(107, 108, 125, 127, 130), "supersede後のswapをcommitできません");
    check(value.close(), "supersede sequenceをcloseできません");
    const auto result = value.snapshot();
    check(result.error == Error::None && result.closed,
          "1.688T→0.312T sequenceがfail-closedになりました");
    check(result.records.size() == 7 && result.swappedCompositionCount == 8 &&
              result.supersededCandidateCount == 1,
          "supersedeでopportunityとswapの数が分離できていません");
    const auto& lost = result.records[5];
    check(lost.actualOpportunityOrdinal == 6 && lost.predictedOpportunityOrdinal == 7 &&
              lost.lostOpportunityCount == 1 &&
              lost.classification == Classification::ForwardOpportunityLoss,
          "1.688 refresh swapのopportunity lossを記録できません");
    check(lost.supersededCandidateCount == 1 && lost.presentedSourceFrame == 7,
          "same-opportunityのlatest candidateをfinalizeしていません (first candidate保持mutation)");
    check(lost.trueDropBefore == 2 && result.gapTrueDrop == 2,
          "supersedeで捨てたframeをtrue dropへ計上していません");
    check(result.records[6].actualOpportunityOrdinal == 7 && result.records[6].repeat,
          "supersede後のopportunityをrepeatとして閉じられません");
    check(result.displayedUnique + result.trueDrop == 12,
          "supersede sequenceでdisplayed + trueDroppedがsource domainと一致しません");
}

// 同一refresh countの2 swapはopportunityを前進させない。
void sameRefreshCountDoesNotAdvanceOrdinal() {
    auto value = scheduler(60, 1, 12);
    Driver driver{&value};
    check(driver.runCadence(3), "同一count testの先行cadenceに失敗しました");
    check(driver.present(103, 104, 61, 63, 65), "同一count 1本目をcommitできません");
    check(driver.present(104, 104, 66, 68, 70), "同一count 2本目をcommitできません");
    check(driver.present(104, 105, 71, 73, 75), "同一count後の前進をcommitできません");
    check(value.close(), "同一count caseをcloseできません");
    const auto result = value.snapshot();
    check(result.records.size() == 5,
          "同一refresh countのswapを新opportunityとして数えました (same-opportunity mutation)");
    check(result.records[3].actualOpportunityOrdinal == 3 &&
              result.records[3].supersededCandidateCount == 1 &&
              result.records[4].actualOpportunityOrdinal == 4,
          "同一refresh count後のordinalが1つずれています");
    check(result.records[3].presentedSourceFrame == 4,
          "同一opportunityでlatest candidateを保持していません");
    check(result.supersededCandidateCount == 1 && result.swappedCompositionCount == 6,
          "supersede countとswap countが一致しません");
}

// refresh count +Nはlost opportunityとsource gapへexactに落とす。
void refreshCountGapAccounting() {
    auto single = scheduler(60, 1, 12);
    Driver one{&single};
    check(one.runCadence(3), "単一gap testの先行cadenceに失敗しました");
    check(one.present(103, 106, 61, 63, 65), "refresh count +3のswapをcommitできません");
    check(one.present(106, 107, 66, 68, 70), "gap後のswapをcommitできません");
    check(single.close(), "gap caseをcloseできません");
    const auto result = single.snapshot();
    check(result.records.size() == 5, "gap caseのfinalize件数が違います");
    check(result.records[3].actualOpportunityOrdinal == 5 &&
              result.records[3].lostOpportunityCount == 2 &&
              result.records[3].forwardReconciliation,
          "refresh count +Nのlost opportunityが違います");
    check(result.records[3].presentedSourceFrame == 3 &&
              result.records[3].expectedSourceFrame == 5 && result.records[3].trueDropBefore == 0,
          "lost opportunityをsource dropへ二重計上しました");
    check(result.records[4].trueDropBefore == 2 && result.gapTrueDrop == 2,
          "lost opportunity後のsource gapを計上していません");
    check(result.lostOpportunityCount == 2 && result.forwardReconciliationCount == 1,
          "lost opportunity総数が違います");
    check(result.tailTrueDrop == 5 && result.displayedUnique + result.trueDrop == 12,
          "gap caseのsource domain accountingが閉じていません");
    check(result.firstEvent.captured &&
              result.firstEvent.classification == Classification::ForwardOpportunityLoss &&
              result.firstEvent.actualOpportunityOrdinal == 5,
          "最初のopportunity lossをfirst eventへ固定できません");
}

void authorityFailuresAreFatal() {
    auto regression = scheduler(60, 1, 12);
    Driver driver{&regression};
    check(driver.runCadence(3), "count regression testの先行cadenceに失敗しました");
    const auto decision = driver.select(103, 61);
    check(decision.valid, "count regression前のselectに失敗しました");
    check(regression.markRenderComplete(63, decision.targetFrame, decision.renderOrdinal),
          "count regression前のrender完了に失敗しました");
    check(!regression.commitSwap(65, authority(100), driver.swapOrdinal) &&
              regression.error() == Error::AuthorityDiscontinuity,
          "refresh count regressionをfail-closedにできません");

    auto vblank = scheduler(60, 1, 12);
    Driver vblankDriver{&vblank};
    check(vblankDriver.runCadence(3), "VBlank regression testの先行cadenceに失敗しました");
    const auto ahead = vblankDriver.select(103, 61);
    check(ahead.valid, "VBlank regression前のselectに失敗しました");
    check(vblank.markRenderComplete(63, ahead.targetFrame, ahead.renderOrdinal),
          "VBlank regression前のrender完了に失敗しました");
    mvm::gpu::PresentationAuthoritySample backward = authority(105);
    backward.qpcVBlank = 1;
    check(!vblank.commitSwap(65, backward, vblankDriver.swapOrdinal) &&
              vblank.error() == Error::AuthorityDiscontinuity,
          "qpcVBlank discontinuityをfail-closedにできません");

    auto missing = scheduler(60, 1, 12);
    check(!missing.selectForRender(1, {}, 0).valid &&
              missing.error() == Error::AuthorityDiscontinuity,
          "authority sampleなしのselectを拒否しません");

    auto changed = scheduler(60, 1, 12);
    Driver changedDriver{&changed};
    check(changedDriver.runCadence(2), "refresh変更testの先行cadenceに失敗しました");
    check(!changed.selectForRender(41, authority(102, 59950, 1000), 2).valid &&
              changed.error() == Error::AuthorityDiscontinuity,
          "refresh rationalの変更をfail-closedにできません");
}

void pairingFailuresAreFatal() {
    auto renderMissing = scheduler(60, 1, 12);
    Driver driver{&renderMissing};
    driver.select(100, 1);
    check(!renderMissing.commitSwap(10, authority(101), 0) &&
              renderMissing.error() == Error::RenderNotCompleted,
          "render completion欠落を拒否しません");
    check(renderMissing.snapshot().firstEvent.classification == Classification::PairingDefect,
          "render completion欠落のfirst-event分類が残りません");

    auto swapMissing = scheduler(60, 1, 12);
    Driver swapDriver{&swapMissing};
    const auto selected = swapDriver.select(100, 1);
    check(swapMissing.markRenderComplete(5, selected.targetFrame, selected.renderOrdinal),
          "swap欠落testのrender完了に失敗しました");
    check(!swapMissing.close() && swapMissing.error() == Error::RenderWithoutSwap,
          "swap欠落を拒否しません");
    check(swapMissing.snapshot().firstEvent.classification == Classification::PairingDefect,
          "swap欠落のfirst-event分類が残りません");

    auto orphanSwap = scheduler(60, 1, 12);
    check(!orphanSwap.commitSwap(10, authority(101), 0) &&
              orphanSwap.error() == Error::SwapWithoutRender,
          "render欠落swapを拒否しません");

    auto frameMismatch = scheduler(60, 1, 12);
    Driver mismatchDriver{&frameMismatch};
    const auto mismatch = mismatchDriver.select(100, 1);
    check(!frameMismatch.markRenderComplete(5, mismatch.targetFrame + 1, mismatch.renderOrdinal) &&
              frameMismatch.error() == Error::PresentedFrameMismatch,
          "presented frame mismatchを拒否しません");

    auto swapOrdinal = scheduler(60, 1, 12);
    Driver ordinalDriver{&swapOrdinal};
    const auto first = ordinalDriver.select(100, 1);
    check(swapOrdinal.markRenderComplete(5, first.targetFrame, first.renderOrdinal),
          "swap ordinal testのrender完了に失敗しました");
    check(!swapOrdinal.commitSwap(10, authority(101), 3) &&
              swapOrdinal.error() == Error::SwapOrdinalMismatch,
          "swap ordinalの飛びを拒否しません");

    auto qpcRegression = scheduler(60, 1, 12);
    Driver qpcDriver{&qpcRegression};
    check(qpcDriver.runCadence(2), "QPC cross-check testの先行cadenceに失敗しました");
    const auto ahead = qpcDriver.select(102, 41);
    check(ahead.valid, "QPC cross-check前のselectに失敗しました");
    check(qpcRegression.markRenderComplete(43, ahead.targetFrame, ahead.renderOrdinal),
          "QPC cross-check前のrender完了に失敗しました");
    check(!qpcRegression.commitSwap(20, authority(103), qpcDriver.swapOrdinal) &&
              qpcRegression.error() == Error::OpportunityRegression,
          "swap QPCの後退をcontinuity cross-checkで拒否しません");
}

void duplicateCallbackDoesNotCreateOpportunity() {
    auto value = scheduler(60, 1, 12);
    Driver driver{&value};
    const auto first = driver.select(100, 1);
    const auto duplicate = driver.select(100, 2);
    check(duplicate.valid && duplicate.duplicateCallback &&
              duplicate.opportunityOrdinal == first.opportunityOrdinal,
          "duplicate callbackをfake opportunityとして扱いました");
    check(value.markRenderComplete(5, first.targetFrame, first.renderOrdinal) &&
              value.commitSwap(10, authority(101), 0),
          "duplicate後のswapをcommitできません");
    check(value.close(), "duplicate callback caseをcloseできません");
    const auto result = value.snapshot();
    check(result.records.size() == 1 && result.trueDrop == 11,
          "duplicate callbackがledger件数を増やしました");
}

// measurement endではpending opportunityをfinalizeしてからtailを数える。
void measurementEndFinalizesPending() {
    auto value = scheduler(60, 1, 12);
    Driver driver{&value};
    check(driver.runCadence(4), "tail testのcadenceに失敗しました");
    auto before = value.snapshot();
    check(before.records.size() == 3 && before.lastFinalizedOpportunityOrdinal == 2,
          "close前にlatest opportunityをfinalizeしてはいけません");
    check(value.close(), "tail caseをcloseできません");
    const auto result = value.snapshot();
    check(result.records.size() == 4 && result.lastFinalizedOpportunityOrdinal == 3,
          "measurement endでpending opportunityをfinalizeしていません");
    check(result.displayedUnique == 4 && result.tailTrueDrop == 8 &&
              result.displayedUnique + result.trueDrop == 12,
          "tail accountingがsource domainへexactに閉じていません");
    for (const auto& record : result.records)
        check(record.presentedSourceFrame < 12, "source domain外のframeをledgerへcommitしました");
}

void overflowIsClosed() {
    Scheduler overflow;
    overflow.start({3600, 0, std::numeric_limits<long long>::max(), 1, 60, 3, 20});
    Driver driver{&overflow, 0, 0, 60, 3};
    check(driver.present(100, 101, 1, 5, 10), "overflow testの初回commitに失敗しました");
    check(!driver.select(101, 11).valid && overflow.error() == Error::ArithmeticOverflow,
          "target arithmetic overflowを拒否しません");
}

void sourceFrameOffsetIsExact() {
    Scheduler value;
    check(value.start({61, 60, 1, 1, 60, 1, kQpcFrequency}),
          "source offset schedulerを開始できません");
    Driver driver{&value};
    const auto first = driver.select(100, 1);
    check(first.valid && first.targetFrame == 60,
          "source offsetがscheduler targetへ反映されていません");
    check(value.markRenderComplete(5, 60, first.renderOrdinal) &&
              value.commitSwap(10, authority(101), 0),
          "source offset targetをexact commitできません");
}

void requiredIntentAuthorityIsProducedAtStart() {
    auto value = scheduler(60, 1, 3);
    const auto initial = value.snapshot();
    check(initial.requiredIntentSetExact && initial.requiredIntentOrdinals.size() == 3 &&
              initial.requiredIntentOrdinals[0] == 0 && initial.requiredIntentOrdinals[1] == 1 &&
              initial.requiredIntentOrdinals[2] == 2,
          "required intent setをscheduler start時点でexactに生成していません");
    Driver driver{&value};
    const auto current = driver.select(100, 1);
    check(current.valid && current.requiredIntentMembershipExact &&
              current.requiredIntentMembership,
          "required set内decisionのmembershipがexactではありません");
}

} // namespace

int main() {
    regularCadences();
    longRenderThenShortSwapIsNotFatal();
    sameRefreshCountDoesNotAdvanceOrdinal();
    refreshCountGapAccounting();
    authorityFailuresAreFatal();
    pairingFailuresAreFatal();
    duplicateCallbackDoesNotCreateOpportunity();
    measurementEndFinalizesPending();
    overflowIsClosed();
    sourceFrameOffsetIsExact();
    requiredIntentAuthorityIsProducedAtStart();
    std::fprintf(stderr, "P2-D5-2/F2 presentation opportunity scheduler: 失敗 %d件\n", failures);
    return failures == 0 ? 0 : 1;
}

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

mvm::gpu::PresentationOpportunityScheduler
scheduler(long long refreshNumerator, long long refreshDenominator, long long qpcFrequency) {
    mvm::gpu::PresentationOpportunityScheduler value;
    check(value.start({3600, 60, 1, refreshNumerator, refreshDenominator, qpcFrequency}),
          "schedulerを開始できません");
    return value;
}

void runRegular(mvm::gpu::PresentationOpportunityScheduler& value, long long count,
                long long intervalQpc) {
    for (long long index = 0; index < count; ++index) {
        const long long qpc = (index + 1) * intervalQpc;
        const auto decision = value.selectForRender(qpc);
        check(decision.valid && !decision.pastSourceDomain, "regular decisionが不正です");
        check(value.commitSwap(qpc, decision.targetFrame), "regular swapをcommitできません");
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
        check(hz5995.commitSwap(qpc, decision.targetFrame), "59.95 Hz swapをcommitできません");
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
        check(one.commitSwap(qpc, decision.targetFrame), "単一gapをcommitできません");
    }
    check(one.close(), "単一gapをcloseできません");
    check(one.snapshot().trueDrop == 1, "単一opportunity gapを1 dropにできません");

    auto multiple = scheduler(60, 1, 60);
    for (long long ordinal = 0; ordinal < 3600; ++ordinal) {
        if (ordinal >= 100 && ordinal < 103)
            continue;
        const long long qpc = ordinal + 1;
        const auto decision = multiple.selectForRender(qpc);
        check(multiple.commitSwap(qpc, decision.targetFrame), "連続gapをcommitできません");
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
    check(value.commitSwap(1, first.targetFrame), "duplicate後のswapをcommitできません");
    check(value.close(), "duplicate callback caseをcloseできません");
    const auto result = value.snapshot();
    check(result.records.size() == 1 && result.trueDrop == 3599,
          "duplicate callbackがledger件数を増やしました");
}

void failuresAreClosed() {
    auto missingRender = scheduler(60, 1, 60);
    check(!missingRender.commitSwap(1, 0) &&
              missingRender.error() == mvm::gpu::PresentationOpportunityError::SwapWithoutRender,
          "render欠落swapを拒否しません");

    auto missingSwap = scheduler(60, 1, 60);
    missingSwap.selectForRender(1);
    check(!missingSwap.close() &&
              missingSwap.error() == mvm::gpu::PresentationOpportunityError::RenderWithoutSwap,
          "swap欠落renderを拒否しません");

    auto mismatch = scheduler(60, 1, 60);
    mismatch.selectForRender(1);
    check(!mismatch.commitSwap(1, 1) &&
              mismatch.error() == mvm::gpu::PresentationOpportunityError::PresentedFrameMismatch,
          "presented frame mismatchを拒否しません");

    auto regression = scheduler(60, 1, 60);
    const auto first = regression.selectForRender(10);
    regression.commitSwap(10, first.targetFrame);
    check(!regression.selectForRender(9).valid &&
              regression.error() == mvm::gpu::PresentationOpportunityError::AmbiguousOpportunity,
          "QPC regressionを拒否しません");

    mvm::gpu::PresentationOpportunityScheduler overflow;
    overflow.start({3600, std::numeric_limits<long long>::max(), 1, 60, 3, 20});
    const auto overflowFirst = overflow.selectForRender(1);
    overflow.commitSwap(1, overflowFirst.targetFrame);
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
    failuresAreClosed();
    measurementEndIsExact();
    std::fprintf(stderr, "P2-D5-2 presentation opportunity scheduler: 12分類 / 失敗 %d件\n",
                 failures);
    return failures == 0 ? 0 : 1;
}

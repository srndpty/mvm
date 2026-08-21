#include "p2_opportunity_ordinal_replay.h"

#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

std::vector<long long> ordinals(long long count) {
    std::vector<long long> result;
    result.reserve(static_cast<std::size_t>(count));
    for (long long ordinal = 0; ordinal < count; ++ordinal)
        result.push_back(ordinal);
    return result;
}

mvm::test::OpportunityOrdinalInput input(long long refreshNumerator, long long refreshDenominator,
                                         long long opportunityCount) {
    return {3600, 60,   1,    refreshNumerator,          refreshDenominator, true, true,
            true, true, true, ordinals(opportunityCount)};
}

void checkClosed(const mvm::test::OpportunityOrdinalSummary& result, long long displayed,
                 long long dropped, long long repeated, const char* label) {
    check(result.valid, label);
    check(result.scheduled == 3600, label);
    check(result.displayed == displayed, label);
    check(result.trueDropped == dropped, label);
    check(result.repeated == repeated, label);
    check(result.firstOutputFrame == 0, label);
    check(result.lastOutputFrame < 3600, label);
    check(result.uniqueFrameStrictlyIncreasing, label);
    check(result.sourceDomainConserved, label);
}

void checkRejected(mvm::test::OpportunityOrdinalInput value,
                   mvm::test::OpportunityOrdinalError expected, const char* label) {
    const auto result = mvm::test::replayOpportunityOrdinals(value);
    check(!result.valid && result.error == expected, label);
}

} // namespace

int main() {
    checkClosed(mvm::test::replayOpportunityOrdinals(input(60, 1, 3600)), 3600, 0, 0,
                "60 Hz / 60 fpsをlossなしで閉じる");
    checkClosed(mvm::test::replayOpportunityOrdinals(input(59950, 1000, 3598)), 3597, 3, 0,
                "59.95 Hzをexact rationalで3 frame lossへ写像する");
    checkClosed(mvm::test::replayOpportunityOrdinals(input(120, 1, 7200)), 3600, 0, 3600,
                "120 Hzでは同じsource frameをrepeatする");
    checkClosed(mvm::test::replayOpportunityOrdinals(input(30, 1, 1800)), 1800, 1800, 0,
                "30 Hzでは飛ばされたsource frameをtrue dropにする");

    auto oneMissing = input(60, 1, 3600);
    oneMissing.opportunityOrdinals.erase(oneMissing.opportunityOrdinals.begin() + 100);
    checkClosed(mvm::test::replayOpportunityOrdinals(oneMissing), 3599, 1, 0,
                "actual opportunity 1件欠落をexactly 1 dropにする");

    auto twoMissing = input(60, 1, 3600);
    twoMissing.opportunityOrdinals.erase(twoMissing.opportunityOrdinals.begin() + 100,
                                         twoMissing.opportunityOrdinals.begin() + 102);
    checkClosed(mvm::test::replayOpportunityOrdinals(twoMissing), 3598, 2, 0,
                "連続2 opportunity欠落を2 dropにする");

    auto burst = input(60, 1, 3600);
    burst.opportunityOrdinals.insert(burst.opportunityOrdinals.begin() + 101, {100, 100, 100});
    checkClosed(mvm::test::replayOpportunityOrdinals(burst), 3600, 0, 3,
                "duplicate / burstでdropを隠さない");

    auto endBoundary = input(59950, 1000, 3598);
    const auto endResult = mvm::test::replayOpportunityOrdinals(endBoundary);
    check(endResult.valid && endResult.pastSourceDomain == 1 &&
              endResult.decisions.back().targetFrame == 3600 &&
              !endResult.decisions.back().displayed,
          "measurement endを跨ぐopportunityでframe 3600を表示しない");

    auto changed = input(60, 1, 3600);
    changed.refreshStable = false;
    checkRejected(changed, mvm::test::OpportunityOrdinalError::RefreshChanged,
                  "refresh rational変更をfail-closedにする");

    auto noDwm = input(60, 1, 3600);
    noDwm.dwmAuthorityAvailable = false;
    checkRejected(noDwm, mvm::test::OpportunityOrdinalError::DwmAuthorityMissing,
                  "DWM authority欠損をfail-closedにする");

    auto noSwap = input(60, 1, 3600);
    noSwap.swapCorrespondenceComplete = false;
    checkRejected(noSwap, mvm::test::OpportunityOrdinalError::SwapCorrespondenceMissing,
                  "renderとswap対応欠損をfail-closedにする");

    auto discontinuous = input(60, 1, 3600);
    discontinuous.dwmAuthorityContinuous = false;
    checkRejected(discontinuous, mvm::test::OpportunityOrdinalError::DwmAuthorityDiscontinuity,
                  "DWM authority discontinuityをfail-closedにする");

    auto ambiguous = input(60, 1, 3600);
    ambiguous.opportunityUnambiguous = false;
    checkRejected(ambiguous, mvm::test::OpportunityOrdinalError::OpportunityAmbiguous,
                  "callbackとswapの曖昧性をfail-closedにする");

    auto regression = input(60, 1, 3600);
    regression.opportunityOrdinals[101] = 99;
    checkRejected(regression, mvm::test::OpportunityOrdinalError::OrdinalRegression,
                  "opportunity ordinal regressionをfail-closedにする");

    auto wrongStart = input(60, 1, 3600);
    wrongStart.opportunityOrdinals.front() = 1;
    checkRejected(wrongStart, mvm::test::OpportunityOrdinalError::FirstOrdinalNotZero,
                  "measurement start境界の欠損をfail-closedにする");

    std::fprintf(stderr, "P2 opportunity ordinal検査: 15 scenario / 失敗 %d件\n", failures);
    return failures == 0 ? 0 : 1;
}

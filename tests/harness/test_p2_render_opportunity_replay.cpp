#include "p2_render_opportunity_replay.h"

#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

std::vector<long long> exactCallbacks(long long count, long long start = 0,
                                      long long frequency = 60000) {
    std::vector<long long> result;
    for (long long frame = 0; frame < count; ++frame)
        result.push_back(start + (frame * frequency) / 60);
    return result;
}

void checkClosed(const mvm::test::RenderOpportunitySummary& result, long long displayed,
                 long long dropped, long long repeated, const char* label) {
    check(result.scheduled == 3600, label);
    check(result.displayed == displayed, label);
    check(result.deadlineDrop == dropped, label);
    check(result.repeated == repeated, label);
    check(result.frameIdentityStrictlyIncreasing, label);
    check(result.frameZeroStarted, label);
    check(result.measurementRangeRespected, label);
}

} // namespace

int main() {
    constexpr long long frequency = 60000;
    constexpr long long end = 3600 * frequency / 60;

    checkClosed(
        mvm::test::replayNearestOpportunitySlots(exactCallbacks(3600), 0, end, frequency, 3600),
        3600, 0, 0, "exact 60 Hzはdropしない");

    auto jitter = exactCallbacks(3600);
    for (std::size_t index = 0; index < jitter.size(); ++index)
        jitter[index] += index % 2 == 0 ? 120 : -120;
    jitter[0] = 0;
    checkClosed(mvm::test::replayNearestOpportunitySlots(jitter, 0, end, frequency, 3600), 3600, 0,
                0, "slot内jitterはdropしない");

    auto oneMissing = exactCallbacks(3600);
    oneMissing.erase(oneMissing.begin() + 100);
    checkClosed(mvm::test::replayNearestOpportunitySlots(oneMissing, 0, end, frequency, 3600), 3599,
                1, 0, "callback 1本の欠落をexactly 1 dropにする");

    auto multiMissing = exactCallbacks(3600);
    multiMissing.erase(multiMissing.begin() + 100, multiMissing.begin() + 103);
    checkClosed(mvm::test::replayNearestOpportunitySlots(multiMissing, 0, end, frequency, 3600),
                3597, 3, 0, "multi-slot gapを対応するdrop数にする");

    auto duplicate = exactCallbacks(3600);
    duplicate.insert(duplicate.begin() + 101, duplicate[100] + 10);
    checkClosed(mvm::test::replayNearestOpportunitySlots(duplicate, 0, end, frequency, 3600), 3600,
                0, 1, "同じslotのcallbackをrepeatedにする");

    const auto tie = mvm::test::replayNearestOpportunitySlots({0, 500}, 0, 3000, frequency, 3);
    check(tie.decisions.size() == 2 && tie.decisions[1].outputFrame == 1,
          "midpoint tieを後続slotへ決定的に割り当てる");

    auto crossesEnd = exactCallbacks(3600);
    crossesEnd.push_back(end);
    checkClosed(mvm::test::replayNearestOpportunitySlots(crossesEnd, 0, end, frequency, 3600), 3600,
                0, 1, "measurement endでframe 3600を出さない");

    std::vector<long long> burst{0};
    for (int index = 0; index < 10; ++index)
        burst.push_back(10 + index);
    burst.push_back(2000);
    const auto burstResult = mvm::test::replayNearestOpportunitySlots(burst, 0, 3000, frequency, 3);
    check(burstResult.displayed == 2 && burstResult.deadlineDrop == 1 && burstResult.repeated == 10,
          "burst callbackで真の欠落を隠さない");

    std::fprintf(stderr, "P2 render opportunity検査: 8 scenario / 失敗 %d件\n", failures);
    return failures == 0 ? 0 : 1;
}

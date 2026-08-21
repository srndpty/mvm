#ifndef MVM_TESTS_P2_RENDER_OPPORTUNITY_REPLAY_H
#define MVM_TESTS_P2_RENDER_OPPORTUNITY_REPLAY_H

#include <vector>

namespace mvm::test {

struct RenderOpportunityDecision {
    bool displayed = false;
    bool repeated = false;
    long long outputFrame = -1;
    long long droppedBefore = 0;
};

struct RenderOpportunitySummary {
    long long scheduled = 0;
    long long displayed = 0;
    long long deadlineDrop = 0;
    long long repeated = 0;
    long long firstOutputFrame = -1;
    long long lastOutputFrame = -1;
    bool frameIdentityStrictlyIncreasing = true;
    bool frameZeroStarted = false;
    bool measurementRangeRespected = true;
    std::vector<RenderOpportunityDecision> decisions;
};

// 診断専用のpure/offline model。nominal 60 Hz deadline間の中点を境界とし、
// callbackを最も近い非重複slotへ割り当てる。production schedulerへは接続しない。
RenderOpportunitySummary replayNearestOpportunitySlots(const std::vector<long long>& callbackQpcs,
                                                       long long startQpc,
                                                       long long endQpcExclusive,
                                                       long long qpcFrequency,
                                                       long long requiredFrameCount);

} // namespace mvm::test

#endif

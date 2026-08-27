#include "p2_render_opportunity_replay.h"

#include <limits>

namespace mvm::test {
namespace {

long long nearestSlot(long long callbackQpc, long long startQpc, long long qpcFrequency) {
    const long long offset = callbackQpc - startQpc;
    if (offset < 0 || offset > std::numeric_limits<long long>::max() / 60)
        return -1;
    const long long scaled = offset * 60;
    long long slot = scaled / qpcFrequency;
    const long long remainder = scaled % qpcFrequency;
    // midpoint tieは後続slotへ割り当て、[d(n)-T/2, d(n)+T/2)を固定する。
    if (remainder >= (qpcFrequency + 1) / 2)
        ++slot;
    return slot;
}

} // namespace

RenderOpportunitySummary replayNearestOpportunitySlots(const std::vector<long long>& callbackQpcs,
                                                       long long startQpc,
                                                       long long endQpcExclusive,
                                                       long long qpcFrequency,
                                                       long long requiredFrameCount) {
    RenderOpportunitySummary result;
    result.decisions.reserve(callbackQpcs.size());
    if (qpcFrequency <= 0 || requiredFrameCount <= 0 || endQpcExclusive <= startQpc)
        return result;

    long long previousOutput = -1;
    for (const long long callbackQpc : callbackQpcs) {
        RenderOpportunityDecision decision;
        const long long slot = nearestSlot(callbackQpc, startQpc, qpcFrequency);
        if (callbackQpc < startQpc || callbackQpc >= endQpcExclusive || slot < 0 ||
            slot >= requiredFrameCount || slot <= previousOutput) {
            decision.repeated = true;
            ++result.repeated;
            result.decisions.push_back(decision);
            continue;
        }

        decision.displayed = true;
        decision.outputFrame = slot;
        decision.droppedBefore = slot - previousOutput - 1;
        result.deadlineDrop += decision.droppedBefore;
        result.displayed++;
        if (result.firstOutputFrame < 0)
            result.firstOutputFrame = slot;
        if (slot <= previousOutput)
            result.frameIdentityStrictlyIncreasing = false;
        previousOutput = slot;
        result.lastOutputFrame = slot;
        result.decisions.push_back(decision);
    }

    result.deadlineDrop += requiredFrameCount - previousOutput - 1;
    result.scheduled = result.displayed + result.deadlineDrop;
    result.frameZeroStarted = result.firstOutputFrame == 0;
    result.measurementRangeRespected = result.lastOutputFrame < requiredFrameCount;
    return result;
}

} // namespace mvm::test

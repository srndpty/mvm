#include "p2_opportunity_ordinal_replay.h"

#include <limits>

namespace mvm::test {
namespace {

bool checkedMultiply(long long left, long long right, long long* output) {
    if (left < 0 || right < 0)
        return false;
    if (left != 0 && right > std::numeric_limits<long long>::max() / left)
        return false;
    *output = left * right;
    return true;
}

bool targetFor(const OpportunityOrdinalInput& input, long long ordinal, long long* target) {
    long long numerator = 0;
    long long denominator = 0;
    if (!checkedMultiply(ordinal, input.sourceFpsNumerator, &numerator) ||
        !checkedMultiply(numerator, input.refreshDenominator, &numerator) ||
        !checkedMultiply(input.sourceFpsDenominator, input.refreshNumerator, &denominator) ||
        denominator <= 0)
        return false;
    *target = numerator / denominator;
    return true;
}

OpportunityOrdinalSummary fail(OpportunityOrdinalError error) {
    OpportunityOrdinalSummary result;
    result.error = error;
    return result;
}

} // namespace

OpportunityOrdinalSummary replayOpportunityOrdinals(const OpportunityOrdinalInput& input) {
    if (input.requiredFrameCount <= 0 || input.sourceFpsNumerator <= 0 ||
        input.sourceFpsDenominator <= 0 || input.refreshNumerator <= 0 ||
        input.refreshDenominator <= 0 || input.opportunityOrdinals.empty())
        return fail(OpportunityOrdinalError::InvalidDomain);
    if (!input.refreshStable)
        return fail(OpportunityOrdinalError::RefreshChanged);
    if (!input.dwmAuthorityAvailable)
        return fail(OpportunityOrdinalError::DwmAuthorityMissing);
    if (!input.dwmAuthorityContinuous)
        return fail(OpportunityOrdinalError::DwmAuthorityDiscontinuity);
    if (!input.swapCorrespondenceComplete)
        return fail(OpportunityOrdinalError::SwapCorrespondenceMissing);
    if (!input.opportunityUnambiguous)
        return fail(OpportunityOrdinalError::OpportunityAmbiguous);
    if (input.opportunityOrdinals.front() != 0)
        return fail(OpportunityOrdinalError::FirstOrdinalNotZero);

    OpportunityOrdinalSummary result;
    result.decisions.reserve(input.opportunityOrdinals.size());
    long long previousOrdinal = -1;
    long long previousOutput = -1;
    for (const long long ordinal : input.opportunityOrdinals) {
        if (ordinal < previousOrdinal)
            return fail(OpportunityOrdinalError::OrdinalRegression);
        if (previousOrdinal >= 0 && ordinal > previousOrdinal + 1)
            result.opportunityGapCount += ordinal - previousOrdinal - 1;

        long long target = -1;
        if (!targetFor(input, ordinal, &target))
            return fail(OpportunityOrdinalError::ArithmeticOverflow);
        OpportunityOrdinalDecision decision;
        decision.opportunityOrdinal = ordinal;
        decision.targetFrame = target;
        if (target >= input.requiredFrameCount) {
            decision.pastSourceDomain = true;
            ++result.pastSourceDomain;
        } else if (target == previousOutput) {
            decision.repeated = true;
            ++result.repeated;
        } else if (target < previousOutput) {
            return fail(OpportunityOrdinalError::OrdinalRegression);
        } else {
            decision.displayed = true;
            decision.droppedBefore = target - previousOutput - 1;
            result.trueDropped += decision.droppedBefore;
            ++result.displayed;
            if (result.firstOutputFrame < 0)
                result.firstOutputFrame = target;
            if (target <= previousOutput)
                result.uniqueFrameStrictlyIncreasing = false;
            previousOutput = target;
            result.lastOutputFrame = target;
        }
        previousOrdinal = ordinal;
        result.decisions.push_back(decision);
    }

    result.lastOpportunityOrdinal = previousOrdinal;
    result.trueDropped += input.requiredFrameCount - previousOutput - 1;
    result.scheduled = result.displayed + result.trueDropped;
    result.frameZeroStarted = result.firstOutputFrame == 0;
    result.sourceDomainRespected = result.lastOutputFrame < input.requiredFrameCount;
    result.sourceDomainConserved = result.scheduled == input.requiredFrameCount;
    result.valid = result.frameZeroStarted && result.uniqueFrameStrictlyIncreasing &&
                   result.sourceDomainRespected && result.sourceDomainConserved;
    return result;
}

const char* opportunityOrdinalErrorName(OpportunityOrdinalError error) {
    switch (error) {
    case OpportunityOrdinalError::None:
        return "NONE";
    case OpportunityOrdinalError::InvalidDomain:
        return "INVALID_DOMAIN";
    case OpportunityOrdinalError::RefreshChanged:
        return "REFRESH_CHANGED";
    case OpportunityOrdinalError::DwmAuthorityMissing:
        return "DWM_AUTHORITY_MISSING";
    case OpportunityOrdinalError::DwmAuthorityDiscontinuity:
        return "DWM_AUTHORITY_DISCONTINUITY";
    case OpportunityOrdinalError::SwapCorrespondenceMissing:
        return "SWAP_CORRESPONDENCE_MISSING";
    case OpportunityOrdinalError::OpportunityAmbiguous:
        return "OPPORTUNITY_AMBIGUOUS";
    case OpportunityOrdinalError::FirstOrdinalNotZero:
        return "FIRST_ORDINAL_NOT_ZERO";
    case OpportunityOrdinalError::OrdinalRegression:
        return "ORDINAL_REGRESSION";
    case OpportunityOrdinalError::ArithmeticOverflow:
        return "ARITHMETIC_OVERFLOW";
    }
    return "UNKNOWN";
}

} // namespace mvm::test

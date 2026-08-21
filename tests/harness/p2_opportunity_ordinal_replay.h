#ifndef MVM_TESTS_P2_OPPORTUNITY_ORDINAL_REPLAY_H
#define MVM_TESTS_P2_OPPORTUNITY_ORDINAL_REPLAY_H

#include <vector>

namespace mvm::test {

enum class OpportunityOrdinalError {
    None,
    InvalidDomain,
    RefreshChanged,
    DwmAuthorityMissing,
    DwmAuthorityDiscontinuity,
    SwapCorrespondenceMissing,
    OpportunityAmbiguous,
    FirstOrdinalNotZero,
    OrdinalRegression,
    ArithmeticOverflow,
};

struct OpportunityOrdinalInput {
    long long requiredFrameCount = 0;
    long long sourceFpsNumerator = 0;
    long long sourceFpsDenominator = 0;
    long long refreshNumerator = 0;
    long long refreshDenominator = 0;
    bool refreshStable = true;
    bool dwmAuthorityAvailable = true;
    bool dwmAuthorityContinuous = true;
    bool swapCorrespondenceComplete = true;
    bool opportunityUnambiguous = true;
    std::vector<long long> opportunityOrdinals;
};

struct OpportunityOrdinalDecision {
    long long opportunityOrdinal = -1;
    long long targetFrame = -1;
    long long droppedBefore = 0;
    bool displayed = false;
    bool repeated = false;
    bool pastSourceDomain = false;
};

struct OpportunityOrdinalSummary {
    bool valid = false;
    OpportunityOrdinalError error = OpportunityOrdinalError::None;
    long long scheduled = 0;
    long long displayed = 0;
    long long trueDropped = 0;
    long long repeated = 0;
    long long pastSourceDomain = 0;
    long long firstOutputFrame = -1;
    long long lastOutputFrame = -1;
    long long lastOpportunityOrdinal = -1;
    long long opportunityGapCount = 0;
    bool frameZeroStarted = false;
    bool uniqueFrameStrictlyIncreasing = true;
    bool sourceDomainRespected = true;
    bool sourceDomainConserved = false;
    std::vector<OpportunityOrdinalDecision> decisions;
};

// P2-Q6診断専用のpure/offline model。実display opportunity ordinalを
// exact refresh rationalで60 fps source domainへ写像し、productionへは接続しない。
OpportunityOrdinalSummary replayOpportunityOrdinals(const OpportunityOrdinalInput& input);

const char* opportunityOrdinalErrorName(OpportunityOrdinalError error);

} // namespace mvm::test

#endif

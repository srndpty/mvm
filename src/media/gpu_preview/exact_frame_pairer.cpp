#include "media/gpu_preview/exact_frame_pairer.h"

namespace mvm::gpu {

PairResult ExactFramePairer::tryPair(long long outputFrameNumber, ComposedFrame& out) {
    counters_.staleADiscardCount +=
        static_cast<long long>(sourceA_.discardBefore(outputFrameNumber));
    counters_.staleBDiscardCount +=
        static_cast<long long>(sourceB_.discardBefore(outputFrameNumber));

    SourceFrameIdentity identityA;
    SourceFrameIdentity identityB;
    const bool hasA = sourceA_.peekFrontIdentity(identityA);
    const bool hasB = sourceB_.peekFrontIdentity(identityB);
    const bool missingA = hasA && identityA.frameNumber > outputFrameNumber;
    const bool missingB = hasB && identityB.frameNumber > outputFrameNumber;

    if (missingA || missingB) {
        if (missingA)
            ++counters_.missingACount;
        if (missingB)
            ++counters_.missingBCount;
        if (missingA && missingB)
            return PairResult::MissingBoth;
        return missingA ? PairResult::MissingA : PairResult::MissingB;
    }
    if (!hasA || !hasB)
        return PairResult::WaitingForSource;

    DecodedGpuFrame frameA;
    DecodedGpuFrame frameB;
    if (!SourceFrameBuffer::takeExactPair(sourceA_, sourceB_, outputFrameNumber, frameA, frameB)) {
        ++counters_.mixedFrameRejected;
        return PairResult::MixedFrame;
    }

    const CompositionResult result = coordinator_.compose(outputFrameNumber, {frameA, frameB}, out);
    switch (result) {
    case CompositionResult::Accepted:
        ++counters_.pairedCount;
        return PairResult::Paired;
    case CompositionResult::MixedFrame:
        ++counters_.mixedFrameRejected;
        return PairResult::MixedFrame;
    case CompositionResult::StaleGeneration:
        ++counters_.staleGenerationRejected;
        return PairResult::StaleGeneration;
    case CompositionResult::FutureGeneration:
        ++counters_.futureGenerationRejected;
        return PairResult::FutureGeneration;
    default:
        return PairResult::Rejected;
    }
}

} // namespace mvm::gpu

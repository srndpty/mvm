#include "media/gpu_preview/exact_frame_pairer.h"

namespace mvm::gpu {

PairResult ExactFramePairer::tryPair(long long outputFrameNumber, ComposedFrame& out) {
    if (sources_.empty())
        return PairResult::Rejected;

    for (size_t i = 0; i < sources_.size(); ++i) {
        const auto discarded =
            static_cast<long long>(sources_[i]->discardBefore(outputFrameNumber));
        counters_.staleDiscardCounts[i] += discarded;
        if (i == 0)
            counters_.staleADiscardCount += discarded;
        else if (i == 1)
            counters_.staleBDiscardCount += discarded;
    }

    bool anyMissing = false;
    bool allMissing = true;
    bool anyEmpty = false;
    bool firstSourceMissing = false;
    for (size_t i = 0; i < sources_.size(); ++i) {
        SourceFrameIdentity identity;
        const bool has = sources_[i]->peekFrontIdentity(identity);
        if (!has)
            anyEmpty = true;
        // front が要求 frame より先へ進んでいる = この frame はもう来ない。
        const bool missing = has && identity.frameNumber > outputFrameNumber;
        if (missing) {
            anyMissing = true;
            if (i == 0)
                firstSourceMissing = true;
            ++counters_.missingCounts[i];
            if (i == 0)
                ++counters_.missingACount;
            else if (i == 1)
                ++counters_.missingBCount;
        } else {
            allMissing = false;
        }
    }

    if (anyMissing) {
        // 2 source 契約の報告をそのまま保つ。source が 1 本の場合、
        // 「両方欠落」は表現できないので MissingA を返す。
        if (allMissing && sources_.size() >= 2)
            return PairResult::MissingBoth;
        return firstSourceMissing ? PairResult::MissingA : PairResult::MissingB;
    }
    if (anyEmpty)
        return PairResult::WaitingForSource;

    std::vector<DecodedGpuFrame> frames;
    if (!SourceFrameBuffer::takeExactAll(sources_, outputFrameNumber, frames)) {
        ++counters_.mixedFrameRejected;
        return PairResult::MixedFrame;
    }

    const CompositionResult result = coordinator_.compose(outputFrameNumber, frames, out);
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

#ifndef MVM_GPU_PREVIEW_EXACT_FRAME_PAIRER_H
#define MVM_GPU_PREVIEW_EXACT_FRAME_PAIRER_H

#include "media/gpu_preview/compositor_coordinator.h"
#include "media/gpu_preview/source_frame_buffer.h"

namespace mvm::gpu {

enum class PairResult {
    Paired = 0,
    WaitingForSource,
    MissingA,
    MissingB,
    MissingBoth,
    StaleGeneration,
    FutureGeneration,
    MixedFrame,
    Rejected,
};

struct ExactPairingCounters {
    long long pairedCount = 0;
    long long missingACount = 0;
    long long missingBCount = 0;
    long long staleADiscardCount = 0;
    long long staleBDiscardCount = 0;
    long long mixedFrameRejected = 0;
    long long staleGenerationRejected = 0;
    long long futureGenerationRejected = 0;
    long long partialPairConsumeCount = 0;
};

// 2本のsource-local bufferから同じoutput frame番号だけを取り出す。
// future frameは消費せず、片方だけ一致してもComposedFrameを作らない。
class ExactFramePairer {
public:
    ExactFramePairer(SourceFrameBuffer& sourceA, SourceFrameBuffer& sourceB,
                     CompositorCoordinator& coordinator)
        : sourceA_(sourceA), sourceB_(sourceB), coordinator_(coordinator) {}

    PairResult tryPair(long long outputFrameNumber, ComposedFrame& out);

    const ExactPairingCounters& counters() const { return counters_; }

private:
    SourceFrameBuffer& sourceA_;
    SourceFrameBuffer& sourceB_;
    CompositorCoordinator& coordinator_;
    ExactPairingCounters counters_;
};

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_EXACT_FRAME_PAIRER_H

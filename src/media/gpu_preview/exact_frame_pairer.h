#ifndef MVM_GPU_PREVIEW_EXACT_FRAME_PAIRER_H
#define MVM_GPU_PREVIEW_EXACT_FRAME_PAIRER_H

#include "media/gpu_preview/compositor_coordinator.h"
#include "media/gpu_preview/source_frame_buffer.h"

#include <vector>

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
    // N source 版の内訳。index は構築時に渡した source 順に対応する。
    // 2 source の場合、index 0/1 はそれぞれ `missingACount` / `missingBCount`、
    // `staleADiscardCount` / `staleBDiscardCount` と同じ値になる。
    std::vector<long long> missingCounts;
    std::vector<long long> staleDiscardCounts;
};

// N 本の source-local buffer から同じ output frame 番号だけを取り出す。
// future frame は消費せず、一つでも一致しなければ ComposedFrame を作らない。
// source が 1 本の場合も同じ判定を通るため、layer 数で経路を分けない。
class ExactFramePairer {
public:
    ExactFramePairer(std::vector<SourceFrameBuffer*> sources, CompositorCoordinator& coordinator)
        : sources_(std::move(sources)), coordinator_(coordinator) {
        valid_ = preflight(sources_);
        counters_.missingCounts.assign(sources_.size(), 0);
        counters_.staleDiscardCounts.assign(sources_.size(), 0);
    }

    // 2 source 版。P1〜P4 の frozen 呼び出し側のために残す。
    ExactFramePairer(SourceFrameBuffer& sourceA, SourceFrameBuffer& sourceB,
                     CompositorCoordinator& coordinator)
        : ExactFramePairer(std::vector<SourceFrameBuffer*>{&sourceA, &sourceB}, coordinator) {}

    // buffer 集合が構築時点で使える形かどうか。false の pairer は
    // `tryPair()` が必ず `Rejected` を返し、buffer を一切触らない。
    bool valid() const { return valid_; }

    PairResult tryPair(long long outputFrameNumber, ComposedFrame& out);

    size_t sourceCount() const { return sources_.size(); }

    const ExactPairingCounters& counters() const { return counters_; }

private:
    // empty / null / 同一 buffer の重複 / 同一 SourceId の別 buffer を弾く。
    // `tryPair()` は `takeExactAll()` へ到達する前に dereference するので、
    // この検査を buffer 集合の入口で行わないと防御が効かない。
    static bool preflight(const std::vector<SourceFrameBuffer*>& sources);

    std::vector<SourceFrameBuffer*> sources_;
    CompositorCoordinator& coordinator_;
    ExactPairingCounters counters_;
    bool valid_ = false;
};

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_EXACT_FRAME_PAIRER_H

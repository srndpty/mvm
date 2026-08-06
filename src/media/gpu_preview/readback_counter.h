/*
 * mvm Phase 1 / P1 - CPU readback / GPU copy の計数
 *
 * exit criteria の 1 つが **cpu_full_frame_readback_count == 0** である。
 * 「readback していないつもり」を根拠にしない。実際に数える。
 *
 * 重要なのは **full-frame と marker 帯を混同しないこと**。
 *   - full frame  : フレーム全体を CPU へ落とした。**1 回でも不合格**
 *   - marker band : 1216x64 (1080p の 3.7%) だけを読んだ。診断情報
 * 1 つのカウンタにまとめると、marker 検証を有効にしただけで
 * 不合格になるか、あるいは full-frame readback が紛れても気づけなくなる。
 *
 * 増やす場所は各実装の 1 箇所だけにする (grep で全部見えること)。
 */

#ifndef MVM_GPU_PREVIEW_READBACK_COUNTER_H
#define MVM_GPU_PREVIEW_READBACK_COUNTER_H

#include <atomic>

namespace mvm::gpu {

class ReadbackCounters {
public:
    // フレーム全体を CPU へ落とした。P1 ではここが増えたら不合格。
    void noteFullFrameReadback() { fullFrame_.fetch_add(1, std::memory_order_relaxed); }

    // marker 帯 (1216x64) だけを読んだ。判定には使わない。
    void noteMarkerBandReadback() { markerBand_.fetch_add(1, std::memory_order_relaxed); }

    // color patch (小領域) だけを読んだ。判定には使わない。
    void noteColorPatchReadback() { colorPatch_.fetch_add(1, std::memory_order_relaxed); }

    // GPU 内の copy / 変換 pass。CPU 転送ではない。
    void noteGpuCopy() { gpuCopy_.fetch_add(1, std::memory_order_relaxed); }

    long long fullFrameReadbacks() const { return fullFrame_.load(std::memory_order_relaxed); }

    long long markerBandReadbacks() const { return markerBand_.load(std::memory_order_relaxed); }

    long long colorPatchReadbacks() const { return colorPatch_.load(std::memory_order_relaxed); }

    long long gpuCopies() const { return gpuCopy_.load(std::memory_order_relaxed); }

    void reset() {
        fullFrame_.store(0, std::memory_order_relaxed);
        markerBand_.store(0, std::memory_order_relaxed);
        colorPatch_.store(0, std::memory_order_relaxed);
        gpuCopy_.store(0, std::memory_order_relaxed);
    }

private:
    std::atomic<long long> fullFrame_{0};
    std::atomic<long long> markerBand_{0};
    std::atomic<long long> colorPatch_{0};
    std::atomic<long long> gpuCopy_{0};
};

// プロセス全体で 1 つ。decode thread と render thread の両方から増える。
//
// グローバルにしているのは、「どこか 1 箇所でも readback したら見える」
// ことが要件だからである。インスタンスごとに数えると、
// 数え忘れた経路が生き残る。
ReadbackCounters& globalReadbackCounters();

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_READBACK_COUNTER_H

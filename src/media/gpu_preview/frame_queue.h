/*
 * mvm Phase 1 / P1 - decode thread と render thread の間の frame 受け渡し
 *
 * IPreviewSurface の実装。Qt には依存しない (だから単体テストできる)。
 *
 * ここが守る 3 つの契約:
 *
 *  1. **stale generation rejection**
 *     seek 前のフレームが 1 枚遅れて届いても表示しない。
 *     これが無いと、seek 直後に飛ぶ前の絵が一瞬出る。
 *
 *  2. **device mismatch rejection**
 *     別の ID3D11Device の texture を受け取らない。
 *     受け取ると描画は「成功」するが結果は未定義になる。
 *     P1 の主要仮説 (device 共有) が崩れていることを、絵ではなく
 *     この拒否で検出する。
 *
 *  3. **frame lifetime**
 *     表示に使ったフレームは、GPU が読み終わるまで解放しない。
 *     D3D11 の immediate context には fence が無いので、
 *     直近 retainDepth 枚を保持して解放を遅らせる。
 */

#ifndef MVM_GPU_PREVIEW_FRAME_QUEUE_H
#define MVM_GPU_PREVIEW_FRAME_QUEUE_H

#include "media/gpu_preview/preview_surface.h"

#include <condition_variable>
#include <deque>
#include <mutex>

namespace mvm::gpu {

// final にしていないのは、deviceOfTexture を差し替えて
// device mismatch の negative test を実 GPU 無しで書くためである。
class PreviewFrameQueue : public IPreviewSurface {
public:
    // capacity: 表示待ちの上限。超えたら submitFrame は RejectedQueueFull を返し、
    //           decode 側が待つ (backpressure)。落として詰め込まない。
    // retainDepth: 表示後に保持しておくフレーム数。
    explicit PreviewFrameQueue(size_t capacity = 3, size_t retainDepth = 3);
    ~PreviewFrameQueue() override;

    // 期待する device。null なら device 検査を行わない。
    void setExpectedDevice(ID3D11Device* device);

    // これより古い generation の frame を拒否する。
    void setCurrentGeneration(unsigned long long generation);
    unsigned long long currentGeneration() const;

    SubmitResult submitFrame(const DecodedGpuFrame& frame) override;
    void clear() override;
    long long displayedFrameNumber() const override;

    // --- render thread 側 ---------------------------------------------------
    // 表示するフレームを 1 枚取り出す。無ければ false。
    bool takeForDisplay(DecodedGpuFrame& out);
    // 実際に描画し終えたことを記録する。**描画してから呼ぶ。**
    // 「submit した番号」を displayedFrameNumber にすると、
    // 表示できていないのに一致したことになる。
    void noteDisplayed(const DecodedGpuFrame& frame);

    // --- decode thread 側 ---------------------------------------------------
    // 空きができるまで待つ。stop() されたら false。
    bool waitForSpace(int timeoutMs);
    void stop();
    void restart();

    // --- 統計 ---------------------------------------------------------------
    size_t depth() const;
    long long submittedCount() const;
    long long displayedCount() const;
    long long rejectedStaleCount() const;
    long long rejectedDeviceMismatchCount() const;
    long long queueFullCount() const;

protected:
    // texture の所有 device。テストから差し替えられるようにしている
    // (device mismatch の negative test を実 GPU 無しで書くため)。
    virtual ID3D11Device* deviceOfTexture(ID3D11Texture2D* texture) const;

private:
    mutable std::mutex mutex_;
    std::condition_variable spaceAvailable_;

    std::deque<DecodedGpuFrame> pending_;
    std::deque<FrameLifetimeToken> retained_;

    size_t capacity_;
    size_t retainDepth_;
    ID3D11Device* expectedDevice_ = nullptr;
    unsigned long long generation_ = 0;
    long long displayedFrame_ = -1;
    bool stopped_ = false;

    long long submitted_ = 0;
    long long displayed_ = 0;
    long long staleRejects_ = 0;
    long long deviceRejects_ = 0;
    long long queueFull_ = 0;
};

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_FRAME_QUEUE_H

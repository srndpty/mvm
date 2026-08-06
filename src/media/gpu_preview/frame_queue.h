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
 *  3. **generation 契約 (fail-closed)**
 *     submit は generation を厳密に照合する。過去 (stale) は正常な破棄、
 *     未来 (future) は表示側がまだ知らない世代なので拒否する。
 *     setCurrentGeneration は逆行 (regression) を拒否する。
 *     「表示に使ったフレームを GPU 完了まで解放しない」責務は
 *     このクラスではなく GpuRetirementQueue が持つ (§1)。
 *     直近 N 枚を保持する retainDepth 方式は **廃止した**。
 */

#ifndef MVM_GPU_PREVIEW_FRAME_QUEUE_H
#define MVM_GPU_PREVIEW_FRAME_QUEUE_H

#include "media/gpu_preview/preview_surface.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>

namespace mvm::gpu {

// final にしていないのは、deviceOfTexture を差し替えて
// device mismatch の negative test を実 GPU 無しで書くためである。
class PreviewFrameQueue : public IPreviewSurface {
public:
    // capacity: 表示待ちの上限。超えたら submitFrame は RejectedQueueFull を返し、
    //           decode 側が待つ (backpressure)。落として詰め込まない。
    explicit PreviewFrameQueue(size_t capacity = 3);
    ~PreviewFrameQueue() override;

    // 期待する device。null なら device 検査を行わない。
    void setExpectedDevice(ID3D11Device* device);

    // SourceId は compositor/source registry が発行し、open 前に登録する。
    bool registerSource(SourceId source, SourceGeneration generation);
    bool unregisterSource(SourceId source);
    size_t registeredSourceCount() const;

    // 表示側が知る最新 generation を **source 単位で**更新する。
    //   new > current : 更新し、その source の pending を破棄する (Updated)
    //   new == current: 何もしない。**pending は破棄しない** (NoOp)
    //   new <  current: 逆行は受け付けない (RejectedRegression / fail-closed)
    //
    // **source 単位であることが要点 (P1.2 §2)。**
    // 全体で 1 つの generation にすると、source A の seek が
    // source B のフレームを stale/future にしてしまう。
    GenerationUpdateResult setCurrentGeneration(SourceId source, SourceGeneration generation);
    SourceGeneration currentGeneration(SourceId source) const;
    // まだ一度も generation を設定していない source かどうか。
    bool knowsSource(SourceId source) const;

    // 合成構成の世代。**compositor (P1.2 では preview 層) が所有する。**
    // decoder は発行しない。表示記録の照合に使う。
    void setCompositionEpoch(CompositionEpoch epoch);
    CompositionEpoch compositionEpoch() const;

    SubmitResult submitFrame(const DecodedGpuFrame& frame) override;
    void clear() override;
    long long displayedFrameNumber() const override;

    // --- render thread 側 ---------------------------------------------------
    // 表示するフレームを 1 枚取り出す。無ければ false。
    bool takeForDisplay(DecodedGpuFrame& out);
    // 実際に描画し終えたことを記録する。**描画してから呼ぶ。**
    // 「submit した番号」を displayedFrameNumber にすると、
    // 表示できていないのに一致したことになる。
    // frame の GPU 完了までの保持は呼び出し側 (GpuRetirementQueue) が行う。
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
    long long rejectedFutureCount() const;
    long long rejectedInvalidCount() const;
    long long rejectedDeviceMismatchCount() const;
    long long queueFullCount() const;
    long long generationRegressionCount() const;

protected:
    // texture の所有 device。テストから差し替えられるようにしている
    // (device mismatch の negative test を実 GPU 無しで書くため)。
    virtual ID3D11Device* deviceOfTexture(ID3D11Texture2D* texture) const;

private:
    mutable std::mutex mutex_;
    std::condition_variable spaceAvailable_;

    std::deque<DecodedGpuFrame> pending_;

    size_t capacity_;
    ID3D11Device* expectedDevice_ = nullptr;
    // source ごとの「表示側が知る最新 generation」。
    // P1.2 では 1 件しか入らないが、構造を単一 global へ固定しない。
    std::map<SourceId, SourceGeneration> generations_;
    CompositionEpoch compositionEpoch_{};
    long long displayedFrame_ = -1;
    bool stopped_ = false;

    long long submitted_ = 0;
    long long displayed_ = 0;
    long long staleRejects_ = 0;
    long long futureRejects_ = 0;
    long long invalidRejects_ = 0;
    long long deviceRejects_ = 0;
    long long queueFull_ = 0;
    long long generationRegressions_ = 0;
};

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_FRAME_QUEUE_H

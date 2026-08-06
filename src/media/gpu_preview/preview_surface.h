/*
 * mvm Phase 1 / P1 - 表示側の境界
 *
 * decoder はこの interface しか知らない。Qt も QRhi も見えない。
 */

#ifndef MVM_GPU_PREVIEW_PREVIEW_SURFACE_H
#define MVM_GPU_PREVIEW_PREVIEW_SURFACE_H

#include "media/gpu_preview/gpu_frame.h"

namespace mvm::gpu {

// submitFrame が受け付けなかった理由。
// 「false」だけでは、seek 直後の正常な破棄なのか
// device 取り違えという致命的な誤りなのかが区別できない。
enum class SubmitResult {
    Accepted = 0,
    RejectedStaleGeneration, // seek より前のフレームが遅れて届いた (正常)
    RejectedInvalidFrame,    // texture が無い / 解像度が 0 など
    RejectedDeviceMismatch,  // 別の ID3D11Device の texture (致命的)
    RejectedNotReady,        // surface がまだ初期化されていない
    RejectedQueueFull,       // 表示が追いついていない (backpressure)
};

const char* toString(SubmitResult r);

class IPreviewSurface {
public:
    virtual ~IPreviewSurface() = default;

    // frame を表示待ちへ載せる。所有権は取らない
    // (lifetime token を通じて参照だけ保持する)。
    virtual SubmitResult submitFrame(const DecodedGpuFrame& frame) = 0;

    // 表示内容を捨てて背景だけにする。保持していた frame も解放する。
    virtual void clear() = 0;

    // 実際に **描画した** フレーム番号。まだ何も描いていなければ -1。
    // 「submit した番号」ではない。ここを取り違えると
    // 表示できていないのに一致したことになる。
    virtual long long displayedFrameNumber() const = 0;
};

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_PREVIEW_SURFACE_H

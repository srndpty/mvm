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
    RejectedStaleGeneration,  // seek より前のフレームが遅れて届いた (正常)
    RejectedFutureGeneration, // 表示側がまだ知らない未来の generation (fail-closed)
    RejectedInvalidFrame,     // texture が無い / 解像度が 0 など
    RejectedDeviceMismatch,   // 別の ID3D11Device の texture (致命的)
    RejectedNotReady,         // surface がまだ初期化されていない
    RejectedQueueFull,        // 表示が追いついていない (backpressure)
};

const char* toString(SubmitResult r);

// setCurrentGeneration の結果。
// **結果 enum と counter を分ける** (§3)。呼び出し側が「更新した / 何もしなかった /
// 逆行を拒否した」を型で受け取り、counter はその後で数える。
enum class GenerationUpdateResult {
    Updated = 0,        // new > current。更新し pending を破棄した
    NoOp,               // new == current。pending は破棄しない
    RejectedRegression, // new < current。逆行は受け付けない (fail-closed)
};

const char* toString(GenerationUpdateResult r);

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

/*
 * P2 境界の純粋契約だけを P1.2 で固定する。
 * decoder の frame に CompositionEpoch を後付けせず、compositor が採用した
 * その時点の epoch を値として保持する。
 */
#ifndef MVM_GPU_PREVIEW_COMPOSED_FRAME_H
#define MVM_GPU_PREVIEW_COMPOSED_FRAME_H

#include "media/gpu_preview/gpu_frame.h"

namespace mvm::gpu {

struct ComposedFrame {
    DecodedGpuFrame sourceFrame;
    CompositionEpoch compositionEpoch{};
};

inline ComposedFrame adoptForComposition(const DecodedGpuFrame& frame, CompositionEpoch epoch) {
    return ComposedFrame{frame, epoch};
}

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_COMPOSED_FRAME_H

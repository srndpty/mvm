/* 静止画 layer (文字など) を compositor へ渡す GPU frame の生成。Qt 非依存。 */
#ifndef MVM_GPU_PREVIEW_STILL_IMAGE_FRAME_H
#define MVM_GPU_PREVIEW_STILL_IMAGE_FRAME_H

#include "media/gpu_preview/d3d11_shared_device.h"
#include "media/gpu_preview/gpu_frame.h"

#include <cstddef>
#include <string>

namespace mvm::gpu {

// width x height の RGBA8 (straight alpha、行間の余白なし) から shader resource 用の
// texture を作り、RGBA8 の DecodedGpuFrame として返す。
//
// texture の参照は frame.lifetime だけが持つ。compositor は描画時に lifetime を
// GPU 完了まで保持するので、呼び出し側は frame を捨てるだけでよい。ただし
// 捨てる前に GpuCompositor::retireLayerTexture を呼んで SRV cache から外すこと。
//
// frameNumber は 0 固定。静止画は時間軸上の位置を持たない。
bool makeStillImageFrame(SharedD3D11Device& device, int width, int height,
                         const unsigned char* rgba, std::size_t byteCount, SourceId sourceId,
                         DecodedGpuFrame& out, std::string& err);

} // namespace mvm::gpu

#endif

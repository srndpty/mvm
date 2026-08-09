#ifndef MVM_GPU_PREVIEW_VISIBLE_UV_H
#define MVM_GPU_PREVIEW_VISIBLE_UV_H

#include "media/gpu_preview/composed_frame.h"

#include <optional>

namespace mvm::gpu {

// logical visible image の UV を physical decode allocation の UV へ変換する。
// physical が logical より小さい入力は暗黙補正せず拒否する。
std::optional<RectF> normalizeVisibleUv(const RectF& sourceUv, int logicalWidth, int logicalHeight,
                                        int physicalWidth, int physicalHeight);

} // namespace mvm::gpu

#endif

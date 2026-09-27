#include "media/gpu_preview/visible_uv.h"

namespace mvm::gpu {

std::optional<RectF> normalizeVisibleUv(const RectF& sourceUv, int logicalWidth, int logicalHeight,
                                        int physicalWidth, int physicalHeight) {
    if (logicalWidth <= 0 || logicalHeight <= 0 || physicalWidth < logicalWidth ||
        physicalHeight < logicalHeight)
        return std::nullopt;
    // 表示領域はallocationの左上に置かれるため、offsetも同じ比で縮める。
    const float scaleU = static_cast<float>(logicalWidth) / static_cast<float>(physicalWidth);
    const float scaleV = static_cast<float>(logicalHeight) / static_cast<float>(physicalHeight);
    return RectF{sourceUv.x * scaleU, sourceUv.y * scaleV, sourceUv.width * scaleU,
                 sourceUv.height * scaleV};
}

} // namespace mvm::gpu

#include "media/gpu_preview/visible_uv.h"

namespace mvm::gpu {

std::optional<RectF> normalizeVisibleUv(const RectF& sourceUv, int logicalWidth, int logicalHeight,
                                        int physicalWidth, int physicalHeight) {
    if (logicalWidth <= 0 || logicalHeight <= 0 || physicalWidth < logicalWidth ||
        physicalHeight < logicalHeight)
        return std::nullopt;
    RectF normalized = sourceUv;
    normalized.width *= static_cast<float>(logicalWidth) / static_cast<float>(physicalWidth);
    normalized.height *= static_cast<float>(logicalHeight) / static_cast<float>(physicalHeight);
    return normalized;
}

} // namespace mvm::gpu

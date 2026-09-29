#include "core/layer_placement.h"

#include <algorithm>
#include <cmath>

namespace mvm::core {
namespace {

bool positiveRect(const LayerRect& rect) {
    return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) &&
           std::isfinite(rect.height) && rect.width > 0.0 && rect.height > 0.0;
}

} // namespace

LayerPlacement placeLayer(int frameWidth, int frameHeight, int canvasWidth, int canvasHeight,
                          const LayerRect& crop, const LayerRect& destination) {
    LayerPlacement result;
    if (frameWidth <= 0 || frameHeight <= 0 || canvasWidth <= 0 || canvasHeight <= 0 ||
        !positiveRect(crop) || !positiveRect(destination))
        return result;

    // 1. letterbox。どちらの辺を合わせるかは整数の積で決める (aspectFit と同じ判定)。
    const long long frameByCanvas = static_cast<long long>(frameWidth) * canvasHeight;
    const long long canvasByFrame = static_cast<long long>(canvasWidth) * frameHeight;
    LayerRect fitted;
    if (frameByCanvas > canvasByFrame) {
        fitted.height = static_cast<double>(canvasByFrame) / static_cast<double>(frameByCanvas);
    } else {
        fitted.width = static_cast<double>(frameByCanvas) / static_cast<double>(canvasByFrame);
    }
    fitted.x = (1.0 - fitted.width) * 0.5;
    fitted.y = (1.0 - fitted.height) * 0.5;

    // 2. crop と素材の重なり。
    const double left = std::max(crop.x, fitted.x);
    const double top = std::max(crop.y, fitted.y);
    const double right = std::min(crop.x + crop.width, fitted.x + fitted.width);
    const double bottom = std::min(crop.y + crop.height, fitted.y + fitted.height);
    if (right <= left || bottom <= top)
        return result;

    result.sourceUv = {(left - fitted.x) / fitted.width, (top - fitted.y) / fitted.height,
                       (right - left) / fitted.width, (bottom - top) / fitted.height};

    // 3. crop 範囲 -> destination の写像を、描く範囲にも同じく適用する。
    const double scaleX = destination.width / crop.width;
    const double scaleY = destination.height / crop.height;
    result.destination = {destination.x + (left - crop.x) * scaleX,
                          destination.y + (top - crop.y) * scaleY, (right - left) * scaleX,
                          (bottom - top) * scaleY};
    result.pivotX = destination.x + destination.width * 0.5;
    result.pivotY = destination.y + destination.height * 0.5;
    result.empty = false;
    return result;
}

} // namespace mvm::core

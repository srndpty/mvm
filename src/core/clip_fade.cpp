#include "core/clip_fade.h"

#include <algorithm>

namespace mvm::core {
namespace {

double edgeFactor(std::int64_t distanceFromEdge, std::int64_t fadeFrames) {
    if (fadeFrames <= 0)
        return 1.0;
    if (distanceFromEdge < 0)
        return 0.0;
    if (distanceFromEdge >= fadeFrames)
        return 1.0;
    if (fadeFrames == 1)
        return 0.0;
    return static_cast<double>(distanceFromEdge) / static_cast<double>(fadeFrames - 1);
}

} // namespace

double clipFadeFactor(std::int64_t localFrame, std::int64_t clipDuration, std::int64_t fadeInFrames,
                      std::int64_t fadeOutFrames) {
    if (clipDuration <= 0 || localFrame < 0 || localFrame >= clipDuration || fadeInFrames < 0 ||
        fadeOutFrames < 0 || fadeInFrames > clipDuration || fadeOutFrames > clipDuration ||
        fadeInFrames > clipDuration - fadeOutFrames) {
        return 0.0;
    }
    const double fadeIn = edgeFactor(localFrame, fadeInFrames);
    const double fadeOut = edgeFactor(clipDuration - 1 - localFrame, fadeOutFrames);
    return std::min(fadeIn, fadeOut);
}

} // namespace mvm::core

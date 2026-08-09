#include "app/preview/display_target_contract.h"

#include <cmath>

namespace mvm::app {
namespace {
constexpr int kRequiredWidth = 1920;
constexpr int kRequiredHeight = 1080;
constexpr double kDprEpsilon = 1e-6;

bool positiveGeometry(const DisplayEnvironmentSnapshot& value) {
    return value.screenGeometryWidth > 0 && value.screenGeometryHeight > 0 &&
           value.availableGeometryWidth > 0 && value.availableGeometryHeight > 0 &&
           value.windowLogicalWidth > 0 && value.windowLogicalHeight > 0 &&
           value.compositorSurfaceLogicalWidth > 0 && value.compositorSurfaceLogicalHeight > 0 &&
           value.rhiTargetPixelWidth > 0 && value.rhiTargetPixelHeight > 0 &&
           std::isfinite(value.devicePixelRatio) && value.devicePixelRatio > 0.0;
}
} // namespace

DisplayTargetPreflightResult evaluateP3C2DisplayTarget(const DisplayEnvironmentSnapshot& value) {
    if (!positiveGeometry(value))
        return {DisplayTargetPreflightState::Waiting, {}};
    if (value.windowLogicalWidth != kRequiredWidth || value.windowLogicalHeight != kRequiredHeight)
        return {DisplayTargetPreflightState::Failed,
                "QQuickWindow logical size が 1920x1080 ではありません"};
    if (value.compositorSurfaceLogicalWidth != kRequiredWidth ||
        value.compositorSurfaceLogicalHeight != kRequiredHeight)
        return {DisplayTargetPreflightState::Failed,
                "CompositorSurface logical size が 1920x1080 ではありません"};
    if (value.rhiTargetPixelWidth != kRequiredWidth ||
        value.rhiTargetPixelHeight != kRequiredHeight)
        return {DisplayTargetPreflightState::Failed,
                "actual RHI target pixel size が 1920x1080 ではありません"};
    if (std::abs(value.devicePixelRatio - 1.0) > kDprEpsilon)
        return {DisplayTargetPreflightState::Failed,
                "devicePixelRatio が P3-C-2 formal precondition の 1.0 ではありません"};
    return {DisplayTargetPreflightState::Passed, {}};
}

bool sameDisplayEnvironment(const DisplayEnvironmentSnapshot& a,
                            const DisplayEnvironmentSnapshot& b) {
    return a.screenName == b.screenName && a.screenOrientation == b.screenOrientation &&
           a.screenGeometryWidth == b.screenGeometryWidth &&
           a.screenGeometryHeight == b.screenGeometryHeight &&
           a.availableGeometryWidth == b.availableGeometryWidth &&
           a.availableGeometryHeight == b.availableGeometryHeight &&
           std::isfinite(a.devicePixelRatio) && std::isfinite(b.devicePixelRatio) &&
           std::abs(a.devicePixelRatio - b.devicePixelRatio) <= kDprEpsilon &&
           a.windowLogicalWidth == b.windowLogicalWidth &&
           a.windowLogicalHeight == b.windowLogicalHeight &&
           a.compositorSurfaceLogicalWidth == b.compositorSurfaceLogicalWidth &&
           a.compositorSurfaceLogicalHeight == b.compositorSurfaceLogicalHeight &&
           a.rhiTargetPixelWidth == b.rhiTargetPixelWidth &&
           a.rhiTargetPixelHeight == b.rhiTargetPixelHeight;
}

} // namespace mvm::app

#include "media/gpu_preview/validation_reference.h"

#include <algorithm>
#include <cmath>

namespace mvm::gpu {
namespace {

int clampByte(double value) {
    return std::clamp(static_cast<int>(std::lround(value)), 0, 255);
}

} // namespace

Rgba8 bt709Limited(double y, double u, double v) {
    const double c = y - 16.0;
    const double d = u - 128.0;
    const double e = v - 128.0;
    return {clampByte(1.164383 * c + 1.792741 * e),
            clampByte(1.164383 * c - 0.213249 * d - 0.532909 * e),
            clampByte(1.164383 * c + 2.112402 * d), 255};
}

Rgba8 straightAlphaBlend(Rgba8 source, Rgba8 destination, double opacity) {
    auto blend = [opacity](int s, int d) { return clampByte(s * opacity + d * (1.0 - opacity)); };
    return {blend(source.r, destination.r), blend(source.g, destination.g),
            blend(source.b, destination.b), 255};
}

bool probeWithinTolerance(const Rgba8& actual, const Rgba8& expected, int rgbTolerance) {
    return std::abs(actual.r - expected.r) <= rgbTolerance &&
           std::abs(actual.g - expected.g) <= rgbTolerance &&
           std::abs(actual.b - expected.b) <= rgbTolerance && actual.a == 255 && expected.a == 255;
}

} // namespace mvm::gpu

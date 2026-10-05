#include "media/math/math_transform.h"

#include "media/math/math_key_material.h"
#include "media/math/math_raster_layout.h"

#include <algorithm>
#include <cstddef>

namespace mvm::math {

using detail::appendKeyField;

std::string mathTransformKey(const MathTransformSpec& spec,
                             const MathToolchainFingerprint& toolchain,
                             const std::string& transformTemplate,
                             const MathTransformAlgorithms& algorithms) {
    std::string material = "mvm-math-transform/1\n";
    appendKeyField(material, "segmenter", algorithms.segmenter);
    appendKeyField(material, "matching", algorithms.matching);
    material += "frames=" + std::to_string(spec.frames) + "\n";
    appendKeyField(material, "source_syntax", spec.source.syntax);
    appendKeyField(material, "source_source", spec.source.source);
    material += "source_font_size=" + std::to_string(spec.source.fontSize) + "\n";
    appendKeyField(material, "target_syntax", spec.target.syntax);
    appendKeyField(material, "target_source", spec.target.source);
    material += "target_font_size=" + std::to_string(spec.target.fontSize) + "\n";
    appendKeyField(material, "backend", toolchain.backendId);
    appendKeyField(material, "toolchain", toolchain.canonical);
    appendKeyField(material, "transform_template", transformTemplate);
    return detail::keyDigest(material);
}

bool mathEndpointPlacement(int maskWidth, int maskHeight, int canvasWidth, int canvasHeight,
                           MathEndpointPlacement& placement) {
    // 整数の位置は静止の配置と同じ規則 (一本化)。
    int left = 0;
    int top = 0;
    if (!mathRasterPlacement(maskWidth, maskHeight, canvasWidth, canvasHeight, left, top))
        return false;
    // 静止の mask の中心 (left + w/2) と、backend が式を置く canvas の中心 (W/2) の差。
    // 2 倍した整数で計算し、0.5 刻みの値を厳密に得る。
    placement = {left, top, (2.0 * left + maskWidth - canvasWidth) / 2.0,
                 (2.0 * top + maskHeight - canvasHeight) / 2.0};
    return true;
}

bool mathTransformPlacement(int sourceWidth, int sourceHeight, int targetWidth, int targetHeight,
                            int canvasWidth, int canvasHeight, MathTransformPlacement& placement) {
    MathTransformPlacement result;
    if (!mathEndpointPlacement(sourceWidth, sourceHeight, canvasWidth, canvasHeight,
                               result.source) ||
        !mathEndpointPlacement(targetWidth, targetHeight, canvasWidth, canvasHeight, result.target))
        return false;
    placement = result;
    return true;
}

bool mathCoverageValid(const MathCoverage& coverage) {
    return coverage.width > 0 && coverage.height > 0 &&
           coverage.alpha.size() ==
               static_cast<std::size_t>(coverage.width) * static_cast<std::size_t>(coverage.height);
}

MathRect mathCoverageBounds(const MathCoverage& coverage) {
    if (!mathCoverageValid(coverage))
        return {};
    int left = coverage.width;
    int top = coverage.height;
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < coverage.height; ++y) {
        const std::uint8_t* row =
            coverage.alpha.data() +
            static_cast<std::size_t>(y) * static_cast<std::size_t>(coverage.width);
        for (int x = 0; x < coverage.width; ++x) {
            if (row[x] == 0)
                continue;
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
    }
    if (right < 0)
        return {};
    return {left, top, right - left + 1, bottom - top + 1};
}

MathRect mathRectUnion(const MathRect& a, const MathRect& b) {
    if (a.empty())
        return b.empty() ? MathRect{} : b;
    if (b.empty())
        return a;
    const int left = std::min(a.x, b.x);
    const int top = std::min(a.y, b.y);
    const int right = std::max(a.x + a.width, b.x + b.width);
    const int bottom = std::max(a.y + a.height, b.y + b.height);
    return {left, top, right - left, bottom - top};
}

bool mathRectTouchesEdge(const MathRect& bounds, int width, int height) {
    return !bounds.empty() && (bounds.x <= 0 || bounds.y <= 0 || bounds.x + bounds.width >= width ||
                               bounds.y + bounds.height >= height);
}

std::int64_t mathEndpointDifference(const MathCoverage& frame, const MathCoverage& mask, int left,
                                    int top) {
    if (!mathCoverageValid(frame) || !mathCoverageValid(mask) || left < 0 || top < 0 ||
        left + mask.width > frame.width || top + mask.height > frame.height)
        return -1;
    std::int64_t different = 0;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const std::uint8_t got =
                frame.alpha[static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) +
                            static_cast<std::size_t>(x)];
            const bool inside =
                x >= left && x < left + mask.width && y >= top && y < top + mask.height;
            const std::uint8_t want = inside ? mask.alpha[static_cast<std::size_t>(y - top) *
                                                              static_cast<std::size_t>(mask.width) +
                                                          static_cast<std::size_t>(x - left)]
                                             : 0;
            if (got != want)
                ++different;
        }
    }
    return different;
}

bool mathTransformColorAt(std::uint32_t fromArgb, std::uint32_t toArgb, std::int64_t frame,
                          std::int64_t frames, std::uint32_t& argb) {
    // 2^40 までなら 2 * 255 * frames が int64 に収まる。
    constexpr std::int64_t kMaximumFrames = std::int64_t{1} << 40;
    if (frames <= 0 || frames > kMaximumFrames || frame < 0 || frame > frames)
        return false;
    std::uint32_t result = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const std::int64_t from = (fromArgb >> shift) & 0xFFu;
        const std::int64_t to = (toArgb >> shift) & 0xFFu;
        // round(from + (to - from) * frame / frames)、0.5 は切り上げ。整数だけで計算する。
        const std::int64_t weighted = from * (frames - frame) + to * frame;
        const std::int64_t channel = (2 * weighted + frames) / (2 * frames);
        result |= static_cast<std::uint32_t>(channel) << shift;
    }
    argb = result;
    return true;
}

bool mathTransformRasterPlacement(int artifactWidth, int artifactHeight, int sourceX, int sourceY,
                                  int sourceWidth, int sourceHeight, int targetX, int targetY,
                                  int targetWidth, int targetHeight, int outputWidth,
                                  int outputHeight, MathTransformRasterPlacement& placement) {
    const auto inside = [&](int x, int y, int width, int height) {
        return width > 0 && height > 0 && x >= 0 && y >= 0 && width <= artifactWidth - x &&
               height <= artifactHeight - y;
    };
    if (artifactWidth <= 0 || artifactHeight <= 0 ||
        !inside(sourceX, sourceY, sourceWidth, sourceHeight) ||
        !inside(targetX, targetY, targetWidth, targetHeight))
        return false;
    // 端点の静止の位置は静止の配置そのもの (一本化)。
    int sourceLeft = 0;
    int sourceTop = 0;
    int targetLeft = 0;
    int targetTop = 0;
    if (!mathRasterPlacement(sourceWidth, sourceHeight, outputWidth, outputHeight, sourceLeft,
                             sourceTop) ||
        !mathRasterPlacement(targetWidth, targetHeight, outputWidth, outputHeight, targetLeft,
                             targetTop))
        return false;
    MathTransformRasterPlacement result{sourceLeft - sourceX, sourceTop - sourceY,
                                        targetLeft - targetX, targetTop - targetY};
    const auto fits = [&](int left, int top) {
        return left >= 0 && top >= 0 && artifactWidth <= outputWidth - left &&
               artifactHeight <= outputHeight - top;
    };
    if (!fits(result.sourceLeft, result.sourceTop) || !fits(result.targetLeft, result.targetTop))
        return false;
    placement = result;
    return true;
}

bool mathTransformArtifactOriginAt(const MathTransformRasterPlacement& placement,
                                   std::int64_t frame, std::int64_t frames, int& left, int& top) {
    // 2^28 までなら 2 * |to - from| (2^32 未満) * frames が int64 に収まる。
    constexpr std::int64_t kMaximumFrames = std::int64_t{1} << 28;
    if (frames <= 0 || frames > kMaximumFrames || frame < 0 || frame >= frames)
        return false;
    // from + (to - from) * frame / frames を、ちょうど半分なら target の側へ丸める。
    // 「0.5 は切り上げ」(+∞ 側) にすると、target が source より 1 画素小さく 2 枚のとき、最後の
    // frame (進み具合 1/2) が source の位置に残り、次の target の静止で 1 画素跳ぶ。
    const auto interpolate = [&](std::int64_t from, std::int64_t to) {
        const std::int64_t delta = to - from;
        const std::int64_t magnitude = delta < 0 ? -delta : delta;
        const std::int64_t step = (2 * magnitude * frame + frames) / (2 * frames);
        return static_cast<int>(from + (delta < 0 ? -step : step));
    };
    left = interpolate(placement.sourceLeft, placement.targetLeft);
    top = interpolate(placement.sourceTop, placement.targetTop);
    return true;
}

} // namespace mvm::math

#include "media/math/math_transform.h"

#include "media/math/math_key_material.h"
#include "media/math/math_raster_layout.h"

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

} // namespace mvm::math

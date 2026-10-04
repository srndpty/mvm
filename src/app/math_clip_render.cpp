#include "app/math_clip_render.h"

#include "media/still_image/static_image.h"
#include "project/project.h"

#include <cstdint>

namespace mvm::app {

math::MathRenderSpec mathRenderSpecFor(const project::MathClipData& data) {
    return {data.syntax, data.source, data.fontSize};
}

math::MathComposeResult composeMathClipRaster(const media::StillImage& mask,
                                              const project::MathClipData& data, int outputWidth,
                                              int outputHeight) {
    math::MathComposeStyle style;
    if (!project::parseArgbColor(data.color, style.colorArgb) ||
        !project::parseArgbColor(data.backgroundColor, style.backgroundArgb)) {
        math::MathComposeResult result;
        result.error = "数式の色は #AARRGGBB で指定してください";
        return result;
    }
    const auto expected =
        static_cast<std::size_t>(mask.width) * static_cast<std::size_t>(mask.height) * 4U;
    if (mask.width <= 0 || mask.height <= 0 || mask.rgba.size() != expected) {
        math::MathComposeResult result;
        result.error = "数式の画像の大きさが不正です";
        return result;
    }
    return math::composeMathRaster(mask.rgba.data(), mask.width, mask.height, style, outputWidth,
                                   outputHeight);
}

math::MathComposeResult composeMathClipFromPng(const std::filesystem::path& png,
                                               const project::MathClipData& data, int outputWidth,
                                               int outputHeight) {
    const auto decoded = media::loadStaticImage(png);
    if (!decoded.success) {
        math::MathComposeResult result;
        result.error = "数式の画像を読めません: " + decoded.error;
        return result;
    }
    return composeMathClipRaster(decoded.image, data, outputWidth, outputHeight);
}

} // namespace mvm::app

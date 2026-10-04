#include "media/math/math_raster_layout.h"

#include <cmath>
#include <cstddef>

namespace mvm::math {
namespace {

struct Straight {
    double r = 0;
    double g = 0;
    double b = 0;
    double a = 0; // 0..1
};

Straight fromArgb(std::uint32_t argb, double coverage) {
    return {static_cast<double>((argb >> 16) & 0xFFu), static_cast<double>((argb >> 8) & 0xFFu),
            static_cast<double>(argb & 0xFFu),
            static_cast<double>((argb >> 24) & 0xFFu) / 255.0 * coverage};
}

std::uint8_t toByte(double value) {
    const double rounded = std::floor(value + 0.5);
    return static_cast<std::uint8_t>(rounded < 0 ? 0 : (rounded > 255 ? 255 : rounded));
}

// 被覆 coverageByte の glyph を背景の上に置いた 1 画素 (straight alpha の over 合成)。
// 静止の合成と Write の patch が共有する。何も無い所は 0 (透明) にする。
void composePixel(std::uint8_t coverageByte, std::uint32_t colorArgb, const Straight& background,
                  std::uint8_t* out) {
    const Straight glyph = fromArgb(colorArgb, coverageByte / 255.0);
    const double alpha = glyph.a + background.a * (1.0 - glyph.a);
    if (alpha <= 0.0) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    const double backgroundWeight = background.a * (1.0 - glyph.a);
    out[0] = toByte((glyph.r * glyph.a + background.r * backgroundWeight) / alpha);
    out[1] = toByte((glyph.g * glyph.a + background.g * backgroundWeight) / alpha);
    out[2] = toByte((glyph.b * glyph.a + background.b * backgroundWeight) / alpha);
    out[3] = toByte(alpha * 255.0);
}

} // namespace

bool mathRasterPlacement(int maskWidth, int maskHeight, int outputWidth, int outputHeight,
                         int& left, int& top) {
    if (maskWidth <= 0 || maskHeight <= 0 || maskWidth > outputWidth || maskHeight > outputHeight)
        return false;
    left = (outputWidth - maskWidth) / 2;
    top = (outputHeight - maskHeight) / 2;
    return true;
}

MathComposeResult composeMathRaster(const std::uint8_t* maskRgba, int maskWidth, int maskHeight,
                                    const MathComposeStyle& style, int outputWidth,
                                    int outputHeight) {
    MathComposeResult result;
    if (!maskRgba || maskWidth <= 0 || maskHeight <= 0 || outputWidth <= 0 || outputHeight <= 0) {
        result.error = "数式の画像または出力の大きさが不正です";
        return result;
    }
    int left = 0;
    int top = 0;
    if (!mathRasterPlacement(maskWidth, maskHeight, outputWidth, outputHeight, left, top)) {
        result.error = "数式が出力サイズを超えています (数式 " + std::to_string(maskWidth) + "x" +
                       std::to_string(maskHeight) + "、出力 " + std::to_string(outputWidth) + "x" +
                       std::to_string(outputHeight) + ")。文字サイズを下げてください";
        return result;
    }

    result.width = outputWidth;
    result.height = outputHeight;
    result.rgba.assign(
        static_cast<std::size_t>(outputWidth) * static_cast<std::size_t>(outputHeight) * 4U, 0);
    const Straight background = fromArgb(style.backgroundArgb, 1.0);
    for (int y = 0; y < maskHeight; ++y) {
        for (int x = 0; x < maskWidth; ++x) {
            const std::size_t source =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(maskWidth) +
                 static_cast<std::size_t>(x)) *
                4U;
            std::uint8_t* out = result.rgba.data() + (static_cast<std::size_t>(top + y) *
                                                          static_cast<std::size_t>(outputWidth) +
                                                      static_cast<std::size_t>(left + x)) *
                                                         4U;
            composePixel(maskRgba[source + 3], style.colorArgb, background, out);
        }
    }
    result.success = true;
    return result;
}

void composeMathPatch(const std::uint8_t* coverage, int maskWidth, int maskHeight,
                      const MathComposeStyle& style, std::uint8_t* out) {
    const Straight background = fromArgb(style.backgroundArgb, 1.0);
    const std::size_t count =
        static_cast<std::size_t>(maskWidth) * static_cast<std::size_t>(maskHeight);
    for (std::size_t index = 0; index < count; ++index)
        composePixel(coverage[index], style.colorArgb, background, out + index * 4U);
}

} // namespace mvm::math

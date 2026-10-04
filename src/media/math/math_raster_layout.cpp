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

} // namespace

MathComposeResult composeMathRaster(const std::uint8_t* maskRgba, int maskWidth, int maskHeight,
                                    const MathComposeStyle& style, int outputWidth,
                                    int outputHeight) {
    MathComposeResult result;
    if (!maskRgba || maskWidth <= 0 || maskHeight <= 0 || outputWidth <= 0 || outputHeight <= 0) {
        result.error = "数式の画像または出力の大きさが不正です";
        return result;
    }
    if (maskWidth > outputWidth || maskHeight > outputHeight) {
        result.error = "数式が出力サイズを超えています (数式 " + std::to_string(maskWidth) + "x" +
                       std::to_string(maskHeight) + "、出力 " + std::to_string(outputWidth) + "x" +
                       std::to_string(outputHeight) + ")。文字サイズを下げてください";
        return result;
    }

    result.width = outputWidth;
    result.height = outputHeight;
    result.rgba.assign(
        static_cast<std::size_t>(outputWidth) * static_cast<std::size_t>(outputHeight) * 4U, 0);
    const int left = (outputWidth - maskWidth) / 2;
    const int top = (outputHeight - maskHeight) / 2;
    const Straight background = fromArgb(style.backgroundArgb, 1.0);
    for (int y = 0; y < maskHeight; ++y) {
        for (int x = 0; x < maskWidth; ++x) {
            const std::size_t source =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(maskWidth) +
                 static_cast<std::size_t>(x)) *
                4U;
            const double coverage = maskRgba[source + 3] / 255.0;
            const Straight glyph = fromArgb(style.colorArgb, coverage);
            // straight alpha の over 合成: glyph を背景の上に置く。
            const double alpha = glyph.a + background.a * (1.0 - glyph.a);
            std::uint8_t* out = result.rgba.data() + (static_cast<std::size_t>(top + y) *
                                                          static_cast<std::size_t>(outputWidth) +
                                                      static_cast<std::size_t>(left + x)) *
                                                         4U;
            if (alpha <= 0.0)
                continue;
            const double backgroundWeight = background.a * (1.0 - glyph.a);
            out[0] = toByte((glyph.r * glyph.a + background.r * backgroundWeight) / alpha);
            out[1] = toByte((glyph.g * glyph.a + background.g * backgroundWeight) / alpha);
            out[2] = toByte((glyph.b * glyph.a + background.b * backgroundWeight) / alpha);
            out[3] = toByte(alpha * 255.0);
        }
    }
    result.success = true;
    return result;
}

} // namespace mvm::math

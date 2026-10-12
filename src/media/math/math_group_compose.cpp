#include "media/math/math_group_compose.h"

#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace mvm::math {
namespace {

// 最適化で演算をまとめられないよう、1 演算ごとに binary32 の値として確定させる。
// (積和を 1 回の丸めにする FMA や、式の組み替えが入ると 1 階調ずれる画素が出る。)
inline float f32(float value) {
    volatile float settled = value;
    return settled;
}

// 1 軸分の素材座標。MLT は先頭の値に 1.0 を足し続けて求める (倍精度)。同じ足し方をしないと、
// 末尾の桁が変わって binary32 へ落とした値が変わることがある。
std::vector<float> sourceCoordinates(double rect, int size) {
    std::vector<float> coordinates(static_cast<std::size_t>(size));
    // MLT: lower = -(rect + size * 1 / 2.0f)、offset = 1 * size / 2.0。
    double position = -(rect + static_cast<double>(size) * 1.0 / 2.0);
    const double offset = static_cast<double>(size) / 2.0;
    for (int index = 0; index < size; ++index, position += 1.0) {
        const double source = position + offset;
        // 範囲の外は NaN で印を付ける (MLT は倍精度のまま 0 <= source <= size - 1 を見る)。
        coordinates[static_cast<std::size_t>(index)] =
            source >= 0.0 && source <= static_cast<double>(size - 1) ? static_cast<float>(source)
                                                                     : std::nanf("");
    }
    return coordinates;
}

} // namespace

bool mathGroupAlphaRoundTripIsIdentity() {
    for (int alpha = 0; alpha <= 255; ++alpha) {
        const float below = f32(static_cast<float>(alpha) * f32(1.0f / 255.0f));
        if (static_cast<int>(static_cast<std::uint8_t>(f32(255.0f * below))) != alpha)
            return false;
    }
    return true;
}

bool PreparedMathRaster::prepare(const std::uint8_t* rgba, int width, int height,
                                 std::size_t stride, PreparedMathRaster& out, std::string& error) {
    out = PreparedMathRaster();
    if (!rgba || width < 2 || height < 2 || stride < static_cast<std::size_t>(width) * 4U) {
        error = "数式 layer の画像の大きさまたは参照が不正です";
        return false;
    }
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4U;
    PreparedMathRaster raster;
    raster.rgba_.resize(rowBytes * static_cast<std::size_t>(height));
    int left = width, top = height, right = -1, bottom = -1;
    for (int row = 0; row < height; ++row) {
        std::uint8_t* to = raster.rgba_.data() + static_cast<std::size_t>(row) * rowBytes;
        std::memcpy(to, rgba + static_cast<std::size_t>(row) * stride, rowBytes);
        // 範囲は複製した側の画素から求める (元の buffer が途中で変わっても食い違わない)。
        for (int column = 0; column < width; ++column) {
            if (to[static_cast<std::size_t>(column) * 4U + 3U] == 0)
                continue;
            left = column < left ? column : left;
            right = column > right ? column : right;
            top = row < top ? row : top;
            bottom = row;
        }
    }
    raster.width_ = width;
    raster.height_ = height;
    if (right >= 0) {
        raster.roiX_ = left;
        raster.roiY_ = top;
        raster.roiWidth_ = right - left + 1;
        raster.roiHeight_ = bottom - top + 1;
    }
    out = std::move(raster);
    return true;
}

bool mathGroupDestinationEligible(const std::uint8_t* destination, int width, int height,
                                  std::size_t stride) {
    if (!destination || width < 2 || height < 2 || stride < static_cast<std::size_t>(width) * 4U)
        return false;
    for (int row = 0; row < height; ++row) {
        const std::uint8_t* line = destination + static_cast<std::size_t>(row) * stride;
        std::uint8_t all = 255;
        for (int column = 0; column < width; ++column)
            all &= line[static_cast<std::size_t>(column) * 4U + 3U];
        if (all != 255)
            return false;
    }
    return true;
}

bool composeMathGroup(std::uint8_t* destination, int width, int height, std::size_t stride,
                      const MathGroupPlacement* layers, std::size_t count, std::string& error,
                      MathGroupStats* stats) {
    if (!destination || width < 2 || height < 2 || stride < static_cast<std::size_t>(width) * 4U ||
        (count > 0 && !layers)) {
        error = "数式の合成先の大きさまたは参照が不正です";
        return false;
    }
    // 先に全部の入力を確かめる。対象外が 1 つでもあれば、何も書かずに拒否する。
    std::vector<detail::UncheckedMathGroupLayer> unchecked(count);
    for (std::size_t index = 0; index < count; ++index) {
        const MathGroupPlacement& layer = layers[index];
        if (!layer.raster || !layer.raster->valid() || layer.raster->width() != width ||
            layer.raster->height() != height || !std::isfinite(layer.rectX) ||
            !std::isfinite(layer.rectY) || !(layer.opacity >= 0.0 && layer.opacity <= 1.0)) {
            error = "数式 layer " + std::to_string(index) +
                    " が不正です (準備済み・等倍・出力と同じ大きさ・不透明度 0〜1 だけに対応)";
            return false;
        }
        auto& item = unchecked[index];
        item.rgba = layer.raster->rgba();
        item.width = width;
        item.height = height;
        item.rectX = layer.rectX;
        item.rectY = layer.rectY;
        item.opacity = layer.opacity;
        item.roiX = layer.raster->roiX();
        item.roiY = layer.raster->roiY();
        item.roiWidth = layer.raster->roiWidth();
        item.roiHeight = layer.raster->roiHeight();
    }
    if (!mathGroupDestinationEligible(destination, width, height, stride)) {
        error = "数式の合成先に不透明でない画素があります (対象外。MLT と比べていない)";
        return false;
    }
    return detail::composeMathGroupUnchecked(destination, width, height, stride, unchecked.data(),
                                             count, error, stats);
}

namespace detail {

bool composeMathGroupUnchecked(std::uint8_t* destination, int width, int height, std::size_t stride,
                               const UncheckedMathGroupLayer* layers, std::size_t count,
                               std::string& error, MathGroupStats* stats) {
    if (!destination || width < 2 || height < 2 || stride < static_cast<std::size_t>(width) * 4U ||
        (count > 0 && !layers)) {
        error = "数式の合成先の大きさまたは参照が不正です";
        return false;
    }
    const bool skipEmpty = mathGroupAlphaRoundTripIsIdentity();
    for (std::size_t index = 0; index < count; ++index) {
        const UncheckedMathGroupLayer& layer = layers[index];
        if (!layer.rgba || layer.width != width || layer.height != height ||
            !std::isfinite(layer.rectX) || !std::isfinite(layer.rectY) ||
            !(layer.opacity >= 0.0 && layer.opacity <= 1.0) || layer.roiWidth < 0 ||
            layer.roiHeight < 0 || layer.roiX < 0 || layer.roiY < 0 ||
            layer.roiX > width - layer.roiWidth || layer.roiY > height - layer.roiHeight) {
            error = "数式 layer " + std::to_string(index) +
                    " が不正です (等倍・出力と同じ大きさ・不透明度 0〜1 だけに対応)";
            return false;
        }
        const bool whole = layer.roiWidth == 0 || layer.roiHeight == 0 || !skipEmpty;
        const std::vector<float> columns = sourceCoordinates(layer.rectX, width);
        const std::vector<float> rows = sourceCoordinates(layer.rectY, height);
        // 不透明度に 1/255 を先に掛ける (MLT の実 binary の演算の順)。
        const float gain = f32(static_cast<float>(layer.opacity) * f32(1.0f / 255.0f));
        const std::size_t layerStride = static_cast<std::size_t>(width) * 4U;
        for (int row = 0; row < height; ++row) {
            const float y = rows[static_cast<std::size_t>(row)];
            if (std::isnan(y))
                continue;
            int n = static_cast<int>(std::floor(y));
            if (n + 2 > height)
                n = height - 2;
            // 補間が読む 2 行 (n, n + 1) が字形の範囲に掛からなければ、この行は結果が変わらない。
            if (!whole && (n + 1 < layer.roiY || n >= layer.roiY + layer.roiHeight))
                continue;
            const float fy = f32(y - static_cast<float>(n));
            const std::uint8_t* upper = layer.rgba + static_cast<std::size_t>(n) * layerStride;
            const std::uint8_t* lower = upper + layerStride;
            std::uint8_t* out = destination + static_cast<std::size_t>(row) * stride;
            for (int column = 0; column < width; ++column) {
                const float x = columns[static_cast<std::size_t>(column)];
                if (std::isnan(x))
                    continue;
                int m = static_cast<int>(std::floor(x));
                if (m + 2 > width)
                    m = width - 2;
                if (!whole && (m + 1 < layer.roiX || m >= layer.roiX + layer.roiWidth))
                    continue;
                const float fx = f32(x - static_cast<float>(m));
                const std::uint8_t* a = upper + static_cast<std::size_t>(m) * 4U;
                const std::uint8_t* b = lower + static_cast<std::size_t>(m) * 4U;
                // 双線形: 上の行と下の行をそれぞれ横に補間し、縦に補間する。
                const auto sample = [&](std::size_t c) {
                    const float top = f32(static_cast<float>(a[c]) +
                                          f32(static_cast<float>(a[4U + c] - a[c]) * fx));
                    const float bottom = f32(static_cast<float>(b[c]) +
                                             f32(static_cast<float>(b[4U + c] - b[c]) * fx));
                    return f32(top + f32(f32(bottom - top) * fy));
                };
                std::uint8_t* pixel = out + static_cast<std::size_t>(column) * 4U;
                const float above = f32(sample(3) * gain);
                // 下の alpha は 1/255 を掛けて求める (MLT の実 binary と同じ。255 で割ると、
                // 下の alpha が 254 などのときに末尾の桁が違う)。
                const float below = f32(static_cast<float>(pixel[3]) * f32(1.0f / 255.0f));
                const float total = f32(f32(above + below) - f32(above * below));
                if (stats)
                    ++stats->visitedPixels;
                // 上下とも alpha 0 の画素は、MLT では 0 で割ることになり結果が定まらない。
                // 不透明な下地の上では起きない (検査を通す入口は透明な下地を拒否する)。
                // 書かずに残す。
                if (!(total > 0.0f))
                    continue;
                const float weight = f32(above / total);
                const float keep = f32(1.0f - weight);
                for (std::size_t c = 0; c < 3; ++c)
                    pixel[c] = static_cast<std::uint8_t>(
                        f32(f32(static_cast<float>(pixel[c]) * keep) + f32(sample(c) * weight)));
                pixel[3] = static_cast<std::uint8_t>(f32(255.0f * total));
            }
        }
    }
    return true;
}

} // namespace detail

} // namespace mvm::math

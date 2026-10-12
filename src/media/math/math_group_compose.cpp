#include "media/math/math_group_compose.h"

#include <atomic>
#include <chrono>
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

std::atomic<long long> gLivePixelBytes{0};

// 保存していない texel (元の画像では 4 byte とも 0)。
const std::uint8_t kZeroTexel[4] = {0, 0, 0, 0};

// 1 画素の合成。a0・a1 は補間が読む上の行の左右の texel、b0・b1 は下の行 (それぞれ RGBA の 4
// byte)。 式と丸めは MLT の実 binary (libmltplus の interpBL_b32) と同じ
// (docs/math-compositor-p312.md)。
inline void blendPixel(std::uint8_t* pixel, const std::uint8_t* a0, const std::uint8_t* a1,
                       const std::uint8_t* b0, const std::uint8_t* b1, float fx, float fy,
                       float gain, MathGroupStats* stats) {
    // 双線形: 上の行と下の行をそれぞれ横に補間し、縦に補間する。
    const auto sample = [&](std::size_t c) {
        const float top =
            f32(static_cast<float>(a0[c]) + f32(static_cast<float>(a1[c] - a0[c]) * fx));
        const float bottom =
            f32(static_cast<float>(b0[c]) + f32(static_cast<float>(b1[c] - b0[c]) * fx));
        return f32(top + f32(f32(bottom - top) * fy));
    };
    const float above = f32(sample(3) * gain);
    // 下の alpha は 1/255 を掛けて求める (MLT の実 binary と同じ。255 で割ると、
    // 下の alpha が 254 などのときに末尾の桁が違う)。
    const float below = f32(static_cast<float>(pixel[3]) * f32(1.0f / 255.0f));
    const float total = f32(f32(above + below) - f32(above * below));
    if (stats)
        ++stats->visitedPixels;
    // 上下とも alpha 0 の画素は、MLT では 0 で割ることになり、色が 0 に書き換わる。
    // ここでは書かずに残す (MLT と違う)。検査を通す入口は alpha 0 の画素がある下地を
    // 拒否するので、この分岐を通るのは検査を通さない入口だけである。
    if (!(total > 0.0f))
        return;
    const float weight = f32(above / total);
    const float keep = f32(1.0f - weight);
    for (std::size_t c = 0; c < 3; ++c)
        pixel[c] = static_cast<std::uint8_t>(
            f32(f32(static_cast<float>(pixel[c]) * keep) + f32(sample(c) * weight)));
    pixel[3] = static_cast<std::uint8_t>(f32(255.0f * total));
}

} // namespace

struct PreparedMathRaster::Pixels {
    std::vector<std::uint8_t> bytes;

    explicit Pixels(std::size_t size) : bytes(size) {
        gLivePixelBytes += static_cast<long long>(bytes.size());
    }

    Pixels(const Pixels&) = delete;
    Pixels& operator=(const Pixels&) = delete;

    ~Pixels() { gLivePixelBytes -= static_cast<long long>(bytes.size()); }
};

long long preparedMathRasterLiveBytes() {
    return gLivePixelBytes.load();
}

const std::uint8_t* PreparedMathRaster::storedRgba() const {
    return pixels_ ? pixels_->bytes.data() : nullptr;
}

std::size_t PreparedMathRaster::storedBytes() const {
    return pixels_ ? pixels_->bytes.size() : 0;
}

void PreparedMathRaster::texel(int x, int y, std::uint8_t out[4]) const {
    if (!pixels_ || x < storedX_ || y < storedY_ || x >= storedX_ + storedWidth_ ||
        y >= storedY_ + storedHeight_) {
        std::memset(out, 0, 4);
        return;
    }
    std::memcpy(out,
                pixels_->bytes.data() + (static_cast<std::size_t>(y - storedY_) *
                                             static_cast<std::size_t>(storedWidth_) +
                                         static_cast<std::size_t>(x - storedX_)) *
                                            4U,
                4);
}

bool mathGroupAlphaRoundTripIsIdentity() {
    for (int alpha = 0; alpha <= 255; ++alpha) {
        const float below = f32(static_cast<float>(alpha) * f32(1.0f / 255.0f));
        if (static_cast<int>(static_cast<std::uint8_t>(f32(255.0f * below))) != alpha)
            return false;
    }
    return true;
}

bool PreparedMathRaster::prepare(const std::uint8_t* rgba, int width, int height,
                                 std::size_t stride, PreparedMathRaster& out, std::string& error,
                                 MathRasterStorage storage, std::string provenance) {
    out = PreparedMathRaster();
    if (!rgba || width < 2 || height < 2 || stride < static_cast<std::size_t>(width) * 4U) {
        error = "数式 layer の画像の大きさまたは参照が不正です";
        return false;
    }
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4U;
    PreparedMathRaster raster;
    raster.storage_ = storage;
    raster.width_ = width;
    raster.height_ = height;
    raster.provenance_ = std::move(provenance);
    // 元の画素を 1 回だけ走査して、字形の範囲 (alpha が 0 でない)、中身のある範囲 (どれかの byte が
    // 0 でない)、照合用の値を求める。FullFrame は走査しながら複製する (複製した側を見る)。
    std::shared_ptr<Pixels> full;
    if (storage == MathRasterStorage::FullFrame)
        full = std::make_shared<Pixels>(rowBytes * static_cast<std::size_t>(height));
    int left = width, top = height, right = -1, bottom = -1;
    int contentLeft = width, contentTop = height, contentRight = -1, contentBottom = -1;
    std::uint64_t checksum = 1469598103934665603ULL ^
                             (static_cast<std::uint64_t>(static_cast<std::uint32_t>(width)) << 32) ^
                             static_cast<std::uint64_t>(static_cast<std::uint32_t>(height));
    for (int row = 0; row < height; ++row) {
        const std::uint8_t* from = rgba + static_cast<std::size_t>(row) * stride;
        if (full) {
            std::uint8_t* to = full->bytes.data() + static_cast<std::size_t>(row) * rowBytes;
            std::memcpy(to, from, rowBytes);
            from = to;
        }
        for (int column = 0; column < width; ++column) {
            std::uint32_t word = 0;
            std::memcpy(&word, from + static_cast<std::size_t>(column) * 4U, 4);
            checksum = (checksum ^ word) * 1099511628211ULL;
            if (word == 0)
                continue;
            contentLeft = column < contentLeft ? column : contentLeft;
            contentRight = column > contentRight ? column : contentRight;
            contentTop = row < contentTop ? row : contentTop;
            contentBottom = row;
            if (from[static_cast<std::size_t>(column) * 4U + 3U] == 0)
                continue;
            left = column < left ? column : left;
            right = column > right ? column : right;
            top = row < top ? row : top;
            bottom = row;
        }
    }
    raster.checksum_ = checksum;
    if (right >= 0) {
        raster.roiX_ = left;
        raster.roiY_ = top;
        raster.roiWidth_ = right - left + 1;
        raster.roiHeight_ = bottom - top + 1;
    }
    if (full) {
        raster.storedWidth_ = width;
        raster.storedHeight_ = height;
        raster.pixels_ = std::move(full);
    } else if (contentRight >= 0) {
        // 中身のある範囲だけを複製する。範囲の外は全 byte が 0 なので、持たなくても元の画像を
        // そのまま読める。元の buffer が走査の後に変わっても、複製した範囲の外は 0 として扱う
        // (走査した時点の画像が正)。
        raster.storedX_ = contentLeft;
        raster.storedY_ = contentTop;
        raster.storedWidth_ = contentRight - contentLeft + 1;
        raster.storedHeight_ = contentBottom - contentTop + 1;
        const std::size_t storedRow = static_cast<std::size_t>(raster.storedWidth_) * 4U;
        auto pixels =
            std::make_shared<Pixels>(storedRow * static_cast<std::size_t>(raster.storedHeight_));
        for (int row = 0; row < raster.storedHeight_; ++row)
            std::memcpy(pixels->bytes.data() + static_cast<std::size_t>(row) * storedRow,
                        rgba + static_cast<std::size_t>(contentTop + row) * stride +
                            static_cast<std::size_t>(contentLeft) * 4U,
                        storedRow);
        raster.pixels_ = std::move(pixels);
    }
    out = std::move(raster);
    return true;
}

int mathGroupMinimumDestinationAlpha(const std::uint8_t* destination, int width, int height,
                                     std::size_t stride) {
    if (!destination || width < 2 || height < 2 || stride < static_cast<std::size_t>(width) * 4U)
        return -1;
    std::uint8_t lowest = 255;
    for (int row = 0; row < height; ++row) {
        const std::uint8_t* line = destination + static_cast<std::size_t>(row) * stride;
        std::uint8_t rowLowest = 255;
        for (int column = 0; column < width; ++column) {
            const std::uint8_t alpha = line[static_cast<std::size_t>(column) * 4U + 3U];
            rowLowest = alpha < rowLowest ? alpha : rowLowest;
        }
        lowest = rowLowest < lowest ? rowLowest : lowest;
    }
    return lowest;
}

bool mathGroupDestinationEligible(const std::uint8_t* destination, int width, int height,
                                  std::size_t stride) {
    return mathGroupMinimumDestinationAlpha(destination, width, height, stride) >=
           kMathGroupMinimumDestinationAlpha;
}

bool composeMathGroup(std::uint8_t* destination, int width, int height, std::size_t stride,
                      const MathGroupPlacement* layers, std::size_t count, std::string& error,
                      MathGroupStats* stats) {
    return detail::composeMathGroupWithAlphaFloor(destination, width, height, stride, layers, count,
                                                  kMathGroupMinimumDestinationAlpha, error, stats);
}

namespace detail {

bool composeMathGroupWithAlphaFloor(std::uint8_t* destination, int width, int height,
                                    std::size_t stride, const MathGroupPlacement* layers,
                                    std::size_t count, int minimumDestinationAlpha,
                                    std::string& error, MathGroupStats* stats) {
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
        item.rgba = layer.raster->storedRgba();
        item.storedRegion = layer.raster->storage() == MathRasterStorage::RoiOnly;
        item.storedX = layer.raster->storedX();
        item.storedY = layer.raster->storedY();
        item.storedWidth = layer.raster->storedWidth();
        item.storedHeight = layer.raster->storedHeight();
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
    const int lowest = mathGroupMinimumDestinationAlpha(destination, width, height, stride);
    if (lowest < minimumDestinationAlpha) {
        error = "数式の合成先の alpha の最小値 " + std::to_string(lowest) + " が対象の下限 " +
                std::to_string(minimumDestinationAlpha) + " より小さい (対象外。書かずに拒否)";
        return false;
    }
    return composeMathGroupUnchecked(destination, width, height, stride, unchecked.data(), count,
                                     error, stats);
}

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
        const bool storedEmpty =
            layer.storedRegion && (layer.storedWidth == 0 || layer.storedHeight == 0);
        if ((!layer.rgba && !storedEmpty) || layer.width != width || layer.height != height ||
            (layer.storedRegion &&
             (layer.storedWidth < 0 || layer.storedHeight < 0 || layer.storedX < 0 ||
              layer.storedY < 0 || layer.storedX > width - layer.storedWidth ||
              layer.storedY > height - layer.storedHeight ||
              // 字形の範囲 (alpha が 0 でない texel) は、保存した範囲の中になければならない。
              (layer.roiWidth > 0 && layer.roiHeight > 0 &&
               (layer.roiX < layer.storedX || layer.roiY < layer.storedY ||
                layer.roiX + layer.roiWidth > layer.storedX + layer.storedWidth ||
                layer.roiY + layer.roiHeight > layer.storedY + layer.storedHeight)))) ||
            !std::isfinite(layer.rectX) || !std::isfinite(layer.rectY) ||
            !(layer.opacity >= 0.0 && layer.opacity <= 1.0) || layer.roiWidth < 0 ||
            layer.roiHeight < 0 || layer.roiX < 0 || layer.roiY < 0 ||
            layer.roiX > width - layer.roiWidth || layer.roiY > height - layer.roiHeight) {
            error = "数式 layer " + std::to_string(index) +
                    " が不正です (等倍・出力と同じ大きさ・不透明度 0〜1 だけに対応)";
            return false;
        }
        // 画素を 1 つも持たない layer (元の画像の全 byte が 0) は、どの画素も変えない
        // (上の alpha が 0 で、下の alpha の書き直しが恒等のとき)。
        if (storedEmpty && skipEmpty)
            continue;
        const bool whole = layer.roiWidth == 0 || layer.roiHeight == 0 || !skipEmpty;
        const auto coordinateBegin =
            stats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
        const std::vector<float> columns = sourceCoordinates(layer.rectX, width);
        const std::vector<float> rows = sourceCoordinates(layer.rectY, height);
        if (stats)
            stats->coordinateNs +=
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - coordinateBegin)
                                               .count());
        // 不透明度に 1/255 を先に掛ける (MLT の実 binary の演算の順)。
        const float gain = f32(static_cast<float>(layer.opacity) * f32(1.0f / 255.0f));
        if (layer.storedRegion) {
            // 保存した範囲だけを持つ layer。素材座標・補間の位置 (m, n)・重み (fx, fy)
            // は、元の画像の 座標のまま求める (全画素を持つ場合と同じ式)。違うのは texel
            // の読み方だけで、 保存した範囲の外の texel は (0, 0, 0, 0) として読む。
            const int storedRight = layer.storedX + layer.storedWidth;
            const int storedBottom = layer.storedY + layer.storedHeight;
            const std::size_t storedStride = static_cast<std::size_t>(layer.storedWidth) * 4U;
            const auto storedRow = [&](int n) -> const std::uint8_t* {
                return n >= layer.storedY && n < storedBottom
                           ? layer.rgba + static_cast<std::size_t>(n - layer.storedY) * storedStride
                           : nullptr;
            };
            for (int row = 0; row < height; ++row) {
                const float y = rows[static_cast<std::size_t>(row)];
                if (std::isnan(y))
                    continue;
                int n = static_cast<int>(std::floor(y));
                if (n + 2 > height)
                    n = height - 2;
                if (!whole && (n + 1 < layer.roiY || n >= layer.roiY + layer.roiHeight))
                    continue;
                const float fy = f32(y - static_cast<float>(n));
                const std::uint8_t* upper = storedRow(n);
                const std::uint8_t* lower = storedRow(n + 1);
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
                    // 左の texel (m) と右の texel (m + 1) が保存した範囲の中にあるか。
                    const bool leftStored = m >= layer.storedX && m < storedRight;
                    const bool rightStored = m + 1 >= layer.storedX && m + 1 < storedRight;
                    const std::size_t leftAt = static_cast<std::size_t>(m - layer.storedX) * 4U;
                    const std::uint8_t* a0 = upper && leftStored ? upper + leftAt : kZeroTexel;
                    const std::uint8_t* a1 =
                        upper && rightStored ? upper + leftAt + 4U : kZeroTexel;
                    const std::uint8_t* b0 = lower && leftStored ? lower + leftAt : kZeroTexel;
                    const std::uint8_t* b1 =
                        lower && rightStored ? lower + leftAt + 4U : kZeroTexel;
                    blendPixel(out + static_cast<std::size_t>(column) * 4U, a0, a1, b0, b1, fx, fy,
                               gain, stats);
                }
            }
            continue;
        }
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
                blendPixel(out + static_cast<std::size_t>(column) * 4U, a, a + 4U, b, b + 4U, fx,
                           fy, gain, stats);
            }
        }
    }
    return true;
}

} // namespace detail

} // namespace mvm::math

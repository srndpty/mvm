#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace mvm::test {
using Pixel = std::array<std::uint8_t, 4>;

// 演算ごとに binary32 へ丸める。MLT の関数・画素・期待値を参照しない。
// double の中間演算を使い、コンパイラの乗算加算融合を避ける。
inline double binary32(double value) {
    volatile float rounded = static_cast<float>(value);
    return rounded;
}

inline Pixel mltSourceOver(Pixel destination, Pixel source, double opacity = 1) {
    const double sourceAlpha = binary32(binary32(source[3] / 255.0) * binary32(opacity));
    const double destinationAlpha = binary32(destination[3] / 255.0);
    const double alpha = binary32(binary32(sourceAlpha + destinationAlpha) -
                                  binary32(sourceAlpha * destinationAlpha));
    if (alpha <= 0)
        throw std::domain_error("MLT の両入力 alpha 0 は除算の定義域外です");
    const double weight = binary32(sourceAlpha / alpha);
    Pixel result{};
    result[3] = static_cast<std::uint8_t>(std::trunc(binary32(255.0 * alpha)));
    for (std::size_t c = 0; c < 3; ++c) {
        const auto value = binary32(binary32(destination[c] * binary32(1.0 - weight)) +
                                    binary32(source[c] * weight));
        if (value < 0 || value > 255)
            throw std::domain_error("RGBA8 の範囲外です");
        result[c] = static_cast<std::uint8_t>(std::trunc(value));
    }
    return result;
}
} // namespace mvm::test

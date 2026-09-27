#include "core/source_frame_mapping.h"

#include <limits>
#include <numeric>

namespace mvm::core {
namespace {

__extension__ using WideInteger = __int128;

bool validRate(FrameRate rate) {
    return rate.num > 0 && rate.den > 0;
}

bool multiply(WideInteger left, WideInteger right, WideInteger& result) {
    return !__builtin_mul_overflow(left, right, &result);
}

// numerator / denominator を丸める。denominator > 0。
WideInteger divideFloor(WideInteger numerator, WideInteger denominator) {
    WideInteger quotient = numerator / denominator;
    if (numerator % denominator != 0 && numerator < 0)
        --quotient;
    return quotient;
}

WideInteger divideCeil(WideInteger numerator, WideInteger denominator) {
    WideInteger quotient = numerator / denominator;
    if (numerator % denominator != 0 && numerator > 0)
        ++quotient;
    return quotient;
}

std::optional<std::int64_t> toInt64(WideInteger value) {
    if (value < std::numeric_limits<std::int64_t>::min() ||
        value > std::numeric_limits<std::int64_t>::max())
        return std::nullopt;
    return static_cast<std::int64_t>(value);
}

// R = output / source を約分した ratio.num / ratio.den として返す。
struct Ratio {
    WideInteger num = 0;
    WideInteger den = 1;
};

std::optional<Ratio> outputPerSource(FrameRate source, FrameRate output) {
    if (!validRate(source) || !validRate(output))
        return std::nullopt;
    // 128 bit には std::gcd が使えないので、64 bit の因子ごとに約分してから掛ける。
    // 64 bit どうしの積なので 128 bit に収まる。
    const auto a = std::gcd(output.num, source.num);
    const auto b = std::gcd(source.den, output.den);
    Ratio ratio;
    ratio.num = static_cast<WideInteger>(output.num / a) * (source.den / b);
    ratio.den = static_cast<WideInteger>(output.den / b) * (source.num / a);
    return ratio;
}

} // namespace

std::optional<FrameRate> multiplyFrameRate(FrameRate rate, FrameRate factor) {
    if (!validRate(rate) || !validRate(factor))
        return std::nullopt;
    // 掛ける前に交差約分して、表せる範囲を広げる。
    const auto a = std::gcd(rate.num, factor.den);
    const auto b = std::gcd(factor.num, rate.den);
    FrameRate product;
    if (__builtin_mul_overflow(rate.num / a, factor.num / b, &product.num) ||
        __builtin_mul_overflow(rate.den / b, factor.den / a, &product.den))
        return std::nullopt;
    const auto common = std::gcd(product.num, product.den);
    product.num /= common;
    product.den /= common;
    return product;
}

std::optional<std::int64_t> convertFrameBoundary(std::int64_t frame, FrameRate from, FrameRate to,
                                                 bool roundUp) {
    if (frame < 0)
        return std::nullopt;
    const auto ratio = outputPerSource(from, to);
    WideInteger numerator = 0;
    if (!ratio || !multiply(frame, ratio->num, numerator))
        return std::nullopt;
    return toInt64(roundUp ? divideCeil(numerator, ratio->den)
                           : divideFloor(numerator, ratio->den));
}

std::optional<std::int64_t> sourceFrameAtOutputPosition(std::int64_t outputPosition,
                                                        FrameRate source, FrameRate output) {
    if (outputPosition < 0)
        return std::nullopt;
    // floor(p / R + 1/2) = floor((2 p R.den + R.num) / (2 R.num))
    const auto ratio = outputPerSource(source, output);
    WideInteger numerator = 0;
    WideInteger denominator = 0;
    if (!ratio || !multiply(2 * static_cast<WideInteger>(outputPosition), ratio->den, numerator) ||
        __builtin_add_overflow(numerator, ratio->num, &numerator) ||
        !multiply(2, ratio->num, denominator))
        return std::nullopt;
    return toInt64(divideFloor(numerator, denominator));
}

std::optional<std::int64_t> firstOutputPositionOfSourceFrame(std::int64_t sourceFrame,
                                                             FrameRate source, FrameRate output) {
    if (sourceFrame < 0)
        return std::nullopt;
    // ceil((s - 1/2) R) = ceil((2 s - 1) R.num / (2 R.den))
    const auto ratio = outputPerSource(source, output);
    WideInteger numerator = 0;
    WideInteger denominator = 0;
    if (!ratio || !multiply(2 * static_cast<WideInteger>(sourceFrame) - 1, ratio->num, numerator) ||
        !multiply(2, ratio->den, denominator))
        return std::nullopt;
    const WideInteger first = divideCeil(numerator, denominator);
    return toInt64(first < 0 ? 0 : first);
}

} // namespace mvm::core

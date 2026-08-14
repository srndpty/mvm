#include "core/checked_output_timebase.h"

#include <limits>
#include <numeric>

namespace mvm::core {
namespace {

// 64bit値3個の積をcheckedに扱うためのGCC拡張。浮動小数へは退避しない。
__extension__ using WideInteger = __int128;
__extension__ using WideUnsigned = unsigned __int128;

bool checkedMultiply(WideInteger left, WideInteger right, WideInteger& result) {
    return !__builtin_mul_overflow(left, right, &result);
}

WideUnsigned magnitude(WideInteger value) {
    if (value >= 0)
        return static_cast<WideUnsigned>(value);
    return static_cast<WideUnsigned>(-(value + 1)) + 1;
}

WideUnsigned greatestCommonDivisor(WideUnsigned left, WideUnsigned right) {
    while (right != 0) {
        const WideUnsigned remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

bool fitsInt64(WideInteger value) {
    return value >= static_cast<WideInteger>(std::numeric_limits<std::int64_t>::min()) &&
           value <= static_cast<WideInteger>(std::numeric_limits<std::int64_t>::max());
}

WideInteger mathematicalFloor(WideInteger numerator, WideInteger denominator) {
    WideInteger quotient = numerator / denominator;
    if (numerator % denominator < 0)
        --quotient;
    return quotient;
}

WideInteger mathematicalCeil(WideInteger numerator, WideInteger denominator) {
    WideInteger quotient = numerator / denominator;
    if (numerator % denominator > 0)
        ++quotient;
    return quotient;
}

OutputTimebaseResult<std::int64_t> narrow(WideInteger value) {
    if (!fitsInt64(value)) {
        return OutputTimebaseResult<std::int64_t>::failure(OutputTimebaseError::ResultOverflow);
    }
    return OutputTimebaseResult<std::int64_t>::success(static_cast<std::int64_t>(value));
}

} // namespace

CheckedOutputTimebase::CheckedOutputTimebase(std::int64_t frameRateNumerator,
                                             std::int64_t frameRateDenominator,
                                             std::int64_t audioSampleRate)
    : frameRateNumerator_(frameRateNumerator), frameRateDenominator_(frameRateDenominator),
      audioSampleRate_(audioSampleRate) {}

OutputTimebaseResult<CheckedOutputTimebase>
CheckedOutputTimebase::create(std::int64_t frameRateNumerator, std::int64_t frameRateDenominator,
                              std::int64_t audioSampleRate) {
    if (frameRateNumerator <= 0) {
        return OutputTimebaseResult<CheckedOutputTimebase>::failure(
            OutputTimebaseError::InvalidNumerator);
    }
    if (frameRateDenominator <= 0) {
        return OutputTimebaseResult<CheckedOutputTimebase>::failure(
            OutputTimebaseError::InvalidDenominator);
    }
    if (audioSampleRate <= 0) {
        return OutputTimebaseResult<CheckedOutputTimebase>::failure(
            OutputTimebaseError::InvalidSampleRate);
    }
    const std::int64_t divisor = std::gcd(frameRateNumerator, frameRateDenominator);
    return OutputTimebaseResult<CheckedOutputTimebase>::success(CheckedOutputTimebase(
        frameRateNumerator / divisor, frameRateDenominator / divisor, audioSampleRate));
}

OutputTimebaseResult<CheckedOutputTimebase>
CheckedOutputTimebase::createQualified(std::int64_t frameRateNumerator,
                                       std::int64_t frameRateDenominator,
                                       std::int64_t audioSampleRate) {
    auto result = create(frameRateNumerator, frameRateDenominator, audioSampleRate);
    if (!result)
        return result;
    if (result.value().frameRateNumerator_ != 60 || result.value().frameRateDenominator_ != 1 ||
        result.value().audioSampleRate_ != 48000) {
        return OutputTimebaseResult<CheckedOutputTimebase>::failure(
            OutputTimebaseError::UnsupportedProductRate);
    }
    return result;
}

CanonicalRational CheckedOutputTimebase::frameRate() const {
    return {frameRateNumerator_, frameRateDenominator_};
}

std::int64_t CheckedOutputTimebase::audioSampleRate() const {
    return audioSampleRate_;
}

OutputTimebaseResult<CanonicalRational>
CheckedOutputTimebase::exactMediaTime(std::int64_t outputFrame) const {
    WideInteger numerator = 0;
    if (!checkedMultiply(static_cast<WideInteger>(outputFrame), frameRateDenominator_, numerator)) {
        return OutputTimebaseResult<CanonicalRational>::failure(
            OutputTimebaseError::IntermediateOverflow);
    }
    WideInteger denominator = frameRateNumerator_;
    const WideUnsigned divisor =
        greatestCommonDivisor(magnitude(numerator), magnitude(denominator));
    numerator /= static_cast<WideInteger>(divisor);
    denominator /= static_cast<WideInteger>(divisor);
    if (!fitsInt64(numerator) || !fitsInt64(denominator)) {
        return OutputTimebaseResult<CanonicalRational>::failure(
            OutputTimebaseError::ResultOverflow);
    }
    return OutputTimebaseResult<CanonicalRational>::success(
        {static_cast<std::int64_t>(numerator), static_cast<std::int64_t>(denominator)});
}

OutputTimebaseResult<std::int64_t>
CheckedOutputTimebase::firstAudioSample(std::int64_t outputFrame) const {
    WideInteger product = 0;
    if (!checkedMultiply(static_cast<WideInteger>(outputFrame), frameRateDenominator_, product) ||
        !checkedMultiply(product, audioSampleRate_, product)) {
        return OutputTimebaseResult<std::int64_t>::failure(
            OutputTimebaseError::IntermediateOverflow);
    }
    return narrow(mathematicalCeil(product, frameRateNumerator_));
}

OutputTimebaseResult<std::int64_t>
CheckedOutputTimebase::outputFrame(std::int64_t audioSample) const {
    WideInteger numerator = 0;
    WideInteger denominator = 0;
    if (!checkedMultiply(static_cast<WideInteger>(audioSample), frameRateNumerator_, numerator) ||
        !checkedMultiply(static_cast<WideInteger>(audioSampleRate_), frameRateDenominator_,
                         denominator)) {
        return OutputTimebaseResult<std::int64_t>::failure(
            OutputTimebaseError::IntermediateOverflow);
    }
    return narrow(mathematicalFloor(numerator, denominator));
}

OutputTimebaseResult<std::int64_t>
CheckedOutputTimebase::seekTargetSample(std::int64_t outputFrame) const {
    return firstAudioSample(outputFrame);
}

OutputTimebaseResult<std::int64_t>
CheckedOutputTimebase::schedulerOutputFrame(std::int64_t audioSample) const {
    return outputFrame(audioSample);
}

OutputTimebaseResult<std::int64_t>
CheckedOutputTimebase::statusOutputFrame(std::int64_t audioSample) const {
    return outputFrame(audioSample);
}

const CheckedOutputTimebase& qualifiedOutputTimebase() {
    static const CheckedOutputTimebase value = [] {
        const auto result = CheckedOutputTimebase::createQualified(60, 1, 48000);
        return result.value();
    }();
    return value;
}

} // namespace mvm::core

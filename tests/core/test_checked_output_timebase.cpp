#include "core/checked_output_timebase.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using mvm::core::CanonicalRational;
using mvm::core::CheckedOutputTimebase;
using mvm::core::OutputTimebaseError;

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

template<typename T>
void requireFailure(const mvm::core::OutputTimebaseResult<T>& result, OutputTimebaseError expected,
                    const std::string& message) {
    require(!result && result.error == expected, message);
}

void configurationAndCanonicalization() {
    const auto reduced = CheckedOutputTimebase::create(120, 2, 48000);
    require(reduced && reduced.value().frameRate() == CanonicalRational{60, 1},
            "120/2を60/1へcanonicalizeできません");
    const auto qualifiedEquivalent = CheckedOutputTimebase::createQualified(120, 2, 48000);
    require(qualifiedEquivalent &&
                qualifiedEquivalent.value().frameRate() == CanonicalRational{60, 1},
            "qualified境界が120/2を60/1として受理しませんでした");
    requireFailure(CheckedOutputTimebase::create(0, 1, 48000),
                   OutputTimebaseError::InvalidNumerator, "分子0を拒否しませんでした");
    requireFailure(CheckedOutputTimebase::create(-1, 1, 48000),
                   OutputTimebaseError::InvalidNumerator, "負の分子を拒否しませんでした");
    requireFailure(CheckedOutputTimebase::create(60, 0, 48000),
                   OutputTimebaseError::InvalidDenominator, "分母0を拒否しませんでした");
    requireFailure(CheckedOutputTimebase::create(60, -1, 48000),
                   OutputTimebaseError::InvalidDenominator, "負の分母を拒否しませんでした");
    requireFailure(CheckedOutputTimebase::create(60, 1, 0), OutputTimebaseError::InvalidSampleRate,
                   "sample rate 0を拒否しませんでした");
    requireFailure(CheckedOutputTimebase::createQualified(24, 1, 48000),
                   OutputTimebaseError::UnsupportedProductRate,
                   "未qualifiedの24/1を製品構成として受理しました");
    requireFailure(CheckedOutputTimebase::createQualified(60, 1, 44100),
                   OutputTimebaseError::UnsupportedProductRate,
                   "未qualifiedの44100 Hzを製品構成として受理しました");
}

void qualifiedMappingAndRounding() {
    const auto created = CheckedOutputTimebase::createQualified(60, 1, 48000);
    require(static_cast<bool>(created), "qualified timebaseを生成できません");
    const CheckedOutputTimebase& timebase = created.value();

    require(timebase.exactMediaTime(0).value() == CanonicalRational{0, 1},
            "frame 0のmedia timeが0ではありません");
    require(timebase.exactMediaTime(1).value() == CanonicalRational{1, 60},
            "frame 1のmedia timeが1/60ではありません");
    require(timebase.exactMediaTime(-1).value() == CanonicalRational{-1, 60},
            "負frameのmedia timeがcanonicalではありません");
    require(timebase.firstAudioSample(0).value() == 0 &&
                timebase.firstAudioSample(1).value() == 800 &&
                timebase.firstAudioSample(-1).value() == -800,
            "60/1・48000 Hzのframe/sample境界が800 sampleではありません");
    require(timebase.outputFrame(0).value() == 0 && timebase.outputFrame(799).value() == 0 &&
                timebase.outputFrame(800).value() == 1,
            "sample 0..799と800のframe境界が不正です");
    require(timebase.outputFrame(-1).value() == -1 && timebase.outputFrame(-800).value() == -1 &&
                timebase.outputFrame(-801).value() == -2,
            "負sampleで数学的floorを使っていません");
    require(timebase.seekTargetSample(7).value() == 5600 &&
                timebase.schedulerOutputFrame(5599).value() == 6 &&
                timebase.statusOutputFrame(5600).value() == 7,
            "seek/scheduler/statusが共通変換規則を使っていません");

    const auto fractional = CheckedOutputTimebase::create(3, 2, 10);
    require(static_cast<bool>(fractional), "fractional test timebaseを生成できません");
    require(fractional.value().firstAudioSample(1).value() == 7 &&
                fractional.value().firstAudioSample(-1).value() == -6,
            "正負のmathematical ceilが不正です");
    require(fractional.value().outputFrame(1).value() == 0 &&
                fractional.value().outputFrame(-1).value() == -1,
            "正負のmathematical floorが不正です");
}

void limitsAndOverflow() {
    constexpr std::int64_t minimum = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
    const auto qualified = CheckedOutputTimebase::createQualified(60, 1, 48000);
    require(static_cast<bool>(qualified), "limit test timebaseを生成できません");

    require(qualified.value().exactMediaTime(minimum) && qualified.value().exactMediaTime(maximum),
            "INT64_MIN/MAXのexact media rationalを扱えません");
    require(qualified.value().outputFrame(minimum) && qualified.value().outputFrame(maximum),
            "INT64_MIN/MAX sampleのfloor変換を扱えません");
    requireFailure(qualified.value().firstAudioSample(minimum), OutputTimebaseError::ResultOverflow,
                   "INT64_MIN frameの最終結果overflowを拒否しませんでした");
    requireFailure(qualified.value().firstAudioSample(maximum), OutputTimebaseError::ResultOverflow,
                   "INT64_MAX frameの最終結果overflowを拒否しませんでした");

    const auto wideIntermediate = CheckedOutputTimebase::create(maximum, maximum - 1, maximum);
    require(static_cast<bool>(wideIntermediate), "intermediate overflow test構成を生成できません");
    requireFailure(wideIntermediate.value().firstAudioSample(maximum),
                   OutputTimebaseError::IntermediateOverflow,
                   "128-bit中間乗算overflowを拒否しませんでした");

    const auto finalResult = CheckedOutputTimebase::create(1, 2, 1);
    require(static_cast<bool>(finalResult), "final overflow test構成を生成できません");
    requireFailure(finalResult.value().exactMediaTime(maximum), OutputTimebaseError::ResultOverflow,
                   "canonical rationalの最終結果overflowを拒否しませんでした");
}

} // namespace

int main() {
    try {
        configurationAndCanonicalization();
        qualifiedMappingAndRounding();
        limitsAndOverflow();
        std::cout << "PASS: checked output timebase contract\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

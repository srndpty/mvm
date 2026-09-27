#include "core/export_eta.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace mvm::core {
namespace {

// 起動直後の数 frame は encoder の立ち上がりで速度が大きく揺れる。
// 1 秒未満の観測では表示しない。
constexpr double kMinimumObservationSeconds = 1.0;

// long long へ安全に変換できる上限 (排他的)。static_cast<double>(LLONG_MAX) は
// 2^63 へ丸められ LLONG_MAX を超えるため、この値以上は整数変換してはならない。
constexpr double kSecondsConversionLimit =
    static_cast<double>(std::numeric_limits<long long>::max());

} // namespace

std::optional<double> estimateExportRemainingSeconds(long long baselineCompleted,
                                                     long long completed, long long total,
                                                     double elapsedSinceBaselineSeconds) {
    if (total <= 0 || baselineCompleted < 0 || completed < baselineCompleted || completed > total ||
        !std::isfinite(elapsedSinceBaselineSeconds) || elapsedSinceBaselineSeconds < 0.0)
        return std::nullopt;
    if (completed == total)
        return 0.0;
    const long long advanced = completed - baselineCompleted;
    if (advanced <= 0 || elapsedSinceBaselineSeconds < kMinimumObservationSeconds)
        return std::nullopt;
    const double secondsPerFrame = elapsedSinceBaselineSeconds / static_cast<double>(advanced);
    const double estimate = secondsPerFrame * static_cast<double>(total - completed);
    // 有限な入力でも積は overflow し得る。表示できない推定値は出さない。
    if (!std::isfinite(estimate) || estimate >= kSecondsConversionLimit)
        return std::nullopt;
    return estimate;
}

std::string formatExportRemaining(double remainingSeconds) {
    long long seconds = 0;
    if (std::isfinite(remainingSeconds) && remainingSeconds > 0.0) {
        const double rounded = std::ceil(remainingSeconds);
        // 範囲外の浮動小数点→整数変換は未定義動作。上限で頭打ちにする。
        seconds = rounded >= kSecondsConversionLimit ? std::numeric_limits<long long>::max()
                                                     : static_cast<long long>(rounded);
    }
    char text[64];
    if (seconds < 60)
        std::snprintf(text, sizeof(text), "残り約 %lld秒", seconds);
    else if (seconds < 3600)
        std::snprintf(text, sizeof(text), "残り約 %lld分%02lld秒", seconds / 60, seconds % 60);
    else
        std::snprintf(text, sizeof(text), "残り約 %lld時間%02lld分", seconds / 3600,
                      (seconds % 3600) / 60);
    return text;
}

} // namespace mvm::core

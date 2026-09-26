#include "core/export_eta.h"

#include <cmath>
#include <cstdio>

namespace mvm::core {
namespace {

// 起動直後の数 frame は encoder の立ち上がりで速度が大きく揺れる。
// 1 秒未満の観測では表示しない。
constexpr double kMinimumObservationSeconds = 1.0;

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
    return secondsPerFrame * static_cast<double>(total - completed);
}

std::string formatExportRemaining(double remainingSeconds) {
    const long long seconds = std::isfinite(remainingSeconds) && remainingSeconds > 0.0
                                  ? static_cast<long long>(std::ceil(remainingSeconds))
                                  : 0;
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

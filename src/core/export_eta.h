#ifndef MVM_CORE_EXPORT_ETA_H
#define MVM_CORE_EXPORT_ETA_H

#include <optional>
#include <string>

namespace mvm::core {

// 書き出しの残り時間 (秒) を推定する。
// baseline は最初に届いた進捗で、準備時間 (素材の open など) を速度から除外するために使う。
// 推定に足る観測が無い間は nullopt を返し、0 秒や不定値へ縮退させない。
std::optional<double> estimateExportRemainingSeconds(long long baselineCompleted,
                                                     long long completed, long long total,
                                                     double elapsedSinceBaselineSeconds);

// 残り秒数を利用者向けの表記にする。例: "残り約 5秒"、"残り約 1分05秒"、"残り約 1時間02分"。
std::string formatExportRemaining(double remainingSeconds);

} // namespace mvm::core

#endif

// 書き出しETAの推定と表記。期待値は実装の式を使わず、手計算した値を直接書く。

#include "core/export_eta.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

bool near(const std::optional<double>& value, double expected) {
    return value && std::abs(*value - expected) < 1e-9;
}

} // namespace

int main() {
    using mvm::core::estimateExportRemainingSeconds;
    using mvm::core::formatExportRemaining;

    // 基準点 0 から 2 秒で 60 frame -> 1 frame 1/30 秒。残り 60 frame は 2 秒。
    check(near(estimateExportRemainingSeconds(0, 60, 120, 2.0), 2.0),
          "一定速度の残り時間が違います");
    // 準備時間を除外する: 基準点 30 から 3 秒で 30 frame 進んだ -> 残り 90 frame は 9 秒。
    check(near(estimateExportRemainingSeconds(30, 60, 150, 3.0), 9.0),
          "基準点からの速度で推定していません");
    check(near(estimateExportRemainingSeconds(0, 120, 120, 5.0), 0.0), "完了時が0秒ではありません");

    // 推定に足る観測が無い間は値を出さない。
    check(!estimateExportRemainingSeconds(10, 10, 120, 5.0), "進捗0で推定しました");
    check(!estimateExportRemainingSeconds(0, 60, 120, 0.5), "1秒未満の観測で推定しました");
    check(!estimateExportRemainingSeconds(0, 60, 0, 2.0), "total 0で推定しました");
    check(!estimateExportRemainingSeconds(-1, 60, 120, 2.0), "負の基準点を受理しました");
    check(!estimateExportRemainingSeconds(70, 60, 120, 2.0), "基準点より戻った進捗を受理しました");
    check(!estimateExportRemainingSeconds(0, 130, 120, 2.0), "totalを超えた進捗を受理しました");
    check(!estimateExportRemainingSeconds(0, 60, 120, std::numeric_limits<double>::quiet_NaN()),
          "NaNの経過時間を受理しました");

    // 有限な入力でも積が overflow する、または long long に収まらない推定は出さない。
    check(!estimateExportRemainingSeconds(0, 1, 100, 1e308), "overflowした推定値を返しました");
    // 1e19 秒/frame x 残り 1 frame = 1e19 秒 > LLONG_MAX (約 9.22e18)。
    check(!estimateExportRemainingSeconds(0, 1, 2, 1e19),
          "long longに収まらない推定値を返しました");
    // 2^63 ちょうどは LLONG_MAX を 1 超える。境界を含めて拒否する。
    check(!estimateExportRemainingSeconds(0, 1, 2, 9223372036854775808.0),
          "2^63秒の推定値を返しました");

    // 表記。端数は切り上げ、0 以下と非有限値は 0 秒。
    check(formatExportRemaining(0.0) == "残り約 0秒", "0秒の表記が違います");
    check(formatExportRemaining(4.2) == "残り約 5秒", "端数を切り上げません");
    check(formatExportRemaining(59.0) == "残り約 59秒", "59秒の表記が違います");
    check(formatExportRemaining(60.0) == "残り約 1分00秒", "1分の表記が違います");
    check(formatExportRemaining(125.0) == "残り約 2分05秒", "分秒の表記が違います");
    check(formatExportRemaining(3599.0) == "残り約 59分59秒", "59分59秒の表記が違います");
    check(formatExportRemaining(3600.0) == "残り約 1時間00分", "1時間の表記が違います");
    check(formatExportRemaining(3725.0) == "残り約 1時間02分", "時間分の表記が違います");
    check(formatExportRemaining(-3.0) == "残り約 0秒", "負値を0秒へ丸めません");
    check(formatExportRemaining(std::numeric_limits<double>::infinity()) == "残り約 0秒",
          "非有限値を0秒へ丸めません");
    // long long を超える有限値は LLONG_MAX 秒で頭打ちにする。
    // 9223372036854775807 秒 = 2562047788015215 時間 + 1807 秒 (30 分 7 秒)。
    check(formatExportRemaining(1e300) == "残り約 2562047788015215時間30分",
          "long longを超える有限値を上限で頭打ちにしません");
    check(formatExportRemaining(9223372036854775808.0) == "残り約 2562047788015215時間30分",
          "2^63秒を上限で頭打ちにしません");

    if (failures != 0) {
        std::fprintf(stderr, "export ETA: FAIL (%d 件)\n", failures);
        return 1;
    }
    std::puts("export ETA: PASS");
    return 0;
}

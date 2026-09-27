// timeline 波形の peak 集計。期待値は実装の式を使わず、手で数えた値を直接書く。

#include "core/waveform_peaks.h"

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

using mvm::core::waveformColumn;
using mvm::core::WaveformPeakBuilder;
using mvm::core::WaveformPeaks;

bool add(WaveformPeakBuilder& builder, std::int64_t start, const std::vector<float>& left,
         const std::vector<float>& right) {
    const float* planes[] = {left.data(), right.data()};
    std::string error;
    return builder.addPlanar(start, planes, static_cast<int>(left.size()), error);
}

} // namespace

int main() {
    // --- reset の入力検査 ---
    {
        WaveformPeakBuilder builder;
        std::string error;
        check(!builder.reset(0, 1, 4, error), "sample rate 0 を受理した");
        check(!builder.reset(48000, 0, 4, error), "channel 0 を受理した");
        check(!builder.reset(48000, 1, 0, error), "peak 幅 0 を受理した");
        WaveformPeaks peaks;
        check(!builder.finish(peaks, error), "未初期化の builder が finish した");
    }

    // --- sample が 1 つも無い波形を成功にしない ---
    {
        WaveformPeakBuilder builder;
        std::string error;
        check(builder.reset(8, 1, 4, error), "reset に失敗した");
        WaveformPeaks peaks;
        check(!builder.finish(peaks, error), "空の波形を成功にした");
    }

    // --- 壊れた PTS で巨大な確保や signed overflow を起こさない ---
    {
        const std::vector<float> values(100, 0.5f);
        const float* planes[] = {values.data(), values.data()};
        WaveformPeakBuilder builder;
        std::string error;
        check(builder.reset(48000, 2, 64, error), "reset に失敗した");
        // startSample + frameCount - 1 が溢れる位置。
        check(!builder.addPlanar(std::numeric_limits<std::int64_t>::max() - 1, planes, 100, error),
              "INT64_MAX 付近の位置を受理した");
        // 溢れはしないが、既定の budget (256MiB) を大きく超える位置。
        // 受理すると stereo で約 2^40/64 peak * 16 byte = 256GiB を確保しにいく。
        check(!builder.addPlanar(std::int64_t{1} << 40, planes, 100, error),
              "巨大な正の位置を受理した");
        check(error.find("timestamp") != std::string::npos, "失敗理由が timestamp を示していない");
        // 全体が素材開始より前なら、INT64_MIN でも捨てるだけで失敗にしない。
        check(builder.addPlanar(std::numeric_limits<std::int64_t>::min(), planes, 100, error),
              "INT64_MIN の位置で失敗した");
        // 失敗した追加の後でも、正常な位置は受理し続ける (巨大な位置で確保していない)。
        check(builder.addPlanar(0, planes, 100, error), "正常な位置を受理できない");
        WaveformPeaks small;
        check(builder.finish(small, error) && small.levels[0].peakCount == 2,
              "失敗した追加が peak 数へ影響している");
    }

    // --- budget の境界: stereo の 1 peak は 16 byte ---
    {
        const std::vector<float> values(4, 0.5f);
        const float* planes[] = {values.data(), values.data()};
        WaveformPeakBuilder builder;
        std::string error;
        check(!builder.reset(8, 2, 4, error, 15), "1 peak も持てない budget を受理した");
        check(builder.reset(8, 2, 4, error, 32), "2 peak 分の budget で reset できない");
        check(builder.addPlanar(4, planes, 4, error), "2 peak 目 (sample 4..7) を拒否した");
        check(!builder.addPlanar(5, planes, 4, error),
              "3 peak 目 (sample 8) に掛かる追加を受理した");
    }

    // --- 2 channel、peak 幅 4、sample rate 8 (1 peak = 0.5 秒) ---
    WaveformPeaks peaks;
    {
        WaveformPeakBuilder builder;
        std::string error;
        check(builder.reset(8, 2, 4, error), "reset に失敗した");
        // peak 0 (sample 0..3): left は正だけ、right は負だけ。
        check(add(builder, 0, {0.5f, 0.25f, 1.0f, 0.75f}, {-0.5f, -1.0f, -0.25f, -0.75f}),
              "peak 0 の追加に失敗した");
        // sample 4..7 (peak 1) は飛ばして、peak 2 (sample 8..11) だけに置く。
        // 素材開始より前 (sample -2, -1) は捨てられる。
        check(add(builder, -2, {1.0f, 1.0f, -0.5f, 0.0f}, {1.0f, 1.0f, 0.0f, 0.0f}),
              "負の位置を含む追加に失敗した");
        check(add(builder, 8, {0.0f, 2.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, -2.0f}),
              "peak 2 の追加に失敗した");
        check(builder.finish(peaks, error), "finish に失敗した");
    }
    check(peaks.sampleRate == 8 && peaks.channels == 2, "format が保たれていない");
    check(!peaks.levels.empty(), "level が無い");
    if (!peaks.levels.empty()) {
        const auto& level = peaks.levels[0];
        check(level.samplesPerPeak == 4 && level.peakCount == 3,
              "最細 level の peak 数が 3 でない");
        // left: peak0 = [min(-0.5 at sample0 from 2 回目の追加), max 1.0]
        //   2 回目の追加の sample 0,1 は -0.5, 0.0 なので peak0 の min は -0.5。
        check(level.minimum[0] == -64 && level.maximum[0] == 127, "left peak0 が不正");
        // peak1 は未到達なので無音。
        check(level.minimum[1] == 0 && level.maximum[1] == 0, "left peak1 が無音でない");
        // peak2: 2.0 は 1.0 へ clamp。min は 0。
        check(level.minimum[2] == 0 && level.maximum[2] == 127, "left peak2 が不正");
        // right: peak0 = [-1.0, 0.0]。0.0 は 2 回目の追加の sample 0, 1。
        check(level.minimum[3] == -127 && level.maximum[3] == 0, "right peak0 が不正");
        check(level.minimum[5] == -127 && level.maximum[5] == 0, "right peak2 が不正");
        check(level.minimum[0] != level.minimum[3], "channel を取り違えている");
    }

    // --- 正だけの bucket の min が 0 へ張り付かない ---
    {
        WaveformPeakBuilder builder;
        std::string error;
        check(builder.reset(8, 1, 4, error), "reset に失敗した");
        const std::vector<float> values = {0.5f, 0.75f, 0.5f, 0.5f};
        const float* planes[] = {values.data()};
        check(builder.addPlanar(0, planes, 4, error), "追加に失敗した");
        WaveformPeaks single;
        check(builder.finish(single, error), "finish に失敗した");
        check(single.levels[0].minimum[0] == 64 && single.levels[0].maximum[0] == 95,
              "正だけの bucket の min/max が不正");
    }

    // --- waveformColumn ---
    {
        const auto first = waveformColumn(peaks, 0, 0.0, 0.5);
        check(first.valid && first.maximum == 1.0f && first.minimum == -64.0f / 127.0f,
              "0..0.5 秒の left が peak0 でない");
        const auto silent = waveformColumn(peaks, 0, 0.5, 1.0);
        check(silent.valid && silent.maximum == 0.0f && silent.minimum == 0.0f,
              "0.5..1.0 秒の left が無音でない");
        // 区間が peak の途中から始まっても、触れた peak を含める。
        const auto spanning = waveformColumn(peaks, 1, 0.25, 1.25);
        check(spanning.valid && spanning.minimum == -1.0f, "複数 peak の min が取れていない");
        check(!waveformColumn(peaks, 0, 1.5, 2.0).valid, "素材の後ろを valid にした");
        check(!waveformColumn(peaks, 0, -1.0, -0.5).valid, "素材の前を valid にした");
        check(!waveformColumn(peaks, 2, 0.0, 0.5).valid, "存在しない channel を valid にした");
        check(!waveformColumn(peaks, 0, 0.5, 0.5).valid, "幅 0 の区間を valid にした");
    }

    // --- 粗い level: 幅の広い区間でも最細 level と同じ min/max を返す ---
    {
        WaveformPeakBuilder builder;
        std::string error;
        // 境界が浮動小数で揺れないよう、1 peak = 1/128 秒にする。
        check(builder.reset(128, 1, 1, error), "reset に失敗した");
        std::vector<float> values(1000, 0.0f);
        values[777] = 0.5f;
        values[123] = -0.25f;
        const float* planes[] = {values.data()};
        check(builder.addPlanar(0, planes, 1000, error), "追加に失敗した");
        WaveformPeaks wide;
        check(builder.finish(wide, error), "finish に失敗した");
        // 1000 peak -> 250 -> 63 -> 16 -> 4 で止まる。
        check(wide.levels.size() == 5, "level 数が 5 でない");
        if (wide.levels.size() == 5) {
            check(wide.levels[4].samplesPerPeak == 256 && wide.levels[4].peakCount == 4,
                  "最粗 level の peak 幅/数が不正");
        }
        // 1000/128 秒 ≒ 7.8 秒を覆う区間。最粗 level (1 peak = 2 秒) が選ばれる。
        // 区間 [6.2, 7.0) 秒は sample 777 (6.07 秒) を含まない。区間幅 0.8 秒と同程度の
        // 粗い level (1 peak = 0.5 秒、6.0 秒から) を選ぶと、区間外の peak が滲み込む。
        const auto outside = waveformColumn(wide, 0, 6.2, 7.0);
        check(outside.valid && outside.maximum == 0.0f && outside.minimum == 0.0f,
              "粗すぎる level を選び、区間外の peak が滲んでいる");
        const auto whole = waveformColumn(wide, 0, 0.0, 10.0);
        check(whole.valid && whole.maximum == 64.0f / 127.0f && whole.minimum == -32.0f / 127.0f,
              "粗い level で peak が落ちている");
        // sample 777 = 777/128 秒 = 6.0703125 秒。
        const auto narrow = waveformColumn(wide, 0, 6.0703125, 6.078125);
        check(narrow.valid && narrow.maximum == 64.0f / 127.0f, "最細 level で peak を拾えない");
        const auto beside = waveformColumn(wide, 0, 6.078125, 6.0859375);
        check(beside.valid && beside.maximum == 0.0f, "隣の区間へ peak が漏れている");
    }

    // cache の budget 計算に使う。量子化後の min/max (1 byte ずつ) を少なくとも数える。
    {
        std::size_t stored = 0;
        for (const auto& level : peaks.levels)
            stored += level.minimum.size() + level.maximum.size();
        check(mvm::core::waveformPeaksMemoryBytes(peaks) >= stored && stored == 12,
              "peak の byte 数が保持している min/max より小さい");
    }

    check(mvm::core::waveformSamplesPerPeak(48000) == 64, "48kHz の peak 幅が 64 でない");
    check(mvm::core::waveformSamplesPerPeak(100) == 1, "低い rate で peak 幅が 1 未満になった");

    if (failures != 0) {
        std::fprintf(stderr, "waveform peaks: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("waveform peaks: ok\n");
    return 0;
}

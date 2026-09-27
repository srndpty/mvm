#include "core/waveform_peaks.h"

#include "core/checked_integer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mvm::core {
namespace {

std::int8_t quantize(float value) {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    return static_cast<std::int8_t>(std::lround(clamped * 127.0f));
}

WaveformPeakLevel coarsen(const WaveformPeakLevel& finer, int channels) {
    WaveformPeakLevel coarser;
    coarser.samplesPerPeak = finer.samplesPerPeak * kWaveformLevelFactor;
    coarser.peakCount = (finer.peakCount + kWaveformLevelFactor - 1) / kWaveformLevelFactor;
    const auto total =
        static_cast<std::size_t>(coarser.peakCount) * static_cast<std::size_t>(channels);
    coarser.minimum.assign(total, 0);
    coarser.maximum.assign(total, 0);
    for (int channel = 0; channel < channels; ++channel) {
        const std::int64_t finerBase = channel * finer.peakCount;
        const std::int64_t coarserBase = channel * coarser.peakCount;
        for (std::int64_t index = 0; index < coarser.peakCount; ++index) {
            const std::int64_t begin = index * kWaveformLevelFactor;
            const std::int64_t end = std::min(finer.peakCount, begin + kWaveformLevelFactor);
            std::int8_t low = std::numeric_limits<std::int8_t>::max();
            std::int8_t high = std::numeric_limits<std::int8_t>::min();
            for (std::int64_t source = begin; source < end; ++source) {
                low = std::min(low, finer.minimum[static_cast<std::size_t>(finerBase + source)]);
                high = std::max(high, finer.maximum[static_cast<std::size_t>(finerBase + source)]);
            }
            coarser.minimum[static_cast<std::size_t>(coarserBase + index)] = low;
            coarser.maximum[static_cast<std::size_t>(coarserBase + index)] = high;
        }
    }
    return coarser;
}

} // namespace

std::size_t waveformPeaksMemoryBytes(const WaveformPeaks& peaks) {
    std::size_t total = sizeof(WaveformPeaks);
    for (const auto& level : peaks.levels)
        total += sizeof(WaveformPeakLevel) + level.minimum.capacity() + level.maximum.capacity();
    return total;
}

std::int64_t waveformSamplesPerPeak(int sampleRate) {
    return std::max<std::int64_t>(1, sampleRate / 750);
}

bool WaveformPeakBuilder::reset(int sampleRate, int channels, std::int64_t samplesPerPeak,
                                std::string& error, std::size_t workingByteBudget) {
    if (sampleRate <= 0 || channels <= 0 || samplesPerPeak <= 0) {
        error = "波形の sample rate / channel 数 / peak 幅が不正です";
        return false;
    }
    // 1 peak あたり channel ごとに float の min/max を 1 つずつ持つ。
    const std::size_t bytesPerPeak = static_cast<std::size_t>(channels) * 2 * sizeof(float);
    const std::size_t maximumPeaks = workingByteBudget / bytesPerPeak;
    if (maximumPeaks == 0) {
        error = "波形の作業領域の上限が小さすぎます";
        return false;
    }
    maximumPeakCount_ = static_cast<std::int64_t>(
        std::min<std::size_t>(maximumPeaks, std::numeric_limits<std::int64_t>::max()));
    sampleRate_ = sampleRate;
    channels_ = channels;
    samplesPerPeak_ = samplesPerPeak;
    peakCount_ = 0;
    minimum_.assign(static_cast<std::size_t>(channels), {});
    maximum_.assign(static_cast<std::size_t>(channels), {});
    return true;
}

bool WaveformPeakBuilder::addPlanar(std::int64_t startSample, const float* const* planes,
                                    int frameCount, std::string& error) {
    if (channels_ <= 0) {
        error = "波形 builder が初期化されていません";
        return false;
    }
    if (frameCount < 0 || (frameCount > 0 && !planes)) {
        error = "波形へ渡す sample 列が不正です";
        return false;
    }
    if (frameCount == 0)
        return true;
    // -startSample は INT64_MIN で溢れるので、全体が負の位置かを先に判定する。
    if (startSample <= -static_cast<std::int64_t>(frameCount))
        return true;
    const std::int64_t firstFrame = startSample < 0 ? -startSample : 0;
    std::int64_t lastSample = 0;
    if (!checkedAdd(startSample, frameCount - 1, lastSample) ||
        lastSample / samplesPerPeak_ >= maximumPeakCount_) {
        error = "音声素材が長すぎるか、timestamp が不正です";
        return false;
    }
    const std::int64_t requiredPeaks = lastSample / samplesPerPeak_ + 1;
    if (requiredPeaks > peakCount_) {
        // 未到達を +inf/-inf にしておき、finish で無音へ置き換える。
        // 0 で初期化すると、正の値だけの bucket の min が 0 に張り付く。
        for (int channel = 0; channel < channels_; ++channel) {
            minimum_[static_cast<std::size_t>(channel)].resize(
                static_cast<std::size_t>(requiredPeaks), std::numeric_limits<float>::infinity());
            maximum_[static_cast<std::size_t>(channel)].resize(
                static_cast<std::size_t>(requiredPeaks), -std::numeric_limits<float>::infinity());
        }
        peakCount_ = requiredPeaks;
    }
    for (int channel = 0; channel < channels_; ++channel) {
        const float* plane = planes[channel];
        if (!plane) {
            error = "波形へ渡す channel が欠けています";
            return false;
        }
        auto& low = minimum_[static_cast<std::size_t>(channel)];
        auto& high = maximum_[static_cast<std::size_t>(channel)];
        for (std::int64_t frame = firstFrame; frame < frameCount; ++frame) {
            const float value = plane[frame];
            if (!std::isfinite(value))
                continue;
            const auto bucket = static_cast<std::size_t>((startSample + frame) / samplesPerPeak_);
            low[bucket] = std::min(low[bucket], value);
            high[bucket] = std::max(high[bucket], value);
        }
    }
    return true;
}

bool WaveformPeakBuilder::finish(WaveformPeaks& peaks, std::string& error) {
    if (channels_ <= 0) {
        error = "波形 builder が初期化されていません";
        return false;
    }
    if (peakCount_ == 0) {
        error = "音声 sample が 1 つも得られませんでした";
        return false;
    }
    WaveformPeakLevel finest;
    finest.samplesPerPeak = samplesPerPeak_;
    finest.peakCount = peakCount_;
    const auto total = static_cast<std::size_t>(peakCount_) * static_cast<std::size_t>(channels_);
    finest.minimum.resize(total);
    finest.maximum.resize(total);
    for (int channel = 0; channel < channels_; ++channel) {
        const auto& low = minimum_[static_cast<std::size_t>(channel)];
        const auto& high = maximum_[static_cast<std::size_t>(channel)];
        const std::int64_t base = channel * peakCount_;
        for (std::int64_t index = 0; index < peakCount_; ++index) {
            const auto source = static_cast<std::size_t>(index);
            const bool touched = low[source] <= high[source];
            finest.minimum[static_cast<std::size_t>(base + index)] =
                touched ? quantize(low[source]) : 0;
            finest.maximum[static_cast<std::size_t>(base + index)] =
                touched ? quantize(high[source]) : 0;
        }
    }

    peaks = WaveformPeaks{};
    peaks.sampleRate = sampleRate_;
    peaks.channels = channels_;
    peaks.levels.push_back(std::move(finest));
    while (peaks.levels.back().peakCount > kWaveformLevelFactor)
        peaks.levels.push_back(coarsen(peaks.levels.back(), channels_));

    minimum_.assign(static_cast<std::size_t>(channels_), {});
    maximum_.assign(static_cast<std::size_t>(channels_), {});
    peakCount_ = 0;
    return true;
}

WaveformColumn waveformColumn(const WaveformPeaks& peaks, int channel, double startSeconds,
                              double endSeconds) {
    WaveformColumn column;
    if (peaks.levels.empty() || peaks.sampleRate <= 0 || channel < 0 || channel >= peaks.channels ||
        !(endSeconds > startSeconds) || !std::isfinite(startSeconds) || !std::isfinite(endSeconds))
        return column;

    // 端の peak は区間の外まで覆うので、peak 幅がそのまま区間外への滲みになる。
    // 区間幅の 1/kWaveformLevelFactor 以下の peak 幅を持つ最も粗い level を選び、
    // 滲みを 1 列の 1/4 未満に抑える。1 列あたりの走査は高々 2 * factor 程度で済む。
    // 区間が最細 peak より狭ければ最細を使う。
    const double span = endSeconds - startSeconds;
    const double maximumSecondsPerPeak = span / static_cast<double>(kWaveformLevelFactor);
    std::size_t levelIndex = 0;
    for (std::size_t index = 1; index < peaks.levels.size(); ++index) {
        const double seconds =
            static_cast<double>(peaks.levels[index].samplesPerPeak) / peaks.sampleRate;
        if (seconds > maximumSecondsPerPeak)
            break;
        levelIndex = index;
    }
    const auto& level = peaks.levels[levelIndex];
    const double secondsPerPeak = static_cast<double>(level.samplesPerPeak) / peaks.sampleRate;
    const double first = std::floor(startSeconds / secondsPerPeak);
    const double last = std::ceil(endSeconds / secondsPerPeak);
    if (last <= 0.0 || first >= static_cast<double>(level.peakCount))
        return column;
    const std::int64_t begin = std::max<std::int64_t>(0, static_cast<std::int64_t>(first));
    const std::int64_t end = std::min<std::int64_t>(
        level.peakCount, std::max<std::int64_t>(begin + 1, static_cast<std::int64_t>(last)));

    const std::int64_t base = channel * level.peakCount;
    std::int8_t low = std::numeric_limits<std::int8_t>::max();
    std::int8_t high = std::numeric_limits<std::int8_t>::min();
    for (std::int64_t index = begin; index < end; ++index) {
        low = std::min(low, level.minimum[static_cast<std::size_t>(base + index)]);
        high = std::max(high, level.maximum[static_cast<std::size_t>(base + index)]);
    }
    column.valid = true;
    column.minimum = static_cast<float>(low) / 127.0f;
    column.maximum = static_cast<float>(high) / 127.0f;
    return column;
}

} // namespace mvm::core

#include "project/audio_adjustment.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace mvm::project {
bool validateAudioAdjustmentSettings(const AudioAdjustmentSettings& s, int trackCount,
                                     std::string& error) {
    const auto bounded = [](double value, double min, double max) {
        return std::isfinite(value) && value >= min && value <= max;
    };
    if ((!s.normalize && !s.duck) || s.bgmTrack < 0 || s.bgmTrack >= trackCount ||
        (s.duck && s.voiceTracks.empty()) || !bounded(s.voiceLufs, -40, -5) ||
        !bounded(s.bgmLufs, -40, -5) || !bounded(s.reductionDb, 0, 60) ||
        !bounded(s.thresholdDb, -96, 0) || s.attackMs < 1 || s.attackMs > 10000 || s.holdMs < 0 ||
        s.holdMs > 10000 || s.releaseMs < 1 || s.releaseMs > 10000) {
        error = "対象トラックまたは自動音量調整の設定が範囲外です";
        return false;
    }
    std::set<int> tracks;
    for (int track : s.voiceTracks) {
        if (track < 0 || track >= trackCount || track == s.bgmTrack ||
            !tracks.insert(track).second) {
            error = "声と BGM は異なるトラックを重複なく指定してください";
            return false;
        }
    }
    return true;
}

std::vector<AudioDetectedRange> mergeAudioDetectedRanges(std::vector<AudioDetectedRange> ranges) {
    std::sort(ranges.begin(), ranges.end(),
              [](const auto& a, const auto& b) { return a.startSample < b.startSample; });
    std::vector<AudioDetectedRange> merged;
    for (const auto& range : ranges) {
        if (range.endSample <= range.startSample)
            continue;
        if (!merged.empty() && range.startSample <= merged.back().endSample)
            merged.back().endSample = std::max(merged.back().endSample, range.endSample);
        else
            merged.push_back(range);
    }
    return merged;
}

std::vector<ClipKeyframe> makeDuckingKeys(const std::vector<AudioDetectedRange>& ranges,
                                          const AudioAdjustmentSettings& s,
                                          std::int64_t clipStartFrame, std::int64_t durationFrames,
                                          std::int64_t fpsNum, std::int64_t fpsDen) {
    if (durationFrames <= 0 || fpsNum <= 0 || fpsDen <= 0)
        return {};
    auto merged = mergeAudioDetectedRanges(ranges);
    const std::int64_t attack = s.attackMs * 48LL, hold = s.holdMs * 48LL,
                       release = s.releaseMs * 48LL;
    std::vector<AudioDetectedRange> envelopes;
    for (const auto& range : merged) {
        if (!envelopes.empty() &&
            range.startSample - attack <= envelopes.back().endSample + hold + release)
            envelopes.back().endSample = std::max(envelopes.back().endSample, range.endSample);
        else
            envelopes.push_back(range);
    }
    const double samplesPerFrame =
        48000.0 * static_cast<double>(fpsDen) / static_cast<double>(fpsNum);
    // 折点は昇順で評価するため、過ぎた区間を毎回先頭から探し直さない。
    std::size_t envelopeIndex = 0;
    const auto valueAt = [&](std::int64_t local) {
        const double sample = static_cast<double>(clipStartFrame + local) * samplesPerFrame;
        while (envelopeIndex < envelopes.size() &&
               sample >= static_cast<double>(envelopes[envelopeIndex].endSample + hold + release))
            ++envelopeIndex;
        if (envelopeIndex < envelopes.size()) {
            const auto& range = envelopes[envelopeIndex];
            if (sample < static_cast<double>(range.startSample - attack))
                return 0.0;
            if (sample < static_cast<double>(range.startSample))
                return -s.reductionDb * (sample - static_cast<double>(range.startSample - attack)) /
                       static_cast<double>(attack);
            if (sample <= static_cast<double>(range.endSample + hold))
                return -s.reductionDb;
            if (sample < static_cast<double>(range.endSample + hold + release))
                return -s.reductionDb *
                       (1 - (sample - static_cast<double>(range.endSample + hold)) /
                                static_cast<double>(release));
        }
        return 0.0;
    };
    // 折点の前後を残し、frame 丸めによる線形補間のずれを防ぐ。
    std::set<std::int64_t> positions{0, durationFrames - 1};
    for (const auto& range : envelopes)
        for (auto sample : {range.startSample - attack, range.startSample, range.endSample + hold,
                            range.endSample + hold + release}) {
            const double local =
                static_cast<double>(sample) / samplesPerFrame - static_cast<double>(clipStartFrame);
            if (local < -1 || local > static_cast<double>(durationFrames))
                continue;
            for (auto frame : {static_cast<std::int64_t>(std::floor(local)),
                               static_cast<std::int64_t>(std::ceil(local))})
                positions.insert(std::clamp<std::int64_t>(frame, 0, durationFrames - 1));
        }
    std::vector<ClipKeyframe> keys;
    for (auto position : positions)
        keys.push_back({position, valueAt(position)});
    // 完全な無減衰なら空カーブにする。
    if (std::all_of(keys.begin(), keys.end(), [](const auto& key) { return key.value == 0; }))
        keys.clear();
    return keys;
}
} // namespace mvm::project

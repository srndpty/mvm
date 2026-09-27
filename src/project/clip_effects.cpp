#include "project/clip_effects.h"

#include "core/clip_fade.h"

#include <algorithm>
#include <cmath>

namespace mvm::project {
namespace {

bool inRange(double value, double minimum, double maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

} // namespace

bool clipEffectsAreDefault(const ClipEffects& effects) {
    return effects == ClipEffects{};
}

bool validateClipEffects(const ClipEffects& effects, std::int64_t sourceNativeDuration,
                         std::string& error) {
    if (!inRange(effects.positionXPercent, -1000.0, 1000.0) ||
        !inRange(effects.positionYPercent, -1000.0, 1000.0)) {
        error = "位置 X/Y は -1000% 以上 1000% 以下である必要があります";
        return false;
    }
    if (!inRange(effects.scalePercent, 1.0, 1000.0)) {
        error = "拡大率は 1% 以上 1000% 以下である必要があります";
        return false;
    }
    if (!inRange(effects.rotationDegrees, -360.0, 360.0)) {
        error = "回転は -360° 以上 360° 以下である必要があります";
        return false;
    }
    if (!inRange(effects.opacityPercent, 0.0, 100.0)) {
        error = "不透明度は 0% 以上 100% 以下である必要があります";
        return false;
    }
    if (!inRange(effects.volumePercent, 0.0, 200.0)) {
        error = "音量は 0% 以上 200% 以下である必要があります";
        return false;
    }
    const double crops[] = {effects.cropLeftPercent, effects.cropTopPercent,
                            effects.cropRightPercent, effects.cropBottomPercent};
    for (double crop : crops) {
        if (!std::isfinite(crop) || crop < 0.0 || crop >= 100.0) {
            error = "Crop は 0% 以上 100% 未満である必要があります";
            return false;
        }
    }
    if (effects.cropLeftPercent + effects.cropRightPercent >= 100.0 ||
        effects.cropTopPercent + effects.cropBottomPercent >= 100.0) {
        error = "左右または上下の Crop 合計は 100% 未満である必要があります";
        return false;
    }
    if (sourceNativeDuration <= 0 || effects.fadeInFrames < 0 || effects.fadeOutFrames < 0 ||
        effects.fadeInFrames > sourceNativeDuration ||
        effects.fadeOutFrames > sourceNativeDuration ||
        effects.fadeInFrames > sourceNativeDuration - effects.fadeOutFrames) {
        error = "フェードイン/アウトは素材固有フレームのclip尺内で重ならない必要があります";
        return false;
    }
    error.clear();
    return true;
}

bool validateClipKeyframes(const std::vector<ClipKeyframe>& keys, std::int64_t timelineDuration,
                           double maximumPercent, std::string& error) {
    std::int64_t previous = -1;
    for (const auto& key : keys) {
        if (key.frame <= previous || key.frame < 0 || key.frame >= timelineDuration ||
            !inRange(key.valuePercent, 0.0, maximumPercent)) {
            error = "キーフレームの位置・順序・値が不正です";
            return false;
        }
        previous = key.frame;
    }
    error.clear();
    return true;
}

double evaluateClipKeys(const std::vector<ClipKeyframe>& keys, double basePercent,
                        std::int64_t localFrame) {
    if (keys.empty())
        return basePercent;
    const auto next = std::lower_bound(
        keys.begin(), keys.end(), localFrame,
        [](const ClipKeyframe& key, std::int64_t frame) { return key.frame < frame; });
    if (next == keys.begin())
        return next->valuePercent;
    if (next == keys.end())
        return keys.back().valuePercent;
    if (next->frame == localFrame)
        return next->valuePercent;
    const auto& previous = *(next - 1);
    const double ratio = static_cast<double>(localFrame - previous.frame) /
                         static_cast<double>(next->frame - previous.frame);
    return previous.valuePercent + (next->valuePercent - previous.valuePercent) * ratio;
}

double evaluateClipOpacity(const ClipEffects& effects, std::int64_t timelineLocalFrame,
                           std::int64_t sourceLocalFrame, std::int64_t sourceDuration) {
    return evaluateClipKeys(effects.opacityKeys, effects.opacityPercent, timelineLocalFrame) /
           100.0 *
           core::clipFadeFactor(sourceLocalFrame, sourceDuration, effects.fadeInFrames,
                                effects.fadeOutFrames);
}

double evaluateClipVolume(const ClipEffects& effects, std::int64_t timelineLocalFrame,
                          std::int64_t sourceLocalFrame, std::int64_t sourceDuration) {
    return evaluateClipKeys(effects.volumeKeys, effects.volumePercent, timelineLocalFrame) / 100.0 *
           core::clipFadeFactor(sourceLocalFrame, sourceDuration, effects.fadeInFrames,
                                effects.fadeOutFrames);
}

void reframeClipKeys(std::vector<ClipKeyframe>& keys, std::int64_t oldDuration,
                     std::int64_t newDuration, std::int64_t newStartInOldFrames) {
    if (keys.empty() || oldDuration <= 0 || newDuration <= 0)
        return;
    const auto old = keys;
    keys.clear();
    const auto append = [&](std::int64_t frame, double value) {
        if (keys.empty() || keys.back().frame != frame)
            keys.push_back({frame, value});
    };
    append(0, evaluateClipKeys(old, old.front().valuePercent, newStartInOldFrames));
    for (const auto& key : old) {
        const auto frame = key.frame - newStartInOldFrames;
        if (frame > 0 && frame < newDuration - 1)
            append(frame, key.valuePercent);
    }
    if (newDuration > 1)
        append(newDuration - 1, evaluateClipKeys(old, old.back().valuePercent,
                                                 newStartInOldFrames + newDuration - 1));
}

ClipEffectMapping mapClipEffects(const ClipEffects& effects) {
    const double left = effects.cropLeftPercent / 100.0;
    const double top = effects.cropTopPercent / 100.0;
    const double width = 1.0 - left - effects.cropRightPercent / 100.0;
    const double height = 1.0 - top - effects.cropBottomPercent / 100.0;
    const double scale = effects.scalePercent / 100.0;
    const double centerX = left + width * 0.5;
    const double centerY = top + height * 0.5;
    const double scaledWidth = width * scale;
    const double scaledHeight = height * scale;

    ClipEffectMapping result;
    result.sourceRect = {left, top, width, height};
    result.destinationRect = {centerX - scaledWidth * 0.5 + effects.positionXPercent / 100.0,
                              centerY - scaledHeight * 0.5 + effects.positionYPercent / 100.0,
                              scaledWidth, scaledHeight};
    result.rotationDegrees = effects.rotationDegrees;
    result.baseOpacity = effects.opacityPercent / 100.0;
    result.fadeInFrames = effects.fadeInFrames;
    result.fadeOutFrames = effects.fadeOutFrames;
    return result;
}

} // namespace mvm::project

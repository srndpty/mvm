#include "project/clip_effects.h"

#include "core/clip_fade.h"
#include "core/layer_placement.h"
#include "core/source_frame_mapping.h"

#include <algorithm>
#include <cmath>

namespace mvm::project {
namespace {

__extension__ using WideInteger = __int128;

bool inRange(double value, double minimum, double maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

} // namespace

const std::vector<EffectChannel>& effectChannels() {
    static const std::vector<EffectChannel> channels = {
        {ClipKeyKind::Opacity, "opacity", &ClipEffects::opacityPercent, &ClipEffects::opacityKeys,
         0, 100},
        {ClipKeyKind::Volume, "volume", &ClipEffects::volumePercent, &ClipEffects::volumeKeys, 0,
         200},
        {ClipKeyKind::Ducking, "ducking", &ClipEffects::duckingDb, &ClipEffects::duckingKeys, -60,
         0},
        {ClipKeyKind::PositionX, "positionX", &ClipEffects::positionXPercent,
         &ClipEffects::positionXKeys, -1000, 1000},
        {ClipKeyKind::PositionY, "positionY", &ClipEffects::positionYPercent,
         &ClipEffects::positionYKeys, -1000, 1000},
        {ClipKeyKind::ScaleX, "scaleX", &ClipEffects::scaleXPercent, &ClipEffects::scaleXKeys, 1,
         1000},
        {ClipKeyKind::ScaleY, "scaleY", &ClipEffects::scaleYPercent, &ClipEffects::scaleYKeys, 1,
         1000},
        {ClipKeyKind::Rotation, "rotation", &ClipEffects::rotationDegrees,
         &ClipEffects::rotationKeys, -360, 360},
        {ClipKeyKind::CropLeft, "cropLeft", &ClipEffects::cropLeftPercent,
         &ClipEffects::cropLeftKeys, 0, 100},
        {ClipKeyKind::CropTop, "cropTop", &ClipEffects::cropTopPercent, &ClipEffects::cropTopKeys,
         0, 100},
        {ClipKeyKind::CropRight, "cropRight", &ClipEffects::cropRightPercent,
         &ClipEffects::cropRightKeys, 0, 100},
        {ClipKeyKind::CropBottom, "cropBottom", &ClipEffects::cropBottomPercent,
         &ClipEffects::cropBottomKeys, 0, 100}};
    return channels;
}

const EffectChannel* effectChannel(ClipKeyKind kind) {
    for (const auto& channel : effectChannels())
        if (channel.kind == kind)
            return &channel;
    return nullptr;
}

bool isAudioEffectChannel(ClipKeyKind kind) {
    return kind == ClipKeyKind::Volume || kind == ClipKeyKind::Ducking;
}

const EffectChannel* effectChannel(const std::string& name) {
    for (const auto& channel : effectChannels())
        if (channel.name == name)
            return &channel;
    return nullptr;
}

namespace {
bool validChannelValue(const EffectChannel& channel, double value) {
    return inRange(value, channel.minimum, channel.maximum) &&
           (channel.kind < ClipKeyKind::CropLeft || value < channel.maximum);
}

double ease(KeyInterpolation interpolation, double t, double c1 = 1.0 / 3, double c2 = 2.0 / 3) {
    switch (interpolation) {
    case KeyInterpolation::Linear:
        return t;
    case KeyInterpolation::EaseIn:
        return t * t;
    case KeyInterpolation::EaseOut:
        return t * (2 - t);
    case KeyInterpolation::EaseInOut:
        return t * t * (3 - 2 * t);
    case KeyInterpolation::Spline:
        return 3 * (1 - t) * (1 - t) * t * c1 + 3 * (1 - t) * t * t * c2 + t * t * t;
    }
    return t;
}

double evaluateKeysAt(const std::vector<ClipKeyframe>& keys, double base, double frame) {
    if (keys.empty())
        return base;
    const auto next =
        std::lower_bound(keys.begin(), keys.end(), frame, [](const ClipKeyframe& key, double at) {
            return static_cast<double>(key.frame) < at;
        });
    if (next == keys.begin())
        return next->value;
    if (next == keys.end())
        return keys.back().value;
    if (static_cast<double>(next->frame) == frame)
        return next->value;
    const auto& previous = *(next - 1);
    const double t = (frame - static_cast<double>(previous.frame)) /
                     static_cast<double>(next->frame - previous.frame);
    const double start =
        ease(previous.interpolation, previous.curveStart, previous.control1, previous.control2);
    const double end =
        ease(previous.interpolation, previous.curveEnd, previous.control1, previous.control2);
    const double progress = ease(
        previous.interpolation, previous.curveStart + (previous.curveEnd - previous.curveStart) * t,
        previous.control1, previous.control2);
    return previous.value + (next->value - previous.value) * (progress - start) / (end - start);
}
} // namespace

ClipEffects evaluateClipEffects(const ClipEffects& effects, std::int64_t localFrame) {
    ClipEffects result;
    result.fadeInFrames = effects.fadeInFrames;
    result.fadeOutFrames = effects.fadeOutFrames;
    result.normalizationGainDb = effects.normalizationGainDb;
    result.audioAdjustmentSettings = effects.audioAdjustmentSettings;
    result.audioAdjustmentFingerprint = effects.audioAdjustmentFingerprint;
    for (const auto& channel : effectChannels()) {
        result.*channel.base =
            evaluateClipKeys(effects.*channel.keys, effects.*channel.base, localFrame);
        (result.*channel.keys).clear();
    }
    return result;
}

void insertClipKey(std::vector<ClipKeyframe>& keys, std::int64_t frame, double value) {
    auto next =
        std::lower_bound(keys.begin(), keys.end(), frame,
                         [](const ClipKeyframe& key, std::int64_t at) { return key.frame < at; });
    if (next != keys.end() && next->frame == frame) {
        next->value = value;
        return;
    }
    ClipKeyframe inserted{frame, value};
    if (next != keys.begin() && next != keys.end()) {
        auto& previous = *(next - 1);
        const double t = static_cast<double>(frame - previous.frame) /
                         static_cast<double>(next->frame - previous.frame);
        const double split = previous.curveStart + (previous.curveEnd - previous.curveStart) * t;
        inserted.interpolation = previous.interpolation;
        inserted.control1 = previous.control1;
        inserted.control2 = previous.control2;
        inserted.curveStart = split;
        inserted.curveEnd = previous.curveEnd;
        previous.curveEnd = split;
    }
    keys.insert(next, inserted);
}

std::size_t removeClipKeys(std::vector<ClipKeyframe>& keys,
                           const std::vector<std::int64_t>& frames) {
    std::vector<ClipKeyframe> kept;
    kept.reserve(keys.size());
    for (const auto& key : keys) {
        if (std::find(frames.begin(), frames.end(), key.frame) == frames.end()) {
            kept.push_back(key);
            continue;
        }
        if (kept.empty())
            continue;
        // insertClipKey が分けた境界は同じ値で書き込まれるので、完全一致で判定できる。
        auto& previous = kept.back();
        if (previous.curveEnd == key.curveStart && previous.interpolation == key.interpolation &&
            previous.control1 == key.control1 && previous.control2 == key.control2)
            previous.curveEnd = key.curveEnd;
    }
    const auto removed = keys.size() - kept.size();
    keys = std::move(kept);
    return removed;
}

bool validateEffectKeys(const ClipEffects& effects, std::int64_t duration, bool audio,
                        std::string& error) {
    for (const auto& channel : effectChannels()) {
        std::int64_t previous = -1;
        const auto& keys = effects.*channel.keys;
        if (!keys.empty() && (isAudioEffectChannel(channel.kind) != audio)) {
            error = "素材種別に適用できないキーフレームです";
            return false;
        }
        for (const auto& key : keys) {
            if (key.frame <= previous || key.frame < 0 || key.frame >= duration ||
                !validChannelValue(channel, key.value) ||
                key.interpolation < KeyInterpolation::Linear ||
                key.interpolation > KeyInterpolation::Spline || !inRange(key.curveStart, 0, 1) ||
                !inRange(key.curveEnd, 0, 1) || key.curveStart >= key.curveEnd ||
                !inRange(key.control1, 0, 1) || !inRange(key.control2, 0, 1) ||
                ease(key.interpolation, key.curveEnd, key.control1, key.control2) <=
                    ease(key.interpolation, key.curveStart, key.control1, key.control2)) {
                error = "キーフレームの位置・値・補間が不正です";
                return false;
            }
            previous = key.frame;
        }
    }
    // 区間ごとの三次多項式の極値まで調べる。異なる補間のクロップ合計も見落とさない。
    for (const auto& pair : {std::pair{ClipKeyKind::CropLeft, ClipKeyKind::CropRight},
                             std::pair{ClipKeyKind::CropTop, ClipKeyKind::CropBottom}}) {
        const auto& a = *effectChannel(pair.first);
        const auto& b = *effectChannel(pair.second);
        std::vector<std::int64_t> boundaries{0, std::max<std::int64_t>(0, duration - 1)};
        for (const auto* channel : {&a, &b})
            for (const auto& key : effects.*channel->keys)
                boundaries.push_back(key.frame);
        std::sort(boundaries.begin(), boundaries.end());
        boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
        const auto sum = [&](double frame) {
            return evaluateKeysAt(effects.*a.keys, effects.*a.base, frame) +
                   evaluateKeysAt(effects.*b.keys, effects.*b.base, frame);
        };
        for (std::size_t i = 0; i < boundaries.size(); ++i) {
            if (sum(static_cast<double>(boundaries[i])) >= 100) {
                error = "クロップ合計は全区間で100%未満である必要があります";
                return false;
            }
            if (i + 1 == boundaries.size())
                break;
            const double start = static_cast<double>(boundaries[i]);
            const double span = static_cast<double>(boundaries[i + 1] - boundaries[i]);
            const double d = sum(start), y1 = sum(start + span / 3) - d,
                         y2 = sum(start + span * 2 / 3) - d, y3 = sum(start + span) - d;
            const double c3 = 4.5 * y3 - 13.5 * y2 + 13.5 * y1;
            const double c2 = -4.5 * y3 + 18 * y2 - 22.5 * y1;
            const double c1 = y3 - c3 - c2;
            const auto invalidAt = [&](double t) {
                return t > 0 && t < 1 && sum(start + span * t) >= 100;
            };
            if (std::abs(c3) < 1e-10) {
                if (std::abs(c2) > 1e-10 && invalidAt(-c1 / (2 * c2))) {
                    error = "キー間でクロップ合計が100%以上になります";
                    return false;
                }
            } else {
                const double discriminant = 4 * c2 * c2 - 12 * c3 * c1;
                if (discriminant >= 0 &&
                    (invalidAt((-2 * c2 + std::sqrt(discriminant)) / (6 * c3)) ||
                     invalidAt((-2 * c2 - std::sqrt(discriminant)) / (6 * c3)))) {
                    error = "キー間でクロップ合計が100%以上になります";
                    return false;
                }
            }
        }
    }
    error.clear();
    return true;
}

bool clipEffectsAreDefault(const ClipEffects& effects) {
    return effects == ClipEffects{};
}

bool validateClipEffects(const ClipEffects& effects, std::int64_t sourceNativeDuration,
                         std::string& error) {
    if (!inRange(effects.normalizationGainDb, -96, 60)) {
        error = "ラウドネス補正は −96〜60 dB の範囲で指定してください";
        return false;
    }
    for (const auto& channel : effectChannels()) {
        if (!validChannelValue(channel, effects.*channel.base)) {
            error = std::string("エフェクトの固定値が範囲外です: ") + channel.name;
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
    ClipEffects effects;
    const bool audio = maximumPercent > 100;
    if (audio)
        effects.volumeKeys = keys;
    else
        effects.opacityKeys = keys;
    if (!validateEffectKeys(effects, timelineDuration, audio, error))
        return false;
    for (const auto& key : keys)
        if (key.value > maximumPercent) {
            error = "キーフレームの値が上限を超えています";
            return false;
        }
    return true;
}

std::pair<double, double> clipKeySplineControls(const ClipKeyframe& key) {
    const double start = ease(key.interpolation, key.curveStart, key.control1, key.control2);
    const double end = ease(key.interpolation, key.curveEnd, key.control1, key.control2);
    const auto value = [&](double t) {
        return (ease(key.interpolation, key.curveStart + (key.curveEnd - key.curveStart) * t,
                     key.control1, key.control2) -
                start) /
               (end - start);
    };
    const double a = 27 * value(1.0 / 3) - 1;
    const double b = 27 * value(2.0 / 3) - 8;
    return {(2 * a - b) / 18, (2 * b - a) / 18};
}

double evaluateClipKeys(const std::vector<ClipKeyframe>& keys, double basePercent,
                        std::int64_t localFrame) {
    return evaluateKeysAt(keys, basePercent, static_cast<double>(localFrame));
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

void rescaleClipKeys(std::vector<ClipKeyframe>& keys, std::int64_t oldDuration,
                     std::int64_t newDuration) {
    if (keys.empty() || oldDuration <= 0 || newDuration <= 0 || oldDuration == newDuration)
        return;
    const auto old = keys;
    keys.clear();
    for (const auto& key : old) {
        // 端から端へ: frame k -> round(k (new - 1) / (old - 1))。
        const std::int64_t frame =
            oldDuration == 1 ? 0
                             : static_cast<std::int64_t>(
                                   (static_cast<WideInteger>(key.frame) * (newDuration - 1) * 2 +
                                    (oldDuration - 1)) /
                                   (static_cast<WideInteger>(oldDuration - 1) * 2));
        if (keys.empty() || keys.back().frame != frame) {
            auto moved = key;
            moved.frame = frame;
            keys.push_back(moved);
        }
    }
}

void reframeClipKeys(std::vector<ClipKeyframe>& keys, std::int64_t oldDuration,
                     std::int64_t newDuration, std::int64_t newStartInOldFrames) {
    if (keys.empty() || oldDuration <= 0 || newDuration <= 0)
        return;
    const auto old = keys;
    keys.clear();
    std::vector<std::int64_t> points{newStartInOldFrames};
    for (const auto& key : old)
        if (key.frame > newStartInOldFrames && key.frame < newStartInOldFrames + newDuration - 1)
            points.push_back(key.frame);
    if (newDuration > 1)
        points.push_back(newStartInOldFrames + newDuration - 1);
    for (std::size_t i = 0; i < points.size(); ++i) {
        ClipKeyframe key{points[i] - newStartInOldFrames,
                         evaluateClipKeys(old, old.front().value, points[i])};
        if (i + 1 < points.size()) {
            const auto next =
                std::upper_bound(old.begin(), old.end(), points[i],
                                 [](std::int64_t frame, const ClipKeyframe& candidate) {
                                     return frame < candidate.frame;
                                 });
            if (next != old.begin() && next != old.end()) {
                const auto& previous = *(next - 1);
                const double span = static_cast<double>(next->frame - previous.frame);
                key.interpolation = previous.interpolation;
                key.control1 = previous.control1;
                key.control2 = previous.control2;
                key.curveStart = previous.curveStart +
                                 (previous.curveEnd - previous.curveStart) *
                                     static_cast<double>(points[i] - previous.frame) / span;
                key.curveEnd = previous.curveStart +
                               (previous.curveEnd - previous.curveStart) *
                                   static_cast<double>(points[i + 1] - previous.frame) / span;
            }
        }
        keys.push_back(key);
    }
}

bool retimeClipKeys(std::vector<ClipKeyframe>& keys, std::int64_t fromFpsNum,
                    std::int64_t fromFpsDen, std::int64_t toFpsNum, std::int64_t toFpsDen,
                    std::int64_t newDuration) {
    if (newDuration <= 0)
        return false;
    std::vector<ClipKeyframe> retimed;
    for (const auto& key : keys) {
        // floor(k to / from + 1/2)。表示 frame の四捨五入と同じ有理数計算を使うため、
        // 移し先を「素材」、移し元を「output」として渡す。
        const auto frame = core::sourceFrameAtOutputPosition(key.frame, {toFpsNum, toFpsDen},
                                                             {fromFpsNum, fromFpsDen});
        if (!frame)
            return false;
        const auto clamped = std::min(*frame, newDuration - 1);
        if (retimed.empty() || retimed.back().frame != clamped) {
            auto moved = key;
            moved.frame = clamped;
            retimed.push_back(moved);
        }
    }
    keys = std::move(retimed);
    return true;
}

std::optional<double> stepVolumePercentByDb(double percent, double stepDb) {
    if (!inRange(percent, 0.0, kVolumeMaximumPercent) || !std::isfinite(stepDb) || stepDb == 0.0)
        return std::nullopt;
    const double gain = std::pow(10.0, stepDb / 20.0);
    if (stepDb > 0.0)
        return std::min(kVolumeMaximumPercent, std::max(percent, kVolumeStepFloorPercent) * gain);
    if (percent <= kVolumeStepFloorPercent)
        return percent;
    return std::max(kVolumeStepFloorPercent, percent * gain);
}

ClipEffectMapping mapClipEffects(const ClipEffects& effects) {
    const double left = effects.cropLeftPercent / 100.0;
    const double top = effects.cropTopPercent / 100.0;
    const double width = 1.0 - left - effects.cropRightPercent / 100.0;
    const double height = 1.0 - top - effects.cropBottomPercent / 100.0;
    const double centerX = left + width * 0.5;
    const double centerY = top + height * 0.5;
    const double scaledWidth = width * effects.scaleXPercent / 100.0;
    const double scaledHeight = height * effects.scaleYPercent / 100.0;

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

namespace {

core::LayerRect layerRect(const NormalizedEffectRect& rect) {
    return {rect.x, rect.y, rect.width, rect.height};
}

} // namespace

ClipVisualGeometry clipVisualGeometry(const ClipEffects& effects, int sourceWidth, int sourceHeight,
                                      int canvasWidth, int canvasHeight) {
    ClipVisualGeometry result;
    const auto mapped = mapClipEffects(effects);
    const auto placed =
        core::placeLayer(sourceWidth, sourceHeight, canvasWidth, canvasHeight,
                         layerRect(mapped.sourceRect), layerRect(mapped.destinationRect));
    if (placed.empty)
        return result;
    result.x = placed.destination.x * canvasWidth;
    result.y = placed.destination.y * canvasHeight;
    result.width = placed.destination.width * canvasWidth;
    result.height = placed.destination.height * canvasHeight;
    result.pivotX = placed.pivotX * canvasWidth;
    result.pivotY = placed.pivotY * canvasHeight;
    result.rotationDegrees = mapped.rotationDegrees;
    result.valid = true;
    return result;
}

bool effectsForVisualRect(ClipEffects& effects, int sourceWidth, int sourceHeight, int canvasWidth,
                          int canvasHeight, double x, double y, double width, double height) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height) ||
        width <= 0.0 || height <= 0.0)
        return false;
    const auto crop = mapClipEffects(effects).sourceRect;
    // crop 範囲をそのまま置いたときに見える範囲 (letterbox との重なり)。拡縮と位置に依らない。
    const auto visible = core::placeLayer(sourceWidth, sourceHeight, canvasWidth, canvasHeight,
                                          layerRect(crop), layerRect(crop));
    if (visible.empty)
        return false;
    const auto& seen = visible.destination;
    const double scaleX = std::clamp(width / canvasWidth / seen.width, 0.01, 10.0);
    const double scaleY = std::clamp(height / canvasHeight / seen.height, 0.01, 10.0);
    // 見える範囲の左上が x, y に来るように、crop 範囲の置き場所 (destination) を決める。
    const double destinationX = x / canvasWidth - (seen.x - crop.x) * scaleX;
    const double destinationY = y / canvasHeight - (seen.y - crop.y) * scaleY;
    // mapClipEffects: destination = crop 中心 - 拡縮後の半分 + 位置。
    const double positionX = destinationX - (crop.x + crop.width * 0.5) + crop.width * scaleX * 0.5;
    const double positionY =
        destinationY - (crop.y + crop.height * 0.5) + crop.height * scaleY * 0.5;
    effects.scaleXPercent = scaleX * 100.0;
    effects.scaleYPercent = scaleY * 100.0;
    effects.positionXPercent = std::clamp(positionX * 100.0, -1000.0, 1000.0);
    effects.positionYPercent = std::clamp(positionY * 100.0, -1000.0, 1000.0);
    return true;
}

} // namespace mvm::project

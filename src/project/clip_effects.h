#ifndef MVM_PROJECT_CLIP_EFFECTS_H
#define MVM_PROJECT_CLIP_EFFECTS_H

#include <cstdint>
#include <string>

namespace mvm::project {

struct ClipEffects {
    double positionXPercent = 0.0;
    double positionYPercent = 0.0;
    double scalePercent = 100.0;
    double rotationDegrees = 0.0;
    double opacityPercent = 100.0;
    double cropLeftPercent = 0.0;
    double cropTopPercent = 0.0;
    double cropRightPercent = 0.0;
    double cropBottomPercent = 0.0;
    std::int64_t fadeInFrames = 0;  // 素材固有フレーム数
    std::int64_t fadeOutFrames = 0; // 素材固有フレーム数
    bool operator==(const ClipEffects&) const = default;
};

struct NormalizedEffectRect {
    double x = 0.0;
    double y = 0.0;
    double width = 1.0;
    double height = 1.0;
    bool operator==(const NormalizedEffectRect&) const = default;
};

struct ClipEffectMapping {
    NormalizedEffectRect sourceRect;
    NormalizedEffectRect destinationRect;
    double rotationDegrees = 0.0;
    double baseOpacity = 1.0;
    std::int64_t fadeInFrames = 0;
    std::int64_t fadeOutFrames = 0;
};

bool clipEffectsAreDefault(const ClipEffects& effects);
bool validateClipEffects(const ClipEffects& effects, std::int64_t sourceNativeDuration,
                         std::string& error);
ClipEffectMapping mapClipEffects(const ClipEffects& effects);

} // namespace mvm::project

#endif

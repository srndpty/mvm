#ifndef MVM_PROJECT_CLIP_EFFECTS_H
#define MVM_PROJECT_CLIP_EFFECTS_H

#include <cstdint>
#include <string>
#include <vector>

namespace mvm::project {

struct ClipKeyframe {
    std::int64_t frame = 0;
    double valuePercent = 100.0;
    bool operator==(const ClipKeyframe&) const = default;
};

struct ClipEffects {
    double positionXPercent = 0.0;
    double positionYPercent = 0.0;
    // 縦横の拡大率は別々に持つ (プレビューの枠のハンドルで片方だけ伸縮できる)。
    double scaleXPercent = 100.0;
    double scaleYPercent = 100.0;
    double rotationDegrees = 0.0;
    double opacityPercent = 100.0;
    double volumePercent = 100.0;
    std::vector<ClipKeyframe> opacityKeys;
    std::vector<ClipKeyframe> volumeKeys;
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
bool validateClipKeyframes(const std::vector<ClipKeyframe>& keys, std::int64_t timelineDuration,
                           double maximumPercent, std::string& error);
double evaluateClipKeys(const std::vector<ClipKeyframe>& keys, double basePercent,
                        std::int64_t localFrame);
double evaluateClipOpacity(const ClipEffects& effects, std::int64_t timelineLocalFrame,
                           std::int64_t sourceLocalFrame, std::int64_t sourceDuration);
double evaluateClipVolume(const ClipEffects& effects, std::int64_t timelineLocalFrame,
                          std::int64_t sourceLocalFrame, std::int64_t sourceDuration);
// 尺 oldDuration の key を尺 newDuration へ伸縮する (レート調整)。key は素材の内容に付いて動き、
// 先頭と末尾の frame はそれぞれ先頭と末尾へ写る。丸めで同じ frame に重なった key は先の 1
// つを残す。
void rescaleClipKeys(std::vector<ClipKeyframe>& keys, std::int64_t oldDuration,
                     std::int64_t newDuration);
void reframeClipKeys(std::vector<ClipKeyframe>& keys, std::int64_t oldDuration,
                     std::int64_t newDuration, std::int64_t newStartInOldFrames);
// timeline fps の違う Project へ clip を移すときの key 換算。key は clip 先頭からの timeline
// frame なので、同じ秒位置に最も近い移し先の frame へ写す (ちょうど 1/2 は後ろへ)。尺
// newDuration の外へ出た key は末尾 frame に寄せ、同じ frame に重なった key は rescaleClipKeys
// と同じく先の 1 つを残す。fps が不正または換算が overflow したら false (keys は変えない)。
bool retimeClipKeys(std::vector<ClipKeyframe>& keys, std::int64_t fromFpsNum,
                    std::int64_t fromFpsDen, std::int64_t toFpsNum, std::int64_t toFpsDen,
                    std::int64_t newDuration);
ClipEffectMapping mapClipEffects(const ClipEffects& effects);

// 素材を canvas に置いたときに見えている矩形 (出力画素、回転前)。preview の枠と
// ハンドルが使う。幾何は preview / 書き出しと同じ core::placeLayer で決める。
//   pivotX/Y        : 回転の中心 (出力画素)
//   valid = false   : 寸法が不正、または crop で何も見えていない
struct ClipVisualGeometry {
    bool valid = false;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    double pivotX = 0.0;
    double pivotY = 0.0;
    double rotationDegrees = 0.0;
};

ClipVisualGeometry clipVisualGeometry(const ClipEffects& effects, int sourceWidth, int sourceHeight,
                                      int canvasWidth, int canvasHeight);
// clipVisualGeometry の逆。見えている矩形を (x, y, width, height) にする位置と拡大率を返す。
// crop と回転は変えない。拡大率は 1..1000%、位置は -1000..1000% に収める
// (収めた分だけ指定の矩形からずれる)。寸法が不正なら false で effects を変えない。
bool effectsForVisualRect(ClipEffects& effects, int sourceWidth, int sourceHeight, int canvasWidth,
                          int canvasHeight, double x, double y, double width, double height);

} // namespace mvm::project

#endif

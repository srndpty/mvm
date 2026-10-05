#include "app/math_clip_render.h"

#include "media/still_image/static_image.h"
#include "project/project.h"
#include "project/timeline_edit.h"

#include <algorithm>
#include <cstdint>

namespace mvm::app {

math::MathRenderSpec mathRenderSpecFor(const project::MathClipData& data) {
    return {data.syntax, data.source, data.fontSize};
}

std::optional<math::MathSequenceSpec> mathSequenceSpecFor(const project::TimelineClip& clip) {
    if (clip.kind != project::TimelineClipKind::Math ||
        clip.mathAnimation.intro != project::MathIntroKind::Write)
        return std::nullopt;
    return math::MathSequenceSpec{mathRenderSpecFor(clip.math), math::MathAnimationKind::Write,
                                  clip.mathAnimation.introFrames};
}

bool mathComposeStyleFor(const project::MathClipData& data, math::MathComposeStyle& style) {
    return project::parseArgbColor(data.color, style.colorArgb) &&
           project::parseArgbColor(data.backgroundColor, style.backgroundArgb);
}

std::optional<std::int64_t> mathIntroFrameAt(const project::TimelineClip& clip,
                                             std::int64_t timelineFpsNum,
                                             std::int64_t timelineFpsDen,
                                             std::int64_t clipLocalFrame) {
    if (clip.kind != project::TimelineClipKind::Math ||
        clip.mathAnimation.intro == project::MathIntroKind::None)
        return -1;
    const auto source = project::clipFadeSourceFrameAt(clip, timelineFpsNum, timelineFpsDen,
                                                       std::max<std::int64_t>(0, clipLocalFrame));
    if (!source.success)
        return std::nullopt;
    return source.frame < clip.mathAnimation.introFrames ? source.frame : -1;
}

math::MathComposeResult composeMathClipRaster(const media::StillImage& mask,
                                              const project::MathClipData& data, int outputWidth,
                                              int outputHeight) {
    math::MathComposeStyle style;
    if (!mathComposeStyleFor(data, style)) {
        math::MathComposeResult result;
        result.error = "数式の色は #AARRGGBB で指定してください";
        return result;
    }
    const auto expected =
        static_cast<std::size_t>(mask.width) * static_cast<std::size_t>(mask.height) * 4U;
    if (mask.width <= 0 || mask.height <= 0 || mask.rgba.size() != expected) {
        math::MathComposeResult result;
        result.error = "数式の画像の大きさが不正です";
        return result;
    }
    return math::composeMathRaster(mask.rgba.data(), mask.width, mask.height, style, outputWidth,
                                   outputHeight);
}

math::MathComposeResult composeMathClipFromPng(const std::filesystem::path& png,
                                               const project::MathClipData& data, int outputWidth,
                                               int outputHeight) {
    const auto decoded = media::loadStaticImage(png);
    if (!decoded.success) {
        math::MathComposeResult result;
        result.error = "数式の画像を読めません: " + decoded.error;
        return result;
    }
    return composeMathClipRaster(decoded.image, data, outputWidth, outputHeight);
}

} // namespace mvm::app

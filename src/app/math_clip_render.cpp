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

std::optional<math::MathTransformSpec>
mathTransformSpecFor(const project::TimelineTransition& transition,
                     const project::TimelineClip& outgoing, const project::TimelineClip& incoming) {
    if (transition.kind != project::TransitionKind::MathTransform ||
        outgoing.kind != project::TimelineClipKind::Math ||
        incoming.kind != project::TimelineClipKind::Math ||
        outgoing.id != transition.outgoingClipId || incoming.id != transition.incomingClipId)
        return std::nullopt;
    return math::MathTransformSpec{mathRenderSpecFor(outgoing.math),
                                   mathRenderSpecFor(incoming.math),
                                   transition.framesBeforeCut + transition.framesAfterCut};
}

namespace {

const project::TimelineClip* findClip(const project::Project& project, const std::string& id) {
    const auto found =
        std::find_if(project.timelineClips.begin(), project.timelineClips.end(),
                     [&](const project::TimelineClip& clip) { return clip.id == id; });
    return found == project.timelineClips.end() ? nullptr : &*found;
}

} // namespace

std::optional<MathTransformWindow>
mathTransformWindowFor(const project::Project& project,
                       const project::TimelineTransition& transition) {
    if (transition.kind != project::TransitionKind::MathTransform)
        return std::nullopt;
    const auto* outgoing = findClip(project, transition.outgoingClipId);
    if (!outgoing)
        return std::nullopt;
    const auto duration = project::timelineClipDuration(project, *outgoing);
    const std::int64_t frames = transition.framesBeforeCut + transition.framesAfterCut;
    if (!duration.success || frames <= 0)
        return std::nullopt;
    const std::int64_t cut = outgoing->timelineStartFrame + duration.frame;
    return MathTransformWindow{cut - transition.framesBeforeCut, frames};
}

std::int64_t mathTransformFrameAt(const MathTransformWindow& window, std::int64_t timelineFrame) {
    const std::int64_t local = timelineFrame - window.start;
    return local >= 0 && local < window.frames ? local : -1;
}

bool mathTransformIsRendered(const project::Project& project,
                             const project::TimelineTransition& transition) {
    if (transition.kind != project::TransitionKind::MathTransform)
        return false;
    const auto* outgoing = findClip(project, transition.outgoingClipId);
    const auto* incoming = findClip(project, transition.incomingClipId);
    return outgoing && incoming && outgoing->enabled && incoming->enabled &&
           project::isTrackOutputEnabled(project, outgoing->track);
}

bool loadMathCoverage(const std::filesystem::path& png, math::MathCoverage& coverage,
                      std::string& error) {
    const auto decoded = media::loadStaticImage(png);
    if (!decoded.success) {
        error = decoded.error;
        return false;
    }
    const auto& image = decoded.image;
    math::MathCoverage result;
    result.width = image.width;
    result.height = image.height;
    result.alpha.resize(image.rgba.size() / 4);
    for (std::size_t at = 0; at < result.alpha.size(); ++at)
        result.alpha[at] = image.rgba[at * 4 + 3];
    if (!math::mathCoverageValid(result)) {
        error = "数式の画像の大きさが不正です";
        return false;
    }
    coverage = std::move(result);
    return true;
}

bool mathComposeStyleFor(const project::MathClipData& data, math::MathComposeStyle& style) {
    return project::parseArgbColor(data.color, style.colorArgb) &&
           project::parseArgbColor(data.backgroundColor, style.backgroundArgb);
}

std::optional<std::int64_t> mathIntroFrameAt(const project::TimelineClip& clip,
                                             std::int64_t timelineFpsNum,
                                             std::int64_t timelineFpsDen,
                                             std::int64_t clipLocalFrame) {
    // intro が見えるかどうかと境界の丸めは Project の mathIntroSourceFrameAt だけが決める
    // (トランジションの条件・reconcile の mathIntroTimelineFrames と同じ正)。
    const auto shown =
        project::mathIntroSourceFrameAt(clip, timelineFpsNum, timelineFpsDen, clipLocalFrame);
    if (!shown.success)
        return std::nullopt;
    return shown.frame;
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

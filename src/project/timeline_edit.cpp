#include "project/timeline_edit.h"

#include "core/source_frame_mapping.h"
#include "project/subtitles.h"
#include "project/timeline_render.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace mvm::project {
namespace {

__extension__ using WideInteger = __int128;

bool validTextColor(const std::string& color) {
    std::uint32_t argb = 0;
    return parseArgbColor(color, argb);
}

bool validTextData(const Project& project, const TextClipData& text) {
    return !text.content.empty() && !text.fontFamily.empty() && text.fontSize >= 1 &&
           text.fontSize <= project.outputHeight && text.x >= 0 && text.x < project.outputWidth &&
           text.y >= 0 && text.y < project.outputHeight &&
           (text.alignment == "left" || text.alignment == "center" || text.alignment == "right") &&
           text.outlineWidth >= 0 && text.outlineWidth <= 32 && validTextColor(text.color) &&
           validTextColor(text.outlineColor) && validTextColor(text.backgroundColor);
}

bool validIndex(const Project& project, int index) {
    return index >= 0 && index < static_cast<int>(project.timelineClips.size());
}

TimelineFrameResult convertBoundary(std::int64_t frame, std::int64_t fromNum, std::int64_t fromDen,
                                    std::int64_t toNum, std::int64_t toDen, bool roundUp) {
    TimelineFrameResult result;
    if (frame < 0 || fromNum <= 0 || fromDen <= 0 || toNum <= 0 || toDen <= 0) {
        result.error = "frame または timebase が不正です";
        return result;
    }
    const auto converted =
        core::convertFrameBoundary(frame, {fromNum, fromDen}, {toNum, toDen}, roundUp);
    if (!converted) {
        result.error = "frame timebase 変換が overflow しました";
        return result;
    }
    result.success = true;
    result.frame = *converted;
    return result;
}

int indexOfId(const Project& project, const std::string& id) {
    for (std::size_t index = 0; index < project.timelineClips.size(); ++index) {
        if (project.timelineClips[index].id == id)
            return static_cast<int>(index);
    }
    return -1;
}

int indexOfId(const ClipIdIndex* index, const Project& project, const std::string& id) {
    return index ? index->find(id) : indexOfId(project, id);
}

struct TimelineInterval {
    TrackRef track;
    std::int64_t start = 0;
    std::int64_t end = 0;
    const TimelineClip* clip = nullptr;
};

// clip の timeline 区間 [start, end)。duration の失敗はそのまま返す。
bool clipInterval(const Project& project, const TimelineClip& clip, std::int64_t& start,
                  std::int64_t& end, std::string& error) {
    const auto duration = timelineClipDuration(project, clip);
    if (!duration.success) {
        error = clip.name + ": " + duration.error;
        return false;
    }
    if (clip.timelineStartFrame < 0 ||
        clip.timelineStartFrame > std::numeric_limits<std::int64_t>::max() - duration.frame) {
        error = "timeline duration が overflow しました";
        return false;
    }
    start = clip.timelineStartFrame;
    end = start + duration.frame;
    return true;
}

// clip の片端を project frame 単位で動かす。検証は呼び出し側が candidate 全体に対して行う。
// left 端を動かしても右端 (clip の終端) は timeline 上で動かない。
bool trimClipBoundary(const Project& project, TimelineClip& clip, TrimEdge edge,
                      std::int64_t projectFrameDelta, std::string& error) {
    if (projectFrameDelta == std::numeric_limits<std::int64_t>::min()) {
        error = "trim delta の絶対値を表せません";
        return false;
    }
    const auto originalDuration = timelineClipDuration(project, clip);
    if (!originalDuration.success) {
        error = originalDuration.error;
        return false;
    }
    if (hasSyntheticSourceDomain(clip)) {
        const std::int64_t originalStart = clip.timelineStartFrame;
        if (originalStart > std::numeric_limits<std::int64_t>::max() - originalDuration.frame) {
            error = "text / image clip の終端が範囲外です";
            return false;
        }
        const std::int64_t originalEnd = originalStart + originalDuration.frame;
        const WideInteger newStartWide =
            edge == TrimEdge::Left ? static_cast<WideInteger>(originalStart) + projectFrameDelta
                                   : originalStart;
        const WideInteger newEndWide =
            edge == TrimEdge::Right ? static_cast<WideInteger>(originalEnd) + projectFrameDelta
                                    : originalEnd;
        if (newStartWide < 0 || newEndWide <= newStartWide ||
            newEndWide > std::numeric_limits<std::int64_t>::max() ||
            newEndWide - newStartWide > std::numeric_limits<std::int64_t>::max() / 2) {
            error = "text / image clip の trim 範囲が不正です";
            return false;
        }
        const auto newStart = static_cast<std::int64_t>(newStartWide);
        const auto newEnd = static_cast<std::int64_t>(newEndWide);
        // 数式の Write は clip の見えている先頭から始まる (左 trim でも先頭から書き直す)。
        // trim は素材 frame を timeline の fps へ揃え直すので、尺も同じ秒数へ換算し、
        // 縮めた尺に収める。
        auto& animation = clip.mathAnimation;
        if (animation.intro != MathIntroKind::None) {
            if (clip.sourceFpsNum != project.timelineFpsNum ||
                clip.sourceFpsDen != project.timelineFpsDen) {
                const auto retimed = sourceBoundaryToTimelineBoundary(
                    animation.introFrames, clip.sourceFpsNum, clip.sourceFpsDen,
                    project.timelineFpsNum, project.timelineFpsDen);
                if (!retimed.success) {
                    error = retimed.error;
                    return false;
                }
                animation.introFrames = std::max<std::int64_t>(1, retimed.frame);
            }
            animation.introFrames = std::min(animation.introFrames, newEnd - newStart);
        }
        clip.timelineStartFrame = newStart;
        clip.sourceFpsNum = project.timelineFpsNum;
        clip.sourceFpsDen = project.timelineFpsDen;
        clip.sourceInFrame = 0;
        clip.sourceOutFrame = newEnd - newStart;
        clip.sourceFrameCount = clip.sourceOutFrame;
        for (const auto& channel : effectChannels())
            reframeClipKeys(clip.effects.*channel.keys, originalDuration.frame, clip.sourceOutFrame,
                            newStart - originalStart);
        return true;
    }
    const std::int64_t originalStart = clip.timelineStartFrame;
    const std::int64_t original = edge == TrimEdge::Left ? clip.sourceInFrame : clip.sourceOutFrame;
    const auto timelineBoundary = clipSourceBoundaryToTimeline(
        clip, original, project.timelineFpsNum, project.timelineFpsDen);
    if (!timelineBoundary.success ||
        (projectFrameDelta < 0 && timelineBoundary.frame < -projectFrameDelta) ||
        (projectFrameDelta > 0 &&
         timelineBoundary.frame > std::numeric_limits<std::int64_t>::max() - projectFrameDelta)) {
        error = timelineBoundary.success ? "trim delta が範囲外です" : timelineBoundary.error;
        return false;
    }
    const auto sourceBoundary =
        clipTimelineBoundaryToSource(clip, timelineBoundary.frame + projectFrameDelta,
                                     project.timelineFpsNum, project.timelineFpsDen);
    if (!sourceBoundary.success) {
        error = sourceBoundary.error;
        return false;
    }
    if (edge == TrimEdge::Left) {
        clip.sourceInFrame = sourceBoundary.frame;
        const auto newDuration = timelineClipDuration(project, clip);
        if (!newDuration.success) {
            error = newDuration.error;
            return false;
        }
        const std::int64_t startDelta = originalDuration.frame - newDuration.frame;
        if ((startDelta < 0 && originalStart < -startDelta) ||
            (startDelta > 0 &&
             originalStart > std::numeric_limits<std::int64_t>::max() - startDelta)) {
            error = "left trim 後の timeline start が範囲外です";
            return false;
        }
        clip.timelineStartFrame = originalStart + startDelta;
    } else {
        clip.sourceOutFrame = sourceBoundary.frame;
    }
    const auto adjustedDuration = timelineClipDuration(project, clip);
    if (!adjustedDuration.success) {
        error = adjustedDuration.error;
        return false;
    }
    const auto localShift = clip.timelineStartFrame - originalStart;
    for (const auto& channel : effectChannels())
        reframeClipKeys(clip.effects.*channel.keys, originalDuration.frame, adjustedDuration.frame,
                        localShift);
    return true;
}

} // namespace

TimelineFrameResult sourceBoundaryToTimelineBoundary(std::int64_t sourceFrame,
                                                     std::int64_t sourceFpsNum,
                                                     std::int64_t sourceFpsDen,
                                                     std::int64_t timelineFpsNum,
                                                     std::int64_t timelineFpsDen) {
    return convertBoundary(sourceFrame, sourceFpsNum, sourceFpsDen, timelineFpsNum, timelineFpsDen,
                           true);
}

TimelineFrameResult timelineBoundaryToSourceBoundary(std::int64_t timelineFrame,
                                                     std::int64_t sourceFpsNum,
                                                     std::int64_t sourceFpsDen,
                                                     std::int64_t timelineFpsNum,
                                                     std::int64_t timelineFpsDen) {
    return convertBoundary(timelineFrame, timelineFpsNum, timelineFpsDen, sourceFpsNum,
                           sourceFpsDen, false);
}

std::optional<core::FrameRate> clipTimebase(const TimelineClip& clip) {
    return core::multiplyFrameRate({clip.sourceFpsNum, clip.sourceFpsDen},
                                   {clip.speedNum, clip.speedDen});
}

TimelineFrameResult clipSourceBoundaryToTimeline(const TimelineClip& clip, std::int64_t sourceFrame,
                                                 std::int64_t timelineFpsNum,
                                                 std::int64_t timelineFpsDen) {
    const auto timebase = clipTimebase(clip);
    if (!timebase) {
        TimelineFrameResult result;
        result.error = "clip の速度と素材 fps の積を表せません";
        return result;
    }
    return sourceBoundaryToTimelineBoundary(sourceFrame, timebase->num, timebase->den,
                                            timelineFpsNum, timelineFpsDen);
}

TimelineFrameResult clipTimelineBoundaryToSource(const TimelineClip& clip,
                                                 std::int64_t timelineFrame,
                                                 std::int64_t timelineFpsNum,
                                                 std::int64_t timelineFpsDen) {
    const auto timebase = clipTimebase(clip);
    if (!timebase) {
        TimelineFrameResult result;
        result.error = "clip の速度と素材 fps の積を表せません";
        return result;
    }
    return timelineBoundaryToSourceBoundary(timelineFrame, timebase->num, timebase->den,
                                            timelineFpsNum, timelineFpsDen);
}

TimelineFrameResult timelineClipDuration(const Project& project, const TimelineClip& clip) {
    const auto begin = clipSourceBoundaryToTimeline(clip, clip.sourceInFrame,
                                                    project.timelineFpsNum, project.timelineFpsDen);
    if (!begin.success)
        return begin;
    const auto end = clipSourceBoundaryToTimeline(clip, clip.sourceOutFrame, project.timelineFpsNum,
                                                  project.timelineFpsDen);
    if (!end.success)
        return end;
    TimelineFrameResult result;
    if (end.frame <= begin.frame) {
        result.error = "clip の timeline duration が 1 frame 未満です";
        return result;
    }
    result.success = true;
    result.frame = end.frame - begin.frame;
    return result;
}

namespace {
TimelineFrameResult mappedSourceFrameAt(const TimelineClip& clip, std::int64_t timelineFpsNum,
                                        std::int64_t timelineFpsDen, std::int64_t clipLocalFrame) {
    TimelineFrameResult result;
    const auto origin =
        clipSourceBoundaryToTimeline(clip, clip.sourceInFrame, timelineFpsNum, timelineFpsDen);
    if (!origin.success)
        return origin;
    const auto end =
        clipSourceBoundaryToTimeline(clip, clip.sourceOutFrame, timelineFpsNum, timelineFpsDen);
    if (!end.success)
        return end;
    if (clipLocalFrame < 0 || clipLocalFrame >= end.frame - origin.frame) {
        result.error = "clip の範囲外の frame です";
        return result;
    }
    // origin の換算が通っているので timebase は必ずある。
    // Sequence は標本を frame 始点へ置く。四捨五入では可視範囲の exclusive end を
    // 左片が表示しうるため、内部区間と分割の authority に使わない。既存素材は従来どおり。
    const auto source =
        clip.kind == TimelineClipKind::EquationSequence
            ? core::convertFrameBoundary(origin.frame + clipLocalFrame,
                                         {timelineFpsNum, timelineFpsDen}, *clipTimebase(clip),
                                         false)
            : core::sourceFrameAtOutputPosition(origin.frame + clipLocalFrame, *clipTimebase(clip),
                                                {timelineFpsNum, timelineFpsDen});
    if (!source) {
        result.error = "timeline frame を素材 frame へ換算できません";
        return result;
    }
    result.success = true;
    result.frame = std::min(*source, clip.sourceFrameCount - 1);
    return result;
}
} // namespace

TimelineFrameResult clipSourceFrameAt(const TimelineClip& clip, std::int64_t timelineFpsNum,
                                      std::int64_t timelineFpsDen, std::int64_t clipLocalFrame) {
    auto result = mappedSourceFrameAt(clip, timelineFpsNum, timelineFpsDen, clipLocalFrame);
    if (result.success && clip.frameHold)
        result.frame = clip.frameHold->sourceFrame;
    return result;
}

namespace {
// 速度が約分済みの正の有理数で、10%〜1000% の範囲にあるか。clip と保持元の速度で共有する。
bool clipSpeedInRange(std::int64_t speedNum, std::int64_t speedDen) {
    return speedNum > 0 && speedDen > 0 && std::gcd(speedNum, speedDen) == 1 &&
           static_cast<WideInteger>(speedNum) * 100 >=
               static_cast<WideInteger>(speedDen) * kMinClipSpeedPercent &&
           static_cast<WideInteger>(speedNum) * 100 <=
               static_cast<WideInteger>(speedDen) * kMaxClipSpeedPercent;
}
} // namespace

TimelineFrameResult frameHoldProducerPosition(const TimelineClip& clip, std::int64_t timelineFpsNum,
                                              std::int64_t timelineFpsDen) {
    TimelineFrameResult result;
    if (!clip.frameHold) {
        result.error = "フレーム保持がありません";
        return result;
    }
    const auto& hold = *clip.frameHold;
    if (hold.sourceFrame < 0 || hold.sourceFpsNum <= 0 || hold.sourceFpsDen <= 0 ||
        hold.sourceFrameCount <= hold.sourceFrame ||
        !clipSpeedInRange(hold.speedNum, hold.speedDen)) {
        result.error = "保持する素材 frame が不正です";
        return result;
    }
    // 位置は保持元 clip と同じ「速度込みの実効 fps」で数える (書き出しは同じ速度の timewarp で
    // 保持する)。元 clip で表示できた frame は、同じ実効 fps なら必ず出力位置を持つ。
    const auto source = core::multiplyFrameRate({hold.sourceFpsNum, hold.sourceFpsDen},
                                                {hold.speedNum, hold.speedDen});
    if (!source) {
        result.error = "保持元の実効 fps を表せません";
        return result;
    }
    const core::FrameRate output{timelineFpsNum, timelineFpsDen};
    const auto position = core::firstOutputPositionOfSourceFrame(hold.sourceFrame, *source, output);
    const auto reverse =
        position ? core::sourceFrameAtOutputPosition(*position, *source, output) : std::nullopt;
    if (!reverse || *reverse != hold.sourceFrame) {
        result.error = "保持する素材 frame を出力位置へ一意に換算できません";
        return result;
    }
    result.success = true;
    result.frame = *position;
    return result;
}

TimelineClip clipVideoSource(const TimelineClip& clip) {
    TimelineClip source = clip;
    if (source.frameHold) {
        source.sourceFpsNum = source.frameHold->sourceFpsNum;
        source.sourceFpsDen = source.frameHold->sourceFpsDen;
        source.sourceFrameCount = source.frameHold->sourceFrameCount;
        source.sourceInFrame = source.frameHold->sourceFrame;
        source.sourceOutFrame = source.sourceInFrame + 1;
        source.frameHold.reset();
    }
    return source;
}

ClipProducerRange clipProducerRange(const TimelineClip& clip, std::int64_t timelineFpsNum,
                                    std::int64_t timelineFpsDen) {
    ClipProducerRange result;
    const auto begin =
        clipSourceBoundaryToTimeline(clip, clip.sourceInFrame, timelineFpsNum, timelineFpsDen);
    const auto end =
        clipSourceBoundaryToTimeline(clip, clip.sourceOutFrame, timelineFpsNum, timelineFpsDen);
    if (!begin.success || !end.success) {
        result.error = "clip の cut 範囲を換算できません";
        return result;
    }
    const auto beyondSource = core::firstOutputPositionOfSourceFrame(
        clip.sourceFrameCount, *clipTimebase(clip), {timelineFpsNum, timelineFpsDen});
    if (!beyondSource) {
        result.error = "clip の cut 範囲を換算できません";
        return result;
    }
    result.begin = begin.frame;
    result.end = std::min(end.frame, *beyondSource);
    result.tailFrames = end.frame - result.end;
    if (result.end <= result.begin) {
        result.error = "clip の cut 範囲が 1 frame 未満です";
        return result;
    }
    result.success = true;
    return result;
}

TimelineFrameResult clipFadeSourceFrameAt(const TimelineClip& clip, std::int64_t timelineFpsNum,
                                          std::int64_t timelineFpsDen,
                                          std::int64_t clipLocalFrame) {
    auto result = mappedSourceFrameAt(clip, timelineFpsNum, timelineFpsDen, clipLocalFrame);
    if (result.success)
        result.frame = std::clamp(result.frame - clip.sourceInFrame, std::int64_t{0},
                                  clip.sourceOutFrame - clip.sourceInFrame - 1);
    return result;
}

TimelineFrameResult mathIntroSourceFrameAt(const TimelineClip& clip, std::int64_t timelineFpsNum,
                                           std::int64_t timelineFpsDen,
                                           std::int64_t clipLocalFrame) {
    TimelineFrameResult result;
    if (clip.kind != TimelineClipKind::Math || clip.mathAnimation.intro == MathIntroKind::None) {
        result.success = true;
        result.frame = -1;
        return result;
    }
    result = clipFadeSourceFrameAt(clip, timelineFpsNum, timelineFpsDen,
                                   std::max<std::int64_t>(0, clipLocalFrame));
    if (result.success && result.frame >= clip.mathAnimation.introFrames)
        result.frame = -1;
    return result;
}

TimelineFrameResult mathIntroTimelineFrames(const Project& project, const TimelineClip& clip) {
    TimelineFrameResult result;
    if (clip.kind != TimelineClipKind::Math || clip.mathAnimation.intro == MathIntroKind::None) {
        result.success = true;
        return result;
    }
    const auto duration = timelineClipDuration(project, clip);
    if (!duration.success)
        return duration;
    // intro の素材 local frame は timeline local frame に対して単調なので、intro が見えなくなる
    // 最初の frame を二分探索で求める。判定は mathIntroSourceFrameAt だけで行う。
    std::int64_t low = 0;
    std::int64_t high = duration.frame;
    while (low < high) {
        const std::int64_t middle = low + (high - low) / 2;
        const auto shown =
            mathIntroSourceFrameAt(clip, project.timelineFpsNum, project.timelineFpsDen, middle);
        if (!shown.success)
            return shown;
        if (shown.frame < 0)
            high = middle;
        else
            low = middle + 1;
    }
    result.success = true;
    result.frame = low;
    return result;
}

bool sourceRateMatchesTimelineRate(const Project& project, const TimelineClip& clip) {
    // 速度込みの実効 fps で比べる。等速でない clip は timeline frame と素材 frame が 1:1
    // にならない。
    const auto timebase = clipTimebase(clip);
    if (!timebase || project.timelineFpsNum <= 0 || project.timelineFpsDen <= 0)
        return false;
    const auto timelineDivisor = std::gcd(project.timelineFpsNum, project.timelineFpsDen);
    return timebase->num == project.timelineFpsNum / timelineDivisor &&
           timebase->den == project.timelineFpsDen / timelineDivisor;
}

namespace {

bool edgeRange(const Project& project, const TimelineClip& clip, TrimEdge edge, std::int64_t& lower,
               std::int64_t& upper, std::string& error);

// トランジションが参照する 2 clip。cut と、トランジションに使える素材の余白・clip の尺
// (いずれも timeline frame)。
struct TransitionClips {
    int outgoing = -1;
    int incoming = -1;
    std::int64_t cut = 0;
    std::int64_t outgoingDuration = 0;
    std::int64_t incomingDuration = 0;
    std::int64_t headHandle = 0; // incoming の先頭より前に使える素材
    std::int64_t tailHandle = 0; // outgoing の終端より後に使える素材
};

// clipIndex は省略できる (null なら clip 列を走査する)。多数のトランジションを解決するときに渡す。
bool resolveTransitionClips(const Project& project, const TimelineTransition& transition,
                            TransitionClips& clips, std::string& error,
                            const ClipIdIndex* clipIndex = nullptr) {
    clips.outgoing = indexOfId(clipIndex, project, transition.outgoingClipId);
    clips.incoming = indexOfId(clipIndex, project, transition.incomingClipId);
    if (!validIndex(project, clips.outgoing) || !validIndex(project, clips.incoming) ||
        clips.outgoing == clips.incoming) {
        error = "トランジションの clip がありません: " + transition.id;
        return false;
    }
    const auto& outgoing = project.timelineClips[static_cast<std::size_t>(clips.outgoing)];
    const auto& incoming = project.timelineClips[static_cast<std::size_t>(clips.incoming)];
    if (!(outgoing.track == incoming.track)) {
        error = "トランジションの clip が同じ track にありません: " + transition.id;
        return false;
    }
    if (outgoing.frameHold || incoming.frameHold) {
        error = "フレーム保持 clip にはトランジションを置けません: " + transition.id;
        return false;
    }
    std::int64_t outgoingStart = 0;
    std::int64_t outgoingEnd = 0;
    std::int64_t incomingStart = 0;
    std::int64_t incomingEnd = 0;
    if (!clipInterval(project, outgoing, outgoingStart, outgoingEnd, error) ||
        !clipInterval(project, incoming, incomingStart, incomingEnd, error))
        return false;
    if (outgoingEnd != incomingStart) {
        error = "トランジションの clip が接していません: " + transition.id;
        return false;
    }
    std::int64_t lower = 0;
    std::int64_t upper = 0;
    if (!edgeRange(project, incoming, TrimEdge::Left, lower, upper, error))
        return false;
    clips.headHandle = -lower;
    if (!edgeRange(project, outgoing, TrimEdge::Right, lower, upper, error))
        return false;
    clips.tailHandle = upper;
    clips.cut = outgoingEnd;
    clips.outgoingDuration = outgoingEnd - outgoingStart;
    clips.incomingDuration = incomingEnd - incomingStart;
    return true;
}

// クロスディゾルブは outgoing を不透明のまま残し、incoming を「素材の余白を黒で埋めた出力全体の
// 1 枚」として不透明度 p で上に重ねて作る (timeline_render.h)。素材の縦横比が出力と違って出る
// 余白はこれで A(1 - p) + B p になる。一方、incoming が透過・縮小・移動・切り抜き・回転されて
// いると、その形を出力全体の黒で囲むことになり Premiere の見た目 (下の track が見える) と違う。
// 各 clip を別々に描いてから混ぜる合成 (compositor の dissolve) を持つまでは、これらの効果と
// 区間の中の不透明度 1 未満は置かせない。 localBegin..localEnd は元の clip の local frame
// (延ばした区間は端の値で評価する)。
const char* const kDissolveRequirement =
    "クロスディゾルブは画面全体を覆う不透明な映像 clip どうしでだけ使えます "
    "(文字・画像は不可。位置・拡大・回転・切り抜き・不透明度を既定値に戻してください): ";

// 長さに依らない条件 (種別・位置・拡大・回転・切り抜き)。
bool dissolveClipShapeEligible(const TimelineClip& clip, std::string& error) {
    const auto& effects = clip.effects;
    if ((clip.kind != TimelineClipKind::Video && clip.kind != TimelineClipKind::Manim) ||
        clip.frameHold || effects.positionXPercent != 0.0 || effects.positionYPercent != 0.0 ||
        effects.scaleXPercent != 100.0 || effects.scaleYPercent != 100.0 ||
        effects.rotationDegrees != 0.0 || effects.cropLeftPercent != 0.0 ||
        effects.cropTopPercent != 0.0 || effects.cropRightPercent != 0.0 ||
        effects.cropBottomPercent != 0.0) {
        error = kDissolveRequirement + clip.name;
        return false;
    }
    return true;
}

// local frame [localBegin, localEnd) で不透明度 (値・key・fade) が 1 か。
bool dissolveClipOpaqueOver(const Project& project, const TimelineClip& clip,
                            std::int64_t localBegin, std::int64_t localEnd, std::string& error) {
    for (std::int64_t local = localBegin; local < localEnd; ++local) {
        const auto sourceLocal =
            clipFadeSourceFrameAt(clip, project.timelineFpsNum, project.timelineFpsDen, local);
        if (!sourceLocal.success) {
            error = sourceLocal.error;
            return false;
        }
        const auto evaluated = evaluateClipEffects(clip.effects, local);
        if (evaluated.positionXPercent != 0 || evaluated.positionYPercent != 0 ||
            evaluated.scaleXPercent != 100 || evaluated.scaleYPercent != 100 ||
            evaluated.rotationDegrees != 0 || evaluated.cropLeftPercent != 0 ||
            evaluated.cropRightPercent != 0 || evaluated.cropTopPercent != 0 ||
            evaluated.cropBottomPercent != 0) {
            error = kDissolveRequirement + clip.name;
            return false;
        }
        if (evaluateClipOpacity(clip.effects, local, sourceLocal.frame,
                                clip.sourceOutFrame - clip.sourceInFrame) < 1.0) {
            error = kDissolveRequirement + clip.name;
            return false;
        }
    }
    return true;
}

bool dissolveClipCoversOpaque(const Project& project, const TimelineClip& clip,
                              std::int64_t localBegin, std::int64_t localEnd, std::string& error) {
    return dissolveClipShapeEligible(clip, error) &&
           dissolveClipOpaqueOver(project, clip, localBegin, localEnd, error);
}

// 数式 clip の先頭の Write が占める timeline frame 数 (Write の無い clip は 0)。境界は
// preview・書き出しと同じ mathIntroTimelineFrames が決める (丸めをここで持たない)。
bool mathWriteTimelineFrames(const Project& project, const TimelineClip& clip, std::int64_t& frames,
                             std::string& error) {
    const auto write = mathIntroTimelineFrames(project, clip);
    if (!write.success) {
        error = clip.name + ": " + write.error;
        return false;
    }
    frames = write.frame;
    return true;
}

// clip の先頭側で既に使われている frame 数 (先頭のトランジションが内側に使う分と Write の
// 大きい方)。末尾側のトランジションは、この内側には置けない。Write は数式 clip だけが
// 持つので、数式 clip に置けない Blend の上限は変わらない。
bool outgoingHeadUsage(const Project& project, const TimelineClip& clip,
                       std::int64_t headTransitionInside, std::int64_t& usage, std::string& error) {
    std::int64_t write = 0;
    if (!mathWriteTimelineFrames(project, clip, write, error))
        return false;
    usage = std::max(headTransitionInside, write);
    return true;
}

// 数式の変形の区間で揃える見た目 (評価した ClipEffects の位置・拡大・回転・切り抜き・不透明度)。
// 数式は出力全面の raster に合成してから ClipEffects を掛けるので、両端の clip でこれが一定かつ
// 等しければ、変形の画素は両端の静止と同じ変換を受け、区間の境で段差が出ない。
using MathTransformLook = std::array<double, 10>;

bool mathTransformLookAt(const Project& project, const TimelineClip& clip, std::int64_t local,
                         MathTransformLook& look, std::string& error) {
    const auto sourceLocal =
        clipFadeSourceFrameAt(clip, project.timelineFpsNum, project.timelineFpsDen, local);
    if (!sourceLocal.success) {
        error = clip.name + ": " + sourceLocal.error;
        return false;
    }
    const auto evaluated = evaluateClipEffects(clip.effects, local);
    look = {evaluated.positionXPercent,
            evaluated.positionYPercent,
            evaluated.scaleXPercent,
            evaluated.scaleYPercent,
            evaluated.rotationDegrees,
            evaluated.cropLeftPercent,
            evaluated.cropTopPercent,
            evaluated.cropRightPercent,
            evaluated.cropBottomPercent,
            evaluateClipOpacity(clip.effects, local, sourceLocal.frame,
                                clip.sourceOutFrame - clip.sourceInFrame)};
    return true;
}

const char* const kMathTransformLookRequirement =
    "数式の変形の区間では、両端の数式 clip の位置・拡大・回転・切り抜き・不透明度を一定にして"
    "揃えてください: ";

// 長さに依らない条件。両端が数式 clip、incoming に Write が無い、両方の背景が透明、cut の両側の
// frame (outgoing の最後と incoming の最初) で見た目が等しい。reference には揃える見た目を返す。
// 描けるかどうか (式・backend) は見ない。
bool mathTransformClipsEligible(const Project& project, const TransitionClips& clips,
                                MathTransformLook& reference, std::string& error) {
    const auto& outgoing = project.timelineClips[static_cast<std::size_t>(clips.outgoing)];
    const auto& incoming = project.timelineClips[static_cast<std::size_t>(clips.incoming)];
    if (outgoing.kind != TimelineClipKind::Math || incoming.kind != TimelineClipKind::Math) {
        error = "数式の変形は隣り合う数式 clip どうしにだけ置けます: " +
                (outgoing.kind != TimelineClipKind::Math ? outgoing.name : incoming.name);
        return false;
    }
    // 変形は incoming の先頭を使うので、P2 では incoming の Write と両立させない。
    if (incoming.mathAnimation.intro != MathIntroKind::None) {
        error = "数式の変形の後ろの数式 clip には Write を付けられません: " + incoming.name;
        return false;
    }
    // 背景の矩形は式の大きさで変わり、変形の端で段差になる (矩形の補間は未対応)。
    for (const auto* clip : {&outgoing, &incoming}) {
        std::uint32_t background = 0;
        if (!parseArgbColor(clip->math.backgroundColor, background) || (background >> 24) != 0) {
            error = "数式の変形の両端の数式 clip は背景を透明にしてください: " + clip->name;
            return false;
        }
    }
    MathTransformLook incomingLook{};
    if (!mathTransformLookAt(project, outgoing, clips.outgoingDuration - 1, reference, error) ||
        !mathTransformLookAt(project, incoming, 0, incomingLook, error))
        return false;
    if (reference != incomingLook) {
        error = kMathTransformLookRequirement + incoming.name;
        return false;
    }
    return true;
}

// clip の local frame [localBegin, localEnd) で見た目が reference と等しいか。
bool mathTransformLookConstantOver(const Project& project, const TimelineClip& clip,
                                   std::int64_t localBegin, std::int64_t localEnd,
                                   const MathTransformLook& reference, std::string& error) {
    for (std::int64_t local = localBegin; local < localEnd; ++local) {
        MathTransformLook look{};
        if (!mathTransformLookAt(project, clip, local, look, error))
            return false;
        if (look != reference) {
            error = kMathTransformLookRequirement + clip.name;
            return false;
        }
    }
    return true;
}

// 長さを含めた数式の変形の条件。区間 [cut - before, cut + after) で見た目が一定であることと、
// outgoing の Write が区間に掛からないこと (Write の後でだけ変形を始める)。
bool mathTransformClipsFit(const Project& project, const TransitionClips& clips,
                           std::int64_t before, std::int64_t after, std::string& error) {
    MathTransformLook reference{};
    if (!mathTransformClipsEligible(project, clips, reference, error))
        return false;
    const auto& outgoing = project.timelineClips[static_cast<std::size_t>(clips.outgoing)];
    const auto& incoming = project.timelineClips[static_cast<std::size_t>(clips.incoming)];
    std::int64_t write = 0;
    if (!mathWriteTimelineFrames(project, outgoing, write, error))
        return false;
    if (before > clips.outgoingDuration - write) {
        error = "数式の変形の区間が前の数式 clip の Write と重なっています: " + outgoing.name;
        return false;
    }
    return mathTransformLookConstantOver(project, outgoing, clips.outgoingDuration - before,
                                         clips.outgoingDuration, reference, error) &&
           mathTransformLookConstantOver(project, incoming, 0, after, reference, error);
}

bool dissolveClipsEligible(const Project& project, const TransitionClips& clips,
                           std::int64_t before, std::int64_t after, std::string& error) {
    const auto& outgoing = project.timelineClips[static_cast<std::size_t>(clips.outgoing)];
    const auto& incoming = project.timelineClips[static_cast<std::size_t>(clips.incoming)];
    if (outgoing.track.kind != TrackKind::Video)
        return true;
    // 延ばした区間は端の frame の値なので、端の frame は必ず含める。
    const auto outgoingBegin = std::max<std::int64_t>(
        0, std::min(clips.outgoingDuration - before, clips.outgoingDuration - 1));
    return dissolveClipCoversOpaque(project, outgoing, outgoingBegin, clips.outgoingDuration,
                                    error) &&
           dissolveClipCoversOpaque(
               project, incoming, 0,
               std::min(std::max<std::int64_t>(after, 1), clips.incomingDuration), error);
}

bool validateTimelineTransitions(const Project& project, std::string& error) {
    std::unordered_set<std::string> ids;
    // clip ID -> その clip の内側に入るトランジションの frame 数 (先頭側 / 終端側)。
    std::unordered_map<std::string, std::int64_t> headInside;
    std::unordered_map<std::string, std::int64_t> tailInside;
    std::unordered_map<std::string, std::int64_t> durations;
    const ClipIdIndex clipIndex(project);
    for (const auto& transition : project.timelineTransitions) {
        if (transition.id.empty() || !ids.insert(transition.id).second) {
            error = "トランジション ID が空または重複しています";
            return false;
        }
        TransitionClips clips;
        if (!resolveTransitionClips(project, transition, clips, error, &clipIndex))
            return false;
        const auto before = transition.framesBeforeCut;
        const auto after = transition.framesAfterCut;
        if (before < 0 || after < 0 || before > std::numeric_limits<std::int64_t>::max() - after ||
            before + after < 1) {
            error = "トランジションの長さが不正です: " + transition.id;
            return false;
        }
        if (before > clips.headHandle || after > clips.tailHandle) {
            error = "トランジションに使う素材の余白が足りません: " + transition.id;
            return false;
        }
        if (before > clips.outgoingDuration || after > clips.incomingDuration) {
            error = "トランジションが clip の尺を超えています: " + transition.id;
            return false;
        }
        const auto& outgoing = project.timelineClips[static_cast<std::size_t>(clips.outgoing)];
        const auto& incoming = project.timelineClips[static_cast<std::size_t>(clips.incoming)];
        if (!tailInside.emplace(outgoing.id, before).second ||
            !headInside.emplace(incoming.id, after).second) {
            error = "同じ clip 端に複数のトランジションがあります: " + transition.id;
            return false;
        }
        // 同じ clip 端の減衰を 2 通りに表さない (掛け合わさって二重に暗くなる)。
        if (outgoing.effects.fadeOutFrames != 0 || incoming.effects.fadeInFrames != 0) {
            error = "トランジションのある clip 端にはフェードを設定できません: " + transition.id;
            return false;
        }
        if (transition.kind == TransitionKind::MathTransform
                ? !mathTransformClipsFit(project, clips, before, after, error)
                : !dissolveClipsEligible(project, clips, before, after, error))
            return false;
        durations[outgoing.id] = clips.outgoingDuration;
        durations[incoming.id] = clips.incomingDuration;
    }
    for (const auto& [clipId, head] : headInside) {
        const auto tail = tailInside.find(clipId);
        if (tail != tailInside.end() && head > durations[clipId] - tail->second) {
            error = "clip の前後のトランジションが重なっています";
            return false;
        }
    }
    return true;
}

} // namespace

void reconcileTimelineTransitions(Project& candidate) {
    struct Entry {
        std::size_t index = 0;
        TransitionClips clips;
    };

    std::vector<Entry> entries;
    const ClipIdIndex clipIndex(candidate);
    for (std::size_t index = 0; index < candidate.timelineTransitions.size(); ++index) {
        Entry entry{index, {}};
        std::string ignored;
        if (resolveTransitionClips(candidate, candidate.timelineTransitions[index], entry.clips,
                                   ignored, &clipIndex))
            entries.push_back(entry);
    }
    std::stable_sort(entries.begin(), entries.end(), [&](const Entry& left, const Entry& right) {
        const auto& leftTrack =
            candidate.timelineClips[static_cast<std::size_t>(left.clips.outgoing)].track;
        const auto& rightTrack =
            candidate.timelineClips[static_cast<std::size_t>(right.clips.outgoing)].track;
        return std::tuple(static_cast<int>(leftTrack.kind), leftTrack.index, left.clips.cut) <
               std::tuple(static_cast<int>(rightTrack.kind), rightTrack.index, right.clips.cut);
    });
    std::vector<bool> keep(candidate.timelineTransitions.size(), false);
    std::unordered_set<int> outgoingUsed;
    std::unordered_set<int> incomingUsed;
    // clip index -> 先頭側のトランジションが clip の内側に使う frame 数。
    std::unordered_map<int, std::int64_t> headInside;
    for (const auto& entry : entries) {
        auto& transition = candidate.timelineTransitions[entry.index];
        const auto& clips = entry.clips;
        if (outgoingUsed.contains(clips.outgoing) || incomingUsed.contains(clips.incoming))
            continue;
        // 先頭のトランジションに加えて、数式 clip の Write も先頭側を使う (Write の後でだけ変形を
        // 始める)。換算できなければ縮められないので、そのまま残して検証に理由を出させる。
        std::int64_t usedHead = 0;
        std::string usageError;
        if (!outgoingHeadUsage(candidate,
                               candidate.timelineClips[static_cast<std::size_t>(clips.outgoing)],
                               headInside.contains(clips.outgoing) ? headInside[clips.outgoing] : 0,
                               usedHead, usageError)) {
            keep[entry.index] = true;
            continue;
        }
        const auto maxBefore = std::max<std::int64_t>(
            0, std::min(clips.headHandle, clips.outgoingDuration - usedHead));
        const auto maxAfter =
            std::max<std::int64_t>(0, std::min(clips.tailHandle, clips.incomingDuration));
        transition.framesBeforeCut =
            std::clamp(transition.framesBeforeCut, std::int64_t{0}, maxBefore);
        transition.framesAfterCut =
            std::clamp(transition.framesAfterCut, std::int64_t{0}, maxAfter);
        if (transition.framesBeforeCut + transition.framesAfterCut < 1)
            continue;
        outgoingUsed.insert(clips.outgoing);
        incomingUsed.insert(clips.incoming);
        headInside[clips.incoming] = transition.framesAfterCut;
        keep[entry.index] = true;
    }
    std::vector<TimelineTransition> kept;
    for (std::size_t index = 0; index < keep.size(); ++index) {
        if (keep[index])
            kept.push_back(std::move(candidate.timelineTransitions[index]));
    }
    candidate.timelineTransitions = std::move(kept);
}

ClipIdIndex::ClipIdIndex(const Project& project) {
    indices_.reserve(project.timelineClips.size());
    for (std::size_t index = 0; index < project.timelineClips.size(); ++index)
        indices_.emplace(project.timelineClips[index].id, static_cast<int>(index));
}

int ClipIdIndex::find(std::string_view id) const {
    const auto found = indices_.find(id);
    return found == indices_.end() ? -1 : found->second;
}

TimelineValidationResult finalizeTimelineCandidate(Project& candidate) {
    reconcileTimelineTransitions(candidate);
    reconcileSubtitleLinks(candidate);
    return validateTimeline(candidate);
}

TimelineValidationResult validateTimeline(const Project& project) {
    TimelineValidationResult result;
    if (project.schemaVersion != kProjectSchemaVersion) {
        result.error = "Project schema_version が " + std::to_string(kProjectSchemaVersion) +
                       " ではありません";
        return result;
    }
    if (!isConfigurableTimelineFrameRate(project.timelineFpsNum, project.timelineFpsDen)) {
        result.error = "Project timeline FPS が対応外です";
        return result;
    }
    // 永続化された fps は約分済みの 1 つの表現だけを authority にする。
    // 120/2 と 60/1 が両方存在すると、UI の一致判定も比較も二重定義になる。
    if (!isCanonicalFrameRate(project.timelineFpsNum, project.timelineFpsDen)) {
        result.error = "Project timeline FPS が約分されていません";
        return result;
    }
    if (!isValidProjectOutputSize(project.outputWidth, project.outputHeight)) {
        result.error = "Project output size が不正です";
        return result;
    }
    if ((project.inFrame &&
         (*project.inFrame < 0 || *project.inFrame == std::numeric_limits<std::int64_t>::max())) ||
        (project.outFrame && (*project.outFrame < 0 ||
                              *project.outFrame == std::numeric_limits<std::int64_t>::max())) ||
        (project.inFrame && project.outFrame && *project.inFrame >= *project.outFrame)) {
        result.error = "イン・アウトの範囲が不正です";
        return result;
    }
    std::int64_t previousMarker = -1;
    for (const auto marker : project.timelineMarkers) {
        if (marker <= previousMarker || marker == std::numeric_limits<std::int64_t>::max()) {
            result.error = "マーカーは重複せず昇順である必要があります";
            return result;
        }
        previousMarker = marker;
    }
    if (project.videoTracks.empty()) {
        result.error = "video track が 1 本もありません";
        return result;
    }
    std::unordered_set<std::string> trackNames;
    for (const auto kind : {TrackKind::Video, TrackKind::Audio}) {
        for (const auto& track : tracksOfKind(project, kind)) {
            if (track.name.empty() || !trackNames.insert(track.name).second) {
                result.error = "track 名が空または重複しています";
                return result;
            }
        }
    }
    // solo は audio だけが持つ。保存 (serialize も validateTimeline を通る) と読み込みの両方で
    // 止め、自分で書いたファイルを自分で読めない状態を作らない。
    for (const auto& track : project.videoTracks) {
        if (track.solo) {
            result.error = "video track は solo を持てません";
            return result;
        }
        if (!track.mixerName.empty() || track.mixerGainDb != 0.0 || track.mixerPan != 0.0) {
            result.error = "video track はミキサー設定を持てません";
            return result;
        }
    }
    for (const auto& track : project.audioTracks) {
        if (!isValidAudioMix(track.mixerGainDb, track.mixerPan)) {
            result.error = "トラック音量またはパンが範囲外です";
            return result;
        }
    }
    std::unordered_set<std::string> ids;

    struct LinkGroupSummary {
        int count = 0;
        bool hasVideo = false;
        bool hasAudio = false;
    };

    std::unordered_map<std::string, LinkGroupSummary> linkGroups;
    std::vector<TimelineInterval> intervals;
    intervals.reserve(project.timelineClips.size());
    std::int64_t totalEnd = 0;
    for (const auto& clip : project.timelineClips) {
        if (clip.id.empty() || !ids.insert(clip.id).second) {
            result.error = "timeline clip ID が空または重複しています";
            return result;
        }
        if ((clipKindHasMediaPath(clip.kind) == clip.mediaPath.empty()) || clip.name.empty()) {
            result.error = "timeline clip の path または name が空です";
            return result;
        }
        if ((clip.kind == TimelineClipKind::Text && !validTextData(project, clip.text)) ||
            (clip.kind != TimelineClipKind::Text && clip.text != TextClipData{})) {
            result.error = "text clip のデータが不正です: " + clip.name;
            return result;
        }
        if (clip.kind == TimelineClipKind::Math) {
            std::string mathError;
            if (!validateMathClipData(clip.math, project.outputHeight, mathError)) {
                result.error = "数式 clip のデータが不正です (" + mathError + "): " + clip.name;
                return result;
            }
        } else if (clip.math != MathClipData{}) {
            result.error = "数式 clip 以外が数式のデータを持っています: " + clip.name;
            return result;
        }
        if (clip.kind == TimelineClipKind::EquationSequence) {
            std::vector<EquationInterval> equationTimeline;
            std::int64_t length = 0;
            if (!validateEquationSequence(clip.equationSequence, project.outputHeight,
                                          result.error) ||
                !equationIntervals(clip.equationSequence, equationTimeline, length, result.error))
                return result;
            if (length != clip.sourceFrameCount || clip.speedNum != 1 || clip.speedDen != 1 ||
                clip.preservePitch || clip.frameHold || !clip.linkGroupId.empty()) {
                result.error = "数式 sequence の素材尺・速度・リンクが不正です";
                return result;
            }
        } else if (clip.equationSequence != EquationSequenceClipData{}) {
            result.error = "数式 sequence 以外が sequence データを持っています";
            return result;
        }
        if (clip.kind == TimelineClipKind::Math) {
            std::string animationError;
            if (!validateMathClipAnimation(
                    clip.mathAnimation, clip.sourceOutFrame - clip.sourceInFrame, animationError)) {
                result.error =
                    "数式 clip の animation が不正です (" + animationError + "): " + clip.name;
                return result;
            }
        } else if (clip.mathAnimation != MathClipAnimation{}) {
            result.error = "数式 clip 以外が数式の animation を持っています: " + clip.name;
            return result;
        }
        if (isStillClipKind(clip.kind) &&
            (clip.speedNum != 1 || clip.speedDen != 1 || !clip.linkGroupId.empty() ||
             clip.preservePitch || clip.frameHold)) {
            result.error = "text / image / math clip の速度またはリンクが不正です: " + clip.name;
            return result;
        }
        // 素材の時間軸を持たないので、素材 frame domain は in = 0・out = 尺。
        // fps は置いたときの timeline の値で、timeline の fps を変えても振り直さない
        // (尺は clip の fps で換算される)。trim すると現在の timeline の fps へ揃う。
        if (hasSyntheticSourceDomain(clip) &&
            (clip.sourceInFrame != 0 || clip.sourceOutFrame != clip.sourceFrameCount)) {
            result.error = "text / image / math clip の素材範囲が尺と一致しません: " + clip.name;
            return result;
        }
        if (clip.sourceFpsNum <= 0 || clip.sourceFpsDen <= 0 || clip.sourceFrameCount <= 0 ||
            clip.sourceInFrame < 0 || clip.sourceOutFrame <= clip.sourceInFrame ||
            clip.sourceOutFrame > clip.sourceFrameCount) {
            result.error = "timeline clip の source range または FPS が不正です: " + clip.name;
            return result;
        }
        if (!clipSpeedInRange(clip.speedNum, clip.speedDen) || !clipTimebase(clip)) {
            result.error = "timeline clip の速度が不正です (約分済みの " +
                           std::to_string(kMinClipSpeedPercent) + "%〜" +
                           std::to_string(kMaxClipSpeedPercent) + "%): " + clip.name;
            return result;
        }
        // 等速のピッチ保持は preview だけが stretcher を通し書き出しは通さない、という食い違いの
        // 元になるので持たせない (速度を変える編集は等速へ戻すときに落とす)。
        if (clip.preservePitch && clip.speedNum == clip.speedDen) {
            result.error = "等速の clip はピッチ保持を持てません: " + clip.name;
            return result;
        }
        if (clip.frameHold &&
            (clip.kind != TimelineClipKind::Video || clip.speedNum != 1 || clip.speedDen != 1 ||
             !clip.linkGroupId.empty() || clip.preservePitch ||
             !frameHoldProducerPosition(clip, project.timelineFpsNum, project.timelineFpsDen)
                  .success)) {
            result.error = "フレーム保持 clip が不正です: " + clip.name;
            return result;
        }
        std::string effectsError;
        if (!validateClipEffects(clip.effects, clip.sourceOutFrame - clip.sourceInFrame,
                                 effectsError)) {
            result.error = clip.name + ": " + effectsError;
            return result;
        }
        if (!isValidTrackRef(project, clip.track)) {
            result.error = "timeline clip の track が存在しません: " + clip.name;
            return result;
        }
        if (!clipKindFitsTrackKind(clip.kind, clip.track.kind)) {
            result.error = "timeline clip の種別と track 種別が一致しません: " + clip.name;
            return result;
        }
        const auto duration = timelineClipDuration(project, clip);
        if (!duration.success ||
            !validateEffectKeys(clip.effects, duration.frame, clip.kind == TimelineClipKind::Audio,
                                effectsError) ||
            (clip.kind == TimelineClipKind::Audio &&
             (!clip.effects.opacityKeys.empty() || clip.effects.opacityPercent != 100.0)) ||
            (clip.kind != TimelineClipKind::Audio &&
             (!clip.effects.volumeKeys.empty() || clip.effects.volumePercent != 100.0 ||
              clip.effects.normalizationGainDb != 0 || clip.effects.duckingDb != 0 ||
              !clip.effects.duckingKeys.empty() || !clip.effects.audioAdjustmentSettings.empty() ||
              !clip.effects.audioAdjustmentFingerprint.empty()))) {
            result.error = clip.name + ": キーフレームが clip の種別または尺に合いません";
            return result;
        }
        if (clip.timelineStartFrame < 0) {
            result.error = "timeline clip の start frame が負です: " + clip.name;
            return result;
        }
        if (!clip.linkGroupId.empty()) {
            auto& group = linkGroups[clip.linkGroupId];
            ++group.count;
            group.hasAudio = group.hasAudio || clip.kind == TimelineClipKind::Audio;
            group.hasVideo = group.hasVideo || clip.kind != TimelineClipKind::Audio;
            if (group.count > 2 || (group.count == 2 && (!group.hasAudio || !group.hasVideo))) {
                result.error =
                    "clipリンクはvideo/audioの1組である必要があります: " + clip.linkGroupId;
                return result;
            }
        }
        std::int64_t start = 0;
        std::int64_t end = 0;
        if (!clipInterval(project, clip, start, end, result.error))
            return result;
        intervals.push_back({clip.track, start, end, &clip});
        totalEnd = std::max(totalEnd, end);
    }
    // 同じ track の重なり。track ごとに開始位置で並べ、それまでの区間の終端の最大とだけ
    // 比べる (全組を比べると clip 数の 2 乗になり、大きな timeline の編集が毎回重くなる)。
    // 区間は空でないので、開始が終端の最大より前なら、その終端を持つ区間と重なっている。
    std::sort(intervals.begin(), intervals.end(),
              [](const TimelineInterval& left, const TimelineInterval& right) {
                  return std::tuple(left.track.kind, left.track.index, left.start, left.clip) <
                         std::tuple(right.track.kind, right.track.index, right.start, right.clip);
              });
    const TimelineInterval* reach = nullptr; // 同じ track で終端が最大の区間
    for (const auto& interval : intervals) {
        if (!reach || !(reach->track == interval.track)) {
            reach = &interval;
            continue;
        }
        ++result.overlapComparisons;
        if (interval.start < reach->end) {
            // timelineClips の並びで前にある clip を先に書く。
            const auto* first = std::min(reach->clip, interval.clip);
            const auto* second = std::max(reach->clip, interval.clip);
            result.error = "同じ track の timeline clip が重複しています: " + first->name + " / " +
                           second->name;
            return result;
        }
        if (interval.end > reach->end)
            reach = &interval;
    }
    for (const auto& [linkGroupId, group] : linkGroups) {
        if (group.count != 2 || !group.hasAudio || !group.hasVideo) {
            result.error = "clipリンクはvideo/audioの1組である必要があります: " + linkGroupId;
            return result;
        }
    }
    if (!validateSubtitles(project, result.error))
        return result;
    if (project.subtitles && !project.subtitles->cues.empty())
        totalEnd = std::max(totalEnd, project.subtitles->cues.back().endFrame);
    if (!validateTimelineTransitions(project, result.error))
        return result;
    result.success = true;
    result.totalFrames = totalEnd;
    return result;
}

int timelineClipIndexAt(const Project& project, TrackRef track, std::int64_t timelineFrame) {
    const TimelineClip* clip = activeClipAt(project, track, timelineFrame);
    return clip ? static_cast<int>(clip - project.timelineClips.data()) : -1;
}

const TimelineClip* activeClipAt(const Project& project, TrackRef track,
                                 std::int64_t timelineFrame) {
    if (!isValidTrackRef(project, track) || timelineFrame < 0)
        return nullptr;
    for (const auto& clip : project.timelineClips) {
        if (!(clip.track == track))
            continue;
        std::int64_t start = 0;
        std::int64_t end = 0;
        std::string ignored;
        if (!clipInterval(project, clip, start, end, ignored))
            continue;
        if (timelineFrame >= start && timelineFrame < end)
            return &clip;
    }
    return nullptr;
}

std::vector<const TimelineClip*> activeClipsAt(const Project& project, TrackKind kind,
                                               std::int64_t timelineFrame) {
    const auto& tracks = tracksOfKind(project, kind);
    std::vector<const TimelineClip*> active(tracks.size(), nullptr);
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        active[index] =
            activeClipAt(project, TrackRef{kind, static_cast<int>(index)}, timelineFrame);
    }
    return active;
}

TimelineFrameResult timelineEndFrame(const Project& project) {
    const auto validation = validateTimeline(project);
    return {validation.success, validation.totalFrames, validation.error};
}

TimelineFrameResult timelineTrackEndFrame(const Project& project, TrackRef track) {
    TimelineFrameResult result;
    if (!isValidTrackRef(project, track)) {
        result.error = "track が存在しません";
        return result;
    }
    std::int64_t end = 0;
    for (const auto& clip : project.timelineClips) {
        if (!(clip.track == track))
            continue;
        std::int64_t start = 0;
        std::int64_t clipEnd = 0;
        if (!clipInterval(project, clip, start, clipEnd, result.error))
            return result;
        end = std::max(end, clipEnd);
    }
    result.success = true;
    result.frame = end;
    return result;
}

TimelineEditResult moveClip(Project& project, const std::string& clipId, TrackRef destinationTrack,
                            std::int64_t newStartFrame) {
    return moveClips(project, {clipId}, clipId, destinationTrack, newStartFrame, LinkMode::Linked);
}

namespace {

// trim した clip の fade を縮めた尺に収める。先頭側を優先して残す。
void clampFadesToLength(TimelineClip& clip) {
    const std::int64_t length = clip.sourceOutFrame - clip.sourceInFrame;
    clip.effects.fadeInFrames = std::min(clip.effects.fadeInFrames, length);
    clip.effects.fadeOutFrames =
        std::min(clip.effects.fadeOutFrames, length - clip.effects.fadeInFrames);
}

// track の [start, end) を上書きする。keep の clip (上に置く clip) は触らない。
bool overwriteTrackRange(Project& candidate, TrackRef track, std::int64_t start, std::int64_t end,
                         const std::unordered_set<std::string>& keep,
                         const std::function<std::string()>& newId, std::string& error) {
    std::vector<std::string> removed;
    const std::size_t originalCount = candidate.timelineClips.size();
    for (std::size_t index = 0; index < originalCount; ++index) {
        auto& clip = candidate.timelineClips[index];
        if (keep.contains(clip.id) || !(clip.track == track))
            continue;
        std::int64_t clipStart = 0;
        std::int64_t clipEnd = 0;
        if (!clipInterval(candidate, clip, clipStart, clipEnd, error))
            return false;
        if (clipEnd <= start || clipStart >= end)
            continue;
        if (clipStart >= start && clipEnd <= end) {
            removed.push_back(clip.id);
            continue;
        }
        if (clipStart < start && clipEnd > end) {
            // 中に置いた: 左を start で止め、右を end から始まる別 clip にする。
            TimelineClip right = clip;
            right.id = newId();
            if (right.kind == TimelineClipKind::EquationSequence &&
                !remapEquationSequenceIds(right.equationSequence, newId, error))
                return false;
            if (right.id.empty() || right.id == clip.id) {
                error = "上書きで分けた clip の ID を作れません";
                return false;
            }
            right.linkGroupId.clear();
            if (!trimClipBoundary(candidate, right, TrimEdge::Left, end - clipStart, error))
                return false;
            right.effects.fadeInFrames = 0;
            // 分けた右側は式の続きなので、fade in と同じく Write を持たない。
            right.mathAnimation = {};
            clampFadesToLength(right);
            for (auto& transition : candidate.timelineTransitions) {
                if (transition.outgoingClipId == clip.id)
                    transition.outgoingClipId = right.id;
            }
            auto& left = candidate.timelineClips[index];
            if (!trimClipBoundary(candidate, left, TrimEdge::Right, start - clipEnd, error))
                return false;
            left.effects.fadeOutFrames = 0;
            clampFadesToLength(left);
            candidate.timelineClips.push_back(std::move(right));
            continue;
        }
        if (clipStart < start) {
            if (!trimClipBoundary(candidate, clip, TrimEdge::Right, start - clipEnd, error))
                return false;
        } else if (!trimClipBoundary(candidate, clip, TrimEdge::Left, end - clipStart, error)) {
            return false;
        }
        clampFadesToLength(clip);
    }
    for (const auto& id : removed) {
        const auto& group =
            candidate.timelineClips[static_cast<std::size_t>(indexOfId(candidate, id))].linkGroupId;
        if (group.empty())
            continue;
        const std::string groupId = group;
        for (auto& clip : candidate.timelineClips)
            if (clip.linkGroupId == groupId)
                clip.linkGroupId.clear();
    }
    std::erase_if(candidate.timelineClips, [&](const TimelineClip& clip) {
        return std::find(removed.begin(), removed.end(), clip.id) != removed.end();
    });
    return true;
}

} // namespace

TimelineEditResult moveClips(Project& project, const std::vector<std::string>& clipIds,
                             const std::string& anchorClipId, TrackRef destinationTrack,
                             std::int64_t newStartFrame, LinkMode linkMode,
                             const std::function<std::string()>& newId) {
    TimelineEditResult result;
    if (clipIds.empty() || !isValidTrackRef(project, destinationTrack) || newStartFrame < 0) {
        result.error = "timeline clip の移動先 track または start frame が不正です";
        return result;
    }
    Project candidate = project;
    const int index = indexOfId(candidate, anchorClipId);
    if (!validIndex(candidate, index)) {
        result.error = "移動する timeline clip がありません";
        return result;
    }
    const auto& anchor = candidate.timelineClips[static_cast<std::size_t>(index)];
    if (!clipKindFitsTrackKind(anchor.kind, destinationTrack.kind)) {
        result.error = "この clip はその種別の track へ移動できません";
        return result;
    }

    std::unordered_set<std::string> movedIds;
    std::unordered_set<std::string> linkGroups;
    for (const auto& clipId : clipIds) {
        const int selectedIndex = indexOfId(candidate, clipId);
        if (!validIndex(candidate, selectedIndex)) {
            result.error = "移動する timeline clip がありません";
            return result;
        }
        const auto& selected = candidate.timelineClips[static_cast<std::size_t>(selectedIndex)];
        movedIds.insert(selected.id);
        if (linkMode == LinkMode::Linked && !selected.linkGroupId.empty())
            linkGroups.insert(selected.linkGroupId);
    }
    if (!movedIds.contains(anchorClipId)) {
        result.error = "anchor clip が選択に含まれていません";
        return result;
    }
    for (const auto& clip : candidate.timelineClips) {
        if (!clip.linkGroupId.empty() && linkGroups.contains(clip.linkGroupId))
            movedIds.insert(clip.id);
    }

    std::int64_t minimumStartFrame = std::numeric_limits<std::int64_t>::max();
    for (const auto& clip : candidate.timelineClips) {
        if (movedIds.contains(clip.id))
            minimumStartFrame = std::min(minimumStartFrame, clip.timelineStartFrame);
    }
    // リンクした字幕も同じ量だけ動くので、字幕が 0 より前へ出ない位置で止める。
    // Alt (Single) の移動では clip のリンク相手と同じく字幕も追従させない。
    if (linkMode == LinkMode::Linked)
        if (const auto cueStart = linkedSubtitleStart(candidate, movedIds))
            minimumStartFrame = std::min(minimumStartFrame, *cueStart);

    const std::int64_t oldStartFrame = anchor.timelineStartFrame;
    const std::int64_t requestedDelta = newStartFrame - oldStartFrame;
    // anchorだけを0へ丸めると、より左にある選択clipやリンク相手が負になる。
    // グループ全体の最左端が0に接する位置で止め、全clipへ同じdeltaを適用する。
    const std::int64_t delta = requestedDelta < 0 && minimumStartFrame < -requestedDelta
                                   ? -minimumStartFrame
                                   : requestedDelta;
    const int trackDelta = destinationTrack.index - anchor.track.index;
    for (auto& clip : candidate.timelineClips) {
        if (!movedIds.contains(clip.id))
            continue;
        if (delta > 0 &&
            clip.timelineStartFrame > std::numeric_limits<std::int64_t>::max() - delta) {
            result.error = "選択clipの移動先が範囲外です";
            return result;
        }
        clip.timelineStartFrame += delta;
        if (clip.track.kind == destinationTrack.kind) {
            const int destinationIndex = clip.track.index + trackDelta;
            const TrackRef translatedTrack{clip.track.kind, destinationIndex};
            if (!isValidTrackRef(candidate, translatedTrack)) {
                result.error = "選択clipの移動先trackが範囲外です";
                return result;
            }
            clip.track = translatedTrack;
        }
    }
    if (linkMode == LinkMode::Linked &&
        !shiftLinkedSubtitles(candidate, movedIds, delta, result.error))
        return result;
    if (newId) {
        struct Span {
            TrackRef track;
            std::int64_t start = 0;
            std::int64_t end = 0;
        };

        std::vector<Span> spans;
        for (const auto& clip : candidate.timelineClips) {
            if (!movedIds.contains(clip.id))
                continue;
            Span span{clip.track};
            if (!clipInterval(candidate, clip, span.start, span.end, result.error))
                return result;
            spans.push_back(span);
        }
        for (const auto& span : spans) {
            if (!overwriteTrackRange(candidate, span.track, span.start, span.end, movedIds, newId,
                                     result.error))
                return result;
        }
    }
    const auto validation = finalizeTimelineCandidate(candidate);
    if (!validation.success) {
        result.error = validation.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    result.selectedIndex = indexOfId(project, anchorClipId);
    return result;
}

TimelineEditResult appendTimelineClip(Project& project, TimelineClip clip, TrackRef track) {
    TimelineEditResult result;
    if (!isValidTrackRef(project, track)) {
        result.error = "追加先の track が存在しません";
        return result;
    }
    if (!clipKindFitsTrackKind(clip.kind, track.kind)) {
        result.error = "clip 種別と track 種別が一致しません";
        return result;
    }
    const auto start = timelineTrackEndFrame(project, track);
    if (!start.success) {
        result.error = start.error;
        return result;
    }
    Project candidate = project;
    clip.track = track;
    clip.timelineStartFrame = start.frame;
    candidate.timelineClips.push_back(std::move(clip));
    const auto validation = finalizeTimelineCandidate(candidate);
    if (!validation.success) {
        result.error = validation.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    result.selectedIndex = static_cast<int>(project.timelineClips.size()) - 1;
    return result;
}

TimelineEditResult deleteTimelineClip(Project& project, int selectedIndex) {
    TimelineEditResult result;
    if (!validIndex(project, selectedIndex)) {
        result.error = "削除する timeline clip がありません";
        return result;
    }
    Project candidate = project;
    const auto& selected = candidate.timelineClips[static_cast<std::size_t>(selectedIndex)];
    const std::string linkGroupId = selected.linkGroupId;
    std::unordered_set<std::string> erasedIds{selected.id};
    if (linkGroupId.empty()) {
        candidate.timelineClips.erase(candidate.timelineClips.begin() + selectedIndex);
    } else {
        for (const auto& clip : candidate.timelineClips)
            if (clip.linkGroupId == linkGroupId)
                erasedIds.insert(clip.id);
        std::erase_if(candidate.timelineClips,
                      [&](const TimelineClip& clip) { return clip.linkGroupId == linkGroupId; });
    }
    eraseLinkedSubtitles(candidate, erasedIds);
    const auto valid = finalizeTimelineCandidate(candidate);
    if (!valid.success) {
        result.error = valid.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    if (!project.timelineClips.empty())
        result.selectedIndex =
            std::min(selectedIndex, static_cast<int>(project.timelineClips.size()) - 1);
    return result;
}

TimelineEditResult placeTimelineClipAt(Project& project, TimelineClip clip, TrackRef track,
                                       std::int64_t timelineStartFrame) {
    TimelineEditResult result;
    if (!isValidTrackRef(project, track) || timelineStartFrame < 0) {
        result.error = "追加先の track または start frame が不正です";
        return result;
    }
    if (!clipKindFitsTrackKind(clip.kind, track.kind)) {
        result.error = "clip 種別と track 種別が一致しません";
        return result;
    }
    Project candidate = project;
    clip.track = track;
    clip.timelineStartFrame = timelineStartFrame;
    candidate.timelineClips.push_back(std::move(clip));
    const auto validation = finalizeTimelineCandidate(candidate);
    if (!validation.success) {
        result.error = validation.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    result.selectedIndex = static_cast<int>(project.timelineClips.size()) - 1;
    return result;
}

namespace {

// kind の track を firstIndex から順に試し、mute されていない track で place が成功したら確定する。
// 既存 track に空きが無ければ track を上へ足していく。足した track は空で mute もされて
// いないので、そこへは必ず置ける (置けなければ別の理由の失敗としてそのまま返す)。
TimelineEditResult
placeOnFirstFreeTrack(Project& project, TrackKind kind, int firstIndex,
                      const std::function<TimelineEditResult(Project&, int)>& place) {
    TimelineEditResult result;
    Project candidate = project;
    for (int trackIndex = std::max(firstIndex, 0);; ++trackIndex) {
        const bool addedTrack =
            trackIndex >= static_cast<int>(tracksOfKind(candidate, kind).size());
        if (addedTrack) {
            const auto added = addTrack(candidate, kind);
            if (!added.success) {
                result.error = added.error;
                return result;
            }
        }
        if (tracksOfKind(candidate, kind)[static_cast<std::size_t>(trackIndex)].muted)
            continue;
        result = place(candidate, trackIndex);
        if (result.success) {
            project = std::move(candidate);
            return result;
        }
        if (addedTrack)
            return result;
    }
}

} // namespace

TimelineEditResult placeStillClipAt(Project& project, TimelineClip clip,
                                    std::int64_t timelineStartFrame) {
    TimelineEditResult result;
    // 数式 sequence も素材を持たない映像 clip として同じ規則で置く (内部の尺は呼び出し側が決める)。
    if ((!isStillClipKind(clip.kind) && clip.kind != TimelineClipKind::EquationSequence) ||
        timelineStartFrame < 0) {
        result.error = "配置する text / image clip または開始位置が不正です";
        return result;
    }
    int highestActive = -1;
    const auto active = activeClipsAt(project, TrackKind::Video, timelineStartFrame);
    for (std::size_t index = 0; index < active.size(); ++index)
        if (active[index])
            highestActive = static_cast<int>(index);
    return placeOnFirstFreeTrack(
        project, TrackKind::Video, highestActive + 1, [&](Project& candidate, int trackIndex) {
            return placeTimelineClipAt(candidate, clip, {TrackKind::Video, trackIndex},
                                       timelineStartFrame);
        });
}

TimelineEditResult placeAudioClipAt(Project& project, TimelineClip clip,
                                    std::int64_t timelineStartFrame) {
    TimelineEditResult result;
    if (clip.kind != TimelineClipKind::Audio || timelineStartFrame < 0) {
        result.error = "配置する audio clip または開始位置が不正です";
        return result;
    }
    return placeOnFirstFreeTrack(
        project, TrackKind::Audio, 0, [&](Project& candidate, int trackIndex) {
            return placeTimelineClipAt(candidate, clip, {TrackKind::Audio, trackIndex},
                                       timelineStartFrame);
        });
}

TimelineEditResult placeMediaAtDrop(Project& project, TimelineClip primary,
                                    std::optional<TimelineClip> linkedAudio, TrackRef target,
                                    std::int64_t timelineStartFrame) {
    TimelineEditResult result;
    const int trackCount = static_cast<int>(tracksOfKind(project, target.kind).size());
    if (target.index < 0 || target.index > trackCount) {
        result.error = "ドロップ先の track が不正です";
        return result;
    }
    timelineStartFrame = std::max<std::int64_t>(timelineStartFrame, 0);
    // リンク対は、ドロップした行の種別に合う側をその track へ置く。
    const TimelineClip& onTarget =
        linkedAudio && target.kind == TrackKind::Audio ? *linkedAudio : primary;
    const TrackKind ownKind =
        onTarget.kind == TimelineClipKind::Audio ? TrackKind::Audio : TrackKind::Video;
    TimelineClip alone = onTarget;
    alone.linkGroupId.clear();
    const auto fitsAt = [&](const Project& base, TrackRef track) {
        Project probe = base;
        return placeTimelineClipAt(probe, alone, track, timelineStartFrame).success;
    };

    Project candidate = project;
    const bool kindFits = clipKindFitsTrackKind(onTarget.kind, target.kind);
    if (kindFits && target.index == trackCount) {
        const auto added = addTrack(candidate, target.kind);
        if (!added.success) {
            result.error = added.error;
            return result;
        }
    }
    // ドロップした行に置けない (種別が違う・既存の clip と重なる) ときは、上書きせずに
    // 同じ種別で空いている track (無ければ新しい track) へ回す。時刻はドロップした位置のまま。
    if (!kindFits || !fitsAt(candidate, target)) {
        int chosen = -1;
        Project scratch = candidate;
        const auto found =
            placeOnFirstFreeTrack(scratch, ownKind, 0, [&](Project& attempt, int index) {
                chosen = index;
                return placeTimelineClipAt(attempt, alone, {ownKind, index}, timelineStartFrame);
            });
        if (!found.success) {
            result.error = found.error;
            return result;
        }
        while (static_cast<int>(tracksOfKind(candidate, ownKind).size()) <= chosen) {
            const auto added = addTrack(candidate, ownKind);
            if (!added.success) {
                result.error = added.error;
                return result;
            }
        }
        target = {ownKind, chosen};
    }
    if (!linkedAudio) {
        result = placeTimelineClipAt(candidate, std::move(primary), target, timelineStartFrame);
    } else {
        // リンク相手は、もう一方の種別で空いている最初の track (無ければ新しい track) へ置く。
        const TrackKind partnerKind =
            target.kind == TrackKind::Video ? TrackKind::Audio : TrackKind::Video;
        result = placeOnFirstFreeTrack(
            candidate, partnerKind, 0, [&](Project& attempt, int partnerIndex) {
                const TrackRef partner{partnerKind, partnerIndex};
                return placeLinkedAvPairAt(
                    attempt, primary, target.kind == TrackKind::Video ? target : partner,
                    *linkedAudio, target.kind == TrackKind::Audio ? target : partner,
                    timelineStartFrame);
            });
    }
    if (result.success)
        project = std::move(candidate);
    return result;
}

TimelineEditResult placeLinkedAvPairAt(Project& project, TimelineClip video, TrackRef videoTrack,
                                       TimelineClip audio, TrackRef audioTrack,
                                       std::int64_t timelineStartFrame) {
    TimelineEditResult result;
    if (!isValidTrackRef(project, videoTrack) || !isValidTrackRef(project, audioTrack) ||
        timelineStartFrame < 0) {
        result.error = "リンクclipの追加先trackまたはstart frameが不正です";
        return result;
    }
    if (video.kind == TimelineClipKind::Audio || audio.kind != TimelineClipKind::Audio ||
        videoTrack.kind != TrackKind::Video || audioTrack.kind != TrackKind::Audio ||
        video.linkGroupId.empty() || video.linkGroupId != audio.linkGroupId) {
        result.error = "リンクclipは同じlink IDを持つvideo/audioの1組である必要があります";
        return result;
    }

    Project candidate = project;
    video.track = videoTrack;
    video.timelineStartFrame = timelineStartFrame;
    audio.track = audioTrack;
    audio.timelineStartFrame = timelineStartFrame;
    const int videoIndex = static_cast<int>(candidate.timelineClips.size());
    candidate.timelineClips.push_back(std::move(video));
    candidate.timelineClips.push_back(std::move(audio));
    const auto validation = finalizeTimelineCandidate(candidate);
    if (!validation.success) {
        result.error = validation.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    result.selectedIndex = videoIndex;
    return result;
}

TimelineEditResult unlinkTimelineClip(Project& project, const std::string& clipId) {
    TimelineEditResult result;
    Project candidate = project;
    const int index = indexOfId(candidate, clipId);
    if (!validIndex(candidate, index)) {
        result.error = "リンク解除する timeline clip がありません";
        return result;
    }
    const std::string linkGroupId =
        candidate.timelineClips[static_cast<std::size_t>(index)].linkGroupId;
    std::unordered_set<std::string> unlinkedIds{clipId};
    for (auto& clip : candidate.timelineClips) {
        if (!linkGroupId.empty() && clip.linkGroupId == linkGroupId) {
            unlinkedIds.insert(clip.id);
            clip.linkGroupId.clear();
        }
    }
    // 文字起こしで作った字幕とのリンクも同じ操作で外す。
    if (unlinkSubtitles(candidate, unlinkedIds) == 0 && linkGroupId.empty()) {
        result.error = "選択した clip はリンクされていません";
        return result;
    }
    const auto valid = finalizeTimelineCandidate(candidate);
    if (!valid.success) {
        result.error = valid.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    result.selectedIndex = index;
    return result;
}

namespace {

// 編集の対象になる clip。clip 自身を先頭に置き、Linked ならリンク相手を後ろへ足す。
std::vector<int> editTargets(const Project& project, int index, LinkMode linkMode) {
    std::vector<int> targets{index};
    const std::string& group = project.timelineClips[static_cast<std::size_t>(index)].linkGroupId;
    if (linkMode == LinkMode::Single || group.empty())
        return targets;
    for (std::size_t other = 0; other < project.timelineClips.size(); ++other) {
        if (static_cast<int>(other) != index && project.timelineClips[other].linkGroupId == group)
            targets.push_back(static_cast<int>(other));
    }
    return targets;
}

bool isTarget(const std::vector<int>& targets, std::size_t index) {
    return std::find(targets.begin(), targets.end(), static_cast<int>(index)) != targets.end();
}

// clip の edge と接している同じ track の clip。見つからなければ -1。
int adjacentClipIndex(const Project& project, int index, TrimEdge edge, std::string& error) {
    const auto& clip = project.timelineClips[static_cast<std::size_t>(index)];
    std::int64_t start = 0;
    std::int64_t end = 0;
    if (!clipInterval(project, clip, start, end, error))
        return -1;
    for (std::size_t other = 0; other < project.timelineClips.size(); ++other) {
        if (static_cast<int>(other) == index)
            continue;
        const auto& candidate = project.timelineClips[other];
        if (!(candidate.track == clip.track))
            continue;
        std::int64_t otherStart = 0;
        std::int64_t otherEnd = 0;
        if (!clipInterval(project, candidate, otherStart, otherEnd, error))
            return -1;
        if ((edge == TrimEdge::Right && otherStart == end) ||
            (edge == TrimEdge::Left && otherEnd == start))
            return static_cast<int>(other);
    }
    return -1;
}

// marked に印を付けた clip のリンク相手にも印を付ける。link は横移動を同期する契約である。
void includeLinkedCounterparts(const Project& project, std::vector<bool>& marked) {
    std::unordered_set<std::string> linkGroups;
    for (std::size_t index = 0; index < marked.size(); ++index) {
        const auto& clip = project.timelineClips[index];
        if (marked[index] && !clip.linkGroupId.empty())
            linkGroups.insert(clip.linkGroupId);
    }
    for (std::size_t index = 0; index < marked.size(); ++index) {
        const auto& clip = project.timelineClips[index];
        if (!clip.linkGroupId.empty() && linkGroups.contains(clip.linkGroupId))
            marked[index] = true;
    }
}

bool shiftStart(TimelineClip& clip, std::int64_t delta, std::string& error) {
    if ((delta < 0 && clip.timelineStartFrame < -delta) ||
        (delta > 0 && clip.timelineStartFrame > std::numeric_limits<std::int64_t>::max() - delta)) {
        error = "clip の移動先が範囲外です: " + clip.name;
        return false;
    }
    clip.timelineStartFrame += delta;
    return true;
}

TimelineEditResult commitCandidate(Project& project, Project candidate, int selectedIndex) {
    TimelineEditResult result;
    const auto valid = finalizeTimelineCandidate(candidate);
    if (!valid.success) {
        result.error = valid.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    result.selectedIndex = selectedIndex;
    return result;
}

// 時間の対応を変える clip の編集の確定。リンクした字幕を音声に合わせて置き直してから確定する
// (remapLinkedSubtitles)。
// リップルの全トラック時間編集で二分した clip は、時間編集の中で右側の字幕を右側の clip へ
// 付け替えてある。ここでは同じ ID の左側だけが対象になり、その字幕は素材の時刻から作り直しても
// 時間編集と同じ位置になる。
TimelineEditResult commitMappingEdit(Project& project, Project candidate, int selectedIndex) {
    TimelineEditResult result;
    if (!remapLinkedSubtitles(project, candidate, result.error))
        return result;
    return commitCandidate(project, std::move(candidate), selectedIndex);
}

// outgoing (edge=Right なら clip 自身) と incoming の境界を動かす。
// 接している clip が無ければ neighborFound=false で何もしない。
bool rollEditPoint(Project& candidate, int index, TrimEdge edge, std::int64_t projectFrameDelta,
                   bool& neighborFound, std::string& error) {
    const int neighbor = adjacentClipIndex(candidate, index, edge, error);
    neighborFound = neighbor >= 0;
    if (!neighborFound)
        return error.empty();
    const int outgoingIndex = edge == TrimEdge::Right ? index : neighbor;
    const int incomingIndex = edge == TrimEdge::Right ? neighbor : index;
    auto& outgoing = candidate.timelineClips[static_cast<std::size_t>(outgoingIndex)];
    auto& incoming = candidate.timelineClips[static_cast<std::size_t>(incomingIndex)];
    if (!trimClipBoundary(candidate, outgoing, TrimEdge::Right, projectFrameDelta, error) ||
        !trimClipBoundary(candidate, incoming, TrimEdge::Left, projectFrameDelta, error))
        return false;
    std::int64_t outgoingStart = 0;
    std::int64_t outgoingEnd = 0;
    std::int64_t incomingStart = 0;
    std::int64_t incomingEnd = 0;
    if (!clipInterval(candidate, outgoing, outgoingStart, outgoingEnd, error) ||
        !clipInterval(candidate, incoming, incomingStart, incomingEnd, error))
        return false;
    // 素材 fps が timeline と異なると frame 境界の丸めで隙間ができうる。
    // 合計尺を保つという契約を守れないので黙って受理しない。
    if (outgoingEnd != incomingStart) {
        error = "境界を素材 frame に揃えられないためローリング編集できません";
        return false;
    }
    return true;
}

// clip の in / out を timeline 上で動かせる範囲 [lower, upper] (project frame)。
bool slipRange(const Project& project, const TimelineClip& clip, std::int64_t& lower,
               std::int64_t& upper, std::string& error) {
    const auto in = clipSourceBoundaryToTimeline(clip, clip.sourceInFrame, project.timelineFpsNum,
                                                 project.timelineFpsDen);
    const auto out = clipSourceBoundaryToTimeline(clip, clip.sourceOutFrame, project.timelineFpsNum,
                                                  project.timelineFpsDen);
    const auto end = clipSourceBoundaryToTimeline(clip, clip.sourceFrameCount,
                                                  project.timelineFpsNum, project.timelineFpsDen);
    if (!in.success || !out.success || !end.success) {
        error = !in.success ? in.error : (!out.success ? out.error : end.error);
        return false;
    }
    lower = -in.frame;
    upper = end.frame - out.frame;
    return true;
}

// in / out を timeline 上で projectFrameDelta だけずらす。素材上の長さ (out - in) は保ち、
// 素材の範囲を超える分は端で止める。in が動いたら moved=true。
bool slipClipSource(const Project& project, TimelineClip& clip, std::int64_t projectFrameDelta,
                    bool& moved, std::string& error) {
    const auto inBoundary = clipSourceBoundaryToTimeline(
        clip, clip.sourceInFrame, project.timelineFpsNum, project.timelineFpsDen);
    if (!inBoundary.success) {
        error = inBoundary.error;
        return false;
    }
    if (projectFrameDelta > 0 &&
        inBoundary.frame > std::numeric_limits<std::int64_t>::max() - projectFrameDelta) {
        error = "スリップ量が範囲外です";
        return false;
    }
    // 素材の先頭より前は存在しないので、timeline 境界の段階で 0 に丸めておく。
    const std::int64_t shiftedBoundary =
        projectFrameDelta < 0 && inBoundary.frame < -projectFrameDelta
            ? 0
            : inBoundary.frame + projectFrameDelta;
    const auto shiftedIn = clipTimelineBoundaryToSource(
        clip, shiftedBoundary, project.timelineFpsNum, project.timelineFpsDen);
    if (!shiftedIn.success) {
        error = shiftedIn.error;
        return false;
    }
    // 素材 fps が timeline と異なる場合、timeline 上の尺は frame 境界の丸めで 1 frame
    // 変わりうる。重なりは validateTimeline が拒否する。
    const std::int64_t sourceLength = clip.sourceOutFrame - clip.sourceInFrame;
    const std::int64_t newIn =
        std::clamp<std::int64_t>(shiftedIn.frame, 0, clip.sourceFrameCount - sourceLength);
    moved = newIn != clip.sourceInFrame;
    clip.sourceInFrame = newIn;
    clip.sourceOutFrame = newIn + sourceLength;
    return true;
}

} // namespace

namespace {

// clip の edge を timeline 上で動かせる範囲 [lower, upper] (project frame)。
// 素材の範囲を超えず、clip の尺を 1 frame 以上に保つ。
bool edgeRange(const Project& project, const TimelineClip& clip, TrimEdge edge, std::int64_t& lower,
               std::int64_t& upper, std::string& error) {
    if (hasSyntheticSourceDomain(clip)) {
        const auto duration = timelineClipDuration(project, clip);
        if (!duration.success) {
            error = duration.error;
            return false;
        }
        if (edge == TrimEdge::Left) {
            lower = -clip.timelineStartFrame;
            upper = duration.frame - 1;
        } else {
            lower = -(duration.frame - 1);
            upper = std::numeric_limits<std::int64_t>::max() / 2 - clip.timelineStartFrame -
                    duration.frame;
        }
        return true;
    }
    const auto in = clipSourceBoundaryToTimeline(clip, clip.sourceInFrame, project.timelineFpsNum,
                                                 project.timelineFpsDen);
    const auto out = clipSourceBoundaryToTimeline(clip, clip.sourceOutFrame, project.timelineFpsNum,
                                                  project.timelineFpsDen);
    const auto end = clipSourceBoundaryToTimeline(clip, clip.sourceFrameCount,
                                                  project.timelineFpsNum, project.timelineFpsDen);
    if (!in.success || !out.success || !end.success) {
        error = !in.success ? in.error : (!out.success ? out.error : end.error);
        return false;
    }
    const std::int64_t duration = out.frame - in.frame;
    if (edge == TrimEdge::Left) {
        lower = -in.frame;
        upper = duration - 1;
    } else {
        lower = -(duration - 1);
        upper = end.frame - out.frame;
    }
    return true;
}

void narrowRange(std::int64_t& lower, std::int64_t& upper, std::int64_t otherLower,
                 std::int64_t otherUpper) {
    lower = std::max(lower, otherLower);
    upper = std::min(upper, otherUpper);
}

} // namespace

TimelineFrameResult clampEdgeEdit(const Project& project, const std::string& clipId, TrimEdge edge,
                                  EdgeEditKind kind, std::int64_t projectFrameDelta,
                                  LinkMode linkMode) {
    TimelineFrameResult result;
    const int index = indexOfId(project, clipId);
    if (!validIndex(project, index)) {
        result.error = "伸縮する timeline clip がありません";
        return result;
    }
    std::int64_t lower = std::numeric_limits<std::int64_t>::min();
    std::int64_t upper = std::numeric_limits<std::int64_t>::max();
    for (const int target : editTargets(project, index, linkMode)) {
        const auto& clip = project.timelineClips[static_cast<std::size_t>(target)];
        std::int64_t clipLower = 0;
        std::int64_t clipUpper = 0;
        if (kind == EdgeEditKind::Roll) {
            const int neighbor = adjacentClipIndex(project, target, edge, result.error);
            if (!result.error.empty())
                return result;
            if (neighbor < 0) {
                // リンク相手が編集点を持たない (L / J カット) なら、相手は範囲を縛らない。
                if (target != index)
                    continue;
                result.error = "ローリング編集には接している隣の clip が必要です";
                return result;
            }
            const int outgoing = edge == TrimEdge::Right ? target : neighbor;
            const int incoming = edge == TrimEdge::Right ? neighbor : target;
            if (!edgeRange(project, project.timelineClips[static_cast<std::size_t>(outgoing)],
                           TrimEdge::Right, clipLower, clipUpper, result.error))
                return result;
            narrowRange(lower, upper, clipLower, clipUpper);
            if (!edgeRange(project, project.timelineClips[static_cast<std::size_t>(incoming)],
                           TrimEdge::Left, clipLower, clipUpper, result.error))
                return result;
            narrowRange(lower, upper, clipLower, clipUpper);
            continue;
        }
        if (!edgeRange(project, clip, edge, clipLower, clipUpper, result.error))
            return result;
        narrowRange(lower, upper, clipLower, clipUpper);
        // 通常の trim は left 端を動かすと clip の開始位置も動く。timeline 先頭より前へは出さない。
        // リップルは開始位置を保つので、この制約を受けない。
        if (kind == EdgeEditKind::Trim && edge == TrimEdge::Left)
            lower = std::max(lower, -clip.timelineStartFrame);
        // 通常の trim は、接している隣の clip の方へは延ばさない (Premiere と同じく止める。
        // 延ばすと重なって検証で失敗していた)。縮める向きは自由で、離れればトランジションは消える。
        if (kind == EdgeEditKind::Trim) {
            const int neighbor = adjacentClipIndex(project, target, edge, result.error);
            if (!result.error.empty())
                return result;
            if (neighbor >= 0) {
                if (edge == TrimEdge::Right)
                    upper = std::min<std::int64_t>(upper, 0);
                else
                    lower = std::max<std::int64_t>(lower, 0);
            }
        }
    }
    result.success = true;
    result.frame = lower > upper ? 0 : std::clamp(projectFrameDelta, lower, upper);
    return result;
}

namespace {

// clampEdgeEdit で止めた量を返す。1 frame も動かせなければ失敗にする。
bool clampedEdgeDelta(const Project& project, const std::string& clipId, TrimEdge edge,
                      EdgeEditKind kind, std::int64_t projectFrameDelta, LinkMode linkMode,
                      std::int64_t& clamped, std::string& error) {
    const auto range = clampEdgeEdit(project, clipId, edge, kind, projectFrameDelta, linkMode);
    if (!range.success) {
        error = range.error;
        return false;
    }
    if (range.frame == 0) {
        error = "素材の端または clip の最小尺に達しているため、これ以上伸縮できません";
        return false;
    }
    clamped = range.frame;
    return true;
}

} // namespace

TimelineEditResult trimTimelineClip(Project& project, const std::string& clipId, TrimEdge edge,
                                    std::int64_t projectFrameDelta, LinkMode linkMode) {
    TimelineEditResult result;
    Project candidate = project;
    const int index = indexOfId(candidate, clipId);
    if (!validIndex(candidate, index)) {
        result.error = "trim する timeline clip がありません";
        return result;
    }
    if (candidate.timelineClips[static_cast<std::size_t>(index)].kind !=
        TimelineClipKind::EquationSequence) {
        if (!clampedEdgeDelta(candidate, clipId, edge, EdgeEditKind::Trim, projectFrameDelta,
                              linkMode, projectFrameDelta, result.error))
            return result;
    }
    for (const int target : editTargets(candidate, index, linkMode)) {
        if (!trimClipBoundary(candidate, candidate.timelineClips[static_cast<std::size_t>(target)],
                              edge, projectFrameDelta, result.error))
            return result;
    }
    return commitMappingEdit(project, std::move(candidate), index);
}

std::optional<TimelineClip> clipWithEdgeAt(const Project& project, const TimelineClip& clip,
                                           TrimEdge edge, std::int64_t timelineFrame,
                                           std::string& error) {
    std::int64_t start = 0;
    std::int64_t end = 0;
    if (!clipInterval(project, clip, start, end, error))
        return std::nullopt;
    TimelineClip moved = clip;
    const std::int64_t delta = timelineFrame - (edge == TrimEdge::Left ? start : end);
    if (delta != 0 && !trimClipBoundary(project, moved, edge, delta, error))
        return std::nullopt;
    std::int64_t movedStart = 0;
    std::int64_t movedEnd = 0;
    if (!clipInterval(project, moved, movedStart, movedEnd, error))
        return std::nullopt;
    const bool exact = edge == TrimEdge::Left ? movedStart == timelineFrame && movedEnd == end
                                              : movedStart == start && movedEnd == timelineFrame;
    if (!exact || moved.sourceInFrame < 0 || moved.sourceOutFrame <= moved.sourceInFrame ||
        moved.sourceOutFrame > moved.sourceFrameCount) {
        error = "clip の端を素材 frame へ一意に換算できません: " + clip.name;
        return std::nullopt;
    }
    return moved;
}

TimelineEditResult splitTimelineClips(Project& project, const std::vector<std::string>& clipIds,
                                      std::int64_t frame, const std::function<std::string()>& newId,
                                      LinkMode linkMode) {
    TimelineEditResult result;
    if (clipIds.empty() || !newId) {
        result.error = "分割する timeline clip がありません";
        return result;
    }
    Project candidate = project;
    const std::size_t originalCount = candidate.timelineClips.size();
    std::vector<bool> requested(originalCount, false);
    for (const auto& id : clipIds) {
        const int index = indexOfId(candidate, id);
        if (!validIndex(candidate, index)) {
            result.error = "分割する timeline clip がありません";
            return result;
        }
        const auto& clip = candidate.timelineClips[static_cast<std::size_t>(index)];
        std::int64_t start = 0;
        std::int64_t end = 0;
        if (!clipInterval(candidate, clip, start, end, result.error))
            return result;
        if (frame <= start || frame >= end) {
            result.error = "分割位置が clip の内側にありません: " + clip.name;
            return result;
        }
        requested[static_cast<std::size_t>(index)] = true;
    }
    // リンク相手は、Linked で、かつ分割位置を内側に含むときだけ一緒に切る。
    std::vector<bool> split = requested;
    if (linkMode == LinkMode::Linked)
        includeLinkedCounterparts(candidate, split);
    for (std::size_t index = 0; index < originalCount; ++index) {
        if (!split[index] || requested[index])
            continue;
        std::int64_t start = 0;
        std::int64_t end = 0;
        if (!clipInterval(candidate, candidate.timelineClips[index], start, end, result.error))
            return result;
        split[index] = frame > start && frame < end;
    }

    // link group の両方を切ったときだけ、右半分どうしを新しい group で結ぶ。
    // 片方だけを切った場合、右半分は相手を持たないので未リンクにする。
    std::unordered_map<std::string, int> splitPerGroup;
    for (std::size_t index = 0; index < originalCount; ++index) {
        const auto& clip = candidate.timelineClips[index];
        if (split[index] && !clip.linkGroupId.empty())
            ++splitPerGroup[clip.linkGroupId];
    }
    std::unordered_map<std::string, std::string> rightLinkGroups;
    for (const auto& [group, count] : splitPerGroup) {
        if (count == 2)
            rightLinkGroups[group] = newId();
    }

    for (std::size_t index = 0; index < originalCount; ++index) {
        if (!split[index])
            continue;
        TimelineClip left = candidate.timelineClips[index];
        TimelineClip right = left;
        std::int64_t start = 0;
        std::int64_t end = 0;
        if (!clipInterval(candidate, left, start, end, result.error) ||
            !trimClipBoundary(candidate, left, TrimEdge::Right, frame - end, result.error) ||
            !trimClipBoundary(candidate, right, TrimEdge::Left, frame - start, result.error))
            return result;
        // 両半分とも同じ timeline 境界から換算するので、素材上の境界は一致するはずである。
        // 文字・静止画は素材の時間軸を持たず、trim が両半分とも in = 0 に振り直す。
        // 境界の一致は timeline 上で見る。
        if (hasSyntheticSourceDomain(left)
                ? left.timelineStartFrame + left.sourceOutFrame != right.timelineStartFrame
                : left.sourceOutFrame != right.sourceInFrame) {
            result.error = "分割位置を素材 frame へ一意に換算できません: " + left.name;
            return result;
        }
        if (left.kind == TimelineClipKind::EquationSequence && right.timelineStartFrame != frame) {
            result.error = "分割位置を sequence の整数素材境界で正確に表現できません";
            return result;
        }
        left.effects.fadeOutFrames = 0;
        left.effects.fadeInFrames =
            std::min(left.effects.fadeInFrames, left.sourceOutFrame - left.sourceInFrame);
        right.effects.fadeInFrames = 0;
        // 右半分は式の続きなので、fade in と同じく Write を持たない。
        right.mathAnimation = {};
        right.effects.fadeOutFrames =
            std::min(right.effects.fadeOutFrames, right.sourceOutFrame - right.sourceInFrame);
        right.id = newId();
        if (right.kind == TimelineClipKind::EquationSequence &&
            !remapEquationSequenceIds(right.equationSequence, newId, result.error))
            return result;
        if (right.id.empty() || right.id == left.id) {
            result.error = "分割後の clip ID を作れません";
            return result;
        }
        relinkSubtitlesAfterSplit(candidate, left.id, right.id, frame, newId);
        if (!right.linkGroupId.empty()) {
            const auto found = rightLinkGroups.find(right.linkGroupId);
            right.linkGroupId = found == rightLinkGroups.end() ? std::string{} : found->second;
        }
        for (auto& transition : candidate.timelineTransitions) {
            if (transition.outgoingClipId == left.id)
                transition.outgoingClipId = right.id;
        }
        candidate.timelineClips[index] = std::move(left);
        candidate.timelineClips.push_back(std::move(right));
    }
    return commitCandidate(project, std::move(candidate), indexOfId(project, clipIds.front()));
}

std::vector<std::string> clipIdsSpanningFrame(const Project& project, std::int64_t frame) {
    std::vector<std::string> ids;
    for (const auto& clip : project.timelineClips) {
        std::int64_t start = 0;
        std::int64_t end = 0;
        std::string ignored;
        if (clipInterval(project, clip, start, end, ignored) && frame > start && frame < end)
            ids.push_back(clip.id);
    }
    return ids;
}

std::vector<std::string> clipIdsSpanningFrame(const Project& project, std::int64_t frame,
                                              const std::vector<std::string>& among) {
    std::vector<std::string> ids;
    for (const auto& id : among) {
        const int index = indexOfId(project, id);
        if (!validIndex(project, index))
            continue;
        std::int64_t start = 0;
        std::int64_t end = 0;
        std::string ignored;
        if (clipInterval(project, project.timelineClips[static_cast<std::size_t>(index)], start,
                         end, ignored) &&
            frame > start && frame < end)
            ids.push_back(id);
    }
    return ids;
}

TimelineEditResult stepClipVolume(Project& project, const std::vector<std::string>& clipIds,
                                  double stepDb) {
    TimelineEditResult result;
    Project candidate = project;
    std::vector<bool> marked(candidate.timelineClips.size(), false);
    for (const auto& id : clipIds) {
        const int index = indexOfId(candidate, id);
        if (!validIndex(candidate, index)) {
            result.error = "音量を変える timeline clip がありません";
            return result;
        }
        marked[static_cast<std::size_t>(index)] = true;
    }
    includeLinkedCounterparts(candidate, marked);
    int firstTarget = -1;
    bool changed = false;
    for (std::size_t index = 0; index < marked.size(); ++index) {
        auto& clip = candidate.timelineClips[index];
        if (!marked[index] || clip.kind != TimelineClipKind::Audio)
            continue;
        if (firstTarget < 0)
            firstTarget = static_cast<int>(index);
        const auto stepped = stepVolumePercentByDb(clip.effects.volumePercent, stepDb);
        if (!stepped) {
            result.error = "音量を段階的に変えられません: " + clip.name;
            return result;
        }
        changed = changed || *stepped != clip.effects.volumePercent;
        clip.effects.volumePercent = *stepped;
        for (auto& key : clip.effects.volumeKeys) {
            if (key.value == 0.0)
                continue;
            const auto steppedKey = stepVolumePercentByDb(key.value, stepDb);
            if (!steppedKey) {
                result.error = "音量キーを段階的に変えられません: " + clip.name;
                return result;
            }
            changed = changed || *steppedKey != key.value;
            key.value = *steppedKey;
        }
    }
    if (firstTarget < 0) {
        result.error = "音量を変えられる audio clip がありません";
        return result;
    }
    if (!changed) {
        result.error = stepDb > 0.0 ? "音量は既に上限です" : "音量は既に下限です";
        return result;
    }
    return commitCandidate(project, std::move(candidate), firstTarget);
}

TimelineEditResult toggleClipsEnabled(Project& project, const std::vector<std::string>& clipIds) {
    TimelineEditResult result;
    if (clipIds.empty()) {
        result.error = "有効/無効を切り換える timeline clip がありません";
        return result;
    }
    Project candidate = project;
    std::vector<bool> marked(candidate.timelineClips.size(), false);
    for (const auto& id : clipIds) {
        const int index = indexOfId(candidate, id);
        if (!validIndex(candidate, index)) {
            result.error = "有効/無効を切り換える timeline clip がありません";
            return result;
        }
        marked[static_cast<std::size_t>(index)] = true;
    }
    includeLinkedCounterparts(candidate, marked);
    bool anyEnabled = false;
    for (std::size_t index = 0; index < marked.size(); ++index)
        anyEnabled = anyEnabled || (marked[index] && candidate.timelineClips[index].enabled);
    for (std::size_t index = 0; index < marked.size(); ++index) {
        if (marked[index])
            candidate.timelineClips[index].enabled = !anyEnabled;
    }
    return commitCandidate(project, std::move(candidate), indexOfId(project, clipIds.front()));
}

std::int64_t defaultTransitionFrames(std::int64_t timelineFpsNum, std::int64_t timelineFpsDen) {
    if (timelineFpsNum <= 0 || timelineFpsDen <= 0)
        return 1;
    return std::max<std::int64_t>(1, (timelineFpsNum + timelineFpsDen / 2) / timelineFpsDen);
}

TimelineEditResult applyDefaultClipFades(Project& project, const std::vector<std::string>& clipIds,
                                         std::int64_t timelineFrames) {
    TimelineEditResult result;
    if (clipIds.empty() || timelineFrames < 1) {
        result.error = "フェードを付ける timeline clip がありません";
        return result;
    }
    Project candidate = project;
    std::vector<bool> marked(candidate.timelineClips.size(), false);
    for (const auto& id : clipIds) {
        const int index = indexOfId(candidate, id);
        if (!validIndex(candidate, index)) {
            result.error = "フェードを付ける timeline clip がありません";
            return result;
        }
        marked[static_cast<std::size_t>(index)] = true;
    }
    includeLinkedCounterparts(candidate, marked);
    std::unordered_set<std::string> headTransitions;
    std::unordered_set<std::string> tailTransitions;
    for (const auto& transition : candidate.timelineTransitions) {
        headTransitions.insert(transition.incomingClipId);
        tailTransitions.insert(transition.outgoingClipId);
    }
    const auto fpsNum = candidate.timelineFpsNum;
    const auto fpsDen = candidate.timelineFpsDen;
    for (std::size_t index = 0; index < marked.size(); ++index) {
        if (!marked[index])
            continue;
        auto& clip = candidate.timelineClips[index];
        const std::int64_t duration = clip.sourceOutFrame - clip.sourceInFrame;
        // timeline 上の境界から timelineFrames だけ内側の位置を素材 frame へ戻した差が、
        // その端のフェードの素材 frame 数になる (速度 2 倍なら素材は 2 倍進む)。
        const auto inBoundary =
            clipSourceBoundaryToTimeline(clip, clip.sourceInFrame, fpsNum, fpsDen);
        const auto outBoundary =
            clipSourceBoundaryToTimeline(clip, clip.sourceOutFrame, fpsNum, fpsDen);
        if (!inBoundary.success || !outBoundary.success) {
            result.error =
                clip.name + ": " + (!inBoundary.success ? inBoundary.error : outBoundary.error);
            return result;
        }
        // 内側の位置は clip の中に収める (1 秒より短い clip で反対側の端を越えない)。
        const auto clipFrames = outBoundary.frame - inBoundary.frame;
        const auto inner = std::min(timelineFrames, clipFrames);
        const auto headSource =
            clipTimelineBoundaryToSource(clip, inBoundary.frame + inner, fpsNum, fpsDen);
        const auto tailSource =
            clipTimelineBoundaryToSource(clip, outBoundary.frame - inner, fpsNum, fpsDen);
        if (!headSource.success || !tailSource.success) {
            result.error =
                clip.name + ": " + (!headSource.success ? headSource.error : tailSource.error);
            return result;
        }
        const std::int64_t wantedIn =
            std::clamp(headSource.frame - clip.sourceInFrame, std::int64_t{0}, duration);
        const std::int64_t wantedOut =
            std::clamp(clip.sourceOutFrame - tailSource.frame, std::int64_t{0}, duration);
        const bool keepHead = headTransitions.contains(clip.id);
        const bool keepTail = tailTransitions.contains(clip.id);
        std::int64_t fadeIn = keepHead ? clip.effects.fadeInFrames : wantedIn;
        std::int64_t fadeOut = keepTail ? clip.effects.fadeOutFrames : wantedOut;
        if (!keepHead && !keepTail) {
            fadeIn = std::min(fadeIn, (duration + 1) / 2);
            fadeOut = std::min(fadeOut, duration - fadeIn);
        } else if (keepHead) {
            fadeOut = std::min(fadeOut, duration - fadeIn);
        } else {
            fadeIn = std::min(fadeIn, duration - fadeOut);
        }
        clip.effects.fadeInFrames = fadeIn;
        clip.effects.fadeOutFrames = fadeOut;
    }
    if (candidate == project) {
        result.error = "フェードは既に付いています";
        return result;
    }
    return commitCandidate(project, std::move(candidate), indexOfId(project, clipIds.front()));
}

std::string touchingClipId(const Project& project, const std::string& clipId, TrimEdge edge) {
    const int index = indexOfId(project, clipId);
    if (!validIndex(project, index))
        return {};
    std::string ignored;
    const int neighbor = adjacentClipIndex(project, index, edge, ignored);
    return neighbor >= 0 ? project.timelineClips[static_cast<std::size_t>(neighbor)].id
                         : std::string{};
}

namespace {

// 編集点にトランジションを置く準備。その編集点の既存のトランジションを外し、両 clip のその端の
// フェードを消して、cut の前後に置ける長さの上限 (余白と、clip の反対側の端のトランジションが
// 内側に使っていない分) を求める。
bool prepareEditTransition(Project& candidate, const std::string& outgoingId,
                           const std::string& incomingId, std::int64_t frames,
                           std::int64_t& maxBefore, std::int64_t& maxAfter, std::string& error) {
    std::erase_if(candidate.timelineTransitions, [&](const TimelineTransition& transition) {
        return transition.outgoingClipId == outgoingId || transition.incomingClipId == incomingId;
    });
    const TimelineTransition probe{"probe", outgoingId, incomingId, 0, 0};
    TransitionClips clips;
    if (!resolveTransitionClips(candidate, probe, clips, error))
        return false;
    candidate.timelineClips[static_cast<std::size_t>(clips.outgoing)].effects.fadeOutFrames = 0;
    candidate.timelineClips[static_cast<std::size_t>(clips.incoming)].effects.fadeInFrames = 0;
    std::int64_t outgoingHeadTransition = 0;
    std::int64_t incomingTailInside = 0;
    for (const auto& other : candidate.timelineTransitions) {
        if (other.incomingClipId == outgoingId)
            outgoingHeadTransition = other.framesAfterCut;
        if (other.outgoingClipId == incomingId)
            incomingTailInside = other.framesBeforeCut;
    }
    std::int64_t outgoingHeadInside = 0;
    if (!outgoingHeadUsage(candidate,
                           candidate.timelineClips[static_cast<std::size_t>(clips.outgoing)],
                           outgoingHeadTransition, outgoingHeadInside, error))
        return false;
    maxBefore = std::max<std::int64_t>(
        0, std::min(clips.headHandle, clips.outgoingDuration - outgoingHeadInside));
    maxAfter = std::max<std::int64_t>(
        0, std::min(clips.tailHandle, clips.incomingDuration - incomingTailInside));
    maxBefore = std::min(maxBefore, frames);
    maxAfter = std::min(maxAfter, frames);
    return true;
}

// 同じ link group の相手。無ければ -1。
int linkPartnerIndex(const Project& project, int index) {
    const auto& clip = project.timelineClips[static_cast<std::size_t>(index)];
    if (clip.linkGroupId.empty())
        return -1;
    for (std::size_t other = 0; other < project.timelineClips.size(); ++other) {
        if (static_cast<int>(other) != index &&
            project.timelineClips[other].linkGroupId == clip.linkGroupId)
            return static_cast<int>(other);
    }
    return -1;
}

// トランジションを置く編集点 (outgoing の終端 = incoming の先頭)。
struct EditPoint {
    std::string outgoing;
    std::string incoming;
};

// 編集点と、Linked ならリンク相手どうしが同じ cut で接している編集点 (ずらして置いた音声は
// 別の編集点なので含めない)。
std::vector<EditPoint> linkedEditPoints(const Project& project, const std::string& outgoingId,
                                        const std::string& incomingId, LinkMode linkMode) {
    std::vector<EditPoint> points{{outgoingId, incomingId}};
    if (linkMode != LinkMode::Linked)
        return points;
    const int outgoingIndex = indexOfId(project, outgoingId);
    const int incomingIndex = indexOfId(project, incomingId);
    const int outgoingPartner =
        validIndex(project, outgoingIndex) ? linkPartnerIndex(project, outgoingIndex) : -1;
    const int incomingPartner =
        validIndex(project, incomingIndex) ? linkPartnerIndex(project, incomingIndex) : -1;
    if (outgoingPartner < 0 || incomingPartner < 0)
        return points;
    const TimelineTransition mainProbe{"probe", outgoingId, incomingId, 0, 0};
    const TimelineTransition partnerProbe{
        "probe", project.timelineClips[static_cast<std::size_t>(outgoingPartner)].id,
        project.timelineClips[static_cast<std::size_t>(incomingPartner)].id, 0, 0};
    TransitionClips mainClips;
    TransitionClips partnerClips;
    std::string ignored;
    if (resolveTransitionClips(project, mainProbe, mainClips, ignored) &&
        resolveTransitionClips(project, partnerProbe, partnerClips, ignored) &&
        partnerClips.cut == mainClips.cut)
        points.push_back({partnerProbe.outgoingClipId, partnerProbe.incomingClipId});
    return points;
}

// 全編集点の既存のトランジションを外し (prepareEditTransition)、cut の前後に置ける長さの上限を
// 編集点どうしの小さい方で求める。映像と音声は同じ長さ・同じ cut の前後で置くため。
bool editPointsUpperBounds(Project& prepared, const std::vector<EditPoint>& points,
                           std::int64_t frames, std::int64_t& maxBefore, std::int64_t& maxAfter,
                           std::string& error) {
    maxBefore = frames;
    maxAfter = frames;
    for (const auto& point : points) {
        std::int64_t pointBefore = 0;
        std::int64_t pointAfter = 0;
        if (!prepareEditTransition(prepared, point.outgoing, point.incoming, frames, pointBefore,
                                   pointAfter, error))
            return false;
        maxBefore = std::min(maxBefore, pointBefore);
        maxAfter = std::min(maxAfter, pointAfter);
    }
    return true;
}

struct PointClips {
    TimelineClip outgoing;
    TimelineClip incoming;
    TransitionClips clips;
    TransitionKind kind = TransitionKind::Blend;
    // MathTransform で区間の見た目を揃える値 (cut の両側の frame の見た目)。
    MathTransformLook mathLook{};
};

// 編集点の clip を引く。長さに依らない条件 (Blend は映像の形と区間の端の frame の不透明度、
// MathTransform は数式 clip・Write・背景・cut の両側の見た目) はここで理由を付けて断る。
bool resolvePointClips(const Project& prepared, const std::vector<EditPoint>& points,
                       TransitionKind kind, std::vector<PointClips>& resolved, std::string& error) {
    resolved.clear();
    for (const auto& point : points) {
        const TimelineTransition probe{"probe", point.outgoing, point.incoming, 0, 0, kind};
        PointClips entry;
        entry.kind = kind;
        if (!resolveTransitionClips(prepared, probe, entry.clips, error))
            return false;
        entry.outgoing = prepared.timelineClips[static_cast<std::size_t>(entry.clips.outgoing)];
        entry.incoming = prepared.timelineClips[static_cast<std::size_t>(entry.clips.incoming)];
        // P3 の EquationSequence は内部の時間の正 (P3-1) の外へ素材範囲を延ばせない。Blend・数式の
        // 変形のどちらも作らない (preview と書き出しは同じ構造上の理由で拒否する)。
        for (const auto* clip : {&entry.outgoing, &entry.incoming})
            if (clip->kind == TimelineClipKind::EquationSequence) {
                error = "数式 sequence には timeline のトランジションを置けません "
                        "(内部の変形を使ってください): " +
                        clip->name;
                return false;
            }
        if (kind == TransitionKind::MathTransform) {
            if (!mathTransformClipsEligible(prepared, entry.clips, entry.mathLook, error))
                return false;
        } else if (entry.outgoing.track.kind == TrackKind::Video &&
                   !dissolveClipsEligible(prepared, entry.clips, 0, 0, error)) {
            return false;
        }
        resolved.push_back(std::move(entry));
    }
    return true;
}

// 区間の中の見た目の条件を満たさないときの理由。
std::string spanLookRequirement(TransitionKind kind) {
    return kind == TransitionKind::MathTransform
               ? std::string(kMathTransformLookRequirement) + "区間の中で見た目が変わっています"
               : std::string(kDissolveRequirement) +
                     "トランジションの区間で不透明度が下がっています";
}

// 置ける長さは cut の前 (before) と後 (after) で独立に決まる。
//   before: incoming を cut - before まで延ばせる (素材 frame にちょうど乗る) こと、
//           映像なら outgoing の最後の before frame が不透明であること
//   after : outgoing を cut + after まで延ばせること、映像なら incoming を cut + after で
//           分けられる (lane 1 の区間の終わり) ことと、incoming の最初の after frame
//           が不透明であること
// 長さごとに区間の不透明度を検査し直すと、長尺素材で候補ごとに数十万 frame を辿る (cut の近くに
// 不透明度の下がる frame がある 2 時間の素材で、吸着の探索が 90 秒以上終わらなかった)。不透明度の
// 条件は長さに対して単調 (長いほど厳しい) なので、cut から連続して不透明な長さを最初に 1 回だけ
// 数える。素材 frame の条件は長さごとに判定して覚える。
class SpanFitter {
public:
    SpanFitter(const Project& prepared, const std::vector<PointClips>& resolved,
               std::int64_t maxBefore, std::int64_t maxAfter)
        : prepared_(prepared), resolved_(resolved), maxBefore_(maxBefore), maxAfter_(maxAfter) {}

    std::int64_t maxBefore() const { return maxBefore_; }

    std::int64_t maxAfter() const { return maxAfter_; }

    // [0, maxBefore] の外は置けない。
    bool beforeFits(std::int64_t before, bool checkOpacity) {
        if (before < 0 || before > maxBefore_ || (checkOpacity && before > opaqueBefore()))
            return false;
        const auto known = beforeEdge_.find(before);
        if (known != beforeEdge_.end())
            return known->second;
        ++edgeProbes_;
        return beforeEdge_[before] = beforeEdgeFits(before);
    }

    bool afterFits(std::int64_t after, bool checkOpacity) {
        if (after < 0 || after > maxAfter_ || (checkOpacity && after > opaqueAfter()))
            return false;
        const auto known = afterEdge_.find(after);
        if (known != afterEdge_.end())
            return known->second;
        ++edgeProbes_;
        return afterEdge_[after] = afterEdgeFits(after);
    }

    // cut の前 / 後に置ける、不透明な長さの上限 (余白の上限で頭打ち)。
    std::int64_t opaqueBefore() {
        if (!opaqueBefore_)
            opaqueBefore_ = countOpaque(true);
        return *opaqueBefore_;
    }

    std::int64_t opaqueAfter() {
        if (!opaqueAfter_)
            opaqueAfter_ = countOpaque(false);
        return *opaqueAfter_;
    }

    std::uint64_t opacityProbes() const { return opacityProbes_; }

    std::uint64_t edgeProbes() const { return edgeProbes_; }

private:
    bool beforeEdgeFits(std::int64_t before) const {
        if (before == 0)
            return true;
        std::string ignored;
        for (const auto& entry : resolved_) {
            if (!clipWithEdgeAt(prepared_, entry.incoming, TrimEdge::Left, entry.clips.cut - before,
                                ignored))
                return false;
        }
        return true;
    }

    bool afterEdgeFits(std::int64_t after) const {
        if (after == 0)
            return true;
        std::string ignored;
        for (const auto& entry : resolved_) {
            const auto cut = entry.clips.cut;
            if (!clipWithEdgeAt(prepared_, entry.outgoing, TrimEdge::Right, cut + after, ignored))
                return false;
            // incoming を cut + after で分けるのはクロスディゾルブの lane 1 だけ。
            if (entry.kind == TransitionKind::Blend &&
                entry.outgoing.track.kind == TrackKind::Video &&
                after < entry.clips.incomingDuration &&
                !clipWithEdgeAt(prepared_, entry.incoming, TrimEdge::Right, cut + after, ignored))
                return false;
        }
        return true;
    }

    // 映像の編集点ごとに cut から連続して条件を満たす frame を数え、その最小値を返す。
    // Blend は不透明であること、MathTransform は見た目が cut の両側と等しいこと。
    // cut の前は outgoing の終端から手前へ、後は incoming の先頭から奥へ数える。
    std::int64_t countOpaque(bool beforeCut) {
        std::int64_t limit = beforeCut ? maxBefore_ : maxAfter_;
        std::string ignored;
        for (const auto& entry : resolved_) {
            if (entry.outgoing.track.kind != TrackKind::Video)
                continue;
            const auto& clip = beforeCut ? entry.outgoing : entry.incoming;
            std::int64_t count = 0;
            while (count < limit) {
                const auto local = beforeCut ? entry.clips.outgoingDuration - 1 - count : count;
                ++opacityProbes_;
                const bool fits =
                    entry.kind == TransitionKind::MathTransform
                        ? mathTransformLookConstantOver(prepared_, clip, local, local + 1,
                                                        entry.mathLook, ignored)
                        : dissolveClipOpaqueOver(prepared_, clip, local, local + 1, ignored);
                if (!fits)
                    break;
                ++count;
            }
            limit = count;
        }
        return limit;
    }

    const Project& prepared_;
    const std::vector<PointClips>& resolved_;
    std::int64_t maxBefore_ = 0;
    std::int64_t maxAfter_ = 0;
    std::optional<std::int64_t> opaqueBefore_;
    std::optional<std::int64_t> opaqueAfter_;
    std::unordered_map<std::int64_t, bool> beforeEdge_;
    std::unordered_map<std::int64_t, bool> afterEdge_;
    std::uint64_t opacityProbes_ = 0;
    std::uint64_t edgeProbes_ = 0;
};

// 選んだ長さは描画区間を作れるはずである。作れなければ理由をそのまま返す (黙って縮めない)。
TimelineEditResult commitTransitionTrial(Project& project, Project trial,
                                         const std::string& selectedClipId) {
    TimelineEditResult result;
    std::vector<TimelineRenderSegment> segments;
    const auto valid = validateTimeline(trial);
    if (!valid.success) {
        result.error = valid.error;
        return result;
    }
    if (!timelineRenderSegments(trial, TrackKind::Video, segments, result.error) ||
        !timelineRenderSegments(trial, TrackKind::Audio, segments, result.error))
        return result;
    const int selectedIndex = indexOfId(project, selectedClipId);
    return commitCandidate(project, std::move(trial), selectedIndex);
}

const TimelineTransition* findTransition(const Project& project, const std::string& id) {
    for (const auto& transition : project.timelineTransitions) {
        if (transition.id == id)
            return &transition;
    }
    return nullptr;
}

// 既存のトランジションの長さを変える編集点。Linked ならリンク相手の編集点のうち、既に
// トランジションがあるものだけを含める (長さの変更で新しく作らない)。
std::vector<EditPoint> spanEditPoints(const Project& project, const TimelineTransition& transition,
                                      LinkMode linkMode) {
    auto points =
        linkedEditPoints(project, transition.outgoingClipId, transition.incomingClipId, linkMode);
    std::erase_if(points, [&](const EditPoint& point) {
        return std::none_of(project.timelineTransitions.begin(), project.timelineTransitions.end(),
                            [&](const TimelineTransition& other) {
                                return other.outgoingClipId == point.outgoing &&
                                       other.incomingClipId == point.incoming;
                            });
    });
    return points;
}

} // namespace

TransitionEditResult applyDefaultEditTransition(Project& project, const std::string& outgoingId,
                                                const std::string& incomingId,
                                                std::int64_t timelineFrames, LinkMode linkMode,
                                                const std::function<std::string()>& newId) {
    TransitionEditResult result;
    if (timelineFrames < 1 || !newId) {
        result.error = "トランジションの長さが不正です";
        return result;
    }

    const auto points = linkedEditPoints(project, outgoingId, incomingId, linkMode);
    Project prepared = project;
    std::int64_t maxBefore = 0;
    std::int64_t maxAfter = 0;
    if (!editPointsUpperBounds(prepared, points, timelineFrames, maxBefore, maxAfter, result.error))
        return result;
    std::vector<PointClips> resolved;
    if (!resolvePointClips(prepared, points, TransitionKind::Blend, resolved, result.error))
        return result;
    // before と after のそれぞれで置ける長さを求めてから、合計が最大で cut に最も近い中央の
    // 組を選ぶ。
    SpanFitter fitter(prepared, resolved, maxBefore, maxAfter);
    const auto choose = [&](bool checkOpacity, std::int64_t& before, std::int64_t& after) {
        std::vector<bool> beforeOk(static_cast<std::size_t>(maxBefore) + 1);
        std::vector<bool> afterOk(static_cast<std::size_t>(maxAfter) + 1);
        for (std::int64_t value = 0; value <= maxBefore; ++value)
            beforeOk[static_cast<std::size_t>(value)] = fitter.beforeFits(value, checkOpacity);
        for (std::int64_t value = 0; value <= maxAfter; ++value)
            afterOk[static_cast<std::size_t>(value)] = fitter.afterFits(value, checkOpacity);
        for (std::int64_t total = std::min(timelineFrames, maxBefore + maxAfter); total >= 1;
             --total) {
            // 合計 total の組のうち、cut を中央に置く組から順に試す。
            for (std::int64_t offset = 0; offset <= total; ++offset) {
                for (const std::int64_t candidate :
                     {total / 2 + offset, total / 2 - offset, (total + 1) / 2 + offset}) {
                    const std::int64_t other = total - candidate;
                    if (candidate < 0 || other < 0 || candidate > maxBefore || other > maxAfter ||
                        !beforeOk[static_cast<std::size_t>(candidate)] ||
                        !afterOk[static_cast<std::size_t>(other)])
                        continue;
                    before = candidate;
                    after = other;
                    return true;
                }
            }
        }
        return false;
    };
    std::int64_t before = 0;
    std::int64_t after = 0;
    if (!choose(true, before, after)) {
        // 不透明度を見なければ置けるなら、余白ではなく不透明度が理由である。
        std::int64_t ignoredBefore = 0;
        std::int64_t ignoredAfter = 0;
        result.error = choose(false, ignoredBefore, ignoredAfter)
                           ? spanLookRequirement(TransitionKind::Blend)
                           : "素材の余白が足りないためトランジションを作れません";
        return result;
    }
    Project trial = prepared;
    std::string firstId;
    for (const auto& point : points) {
        const std::string id = newId();
        if (id.empty()) {
            result.error = "トランジションの ID を作れません";
            return result;
        }
        if (firstId.empty())
            firstId = id;
        trial.timelineTransitions.push_back({id, point.outgoing, point.incoming, before, after});
    }
    const auto committed = commitTransitionTrial(project, std::move(trial), incomingId);
    if (!committed.success) {
        result.error = committed.error;
        return result;
    }
    result.success = true;
    result.transitionId = firstId;
    result.frames = before + after;
    result.transitionCount = static_cast<int>(points.size());
    return result;
}

namespace {
std::int64_t nearestInRange(std::int64_t target, std::int64_t lower, std::int64_t upper,
                            std::int64_t towards, const std::function<bool(std::int64_t)>& fits);
} // namespace

TransitionEditResult applyMathTransformTransition(Project& project, const std::string& outgoingId,
                                                  const std::string& incomingId,
                                                  std::int64_t timelineFrames,
                                                  const std::function<std::string()>& newId) {
    TransitionEditResult result;
    if (timelineFrames < 1 || !newId) {
        result.error = "数式の変形の長さが不正です";
        return result;
    }
    // 数式 clip はリンクを持たないので、編集点は 1 つだけ。
    const std::vector<EditPoint> points{{outgoingId, incomingId}};
    Project prepared = project;
    std::int64_t maxBefore = 0;
    std::int64_t maxAfter = 0;
    if (!editPointsUpperBounds(prepared, points, timelineFrames, maxBefore, maxAfter, result.error))
        return result;
    std::vector<PointClips> resolved;
    if (!resolvePointClips(prepared, points, TransitionKind::MathTransform, resolved, result.error))
        return result;
    // cut から始める (incoming を置いた所から変形が始まる)。incoming が短い・区間の中で見た目が
    // 変わるなら、その手前まで縮める。
    SpanFitter fitter(prepared, resolved, maxBefore, maxAfter);
    const auto after = nearestInRange(maxAfter, 1, maxAfter, -1, [&](std::int64_t value) {
        return fitter.afterFits(value, true);
    });
    if (after < 1) {
        result.error = maxAfter < 1
                           ? std::string("数式の変形に使える後ろの数式 clip の尺がありません")
                           : spanLookRequirement(TransitionKind::MathTransform);
        return result;
    }
    const std::string id = newId();
    if (id.empty()) {
        result.error = "数式の変形の ID を作れません";
        return result;
    }
    Project trial = prepared;
    trial.timelineTransitions.push_back(
        {id, outgoingId, incomingId, 0, after, TransitionKind::MathTransform});
    const auto committed = commitTransitionTrial(project, std::move(trial), incomingId);
    if (!committed.success) {
        result.error = committed.error;
        return result;
    }
    result.success = true;
    result.transitionId = id;
    result.frames = after;
    result.transitionCount = 1;
    return result;
}

TransitionSpanLimits transitionSpanLimits(const Project& project, const std::string& transitionId,
                                          LinkMode linkMode) {
    TransitionSpanLimits result;
    const auto* transition = findTransition(project, transitionId);
    if (!transition) {
        result.error = "トランジションがありません";
        return result;
    }
    Project prepared = project;
    if (!editPointsUpperBounds(prepared, spanEditPoints(project, *transition, linkMode),
                               std::numeric_limits<std::int64_t>::max(), result.maxBefore,
                               result.maxAfter, result.error))
        return result;
    result.success = true;
    return result;
}

namespace {

// [lower, upper] のうち fits を満たし target に最も近い値。同じ距離なら towards (+1 / -1) の向きを
// 選ぶ。無ければ -1。
std::int64_t nearestInRange(std::int64_t target, std::int64_t lower, std::int64_t upper,
                            std::int64_t towards, const std::function<bool(std::int64_t)>& fits) {
    if (lower > upper)
        return -1;
    target = std::clamp(target, lower, upper);
    for (std::int64_t distance = 0;; ++distance) {
        const std::int64_t first = target + towards * distance;
        const std::int64_t second = target - towards * distance;
        const bool firstInside = first >= lower && first <= upper;
        const bool secondInside = second >= lower && second <= upper;
        if (!firstInside && !secondInside)
            return -1;
        if (firstInside && fits(first))
            return first;
        if (secondInside && fits(second))
            return second;
    }
}

std::int64_t towardsChange(std::int64_t requested, std::int64_t current) {
    return requested >= current ? 1 : -1;
}

} // namespace

TransitionSpanFit nearestTransitionSpan(const Project& project, const std::string& transitionId,
                                        std::int64_t framesBeforeCut, std::int64_t framesAfterCut,
                                        SpanFitMode mode, LinkMode linkMode) {
    TransitionSpanFit result;
    const auto* transition = findTransition(project, transitionId);
    if (!transition) {
        result.error = "トランジションがありません";
        return result;
    }
    const auto points = spanEditPoints(project, *transition, linkMode);
    Project prepared = project;
    std::int64_t maxBefore = 0;
    std::int64_t maxAfter = 0;
    std::vector<PointClips> resolved;
    if (!editPointsUpperBounds(prepared, points, std::numeric_limits<std::int64_t>::max(),
                               maxBefore, maxAfter, result.error) ||
        !resolvePointClips(prepared, points, transition->kind, resolved, result.error))
        return result;
    SpanFitter fitter(prepared, resolved, maxBefore, maxAfter);
    const auto beforeFits = [&](std::int64_t value) { return fitter.beforeFits(value, true); };
    const auto afterFits = [&](std::int64_t value) { return fitter.afterFits(value, true); };
    // 不透明度の上限より外は置けないので、探索をその内側に限る。
    const std::int64_t beforeLimit = std::min(maxBefore, fitter.opaqueBefore());
    const std::int64_t afterLimit = std::min(maxAfter, fitter.opaqueAfter());
    const std::int64_t currentBefore = transition->framesBeforeCut;
    const std::int64_t currentAfter = transition->framesAfterCut;
    std::int64_t before = -1;
    std::int64_t after = -1;
    if (mode == SpanFitMode::EachSide) {
        // 片側の端のドラッグ。動かさなかった側は今の値のまま (置けるので変わらない)。
        before = nearestInRange(framesBeforeCut, 0, beforeLimit,
                                towardsChange(framesBeforeCut, currentBefore), beforeFits);
        after = nearestInRange(framesAfterCut, 0, afterLimit,
                               towardsChange(framesAfterCut, currentAfter), afterFits);
    } else {
        // 長さ・配置・本体のドラッグ。総尺を第一に保ち、その総尺の組のうち cut の前が指定に
        // 最も近いものを選ぶ。指定の総尺で置けなければ、最も近い総尺へ落とす。
        const auto topBefore = nearestInRange(beforeLimit, 0, beforeLimit, -1, beforeFits);
        const auto topAfter = nearestInRange(afterLimit, 0, afterLimit, -1, afterFits);
        const std::int64_t requestedTotal = framesBeforeCut + framesAfterCut;
        nearestInRange(
            requestedTotal, 1, topBefore + topAfter,
            towardsChange(requestedTotal, currentBefore + currentAfter), [&](std::int64_t total) {
                const auto chosen = nearestInRange(
                    framesBeforeCut, std::max<std::int64_t>(0, total - topAfter),
                    std::min(topBefore, total), towardsChange(framesBeforeCut, currentBefore),
                    [&](std::int64_t value) {
                        return beforeFits(value) && afterFits(total - value);
                    });
                if (chosen < 0)
                    return false;
                before = chosen;
                after = total - chosen;
                return true;
            });
    }
    result.opacityProbes = fitter.opacityProbes();
    result.edgeProbes = fitter.edgeProbes();
    if (before < 0 || after < 0 || before + after < 1) {
        result.error = "素材の余白と不透明度の範囲に置ける長さがありません";
        return result;
    }
    result.success = true;
    result.framesBeforeCut = before;
    result.framesAfterCut = after;
    return result;
}

TransitionEditResult setTimelineTransitionSpan(Project& project, const std::string& transitionId,
                                               std::int64_t framesBeforeCut,
                                               std::int64_t framesAfterCut, LinkMode linkMode) {
    TransitionEditResult result;
    const auto* transition = findTransition(project, transitionId);
    if (!transition) {
        result.error = "変更するトランジションがありません";
        return result;
    }
    if (framesBeforeCut < 0 || framesAfterCut < 0 ||
        framesBeforeCut > std::numeric_limits<std::int64_t>::max() - framesAfterCut ||
        framesBeforeCut + framesAfterCut < 1) {
        result.error = "トランジションの長さは 1 フレーム以上にしてください";
        return result;
    }
    if (transition->framesBeforeCut == framesBeforeCut &&
        transition->framesAfterCut == framesAfterCut) {
        result.error = "トランジションの長さは変わっていません";
        return result;
    }
    const auto points = spanEditPoints(project, *transition, linkMode);
    Project prepared = project;
    std::int64_t maxBefore = 0;
    std::int64_t maxAfter = 0;
    if (!editPointsUpperBounds(prepared, points, std::numeric_limits<std::int64_t>::max(),
                               maxBefore, maxAfter, result.error))
        return result;
    if (framesBeforeCut > maxBefore || framesAfterCut > maxAfter) {
        result.error = "素材の余白が足りません (cut の前は最大 " + std::to_string(maxBefore) +
                       "、後は最大 " + std::to_string(maxAfter) + " フレーム)";
        return result;
    }
    std::vector<PointClips> resolved;
    if (!resolvePointClips(prepared, points, transition->kind, resolved, result.error))
        return result;
    // 余白の内側でも、素材 frame に乗らない長さ (速度変更) と不透明度の下がる区間は断る。
    // 置ける長さへ黙って丸めない。
    SpanFitter fitter(prepared, resolved, maxBefore, maxAfter);
    if (!fitter.beforeFits(framesBeforeCut, false) || !fitter.afterFits(framesAfterCut, false)) {
        result.error = "この長さは素材のフレームに合わないため設定できません";
        return result;
    }
    if (!fitter.beforeFits(framesBeforeCut, true) || !fitter.afterFits(framesAfterCut, true)) {
        result.error = spanLookRequirement(transition->kind);
        return result;
    }
    // ID と並び順を保つため、元の project の中で値だけを置き換える。
    Project trial = project;
    for (auto& entry : trial.timelineTransitions) {
        const bool target = std::any_of(points.begin(), points.end(), [&](const EditPoint& point) {
            return entry.outgoingClipId == point.outgoing && entry.incomingClipId == point.incoming;
        });
        if (!target)
            continue;
        entry.framesBeforeCut = framesBeforeCut;
        entry.framesAfterCut = framesAfterCut;
    }
    // transition は project の中を指すので、commit で置き換わる前に複写しておく。
    const std::string incomingId = transition->incomingClipId;
    const auto committed = commitTransitionTrial(project, std::move(trial), incomingId);
    if (!committed.success) {
        result.error = committed.error;
        return result;
    }
    result.success = true;
    result.transitionId = transitionId;
    result.frames = framesBeforeCut + framesAfterCut;
    result.transitionCount = static_cast<int>(points.size());
    return result;
}

TimelineEditResult deleteTimelineTransition(Project& project, const std::string& transitionId) {
    TimelineEditResult result;
    Project candidate = project;
    const auto removed =
        std::erase_if(candidate.timelineTransitions, [&](const TimelineTransition& transition) {
            return transition.id == transitionId;
        });
    if (removed == 0) {
        result.error = "削除するトランジションがありません";
        return result;
    }
    return commitCandidate(project, std::move(candidate), -1);
}

struct RippleSource {
    TrackRef track;
    std::int64_t originalEnd = 0;
};

namespace {
// 候補内でも衝突しないIDを作る。時間編集は呼び出し元のID生成器に依存しない。
std::string timeEditId(const Project& project, const std::string& base) {
    for (std::uint64_t n = 1;; ++n) {
        const auto id = base + "-time-" + std::to_string(n);
        bool used = false;
        for (const auto& clip : project.timelineClips)
            used = used || clip.id == id || clip.linkGroupId == id;
        if (project.subtitles)
            for (const auto& cue : project.subtitles->cues)
                used = used || cue.id == id;
        if (!used)
            return id;
    }
}

bool timeEditCandidate(Project& candidate, std::int64_t start, std::int64_t removed,
                       std::int64_t inserted, const std::unordered_set<std::string>& excluded,
                       std::string& error) {
    if (start < 0 || removed < 0 || inserted < 0 ||
        start > std::numeric_limits<std::int64_t>::max() - removed) {
        error = "時間編集の区間が不正です";
        return false;
    }
    const auto end = start + removed;
    if (removed == 0 && inserted == 0)
        return true;
    std::vector<TimelineClip> output;
    std::unordered_map<std::string, std::string> rightGroups;
    // 分割した clip の系譜 (元の ID → 右側の新しい ID と、時間編集後の右側の開始位置)。
    // 字幕は ID ではなくこの系譜で、右側の音声にあたる字幕を右側の clip へ付け替える。
    std::vector<std::tuple<std::string, std::string, std::int64_t>> lineage;
    for (const auto& original : candidate.timelineClips) {
        std::int64_t a = 0, b = 0;
        if (!clipInterval(candidate, original, a, b, error))
            return false;
        if (excluded.contains(original.id) || b <= start) {
            output.push_back(original);
            continue;
        }
        if (a < start) {
            auto left = original;
            if (!trimClipBoundary(candidate, left, TrimEdge::Right, start - b, error))
                return false;
            std::int64_t leftStart = 0, leftEnd = 0;
            if (!clipInterval(candidate, left, leftStart, leftEnd, error))
                return false;
            if (leftStart != a || leftEnd != start) {
                error = "時間編集の境界を素材フレームで正確に表現できません";
                return false;
            }
            left.effects.fadeOutFrames = 0;
            clampFadesToLength(left);
            output.push_back(std::move(left));
        }
        if (b > end) {
            auto right = original;
            if (a < end && !trimClipBoundary(candidate, right, TrimEdge::Left, end - a, error))
                return false;
            std::int64_t rightStart = 0, rightEnd = 0;
            if (!clipInterval(candidate, right, rightStart, rightEnd, error))
                return false;
            if (rightStart != std::max(a, end) || rightEnd != b) {
                error = "時間編集の境界を素材フレームで正確に表現できません";
                return false;
            }
            if (a < start) {
                right.id = timeEditId(candidate, original.id);
                if (right.kind == TimelineClipKind::EquationSequence) {
                    std::uint64_t serial = 0;
                    if (!remapEquationSequenceIds(
                            right.equationSequence,
                            [&] { return right.id + "-internal-" + std::to_string(++serial); },
                            error))
                        return false;
                }
                // 同じリンクの両半分に同じ新規グループを割り当てる。
                if (!original.linkGroupId.empty()) {
                    auto& group = rightGroups[original.linkGroupId];
                    if (group.empty())
                        group = timeEditId(candidate, original.linkGroupId);
                    right.linkGroupId = group;
                }
                for (auto& transition : candidate.timelineTransitions)
                    if (transition.outgoingClipId == original.id)
                        transition.outgoingClipId = right.id;
            }
            const WideInteger shifted =
                static_cast<WideInteger>(right.timelineStartFrame) - removed + inserted;
            if (shifted < 0 || shifted > std::numeric_limits<std::int64_t>::max()) {
                error = "クリップの移動先が範囲外です";
                return false;
            }
            right.timelineStartFrame = static_cast<std::int64_t>(shifted);
            if (a < end) {
                right.effects.fadeInFrames = 0;
                right.mathAnimation = {};
                clampFadesToLength(right);
            }
            if (right.id != original.id)
                lineage.emplace_back(original.id, right.id, right.timelineStartFrame);
            output.push_back(std::move(right));
        }
    }
    candidate.timelineClips = std::move(output);
    // 削除区間に丸ごと入って消えた clip へのリンクを外す (字幕は残す)。字幕の時間編集は
    // 検証まで行うので、その前に外さないと、区間の外へはみ出した字幕が 1 つあるだけで
    // 時間編集全体が失敗する。二分した clip は左側が元の ID のまま残るので外れない。
    reconcileSubtitleLinks(candidate);
    std::unordered_map<std::string, int> counts;
    for (const auto& clip : candidate.timelineClips)
        if (!clip.linkGroupId.empty())
            ++counts[clip.linkGroupId];
    for (auto& clip : candidate.timelineClips)
        if (!clip.linkGroupId.empty() && counts[clip.linkGroupId] != 2)
            clip.linkGroupId.clear();
    std::uint64_t cueId = 0;
    const auto newId = [&] {
        return timeEditId(candidate, "subtitle-ripple-" + std::to_string(++cueId));
    };
    if (!editSubtitleTime(candidate, start, removed, inserted, newId, error))
        return false;
    // 時間編集は境界を跨ぐ字幕も境界で分けているので、右側の clip の開始以降に始まる字幕が
    // 右側の音声にあたる。
    if (candidate.subtitles)
        for (const auto& [originalId, rightId, rightStart] : lineage)
            for (auto& cue : candidate.subtitles->cues)
                if (cue.linkClipId == originalId && cue.startFrame >= rightStart)
                    cue.linkClipId = rightId;
    return true;
}
} // namespace

TimelineEditResult editTimelineTime(Project& project, std::int64_t start, std::int64_t removed,
                                    std::int64_t inserted) {
    Project candidate = project;
    std::string error;
    if (!timeEditCandidate(candidate, start, removed, inserted, {}, error)) {
        TimelineEditResult result;
        result.error = error;
        return result;
    }
    return commitCandidate(project, std::move(candidate), -1);
}

bool shiftFollowingClips(Project& candidate, const std::vector<RippleSource>& sources,
                         const std::vector<int>& targets, int index, std::int64_t shift,
                         std::string& error) {
    if (shift == 0)
        return true;
    std::unordered_set<std::string> excluded;
    for (int target : targets)
        excluded.insert(candidate.timelineClips[static_cast<std::size_t>(target)].id);
    const auto boundary = sources.front().originalEnd;
    (void)index;
    return timeEditCandidate(candidate, shift < 0 ? boundary + shift : boundary,
                             shift < 0 ? -shift : 0, shift > 0 ? shift : 0, excluded, error);
}

TimelineEditResult rippleTrimTimelineClip(Project& project, const std::string& clipId,
                                          TrimEdge edge, std::int64_t projectFrameDelta,
                                          LinkMode linkMode) {
    TimelineEditResult result;
    Project candidate = project;
    const int index = indexOfId(candidate, clipId);
    if (!validIndex(candidate, index)) {
        result.error = "リップルトリムする timeline clip がありません";
        return result;
    }
    if (!clampedEdgeDelta(candidate, clipId, edge, EdgeEditKind::Ripple, projectFrameDelta,
                          linkMode, projectFrameDelta, result.error))
        return result;
    const std::vector<int> targets = editTargets(candidate, index, linkMode);

    // 操作対象の尺を変えてから、元の終端で全トラックへ時間の増減を波及させる。
    std::vector<RippleSource> sources;
    std::int64_t shift = 0;
    for (std::size_t order = 0; order < targets.size(); ++order) {
        auto& clip = candidate.timelineClips[static_cast<std::size_t>(targets[order])];
        std::int64_t originalStart = 0;
        std::int64_t originalEnd = 0;
        if (!clipInterval(candidate, clip, originalStart, originalEnd, result.error) ||
            !trimClipBoundary(candidate, clip, edge, projectFrameDelta, result.error))
            return result;
        // left 端を動かしても clip は元の開始位置に留め、尺の変化を後ろへ波及させる。
        clip.timelineStartFrame = originalStart;
        std::int64_t newStart = 0;
        std::int64_t newEnd = 0;
        if (!clipInterval(candidate, clip, newStart, newEnd, result.error))
            return result;
        // リンク相手と後ろのずれ方が違うと同期が崩れる。素材 fps の丸めで食い違うなら拒否する。
        if (order == 0)
            shift = newEnd - originalEnd;
        else if (newEnd - originalEnd != shift) {
            result.error = "リンク相手と尺の変化量が一致しないためリップルトリムできません";
            return result;
        }
        sources.push_back({clip.track, originalEnd});
    }

    if (!shiftFollowingClips(candidate, sources, targets, index, shift, result.error))
        return result;
    const int selected = indexOfId(candidate, clipId);
    return commitMappingEdit(project, std::move(candidate), selected);
}

TimelineEditResult rollTimelineEdit(Project& project, const std::string& clipId, TrimEdge edge,
                                    std::int64_t projectFrameDelta, LinkMode linkMode) {
    TimelineEditResult result;
    Project candidate = project;
    const int index = indexOfId(candidate, clipId);
    if (!validIndex(candidate, index)) {
        result.error = "ローリング編集する timeline clip がありません";
        return result;
    }
    if (!clampedEdgeDelta(candidate, clipId, edge, EdgeEditKind::Roll, projectFrameDelta, linkMode,
                          projectFrameDelta, result.error))
        return result;
    const std::vector<int> targets = editTargets(candidate, index, linkMode);
    for (std::size_t order = 0; order < targets.size(); ++order) {
        bool neighborFound = false;
        if (!rollEditPoint(candidate, targets[order], edge, projectFrameDelta, neighborFound,
                           result.error))
            return result;
        // 操作した clip には接している clip が必要。リンク相手は L カット / J カットで
        // 編集点を持たないことがあり、その場合は相手の編集点を動かさない。
        if (order == 0 && !neighborFound) {
            result.error = "ローリング編集には接している隣の clip が必要です";
            return result;
        }
    }
    return commitMappingEdit(project, std::move(candidate), index);
}

TimelineEditResult slipTimelineClip(Project& project, const std::string& clipId,
                                    std::int64_t projectFrameDelta, LinkMode linkMode) {
    TimelineEditResult result;
    Project candidate = project;
    const int index = indexOfId(candidate, clipId);
    if (!validIndex(candidate, index)) {
        result.error = "スリップする timeline clip がありません";
        return result;
    }
    if (hasSyntheticSourceDomain(candidate.timelineClips[static_cast<std::size_t>(index)])) {
        result.error = "text / image clip は素材位置を持たないためスリップできません";
        return result;
    }
    const std::vector<int> targets = editTargets(candidate, index, linkMode);
    // リンク相手と同じ量だけずらさないと同期が崩れる。全員がずらせる範囲へ先に丸める。
    std::int64_t lower = std::numeric_limits<std::int64_t>::min();
    std::int64_t upper = std::numeric_limits<std::int64_t>::max();
    for (const int target : targets) {
        std::int64_t clipLower = 0;
        std::int64_t clipUpper = 0;
        if (!slipRange(candidate, candidate.timelineClips[static_cast<std::size_t>(target)],
                       clipLower, clipUpper, result.error))
            return result;
        lower = std::max(lower, clipLower);
        upper = std::min(upper, clipUpper);
    }
    const std::int64_t delta = std::clamp(projectFrameDelta, std::min(lower, upper), upper);
    bool anyMoved = false;
    for (const int target : targets) {
        bool moved = false;
        if (!slipClipSource(candidate, candidate.timelineClips[static_cast<std::size_t>(target)],
                            delta, moved, result.error))
            return result;
        anyMoved = anyMoved || moved;
    }
    if (!anyMoved) {
        result.error = "素材の端に達しているためスリップできません";
        return result;
    }
    return commitMappingEdit(project, std::move(candidate), index);
}

TimelineFrameResult clampSlideEdit(const Project& project, const std::string& clipId,
                                   std::int64_t projectFrameDelta, LinkMode linkMode) {
    TimelineFrameResult result;
    const int index = indexOfId(project, clipId);
    if (!validIndex(project, index)) {
        result.error = "スライドする timeline clip がありません";
        return result;
    }
    const std::vector<int> targets = editTargets(project, index, linkMode);
    std::int64_t lower = std::numeric_limits<std::int64_t>::min();
    std::int64_t upper = std::numeric_limits<std::int64_t>::max();
    for (const int target : targets) {
        const auto& clip = project.timelineClips[static_cast<std::size_t>(target)];
        std::int64_t start = 0;
        std::int64_t end = 0;
        if (!clipInterval(project, clip, start, end, result.error))
            return result;
        const int previous = adjacentClipIndex(project, target, TrimEdge::Left, result.error);
        if (!result.error.empty())
            return result;
        const int next = adjacentClipIndex(project, target, TrimEdge::Right, result.error);
        if (!result.error.empty())
            return result;
        // スライドは前後の編集点を保ったまま中身だけを動かす編集である。操作した clip に
        // 接している前後の clip が無ければ単なる移動になるので拒否する。リンク相手は
        // L / J カットで編集点を持たないことがあり、その場合は相手の前後を追従させない。
        if (target == index && (previous < 0 || next < 0)) {
            result.error =
                "スライドには前後に接している clip が必要です (移動は選択ツールで行ってください)";
            return result;
        }
        lower = std::max(lower, -start);
        std::int64_t neighborLower = 0;
        std::int64_t neighborUpper = 0;
        // 前の clip は out を、後ろの clip は in を同じ量だけ動かす。どちらも素材の範囲と
        // 1 frame 以上の尺を保てる量までに止める。
        if (previous >= 0) {
            if (!edgeRange(project, project.timelineClips[static_cast<std::size_t>(previous)],
                           TrimEdge::Right, neighborLower, neighborUpper, result.error))
                return result;
            narrowRange(lower, upper, neighborLower, neighborUpper);
        }
        if (next >= 0) {
            if (!edgeRange(project, project.timelineClips[static_cast<std::size_t>(next)],
                           TrimEdge::Left, neighborLower, neighborUpper, result.error))
                return result;
            narrowRange(lower, upper, neighborLower, neighborUpper);
        }
        // 接していない同じ track の clip とは、間の空白の分だけしか動けない。
        for (std::size_t other = 0; other < project.timelineClips.size(); ++other) {
            const auto& blocker = project.timelineClips[other];
            if (static_cast<int>(other) == previous || static_cast<int>(other) == next ||
                isTarget(targets, other) || !(blocker.track == clip.track))
                continue;
            std::int64_t blockerStart = 0;
            std::int64_t blockerEnd = 0;
            if (!clipInterval(project, blocker, blockerStart, blockerEnd, result.error))
                return result;
            if (blockerEnd <= start)
                lower = std::max(lower, blockerEnd - start);
            else if (blockerStart >= end)
                upper = std::min(upper, blockerStart - end);
        }
    }
    result.success = true;
    result.frame = lower > upper ? 0 : std::clamp(projectFrameDelta, lower, upper);
    return result;
}

TimelineEditResult slideTimelineClip(Project& project, const std::string& clipId,
                                     std::int64_t projectFrameDelta, LinkMode linkMode) {
    TimelineEditResult result;
    Project candidate = project;
    const int index = indexOfId(candidate, clipId);
    if (!validIndex(candidate, index)) {
        result.error = "スライドする timeline clip がありません";
        return result;
    }
    const auto clamped = clampSlideEdit(candidate, clipId, projectFrameDelta, linkMode);
    if (!clamped.success) {
        result.error = clamped.error;
        return result;
    }
    if (clamped.frame == 0) {
        result.error = "前後の clip の素材の端に達しているため、これ以上スライドできません";
        return result;
    }
    projectFrameDelta = clamped.frame;
    const std::vector<int> targets = editTargets(candidate, index, linkMode);
    std::vector<bool> slid(candidate.timelineClips.size(), false);
    for (const int target : targets)
        slid[static_cast<std::size_t>(target)] = true;

    // 隣接関係は移動前の配置で決める。全部決めてから書き換える。
    struct Neighbors {
        int previous = -1;
        int next = -1;
    };

    std::vector<Neighbors> neighbors(candidate.timelineClips.size());
    for (std::size_t slide = 0; slide < slid.size(); ++slide) {
        if (!slid[slide])
            continue;
        neighbors[slide].previous =
            adjacentClipIndex(candidate, static_cast<int>(slide), TrimEdge::Left, result.error);
        if (!result.error.empty())
            return result;
        neighbors[slide].next =
            adjacentClipIndex(candidate, static_cast<int>(slide), TrimEdge::Right, result.error);
        if (!result.error.empty())
            return result;
    }
    for (std::size_t slide = 0; slide < slid.size(); ++slide) {
        if (!slid[slide])
            continue;
        if (!shiftStart(candidate.timelineClips[slide], projectFrameDelta, result.error))
            return result;
        const int previous = neighbors[slide].previous;
        if (previous >= 0 && !slid[static_cast<std::size_t>(previous)] &&
            !trimClipBoundary(candidate,
                              candidate.timelineClips[static_cast<std::size_t>(previous)],
                              TrimEdge::Right, projectFrameDelta, result.error))
            return result;
        const int next = neighbors[slide].next;
        if (next >= 0 && !slid[static_cast<std::size_t>(next)] &&
            !trimClipBoundary(candidate, candidate.timelineClips[static_cast<std::size_t>(next)],
                              TrimEdge::Left, projectFrameDelta, result.error))
            return result;
    }
    return commitMappingEdit(project, std::move(candidate), index);
}

std::vector<std::string> clipIdsFromFrame(const Project& project, std::int64_t frame,
                                          SelectDirection direction,
                                          std::optional<TrackRef> track) {
    std::vector<std::string> ids;
    for (const auto& clip : project.timelineClips) {
        if (track && !(clip.track == *track))
            continue;
        std::int64_t start = 0;
        std::int64_t end = 0;
        std::string ignored;
        if (!clipInterval(project, clip, start, end, ignored))
            continue;
        if (direction == SelectDirection::Forward ? end > frame : start <= frame)
            ids.push_back(clip.id);
    }
    return ids;
}

TimelineEditResult appendManimTimelineClipAt(Project& project, const ManimAsset& asset,
                                             std::string clipId, std::int64_t sourceFpsNum,
                                             std::int64_t sourceFpsDen,
                                             std::int64_t sourceFrameCount,
                                             std::int64_t timelineStartFrame, TrackRef track) {
    TimelineEditResult result;
    if (asset.generatedVideoPath.empty() || asset.sceneName.empty() || clipId.empty()) {
        result.error = "timeline に配置できる生成済み Manim asset ではありません";
        return result;
    }
    if (!isValidTrackRef(project, track) || track.kind != TrackKind::Video) {
        result.error = "Manim clip の配置先 video track が存在しません";
        return result;
    }
    const auto existing =
        std::find_if(project.timelineClips.begin(), project.timelineClips.end(),
                     [](const auto& clip) { return clip.kind == TimelineClipKind::Manim; });
    if (existing != project.timelineClips.end()) {
        result.error = "Manim asset はすでに timeline に配置されています";
        return result;
    }
    Project candidate = project;
    candidate.timelineClips.push_back({TimelineClipKind::Manim,
                                       asset.generatedVideoPath,
                                       asset.sceneName,
                                       std::move(clipId),
                                       {}, // Manim は素材 (mediaItemId) を指さない
                                       sourceFpsNum,
                                       sourceFpsDen,
                                       sourceFrameCount,
                                       0,
                                       sourceFrameCount,
                                       timelineStartFrame,
                                       {},
                                       track,
                                       {},
                                       1,
                                       1,
                                       false,
                                       {},
                                       {}});
    const auto valid = finalizeTimelineCandidate(candidate);
    if (!valid.success) {
        result.error = valid.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    result.selectedIndex = static_cast<int>(project.timelineClips.size()) - 1;
    return result;
}

TimelineEditResult setTimelineFrameRate(Project& project, std::int64_t fpsNum,
                                        std::int64_t fpsDen) {
    return setProjectVideoSettings(project, project.outputWidth, project.outputHeight, fpsNum,
                                   fpsDen);
}

TimelineEditResult setProjectVideoSettings(Project& project, int width, int height,
                                           std::int64_t fpsNum, std::int64_t fpsDen) {
    TimelineEditResult result;
    if (!isValidProjectOutputSize(width, height)) {
        result.error = "Project output size が不正です";
        return result;
    }
    if (!isConfigurableTimelineFrameRate(fpsNum, fpsDen) || !isCanonicalFrameRate(fpsNum, fpsDen)) {
        result.error = "対応していない timeline frame rate です";
        return result;
    }
    if (project.outputWidth == width && project.outputHeight == height &&
        project.timelineFpsNum == fpsNum && project.timelineFpsDen == fpsDen) {
        result.success = true;
        return result;
    }
    Project candidate = project;
    if (project.timelineFpsNum != fpsNum || project.timelineFpsDen != fpsDen) {
        const auto convertFrame = [&](std::int64_t& frame) {
            const auto converted = sourceBoundaryToTimelineBoundary(
                frame, project.timelineFpsNum, project.timelineFpsDen, fpsNum, fpsDen);
            if (!converted.success)
                return false;
            frame = converted.frame;
            return true;
        };
        if (candidate.subtitles)
            for (auto& cue : candidate.subtitles->cues) {
                const auto convertSubtitleFrame = [&](std::int64_t& frame) {
                    const WideInteger num =
                        static_cast<WideInteger>(frame) * project.timelineFpsDen * fpsNum;
                    const WideInteger den =
                        static_cast<WideInteger>(project.timelineFpsNum) * fpsDen;
                    const auto value = (num + den / 2) / den;
                    if (value > std::numeric_limits<std::int64_t>::max())
                        return false;
                    frame = static_cast<std::int64_t>(value);
                    return true;
                };
                if (!convertSubtitleFrame(cue.startFrame) || !convertSubtitleFrame(cue.endFrame)) {
                    result.error = "字幕を新しいFPSへ換算できません";
                    return result;
                }
            }
        for (auto& marker : candidate.timelineMarkers) {
            if (!convertFrame(marker)) {
                result.error = "マーカーを新しいtimeline frame rateへ変換できません";
                return result;
            }
        }
        candidate.timelineMarkers.erase(
            std::unique(candidate.timelineMarkers.begin(), candidate.timelineMarkers.end()),
            candidate.timelineMarkers.end());
        if ((candidate.inFrame && !convertFrame(*candidate.inFrame)) ||
            (candidate.outFrame && !convertFrame(*candidate.outFrame))) {
            result.error = "イン・アウトを新しいtimeline frame rateへ変換できません";
            return result;
        }
        if (candidate.inFrame && candidate.outFrame && *candidate.inFrame >= *candidate.outFrame) {
            result.error = "frame rate変更でイン・アウトの範囲が空になります";
            return result;
        }
        for (auto& clip : candidate.timelineClips) {
            const auto converted =
                sourceBoundaryToTimelineBoundary(clip.timelineStartFrame, project.timelineFpsNum,
                                                 project.timelineFpsDen, fpsNum, fpsDen);
            if (!converted.success) {
                result.error =
                    "clip開始位置を新しいtimeline frame rateへ変換できません: " + clip.name;
                return result;
            }
            clip.timelineStartFrame = converted.frame;
        }
        for (auto& transition : candidate.timelineTransitions) {
            if (!convertFrame(transition.framesBeforeCut) ||
                !convertFrame(transition.framesAfterCut)) {
                result.error = "トランジションを新しいtimeline frame rateへ変換できません";
                return result;
            }
        }
    }
    candidate.outputWidth = width;
    candidate.outputHeight = height;
    candidate.timelineFpsNum = fpsNum;
    candidate.timelineFpsDen = fpsDen;
    const auto valid = finalizeTimelineCandidate(candidate);
    if (!valid.success) {
        result.error = "Project設定変更後のtimelineが不正です: " + valid.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    return result;
}

TimelineEditResult addTrack(Project& project, TrackKind kind) {
    TimelineEditResult result;
    Project candidate = project;
    auto& tracks = tracksOfKind(candidate, kind);
    const int index = static_cast<int>(tracks.size());
    tracks.push_back(Track{defaultTrackName(kind, index), false});
    const auto valid = finalizeTimelineCandidate(candidate);
    if (!valid.success) {
        result.error = valid.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    result.selectedIndex = index;
    return result;
}

TimelineEditResult removeTrack(Project& project, TrackRef track) {
    TimelineEditResult result;
    if (!isValidTrackRef(project, track)) {
        result.error = "削除する track が存在しません";
        return result;
    }
    if (track.kind == TrackKind::Video && project.videoTracks.size() <= 1) {
        result.error = "video track は最低 1 本必要です";
        return result;
    }
    for (const auto& clip : project.timelineClips) {
        if (clip.track == track) {
            result.error = "clip が載っている track は削除できません: " + clip.name;
            return result;
        }
    }
    Project candidate = project;
    auto& tracks = tracksOfKind(candidate, track.kind);
    tracks.erase(tracks.begin() + track.index);
    // 削除位置より後ろの track index を詰め、既定名も振り直す。
    for (std::size_t index = 0; index < tracks.size(); ++index)
        tracks[index].name = defaultTrackName(track.kind, static_cast<int>(index));
    for (auto& clip : candidate.timelineClips) {
        if (clip.track.kind == track.kind && clip.track.index > track.index)
            --clip.track.index;
    }
    const auto valid = finalizeTimelineCandidate(candidate);
    if (!valid.success) {
        result.error = valid.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    result.selectedIndex = track.index;
    return result;
}

TimelineEditResult setTrackMuted(Project& project, TrackRef track, bool muted) {
    return setTracksMuted(project, track.kind, {track.index}, muted);
}

TimelineEditResult setTracksMuted(Project& project, TrackKind kind, const std::vector<int>& indices,
                                  bool muted) {
    TimelineEditResult result;
    if (indices.empty()) {
        result.error = "track が指定されていません";
        return result;
    }
    for (const int index : indices) {
        if (!isValidTrackRef(project, {kind, index})) {
            result.error = "track が存在しません";
            return result;
        }
    }
    auto& tracks = tracksOfKind(project, kind);
    for (const int index : indices)
        tracks[static_cast<std::size_t>(index)].muted = muted;
    result.success = true;
    result.selectedIndex = indices.front();
    return result;
}

TimelineEditResult setTrackSolo(Project& project, TrackRef track, bool solo) {
    TimelineEditResult result;
    if (!isValidTrackRef(project, track)) {
        result.error = "track が存在しません";
        return result;
    }
    if (track.kind != TrackKind::Audio) {
        result.error = "solo は audio track だけに設定できます";
        return result;
    }
    project.audioTracks[static_cast<std::size_t>(track.index)].solo = solo;
    result.success = true;
    result.selectedIndex = track.index;
    return result;
}

TimelineGap gapAt(const Project& project, TrackRef track, std::int64_t timelineFrame) {
    TimelineGap gap;
    if (!isValidTrackRef(project, track) || timelineFrame < 0) {
        gap.error = "track または frame が不正です";
        return gap;
    }
    std::int64_t gapStart = 0;
    std::int64_t gapEnd = std::numeric_limits<std::int64_t>::max();
    bool hasFollowing = false;
    for (const auto& clip : project.timelineClips) {
        if (!(clip.track == track))
            continue;
        std::int64_t start = 0;
        std::int64_t end = 0;
        if (!clipInterval(project, clip, start, end, gap.error))
            return gap;
        if (timelineFrame >= start && timelineFrame < end) {
            gap.error = "その位置には clip があります";
            return gap;
        }
        if (end <= timelineFrame)
            gapStart = std::max(gapStart, end);
        if (start > timelineFrame) {
            gapEnd = std::min(gapEnd, start);
            hasFollowing = true;
        }
    }
    if (!hasFollowing) {
        gap.error = "後続 clip が無いため詰める gap がありません";
        return gap;
    }
    gap.found = true;
    gap.start = gapStart;
    gap.end = gapEnd;
    return gap;
}

TimelineEditResult rippleDeleteGap(Project& project, TrackRef track, std::int64_t timelineFrame) {
    TimelineEditResult result;
    const TimelineGap gap = gapAt(project, track, timelineFrame);
    if (!gap.found) {
        result.error = gap.error;
        return result;
    }
    const std::int64_t shift = gap.end - gap.start;
    if (shift <= 0) {
        result.error = "詰める空白がありません";
        return result;
    }
    return editTimelineTime(project, gap.start, shift, 0);
}

ClipKeyEditPreview previewClipKeyEdit(const Project& project, const std::string& clipId,
                                      ClipKeyKind kind, std::int64_t originalFrame,
                                      std::int64_t requestedFrame, double requestedPercent) {
    ClipKeyEditPreview result;
    const int index = indexOfId(project, clipId);
    if (!validIndex(project, index)) {
        result.error = "キーフレームを編集する clip がありません";
        return result;
    }
    const auto& clip = project.timelineClips[static_cast<std::size_t>(index)];
    if (!effectChannel(kind) ||
        isAudioEffectChannel(kind) != (clip.kind == TimelineClipKind::Audio) ||
        !std::isfinite(requestedPercent)) {
        result.error = "キーフレームの種別または値が不正です";
        return result;
    }
    const auto duration = timelineClipDuration(project, clip);
    if (!duration.success)
        result.error = duration.error;
    if (!duration.success)
        return result;
    result.effects = clip.effects;
    auto& keys = result.effects.*effectChannel(kind)->keys;
    auto found = std::find_if(keys.begin(), keys.end(), [originalFrame](const auto& key) {
        return key.frame == originalFrame;
    });
    if (originalFrame >= 0 && found == keys.end()) {
        result.error = "移動元のキーフレームがありません";
        return result;
    }
    const auto& channel = *effectChannel(kind);
    const double value = std::clamp(requestedPercent, channel.minimum, channel.maximum);
    if (originalFrame < 0) {
        result.frame = std::clamp<std::int64_t>(requestedFrame, 0, duration.frame - 1);
        found = std::find_if(keys.begin(), keys.end(),
                             [&](const auto& key) { return key.frame == result.frame; });
        if (found != keys.end())
            found->value = value;
        else
            insertClipKey(keys, result.frame, value);
        std::sort(keys.begin(), keys.end(),
                  [](const auto& a, const auto& b) { return a.frame < b.frame; });
    } else {
        const auto position = static_cast<std::size_t>(found - keys.begin());
        const auto lower = position == 0 ? 0 : keys[position - 1].frame + 1;
        const auto upper =
            position + 1 == keys.size() ? duration.frame - 1 : keys[position + 1].frame - 1;
        result.frame = std::clamp(requestedFrame, lower, upper);
        keys[position].frame = result.frame;
        keys[position].value = value;
    }
    Project candidate = project;
    candidate.timelineClips[static_cast<std::size_t>(index)].effects = result.effects;
    const auto valid = finalizeTimelineCandidate(candidate);
    if (!valid.success) {
        result.error = valid.error;
        return result;
    }
    result.success = true;
    return result;
}

TimelineEditResult editClipKey(Project& project, const std::string& clipId, ClipKeyKind kind,
                               std::int64_t originalFrame, std::int64_t requestedFrame,
                               double requestedPercent) {
    const auto preview =
        previewClipKeyEdit(project, clipId, kind, originalFrame, requestedFrame, requestedPercent);
    if (!preview.success)
        return {false, -1, preview.error};
    Project candidate = project;
    const int index = indexOfId(candidate, clipId);
    candidate.timelineClips[static_cast<std::size_t>(index)].effects = preview.effects;
    return commitCandidate(project, std::move(candidate), index);
}

TimelineEditResult deleteClipKey(Project& project, const std::string& clipId, ClipKeyKind kind,
                                 std::int64_t frame) {
    const int index = indexOfId(project, clipId);
    if (!validIndex(project, index))
        return {false, -1, "キーフレームを削除する clip がありません"};
    Project candidate = project;
    auto& clip = candidate.timelineClips[static_cast<std::size_t>(index)];
    if (!effectChannel(kind) ||
        isAudioEffectChannel(kind) != (clip.kind == TimelineClipKind::Audio))
        return {false, -1, "キーフレームの種別が不正です"};
    auto& keys = clip.effects.*effectChannel(kind)->keys;
    const auto found = std::find_if(keys.begin(), keys.end(),
                                    [frame](const auto& key) { return key.frame == frame; });
    if (found == keys.end())
        return {false, -1, "削除するキーフレームがありません"};
    if (keys.size() == 1)
        clip.effects.*effectChannel(kind)->base = found->value;
    removeClipKeys(keys, {frame});
    return commitCandidate(project, std::move(candidate), index);
}

namespace {

// 尺を newDuration にする速度 (out - in) R / D を、約分した有理数で返す。
bool speedForDuration(const Project& project, const TimelineClip& clip, std::int64_t newDuration,
                      std::int64_t& speedNum, std::int64_t& speedDen) {
    if (newDuration <= 0)
        return false;
    // (out - in) tlNum srcDen / (tlDen srcNum D)
    const WideInteger length = clip.sourceOutFrame - clip.sourceInFrame;
    WideInteger num = length * project.timelineFpsNum * clip.sourceFpsDen;
    WideInteger den =
        static_cast<WideInteger>(project.timelineFpsDen) * clip.sourceFpsNum * newDuration;
    if (num <= 0 || den <= 0)
        return false;
    WideInteger a = num, b = den;
    while (b != 0) {
        const WideInteger r = a % b;
        a = b;
        b = r;
    }
    num /= a;
    den /= a;
    if (num > std::numeric_limits<std::int64_t>::max() ||
        den > std::numeric_limits<std::int64_t>::max())
        return false;
    speedNum = static_cast<std::int64_t>(num);
    speedDen = static_cast<std::int64_t>(den);
    return true;
}

// 操作した clip の尺が newDuration になるよう、対象 clip へ速度を適用する。
// left 端なら各 clip の終端を、right 端なら開始位置を保つ。key は尺に合わせて伸縮する。
bool applyClipSpeed(Project& candidate, int index, TrimEdge edge, std::int64_t speedNum,
                    std::int64_t speedDen, LinkMode linkMode, std::string& error) {
    const auto& operated = candidate.timelineClips[static_cast<std::size_t>(index)];
    const std::int64_t originalNum = operated.speedNum;
    const std::int64_t originalDen = operated.speedDen;
    for (const int target : editTargets(candidate, index, linkMode)) {
        auto& clip = candidate.timelineClips[static_cast<std::size_t>(target)];
        if (clip.speedNum != originalNum || clip.speedDen != originalDen) {
            error = "リンク相手と速度が違うため、一緒にレート調整できません (Alt "
                    "で片方だけ調整できます)";
            return false;
        }
        const auto before = timelineClipDuration(candidate, clip);
        if (!before.success) {
            error = before.error;
            return false;
        }
        clip.speedNum = speedNum;
        clip.speedDen = speedDen;
        // 等速では伸縮しないのでピッチ保持は意味を持たない (validateTimeline が拒否する)。
        if (speedNum == speedDen)
            clip.preservePitch = false;
        const auto after = timelineClipDuration(candidate, clip);
        if (!after.success) {
            error = after.error;
            return false;
        }
        if (edge == TrimEdge::Left) {
            const std::int64_t end = clip.timelineStartFrame + before.frame;
            clip.timelineStartFrame = end - after.frame;
        }
        for (const auto& channel : effectChannels())
            rescaleClipKeys(clip.effects.*channel.keys, before.frame, after.frame);
    }
    return true;
}

bool applyRateStretch(Project& candidate, int index, TrimEdge edge, std::int64_t newDuration,
                      LinkMode linkMode, std::string& error) {
    std::int64_t speedNum = 0;
    std::int64_t speedDen = 0;
    if (!speedForDuration(candidate, candidate.timelineClips[static_cast<std::size_t>(index)],
                          newDuration, speedNum, speedDen)) {
        error = "この尺にする速度を表せません";
        return false;
    }
    return applyClipSpeed(candidate, index, edge, speedNum, speedDen, linkMode, error);
}

// 尺 D で伸縮した candidate が成り立つか。判定は validateTimeline に一本化する
// (速度の範囲、1 frame 以上の尺、timeline 先頭、同じ track の重なり)。
bool rateStretchFeasible(const Project& project, int index, TrimEdge edge, std::int64_t newDuration,
                         LinkMode linkMode) {
    Project candidate = project;
    std::string error;
    return applyRateStretch(candidate, index, edge, newDuration, linkMode, error) &&
           finalizeTimelineCandidate(candidate).success;
}

} // namespace

TimelineFrameResult clampRateEdit(const Project& project, const std::string& clipId, TrimEdge edge,
                                  std::int64_t projectFrameDelta, LinkMode linkMode) {
    TimelineFrameResult result;
    const int index = indexOfId(project, clipId);
    if (!validIndex(project, index)) {
        result.error = "レート調整する timeline clip がありません";
        return result;
    }
    if (hasSyntheticSourceDomain(project.timelineClips[static_cast<std::size_t>(index)])) {
        result.error = "text / image clip の尺は trim で変更してください";
        return result;
    }
    const auto& clip = project.timelineClips[static_cast<std::size_t>(index)];
    for (const int target : editTargets(project, index, linkMode)) {
        const auto& other = project.timelineClips[static_cast<std::size_t>(target)];
        if (other.speedNum != clip.speedNum || other.speedDen != clip.speedDen) {
            result.error = "リンク相手と速度が違うため、一緒にレート調整できません (Alt "
                           "で片方だけ調整できます)";
            return result;
        }
    }
    const auto duration = timelineClipDuration(project, clip);
    if (!duration.success) {
        result.error = duration.error;
        return result;
    }
    // right 端は右へ動かすと伸び、left 端は左へ動かすと伸びる。
    const auto durationFor = [&](std::int64_t delta) -> std::int64_t {
        const WideInteger value =
            static_cast<WideInteger>(duration.frame) +
            (edge == TrimEdge::Right ? delta : -static_cast<WideInteger>(delta));
        if (value < 1)
            return 0;
        if (value > std::numeric_limits<std::int64_t>::max() / 4)
            return std::numeric_limits<std::int64_t>::max() / 4;
        return static_cast<std::int64_t>(value);
    };
    // 伸縮できる範囲は現在の尺 (delta 0) から要求した向きに連続している
    // (各条件は尺の上限か下限なので)。成り立つ最も遠い量を二分探索で求める。
    std::int64_t reachable = 0;
    std::int64_t blocked = projectFrameDelta;
    if (projectFrameDelta != 0 && durationFor(projectFrameDelta) > 0 &&
        rateStretchFeasible(project, index, edge, durationFor(projectFrameDelta), linkMode)) {
        reachable = projectFrameDelta;
    } else {
        while ((blocked > reachable ? blocked - reachable : reachable - blocked) > 1) {
            const std::int64_t middle = reachable + (blocked - reachable) / 2;
            const std::int64_t candidateDuration = durationFor(middle);
            if (candidateDuration > 0 &&
                rateStretchFeasible(project, index, edge, candidateDuration, linkMode))
                reachable = middle;
            else
                blocked = middle;
        }
    }
    result.success = true;
    result.frame = reachable;
    return result;
}

namespace {

// clampRateEdit で止めた量で伸縮した candidate を作る。表示と確定の両方がこれを使う。
bool rateStretchCandidate(const Project& project, const std::string& clipId, TrimEdge edge,
                          std::int64_t projectFrameDelta, LinkMode linkMode, Project& candidate,
                          int& index, std::int64_t& appliedDelta, std::string& error) {
    const auto clamped = clampRateEdit(project, clipId, edge, projectFrameDelta, linkMode);
    if (!clamped.success) {
        error = clamped.error;
        return false;
    }
    candidate = project;
    index = indexOfId(candidate, clipId);
    appliedDelta = clamped.frame;
    if (appliedDelta == 0)
        return true;
    const auto duration =
        timelineClipDuration(candidate, candidate.timelineClips[static_cast<std::size_t>(index)]);
    if (!duration.success) {
        error = duration.error;
        return false;
    }
    const std::int64_t newDuration =
        duration.frame + (edge == TrimEdge::Right ? appliedDelta : -appliedDelta);
    return applyRateStretch(candidate, index, edge, newDuration, linkMode, error);
}

} // namespace

RateStretchPreview previewRateStretch(const Project& project, const std::string& clipId,
                                      TrimEdge edge, std::int64_t projectFrameDelta,
                                      LinkMode linkMode) {
    RateStretchPreview result;
    Project candidate;
    int index = -1;
    if (!rateStretchCandidate(project, clipId, edge, projectFrameDelta, linkMode, candidate, index,
                              result.appliedDelta, result.error))
        return result;
    for (const int target : editTargets(candidate, index, linkMode)) {
        const auto& clip = candidate.timelineClips[static_cast<std::size_t>(target)];
        const auto& before = project.timelineClips[static_cast<std::size_t>(target)];
        const auto duration = timelineClipDuration(candidate, clip);
        const auto beforeDuration = timelineClipDuration(project, before);
        if (!duration.success || !beforeDuration.success) {
            result.error = duration.success ? beforeDuration.error : duration.error;
            result.clips.clear();
            return result;
        }
        result.clips.push_back({clip.id, clip.timelineStartFrame, duration.frame, clip.speedNum,
                                clip.speedDen, clip.timelineStartFrame - before.timelineStartFrame,
                                (clip.timelineStartFrame + duration.frame) -
                                    (before.timelineStartFrame + beforeDuration.frame)});
    }
    result.success = true;
    return result;
}

TimelineEditResult rateStretchTimelineClip(Project& project, const std::string& clipId,
                                           TrimEdge edge, std::int64_t projectFrameDelta,
                                           LinkMode linkMode) {
    TimelineEditResult result;
    Project candidate;
    int index = -1;
    std::int64_t appliedDelta = 0;
    if (!rateStretchCandidate(project, clipId, edge, projectFrameDelta, linkMode, candidate, index,
                              appliedDelta, result.error))
        return result;
    if (appliedDelta == 0) {
        result.error = "速度の範囲 (" + std::to_string(kMinClipSpeedPercent) + "%〜" +
                       std::to_string(kMaxClipSpeedPercent) +
                       "%) または隣の clip に達しているため、これ以上伸縮できません";
        return result;
    }
    return commitMappingEdit(project, std::move(candidate), index);
}

namespace {
// 伸ばした targets の区間に掛かる、同じ track の targets 以外の clip の ID。
bool clipsCoveredByTargets(const Project& candidate, const std::vector<int>& targets,
                           std::vector<std::string>& covered, std::string& error) {
    for (const int target : targets) {
        const auto& clip = candidate.timelineClips[static_cast<std::size_t>(target)];
        std::int64_t start = 0, end = 0;
        if (!clipInterval(candidate, clip, start, end, error))
            return false;
        for (std::size_t other = 0; other < candidate.timelineClips.size(); ++other) {
            const auto& following = candidate.timelineClips[other];
            if (isTarget(targets, other) || following.track != clip.track)
                continue;
            std::int64_t otherStart = 0, otherEnd = 0;
            if (!clipInterval(candidate, following, otherStart, otherEnd, error))
                return false;
            if (otherStart < end && otherEnd > start)
                covered.push_back(following.id);
        }
    }
    return true;
}

// covered を targets の終端まで上書きする。丸ごと覆われた clip は消し、リンク相手は
// 片方だけのリンクにならないよう未リンクにする。はみ出す clip は左端を targets の終端まで削る
// (effect は残す。縮めた尺に収まらない fade だけ詰める)。
bool overwriteCoveredClips(Project& candidate, const std::vector<int>& targets,
                           const std::vector<std::string>& covered, std::string& error) {
    std::vector<std::pair<TrackRef, std::int64_t>> ends;
    for (const int target : targets) {
        const auto& clip = candidate.timelineClips[static_cast<std::size_t>(target)];
        std::int64_t start = 0, end = 0;
        if (!clipInterval(candidate, clip, start, end, error))
            return false;
        ends.emplace_back(clip.track, end);
    }
    std::vector<std::string> removed;
    for (const auto& id : covered) {
        const int index = indexOfId(candidate, id);
        auto& clip = candidate.timelineClips[static_cast<std::size_t>(index)];
        std::int64_t start = 0, end = 0;
        if (!clipInterval(candidate, clip, start, end, error))
            return false;
        const auto found = std::find_if(
            ends.begin(), ends.end(), [&](const auto& entry) { return entry.first == clip.track; });
        const std::int64_t coverEnd = found->second;
        if (end <= coverEnd) {
            removed.push_back(id);
            continue;
        }
        if (!trimClipBoundary(candidate, clip, TrimEdge::Left, coverEnd - start, error))
            return false;
        // 通常の左 trim と同じく、上書きは尺を縮めるだけで effect は消さない。縮めた尺に
        // fade が収まらないときだけ詰める。末尾は動かないので fade out を優先して残し、
        // fade in は残りの尺までにする (validateClipEffects の fade in + fade out <= 尺)。
        const std::int64_t length = clip.sourceOutFrame - clip.sourceInFrame;
        clip.effects.fadeOutFrames = std::min(clip.effects.fadeOutFrames, length);
        clip.effects.fadeInFrames =
            std::min(clip.effects.fadeInFrames, length - clip.effects.fadeOutFrames);
    }
    for (const auto& id : removed) {
        const auto& group =
            candidate.timelineClips[static_cast<std::size_t>(indexOfId(candidate, id))].linkGroupId;
        if (group.empty())
            continue;
        const std::string groupId = group;
        for (auto& clip : candidate.timelineClips)
            if (clip.linkGroupId == groupId)
                clip.linkGroupId.clear();
    }
    std::erase_if(candidate.timelineClips, [&](const TimelineClip& clip) {
        return std::find(removed.begin(), removed.end(), clip.id) != removed.end();
    });
    return true;
}

bool speedDurationCandidate(const Project& project, const std::string& clipId,
                            const ClipSpeedDurationEdit& edit, LinkMode linkMode,
                            Project& candidate, int& index, bool& overlapsFollowing,
                            std::string& error) {
    overlapsFollowing = false;
    candidate = project;
    index = indexOfId(candidate, clipId);
    if (!validIndex(candidate, index)) {
        error = "速度を変更する clip がありません";
        return false;
    }
    const auto targets = editTargets(candidate, index, linkMode);
    std::vector<std::pair<TrackRef, std::int64_t>> oldEnds;
    for (int target : targets) {
        const auto& clip = candidate.timelineClips[static_cast<std::size_t>(target)];
        std::int64_t start = 0, end = 0;
        if (!clipInterval(candidate, clip, start, end, error))
            return false;
        oldEnds.emplace_back(clip.track, end);
    }
    const auto& operated = candidate.timelineClips[static_cast<std::size_t>(index)];
    if (hasSyntheticSourceDomain(operated)) {
        if (edit.input != ClipSpeedDurationEdit::Input::Duration || edit.preservePitch ||
            edit.durationFrames < 1) {
            error = "静止 clip は尺だけ変更できます";
            return false;
        }
        const auto before = timelineClipDuration(candidate, operated);
        if (!before.success ||
            !trimClipBoundary(candidate, candidate.timelineClips[static_cast<std::size_t>(index)],
                              TrimEdge::Right, edit.durationFrames - before.frame, error))
            return false;
    } else {
        std::int64_t num = edit.speedNum, den = edit.speedDen;
        if (edit.input == ClipSpeedDurationEdit::Input::Duration &&
            !speedForDuration(candidate, operated, edit.durationFrames, num, den)) {
            error = "この尺にする速度を表せません";
            return false;
        }
        if (num <= 0 || den <= 0) {
            error = "速度が不正です";
            return false;
        }
        const auto divisor = std::gcd(num, den);
        num /= divisor;
        den /= divisor;
        if (!applyClipSpeed(candidate, index, TrimEdge::Right, num, den, linkMode, error))
            return false;
        // 等速ではピッチ保持を持たせない。preview と書き出しの両方が 1/1 では伸縮しない。
        for (int target : targets)
            candidate.timelineClips[static_cast<std::size_t>(target)].preservePitch =
                edit.preservePitch && num != den;
    }
    std::int64_t shift = 0;
    for (std::size_t order = 0; order < targets.size(); ++order) {
        std::int64_t start = 0, end = 0;
        if (!clipInterval(candidate,
                          candidate.timelineClips[static_cast<std::size_t>(targets[order])], start,
                          end, error))
            return false;
        const auto delta = end - oldEnds[order].second;
        if (order == 0)
            shift = delta;
        else if (shift != delta) {
            error = "リンク相手と尺の変化量が一致しません";
            return false;
        }
    }
    if (edit.ripple && shift != 0) {
        std::vector<RippleSource> sources;
        for (const auto& [track, oldEnd] : oldEnds)
            sources.push_back({track, oldEnd});
        if (!shiftFollowingClips(candidate, sources, targets, index, shift, error))
            return false;
    }
    if (!edit.ripple && shift > 0) {
        std::vector<std::string> covered;
        if (!clipsCoveredByTargets(candidate, targets, covered, error))
            return false;
        if (!covered.empty()) {
            if (!edit.overwrite) {
                overlapsFollowing = true;
                error = "後続の clip と重なります";
                return false;
            }
            if (!overwriteCoveredClips(candidate, targets, covered, error))
                return false;
            // 削除で後ろの index がずれるので、操作した clip を ID で引き直す。
            index = indexOfId(candidate, clipId);
        }
    }
    index = indexOfId(candidate, clipId);
    if (candidate == project) {
        error = "変更がありません";
        return false;
    }
    const auto valid = finalizeTimelineCandidate(candidate);
    if (!valid.success) {
        error = valid.error;
        return false;
    }
    return true;
}
} // namespace

ClipSpeedDurationPreview previewClipSpeedDuration(const Project& project, const std::string& clipId,
                                                  const ClipSpeedDurationEdit& edit,
                                                  LinkMode linkMode) {
    ClipSpeedDurationPreview preview;
    Project candidate;
    int index = -1;
    if (!speedDurationCandidate(project, clipId, edit, linkMode, candidate, index,
                                preview.overlapsFollowing, preview.error))
        return preview;
    const auto& clip = candidate.timelineClips[static_cast<std::size_t>(index)];
    const auto duration = timelineClipDuration(candidate, clip);
    if (!duration.success) {
        preview.error = duration.error;
        return preview;
    }
    preview.success = true;
    preview.durationFrames = duration.frame;
    preview.speedNum = clip.speedNum;
    preview.speedDen = clip.speedDen;
    return preview;
}

TimelineEditResult setClipSpeedDuration(Project& project, const std::string& clipId,
                                        const ClipSpeedDurationEdit& edit, LinkMode linkMode) {
    TimelineEditResult result;
    Project candidate;
    int index = -1;
    bool overlapsFollowing = false;
    if (!speedDurationCandidate(project, clipId, edit, linkMode, candidate, index,
                                overlapsFollowing, result.error))
        return result;
    return commitMappingEdit(project, std::move(candidate), index);
}

TimelineEditResult insertFrameHold(Project& project, const std::string& clipId, std::int64_t frame,
                                   std::int64_t holdFrames,
                                   const std::function<std::string()>& newId) {
    TimelineEditResult result;
    const int index = indexOfId(project, clipId);
    if (!validIndex(project, index) || holdFrames < 1) {
        result.error = "保持する映像 clip または尺が不正です";
        return result;
    }
    const auto& original = project.timelineClips[static_cast<std::size_t>(index)];
    std::int64_t start = 0, end = 0;
    if (!clipInterval(project, original, start, end, result.error))
        return result;
    if (original.kind != TimelineClipKind::Video || original.frameHold || frame <= start ||
        frame >= end) {
        result.error = "再生ヘッドは通常の映像 clip の内側に置いてください";
        return result;
    }
    const auto sourceFrame =
        clipSourceFrameAt(original, project.timelineFpsNum, project.timelineFpsDen, frame - start);
    if (!sourceFrame.success) {
        result.error = sourceFrame.error;
        return result;
    }
    // 保持中もモーションは継続し、保持後の右側 clip は保持尺だけ進んだ位置から再開する。
    // 動きは timeline の時間に付き、映像だけが止まる。不透明度は挿入位置の値 (key と fade 込み) を
    // 保持 clip の基本値へ焼き込む。automation を捨てるだけだと、fade の途中などで保持へ
    // 入った瞬間に基本値へ跳び、右側の clip へ戻るとまた元の値へ跳ぶ。
    const auto fadeFrame = clipFadeSourceFrameAt(original, project.timelineFpsNum,
                                                 project.timelineFpsDen, frame - start);
    if (!fadeFrame.success) {
        result.error = fadeFrame.error;
        return result;
    }
    const double heldOpacityPercent =
        100.0 * evaluateClipOpacity(original.effects, frame - start, fadeFrame.frame,
                                    original.sourceOutFrame - original.sourceInFrame);
    Project candidate = project;
    const auto spanning = clipIdsSpanningFrame(candidate, frame);
    const auto split = splitTimelineClips(candidate, spanning, frame, newId, LinkMode::Linked);
    if (!split.success)
        return split;
    // 右側は分割で元の動きを切り出してある。その続きを保持が消費するので、右側は元の
    // 動きを保持尺だけ先へ進めて取り直す。そうしないと保持の終わりから右側の先頭へ戻る。
    const auto right = std::find_if(candidate.timelineClips.begin(), candidate.timelineClips.end(),
                                    [&](const auto& clip) {
                                        return clip.id != clipId && clip.track == original.track &&
                                               clip.timelineStartFrame == frame;
                                    });
    if (right == candidate.timelineClips.end()) {
        result.error = "保持位置の右側の clip がありません";
        return result;
    }
    for (const auto& channel : effectChannels()) {
        if (channel.kind == ClipKeyKind::Opacity || isAudioEffectChannel(channel.kind))
            continue;
        right->effects.*channel.keys = original.effects.*channel.keys;
        reframeClipKeys(right->effects.*channel.keys, end - start, end - frame,
                        frame - start + holdFrames);
    }
    for (auto& clip : candidate.timelineClips) {
        if (clip.timelineStartFrame >= frame && !shiftStart(clip, holdFrames, result.error))
            return result;
    }
    // 保持は保持位置への時間の挿入でもある。字幕も clip と同じだけ後ろへ送る (保持位置を跨ぐ
    // リンク字幕は上の分割で既に左右へ分けてある)。
    if (!editSubtitleTime(candidate, frame, 0, holdFrames, newId, result.error))
        return result;
    TimelineClip hold =
        candidate.timelineClips[static_cast<std::size_t>(indexOfId(candidate, clipId))];
    hold.id = newId();
    hold.name += " (保持)";
    hold.linkGroupId.clear();
    hold.timelineStartFrame = frame;
    hold.sourceFpsNum = project.timelineFpsNum;
    hold.sourceFpsDen = project.timelineFpsDen;
    hold.sourceFrameCount = holdFrames;
    hold.sourceInFrame = 0;
    hold.sourceOutFrame = holdFrames;
    hold.speedNum = 1;
    hold.speedDen = 1;
    hold.preservePitch = false;
    hold.frameHold =
        FrameHold{sourceFrame.frame,         original.sourceFpsNum, original.sourceFpsDen,
                  original.sourceFrameCount, original.speedNum,     original.speedDen};
    for (const auto& channel : effectChannels()) {
        if (channel.kind == ClipKeyKind::Opacity || isAudioEffectChannel(channel.kind))
            continue;
        hold.effects.*channel.keys = original.effects.*channel.keys;
        reframeClipKeys(hold.effects.*channel.keys, end - start, holdFrames, frame - start);
    }
    hold.effects.opacityPercent = heldOpacityPercent;
    hold.effects.opacityKeys.clear();
    hold.effects.volumeKeys.clear();
    hold.effects.fadeInFrames = 0;
    hold.effects.fadeOutFrames = 0;
    candidate.timelineClips.push_back(std::move(hold));
    const int holdIndex = static_cast<int>(candidate.timelineClips.size()) - 1;
    return commitCandidate(project, std::move(candidate), holdIndex);
}

} // namespace mvm::project

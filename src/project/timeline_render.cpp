#include "project/timeline_render.h"

#include "project/clip_effects.h"
#include "project/timeline_edit.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace mvm::project {
namespace {

constexpr double kHalfPi = 1.57079632679489661923;

struct ClipTransition {
    const TimelineTransition* transition = nullptr;
    std::int64_t cut = 0;
};

bool transitionIsRendered(const Project& project, const ClipIdIndex& clipIndex,
                          const TimelineTransition& transition, TrackKind kind, int& outgoing,
                          int& incoming) {
    outgoing = clipIndex.find(transition.outgoingClipId);
    incoming = clipIndex.find(transition.incomingClipId);
    if (outgoing < 0 || incoming < 0)
        return false;
    const auto& a = project.timelineClips[static_cast<std::size_t>(outgoing)];
    const auto& b = project.timelineClips[static_cast<std::size_t>(incoming)];
    return a.enabled && b.enabled && a.track.kind == kind && b.track.kind == kind;
}

// clip の timeline 上の区間 [start, end)。
bool clipSpan(const Project& project, const TimelineClip& clip, std::int64_t& start,
              std::int64_t& end, std::string& error) {
    const auto duration = timelineClipDuration(project, clip);
    if (!duration.success) {
        error = clip.name + ": " + duration.error;
        return false;
    }
    start = clip.timelineStartFrame;
    end = start + duration.frame;
    return true;
}

bool withEdge(const Project& project, TimelineClip& clip, TrimEdge edge, std::int64_t frame,
              std::string& error) {
    auto moved = clipWithEdgeAt(project, clip, edge, frame, error);
    if (!moved)
        return false;
    clip = std::move(*moved);
    return true;
}

// 元の clip の local frame を clip の中に収めて、effect と素材 frame を評価する。
struct OriginalFrame {
    std::int64_t local = 0;
    std::int64_t sourceLocal = 0;
    std::int64_t sourceDuration = 0;
};

std::optional<OriginalFrame> originalFrameAt(const TimelineClip& original,
                                             std::int64_t timelineFpsNum,
                                             std::int64_t timelineFpsDen,
                                             std::int64_t timelineFrame) {
    Project timebase;
    timebase.timelineFpsNum = timelineFpsNum;
    timebase.timelineFpsDen = timelineFpsDen;
    const auto duration = timelineClipDuration(timebase, original);
    if (!duration.success)
        return std::nullopt;
    OriginalFrame result;
    result.local = std::clamp(timelineFrame - original.timelineStartFrame, std::int64_t{0},
                              duration.frame - 1);
    const auto sourceLocal =
        clipFadeSourceFrameAt(original, timelineFpsNum, timelineFpsDen, result.local);
    if (!sourceLocal.success)
        return std::nullopt;
    result.sourceLocal = sourceLocal.frame;
    result.sourceDuration = original.sourceOutFrame - original.sourceInFrame;
    return result;
}

} // namespace

bool hasRenderedTransitions(const Project& project, TrackKind kind) {
    if (project.timelineTransitions.empty())
        return false;
    const ClipIdIndex clipIndex(project);
    for (const auto& transition : project.timelineTransitions) {
        int outgoing = -1;
        int incoming = -1;
        if (transitionIsRendered(project, clipIndex, transition, kind, outgoing, incoming))
            return true;
    }
    return false;
}

bool timelineRenderSegments(const Project& project, TrackKind kind,
                            std::vector<TimelineRenderSegment>& segments, std::string& error) {
    segments.clear();
    std::unordered_map<int, ClipTransition> heads; // incoming clip -> トランジション
    std::unordered_map<int, ClipTransition> tails; // outgoing clip -> トランジション
    const ClipIdIndex clipIndex(project);
    for (const auto& transition : project.timelineTransitions) {
        int outgoing = -1;
        int incoming = -1;
        if (!transitionIsRendered(project, clipIndex, transition, kind, outgoing, incoming))
            continue;
        std::int64_t start = 0;
        std::int64_t cut = 0;
        if (!clipSpan(project, project.timelineClips[static_cast<std::size_t>(outgoing)], start,
                      cut, error))
            return false;
        heads[incoming] = {&transition, cut};
        tails[outgoing] = {&transition, cut};
    }
    for (std::size_t index = 0; index < project.timelineClips.size(); ++index) {
        const auto& original = project.timelineClips[index];
        if (!original.enabled || original.track.kind != kind)
            continue;
        const auto head = heads.find(static_cast<int>(index));
        const auto tail = tails.find(static_cast<int>(index));
        TimelineClip extended = original;
        std::optional<TransitionEnvelope> fadeIn;
        std::optional<TransitionEnvelope> fadeOut;
        std::int64_t headRegionEnd = 0;
        if (head != heads.end()) {
            const auto& transition = *head->second.transition;
            const auto regionStart = head->second.cut - transition.framesBeforeCut;
            headRegionEnd = head->second.cut + transition.framesAfterCut;
            fadeIn = TransitionEnvelope{regionStart,
                                        transition.framesBeforeCut + transition.framesAfterCut};
            if (!withEdge(project, extended, TrimEdge::Left, regionStart, error))
                return false;
        }
        if (tail != tails.end()) {
            const auto& transition = *tail->second.transition;
            fadeOut = TransitionEnvelope{tail->second.cut - transition.framesBeforeCut,
                                         transition.framesBeforeCut + transition.framesAfterCut};
            if (!withEdge(project, extended, TrimEdge::Right,
                          tail->second.cut + transition.framesAfterCut, error))
                return false;
        }
        const int clipIndex = static_cast<int>(index);
        if (kind == TrackKind::Audio) {
            segments.push_back({clipIndex, original, extended, 0, fadeIn, fadeOut});
            continue;
        }
        if (!fadeIn) {
            segments.push_back({clipIndex, original, extended, 0, std::nullopt, std::nullopt});
            continue;
        }
        // 映像: 頭のトランジション区間を lane 1 へ切り出し、残りを lane 0 に置く。
        std::int64_t start = 0;
        std::int64_t end = 0;
        if (!clipSpan(project, extended, start, end, error))
            return false;
        TimelineClip over = extended;
        if (headRegionEnd < end && !withEdge(project, over, TrimEdge::Right, headRegionEnd, error))
            return false;
        segments.push_back({clipIndex, original, over, 1, fadeIn, std::nullopt});
        if (headRegionEnd >= end)
            continue;
        TimelineClip rest = extended;
        if (!withEdge(project, rest, TrimEdge::Left, headRegionEnd, error))
            return false;
        // 分割と同じく、2 つの区間の素材境界が一致しなければ 1 frame ずれて見える。
        if (!hasSyntheticSourceDomain(over) && over.sourceOutFrame != rest.sourceInFrame) {
            error = "トランジションの区間を素材 frame へ一意に換算できません: " + original.name;
            return false;
        }
        segments.push_back({clipIndex, original, rest, 0, std::nullopt, std::nullopt});
    }
    return true;
}

double transitionProgress(const TransitionEnvelope& envelope, std::int64_t timelineFrame) {
    if (envelope.frames <= 0)
        return 1.0;
    const double progress = (static_cast<double>(timelineFrame - envelope.startFrame) + 0.5) /
                            static_cast<double>(envelope.frames);
    return std::clamp(progress, 0.0, 1.0);
}

std::optional<double> renderSegmentOpacity(const TimelineRenderSegment& segment,
                                           std::int64_t timelineFpsNum, std::int64_t timelineFpsDen,
                                           std::int64_t timelineFrame) {
    const auto at =
        originalFrameAt(segment.original, timelineFpsNum, timelineFpsDen, timelineFrame);
    if (!at)
        return std::nullopt;
    double opacity = evaluateClipOpacity(segment.original.effects, at->local, at->sourceLocal,
                                         at->sourceDuration);
    if (segment.fadeIn)
        opacity *= transitionProgress(*segment.fadeIn, timelineFrame);
    return opacity;
}

std::optional<double> renderSegmentGain(const TimelineRenderSegment& segment,
                                        std::int64_t timelineFpsNum, std::int64_t timelineFpsDen,
                                        std::int64_t timelineFrame) {
    const auto at =
        originalFrameAt(segment.original, timelineFpsNum, timelineFpsDen, timelineFrame);
    if (!at)
        return std::nullopt;
    double gain = evaluateClipVolume(segment.original.effects, at->local, at->sourceLocal,
                                     at->sourceDuration);
    // 等パワー: 重なった 2 clip の gain の二乗和が 1 になる。
    if (segment.fadeIn)
        gain *= std::sin(transitionProgress(*segment.fadeIn, timelineFrame) * kHalfPi);
    if (segment.fadeOut)
        gain *= std::cos(transitionProgress(*segment.fadeOut, timelineFrame) * kHalfPi);
    return gain;
}

} // namespace mvm::project

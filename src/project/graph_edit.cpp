#include "project/graph_edit.h"

namespace mvm::project {
TimelineEditResult editGraph(Project& project, const std::string& id,
                             const std::function<bool(GraphClipData&, std::string&)>& edit) {
    TimelineEditResult result;
    auto candidate = project;
    for (std::size_t i = 0; i < candidate.timelineClips.size(); ++i) {
        auto& clip = candidate.timelineClips[i];
        if (clip.id != id)
            continue;
        if (clip.kind != TimelineClipKind::Graph || !edit) {
            result.error = "Graph の編集対象が不正です";
            return result;
        }
        if (!edit(clip.graph, result.error))
            return result;
        const auto valid = validateTimeline(candidate);
        if (!valid.success) {
            result.error = valid.error;
            return result;
        }
        project = std::move(candidate);
        result.success = true;
        result.selectedIndex = static_cast<int>(i);
        return result;
    }
    result.error = "Graph が存在しません";
    return result;
}

TimelineEditResult addGraph(Project& project, std::string id, GraphFunctionId function,
                            std::string name, TrackRef track, std::int64_t start) {
    TimelineClip clip;
    clip.kind = TimelineClipKind::Graph;
    clip.id = std::move(id);
    clip.name = std::move(name);
    clip.graph = defaultGraph(std::move(function));
    clip.sourceFpsNum = project.timelineFpsNum;
    clip.sourceFpsDen = project.timelineFpsDen;
    clip.sourceFrameCount = defaultStillClipFrames(project.timelineFpsNum, project.timelineFpsDen);
    clip.sourceOutFrame = clip.sourceFrameCount;
    return placeTimelineClipAt(project, std::move(clip), track, start);
}

bool copyGraphClip(const TimelineClip& source, TimelineClip& copy,
                   const std::function<std::string()>& fresh, std::string& error) {
    if (source.kind != TimelineClipKind::Graph || !fresh) {
        error = "Graph のコピー対象が不正です";
        return false;
    }
    auto candidate = source;
    candidate.id = fresh();
    if (candidate.id.empty() || candidate.id == source.id ||
        !remapGraphIds(candidate.graph, fresh, error)) {
        if (error.empty())
            error = "Graph のコピー ID を発行できません";
        return false;
    }
    copy = std::move(candidate);
    return true;
}

TimelineEditResult setGraphSourceDuration(Project& project, const std::string& id,
                                          std::int64_t frames) {
    TimelineEditResult result;
    auto candidate = project;
    for (std::size_t i = 0; i < candidate.timelineClips.size(); ++i) {
        auto& c = candidate.timelineClips[i];
        if (c.id != id)
            continue;
        if (c.kind != TimelineClipKind::Graph) {
            result.error = "Graph ではありません";
            return result;
        }
        const bool full = c.sourceOutFrame == c.sourceFrameCount;
        c.sourceFrameCount = frames;
        if (full)
            c.sourceOutFrame = frames;
        const auto valid = validateTimeline(candidate);
        if (!valid.success) {
            result.error = valid.error;
            return result;
        }
        project = std::move(candidate);
        result.success = true;
        result.selectedIndex = static_cast<int>(i);
        return result;
    }
    result.error = "Graph が存在しません";
    return result;
}

std::optional<GraphFrame> evaluateGraphClip(const TimelineClip& clip, core::FrameRate output,
                                            std::int64_t local, std::string& error) {
    if (clip.kind != TimelineClipKind::Graph || clip.speedNum != 1 || clip.speedDen != 1 ||
        validateGraph(clip.graph, clip.sourceFrameCount) != GraphValidationStatus::Valid) {
        error = "Graph の種類・構造・速度が不正です";
        return std::nullopt;
    }
    const auto source = clipSourceFrameAt(clip, output.num, output.den, local);
    if (!source.success) {
        error = source.error;
        return std::nullopt;
    }
    GraphFrame frame;
    frame.sourceFrame = source.frame;
    if (clip.graph.intro.kind == GraphIntroKind::Draw && source.frame < clip.graph.intro.frames) {
        frame.progressNumerator = source.frame;
        frame.progressDenominator = clip.graph.intro.frames;
    }
    return frame;
}
} // namespace mvm::project

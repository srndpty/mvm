#include "project/equation_sequence_edit.h"

#include <algorithm>
#include <limits>

namespace mvm::project {
TimelineEditResult
editEquationSequence(Project& project, const std::string& id,
                     const std::function<bool(EquationSequenceClipData&, std::string&)>& edit) {
    TimelineEditResult result;
    auto candidate = project;
    for (std::size_t i = 0; i < candidate.timelineClips.size(); ++i) {
        auto& clip = candidate.timelineClips[i];
        if (clip.id != id)
            continue;
        if (clip.kind != TimelineClipKind::EquationSequence || !edit) {
            result.error = "数式 sequence の編集対象が不正です";
            return result;
        }
        if (!edit(clip.equationSequence, result.error))
            return result;
        std::vector<EquationInterval> intervals;
        std::int64_t length = 0;
        if (!equationIntervals(clip.equationSequence, intervals, length, result.error))
            return result;
        // 内部編集は trim ではない。見えている範囲を暗黙に縮めない。
        clip.sourceFrameCount = length;
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
    result.error = "数式 sequence が存在しません";
    return result;
}

TimelineEditResult addEquationSequence(Project& project, EquationSequenceClipData data,
                                       std::string id, std::string name, TrackRef track,
                                       std::int64_t start) {
    TimelineEditResult result;
    TimelineClip clip;
    clip.kind = TimelineClipKind::EquationSequence;
    clip.id = std::move(id);
    clip.name = std::move(name);
    clip.track = track;
    clip.timelineStartFrame = start;
    clip.sourceFpsNum = project.timelineFpsNum;
    clip.sourceFpsDen = project.timelineFpsDen;
    clip.equationSequence = std::move(data);
    std::vector<EquationInterval> intervals;
    if (!equationIntervals(clip.equationSequence, intervals, clip.sourceFrameCount, result.error))
        return result;
    clip.sourceOutFrame = clip.sourceFrameCount;
    auto candidate = project;
    candidate.timelineClips.push_back(std::move(clip));
    const auto valid = validateTimeline(candidate);
    if (!valid.success) {
        result.error = valid.error;
        return result;
    }
    result.selectedIndex = static_cast<int>(candidate.timelineClips.size() - 1);
    project = std::move(candidate);
    result.success = true;
    return result;
}

bool copyEquationSequenceClip(const TimelineClip& source, TimelineClip& copy,
                              const std::function<std::string()>& newId, std::string& error) {
    if (source.kind != TimelineClipKind::EquationSequence || !newId) {
        error = "数式 sequence のコピー対象が不正です";
        return false;
    }
    auto candidate = source;
    candidate.id = newId();
    if (candidate.id.empty() || candidate.id == source.id ||
        !remapEquationSequenceIds(candidate.equationSequence, newId, error)) {
        if (error.empty())
            error = "コピーの ID を作れません";
        return false;
    }
    copy = std::move(candidate);
    return true;
}

std::optional<EquationEvaluation> evaluateEquationClip(const TimelineClip& clip,
                                                       core::FrameRate output, std::int64_t frame,
                                                       std::string& error) {
    if (clip.kind != TimelineClipKind::EquationSequence || clip.speedNum != 1 ||
        clip.speedDen != 1) {
        error = "数式 sequence の種類または速度が不正です";
        return std::nullopt;
    }
    const auto source = clipSourceFrameAt(clip, output.num, output.den, frame);
    if (!source.success) {
        error = source.error;
        return std::nullopt;
    }
    return evaluateEquationSequence(clip.equationSequence, source.frame, error);
}

EquationRenderability equationRenderability(const TimelineClip& clip, core::FrameRate output,
                                            int height) {
    EquationRenderability result;
    if (clip.kind != TimelineClipKind::EquationSequence || clip.speedNum != 1 ||
        clip.speedDen != 1 ||
        !validateEquationSequence(clip.equationSequence, height, result.error)) {
        if (result.error.empty())
            result.error = "数式 sequence の種類または速度が不正です";
        return result;
    }
    std::vector<EquationInterval> intervals;
    std::int64_t length = 0;
    if (!equationIntervals(clip.equationSequence, intervals, length, result.error))
        return result;
    if (length != clip.sourceFrameCount || clip.sourceInFrame < 0 ||
        clip.sourceOutFrame <= clip.sourceInFrame || clip.sourceOutFrame > length) {
        result.error = "数式 sequence の素材範囲が不正です";
        return result;
    }
    const core::FrameRate source{clip.sourceFpsNum, clip.sourceFpsDen};
    const auto visibleBegin = core::convertFrameBoundary(clip.sourceInFrame, source, output, true);
    const auto visibleEnd = core::convertFrameBoundary(clip.sourceOutFrame, source, output, true);
    if (!visibleBegin || !visibleEnd || *visibleEnd <= *visibleBegin) {
        result.error = "出力時間軸の換算が不成立または overflow です";
        return result;
    }
    for (const auto& a : clip.equationSequence.actions) {
        const auto state =
            std::find_if(clip.equationSequence.states.begin(), clip.equationSequence.states.end(),
                         [&](const auto& s) { return s.id == a.state; });
        const auto index = static_cast<std::size_t>(state - clip.equationSequence.states.begin());
        const auto interval = std::find_if(intervals.begin(), intervals.end(), [&](const auto& i) {
            return !i.transition && i.index == index;
        });
        std::int64_t begin = 0, end = 0;
        if (__builtin_add_overflow(interval->begin, a.start, &begin) ||
            __builtin_add_overflow(begin, a.duration, &end)) {
            result.error = "action の絶対区間が overflow です";
            return result;
        }
        const auto p = std::find_if(state->parts.begin(), state->parts.end(),
                                    [&](const auto& part) { return part.id == a.target; });
        if (a.targetStatus == EquationTargetStatus::Missing || p == state->parts.end() ||
            p->binding.status != BindingStatus::Bound)
            result.unresolvedActions.push_back(a.id);
        if (end <= clip.sourceInFrame || begin >= clip.sourceOutFrame)
            continue;
        const auto first = core::convertFrameBoundary(begin, source, output, true);
        const auto after = core::convertFrameBoundary(end, source, output, true);
        if (!first || !after) {
            result.error = "action の出力区間が overflow です";
            return result;
        }
        if (std::max(*first, *visibleBegin) >= std::min(*after, *visibleEnd))
            result.unsampledActions.push_back(a.id);
    }
    const bool invalidBinding =
        std::any_of(clip.equationSequence.states.begin(), clip.equationSequence.states.end(),
                    [](const auto& s) {
                        return std::any_of(s.parts.begin(), s.parts.end(), [](const auto& p) {
                            return p.binding.status != BindingStatus::Bound;
                        });
                    });
    result.renderable =
        !invalidBinding && result.unsampledActions.empty() && result.unresolvedActions.empty();
    if (!result.renderable)
        result.error = "部分式の参照が未解決、または action の出力標本がありません";
    return result;
}
} // namespace mvm::project

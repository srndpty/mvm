#include "media/math/equation_sequence_render.h"

#include "media/math/math_key_material.h"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace mvm::math {
namespace {

using detail::appendKeyField;
using detail::keyDigest;

bool fail(std::string& error, std::string message) {
    error = std::move(message);
    return false;
}

std::string argbText(std::uint32_t argb) {
    char text[16] = {};
    std::snprintf(text, sizeof text, "#%08X", static_cast<unsigned>(argb));
    return text;
}

// handle の集合が pairs と unmatched でちょうど一回ずつ使われるか。
bool coversExactly(std::size_t count, const std::vector<std::size_t>& paired,
                   const std::vector<std::size_t>& unmatched) {
    std::vector<int> used(count, 0);
    for (const auto* list : {&paired, &unmatched})
        for (const auto index : *list) {
            if (index >= count || used[index]++)
                return false;
        }
    return std::all_of(used.begin(), used.end(), [](int value) { return value == 1; });
}

bool progressArguments(std::int64_t frame, std::int64_t frames) {
    // 2N + 1 が int64 をあふれない範囲。
    return frames >= 1 && frames <= (std::numeric_limits<std::int64_t>::max() - 1) / 2 &&
           frame >= 0 && frame < frames;
}

} // namespace

const char* equationRenderOperationName(EquationRenderOperation operation) {
    switch (operation) {
    case EquationRenderOperation::Outline:
        return "outline";
    case EquationRenderOperation::Pulse:
        return "pulse";
    }
    return "unknown";
}

bool validateEquationSequenceRenderSpec(const EquationSequenceRenderSpec& spec,
                                        std::string& error) {
    if (spec.compilerVersion.empty())
        return fail(error, "compiler の版がありません");
    if (spec.states.empty())
        return fail(error, "状態がありません");
    if (spec.transitions.size() + 1 != spec.states.size())
        return fail(error, "transition の数が状態の数 - 1 ではありません");
    for (std::size_t s = 0; s < spec.states.size(); ++s) {
        const auto& state = spec.states[s];
        const std::string where = "状態 " + std::to_string(s) + ": ";
        if (state.still.syntax != "latex" || state.still.source.empty() ||
            state.still.fontSize <= 0)
            return fail(error, where + "式の値が不正です");
        if ((state.backgroundArgb >> 24) != 0)
            return fail(error, where + "背景は透明だけを受け付けます");
        if (state.holdFrames < 1)
            return fail(error, where + "hold は 1 frame 以上です");
        if (state.segmenterVersion.empty() || state.segments.empty())
            return fail(error, where + "segment がありません");
        std::string joined;
        for (const auto& segment : state.segments) {
            if (segment.text.empty())
                return fail(error, where + "空の segment があります");
            joined += segment.text;
        }
        if (joined != state.still.source)
            return fail(error, where + "segment を連結しても式と一致しません");
    }
    for (std::size_t t = 0; t < spec.transitions.size(); ++t) {
        const auto& transition = spec.transitions[t];
        const std::string where = "transition " + std::to_string(t) + ": ";
        if (transition.fromState != t || transition.toState != t + 1)
            return fail(error, where + "隣接する状態を結んでいません");
        if (transition.frames < 1 || transition.matcherVersion.empty())
            return fail(error, where + "尺または照合の版が不正です");
        const auto& m = transition.matching;
        std::vector<std::size_t> sources;
        std::vector<std::size_t> targets;
        for (std::size_t at = 0; at < m.pairs.size(); ++at) {
            if (at > 0 && m.pairs[at - 1].source >= m.pairs[at].source)
                return fail(error, where + "対が source の順ではありません");
            sources.push_back(m.pairs[at].source);
            targets.push_back(m.pairs[at].target);
        }
        if (!coversExactly(spec.states[t].segments.size(), sources, m.unmatchedSource) ||
            !coversExactly(spec.states[t + 1].segments.size(), targets, m.unmatchedTarget))
            return fail(error, where + "handle をちょうど一回ずつ使っていません");
    }
    for (std::size_t a = 0; a < spec.actions.size(); ++a) {
        const auto& action = spec.actions[a];
        const std::string where = "action " + std::to_string(a) + ": ";
        if (action.state >= spec.states.size())
            return fail(error, where + "状態がありません");
        const auto& state = spec.states[action.state];
        if (action.segment >= state.segments.size())
            return fail(error, where + "segment がありません");
        if (action.start < 0 || action.duration < 1 || action.start > state.holdFrames ||
            action.duration > state.holdFrames - action.start)
            return fail(error, where + "区間が hold の外です");
        if (action.operation != EquationRenderOperation::Outline &&
            action.operation != EquationRenderOperation::Pulse)
            return fail(error, where + "未知の operation です");
        if (a > 0) {
            const auto& previous = spec.actions[a - 1];
            if (previous.state > action.state ||
                (previous.state == action.state &&
                 previous.start + previous.duration > action.start))
                return fail(error, where + "action が状態・start の順でないか、区間が重なります");
        }
    }
    return true;
}

std::string equationSequenceRenderKey(const EquationSequenceRenderSpec& spec,
                                      const MathToolchainFingerprint& toolchain,
                                      const std::string& sequenceTemplate) {
    const auto number = [](std::string& material, const char* name, std::int64_t value) {
        material += name;
        material += '=';
        material += std::to_string(value);
        material += '\n';
    };
    std::string material = std::string(kEquationSequenceKeyVersion) + "\n";
    appendKeyField(material, "compiler", spec.compilerVersion);
    appendKeyField(material, "raster", kEquationSequenceRasterVersion);
    appendKeyField(material, "progress", kEquationSequenceProgressVersion);
    appendKeyField(material, "accent", argbText(kEquationActionAccentArgb));
    number(material, "states", static_cast<std::int64_t>(spec.states.size()));
    for (const auto& state : spec.states) {
        appendKeyField(material, "syntax", state.still.syntax);
        appendKeyField(material, "source", state.still.source);
        number(material, "font_size", state.still.fontSize);
        appendKeyField(material, "foreground", argbText(state.foregroundArgb));
        appendKeyField(material, "background", argbText(state.backgroundArgb));
        number(material, "hold", state.holdFrames);
        appendKeyField(material, "segmenter", state.segmenterVersion);
        number(material, "segments", static_cast<std::int64_t>(state.segments.size()));
        for (const auto& segment : state.segments) {
            appendKeyField(material, "kind",
                           segment.kind == EquationRenderSegmentKind::Semantic ? "semantic"
                                                                               : "auto");
            appendKeyField(material, "text", segment.text);
        }
    }
    number(material, "transitions", static_cast<std::int64_t>(spec.transitions.size()));
    for (const auto& transition : spec.transitions) {
        number(material, "from", static_cast<std::int64_t>(transition.fromState));
        number(material, "to", static_cast<std::int64_t>(transition.toState));
        number(material, "frames", transition.frames);
        appendKeyField(material, "matcher", transition.matcherVersion);
        std::string pairs;
        for (const auto& pair : transition.matching.pairs)
            pairs += std::to_string(pair.source) + ":" + std::to_string(pair.target) + ",";
        appendKeyField(material, "pairs", pairs);
        std::string list;
        for (const auto index : transition.matching.unmatchedSource)
            list += std::to_string(index) + ",";
        appendKeyField(material, "unmatched_source", list);
        list.clear();
        for (const auto index : transition.matching.unmatchedTarget)
            list += std::to_string(index) + ",";
        appendKeyField(material, "unmatched_target", list);
    }
    number(material, "actions", static_cast<std::int64_t>(spec.actions.size()));
    for (const auto& action : spec.actions) {
        number(material, "state", static_cast<std::int64_t>(action.state));
        number(material, "segment", static_cast<std::int64_t>(action.segment));
        number(material, "start", action.start);
        number(material, "duration", action.duration);
        appendKeyField(material, "operation", equationRenderOperationName(action.operation));
    }
    appendKeyField(material, "backend", toolchain.backendId);
    appendKeyField(material, "toolchain", toolchain.canonical);
    appendKeyField(material, "sequence_template", sequenceTemplate);
    return keyDigest(material);
}

bool equationTransitionProgress(std::int64_t frame, std::int64_t frames, std::int64_t& numerator,
                                std::int64_t& denominator) {
    if (!progressArguments(frame, frames))
        return false;
    numerator = frame;
    denominator = frames;
    return true;
}

bool equationOutlineProgress(std::int64_t frame, std::int64_t frames, std::int64_t& numerator,
                             std::int64_t& denominator) {
    if (!progressArguments(frame, frames))
        return false;
    numerator = 2 * frame + 1;
    denominator = 2 * frames;
    return true;
}

bool equationPulseWeight(std::int64_t frame, std::int64_t frames, std::int64_t& numerator,
                         std::int64_t& denominator) {
    if (!progressArguments(frame, frames))
        return false;
    const std::int64_t offset = 2 * frame + 1 - frames;
    numerator = frames - (offset < 0 ? -offset : offset);
    denominator = frames;
    return true;
}

std::uint8_t equationCoverageOver(std::uint8_t under, std::uint8_t over) {
    const int rest = under * (255 - over);
    return static_cast<std::uint8_t>(over + (rest + 127) / 255);
}

bool composeEquationCoverage(const MathCoverage& under, const MathCoverage& over,
                             MathCoverage& out) {
    if (!mathCoverageValid(under) || !mathCoverageValid(over) || under.width != over.width ||
        under.height != over.height)
        return false;
    MathCoverage result{under.width, under.height, std::vector<std::uint8_t>(under.alpha.size())};
    for (std::size_t i = 0; i < result.alpha.size(); ++i)
        result.alpha[i] = equationCoverageOver(under.alpha[i], over.alpha[i]);
    out = std::move(result);
    return true;
}

const char* equationBackendFailureName(EquationBackendFailure failure) {
    switch (failure) {
    case EquationBackendFailure::None:
        return "none";
    case EquationBackendFailure::NotValidated:
        return "not_validated";
    case EquationBackendFailure::InvalidRequest:
        return "invalid_request";
    case EquationBackendFailure::StructureReportMissing:
        return "structure_report_missing";
    case EquationBackendFailure::StructureReportMalformed:
        return "structure_report_malformed";
    case EquationBackendFailure::GroupingFallback:
        return "grouping_fallback";
    case EquationBackendFailure::SegmentCountMismatch:
        return "segment_count_mismatch";
    case EquationBackendFailure::MissingSegmentObject:
        return "missing_segment_object";
    case EquationBackendFailure::SegmentTypeMismatch:
        return "segment_type_mismatch";
    case EquationBackendFailure::SegmentTextMismatch:
        return "segment_text_mismatch";
    case EquationBackendFailure::EmptyActionTarget:
        return "empty_action_target";
    case EquationBackendFailure::EmptyTransitionHandle:
        return "empty_transition_handle";
    case EquationBackendFailure::SharedDescendant:
        return "shared_descendant";
    case EquationBackendFailure::AliasedPointData:
        return "aliased_point_data";
    case EquationBackendFailure::UnclaimedDescendant:
        return "unclaimed_descendant";
    case EquationBackendFailure::StructureChangedBetweenPhases:
        return "structure_changed_between_phases";
    case EquationBackendFailure::FrameCountMismatch:
        return "frame_count_mismatch";
    case EquationBackendFailure::FrameSizeMismatch:
        return "frame_size_mismatch";
    case EquationBackendFailure::CorruptFrame:
        return "corrupt_frame";
    case EquationBackendFailure::EdgeContact:
        return "edge_contact";
    case EquationBackendFailure::StaticMismatch:
        return "static_mismatch";
    case EquationBackendFailure::EndpointMismatch:
        return "endpoint_mismatch";
    case EquationBackendFailure::ActionMutatedState:
        return "action_mutated_state";
    case EquationBackendFailure::PulseBaseMismatch:
        return "pulse_base_mismatch";
    }
    return "unknown";
}

} // namespace mvm::math

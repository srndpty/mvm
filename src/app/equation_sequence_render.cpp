#include "app/equation_sequence_render.h"

#include "project/project.h"

#include <algorithm>

namespace mvm::app {

std::optional<math::EquationSequenceRenderSpec>
equationSequenceRenderSpecFor(const EquationSequenceSpec& spec, std::string& error) {
    math::EquationSequenceRenderSpec result;
    result.compilerVersion = spec.compilerVersion;
    for (const auto& state : spec.states) {
        math::EquationRenderState out;
        out.still = {state.equation.syntax, state.equation.source, state.equation.fontSize};
        if (!project::parseArgbColor(state.equation.color, out.foregroundArgb) ||
            !project::parseArgbColor(state.equation.backgroundColor, out.backgroundArgb)) {
            error = "状態の色の形式が不正です";
            return std::nullopt;
        }
        out.holdFrames = state.holdFrames;
        out.segmenterVersion = state.segmenterVersion;
        for (const auto& segment : state.segments)
            out.segments.push_back({segment.kind == EquationSegmentKind::Semantic
                                        ? math::EquationRenderSegmentKind::Semantic
                                        : math::EquationRenderSegmentKind::Auto,
                                    segment.text});
        result.states.push_back(std::move(out));
    }
    for (const auto& transition : spec.transitions)
        result.transitions.push_back({transition.fromState, transition.toState, transition.frames,
                                      transition.matching, transition.matcherVersion});
    for (const auto& action : spec.actions)
        result.actions.push_back({action.state, action.segment, action.start, action.duration,
                                  action.operation == project::EquationOperation::Pulse
                                      ? math::EquationRenderOperation::Pulse
                                      : math::EquationRenderOperation::Outline});
    if (!math::validateEquationSequenceRenderSpec(result, error))
        return std::nullopt;
    return result;
}

std::optional<EquationFrameLookup>
equationSequenceFrameAt(const project::EquationSequenceClipData& data,
                        const EquationSequenceSpec& spec, std::int64_t sourceFrame,
                        std::string& error) {
    const auto evaluation = project::evaluateEquationSequence(data, sourceFrame, error);
    if (!evaluation)
        return std::nullopt;
    if (data.states.size() != spec.states.size() ||
        data.transitions.size() != spec.transitions.size() ||
        data.actions.size() != spec.actions.size()) {
        error = "正準入力が Project の sequence と対応しません";
        return std::nullopt;
    }
    const auto stateIndex = [&](const project::StateId& id) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < data.states.size(); ++i)
            if (data.states[i].id == id)
                return i;
        return std::nullopt;
    };
    const auto state = stateIndex(evaluation->state);
    if (!state) {
        error = "評価した状態が sequence にありません";
        return std::nullopt;
    }
    EquationFrameLookup lookup;
    lookup.state = *state;
    if (evaluation->transition) {
        // P3-1 の transition は状態 i → i+1 の i 番目。
        if (*state >= spec.transitions.size() ||
            spec.transitions[*state].frames != evaluation->frames) {
            error = "評価した変形が正準入力と対応しません";
            return std::nullopt;
        }
        lookup.kind = EquationFrameKind::Transition;
        lookup.transition = *state;
        lookup.frame = evaluation->localFrame;
        lookup.frames = evaluation->frames;
        return lookup;
    }
    if (!evaluation->activeAction) {
        lookup.kind = EquationFrameKind::Hold;
        return lookup;
    }
    const auto action = std::find_if(data.actions.begin(), data.actions.end(), [&](const auto& a) {
        return a.id == *evaluation->activeAction;
    });
    if (action == data.actions.end()) {
        error = "評価した action が sequence にありません";
        return std::nullopt;
    }
    // 同じ状態の action は重ならない (P3-1 の検証) ので、状態と start で一意に決まる。
    for (std::size_t i = 0; i < spec.actions.size(); ++i) {
        const auto& candidate = spec.actions[i];
        if (candidate.state != *state || candidate.start != action->start)
            continue;
        if (candidate.duration != action->duration) {
            error = "評価した action が正準入力と対応しません";
            return std::nullopt;
        }
        lookup.kind = EquationFrameKind::HoldAction;
        lookup.action = i;
        lookup.frame = evaluation->localFrame - candidate.start;
        lookup.frames = candidate.duration;
        return lookup;
    }
    error = "評価した action が正準入力にありません";
    return std::nullopt;
}

EquationCompileFailure equationTargetReadiness(EquationTargetProof proof,
                                               const math::EquationBackendValidation& validation,
                                               std::size_t state, std::size_t segment) {
    if (proof == EquationTargetProof::Empty)
        return EquationCompileFailure::UnsupportedEmptyTarget;
    if (!validation.ready())
        return EquationCompileFailure::BackendValidationRequired;
    for (const auto& item : validation.segments) {
        if (item.state == state && item.segment == segment)
            return item.nonEmpty ? EquationCompileFailure::None
                                 : EquationCompileFailure::UnsupportedEmptyTarget;
    }
    return EquationCompileFailure::BackendValidationRequired;
}

} // namespace mvm::app

#include "app/equation_sequence_preview.h"

#include "media/math/equation_sequence_render.h"
#include "media/math/math_raster_layout.h"
#include "project/timeline_edit.h"

#include <algorithm>
#include <cstring>

namespace mvm::app {
namespace {

EquationPreviewRect rectUnion(const EquationPreviewRect& a, const EquationPreviewRect& b) {
    if (a.empty())
        return b;
    if (b.empty())
        return a;
    const int left = std::min(a.x, b.x);
    const int top = std::min(a.y, b.y);
    const int right = std::max(a.x + a.width, b.x + b.width);
    const int bottom = std::max(a.y + a.height, b.y + b.height);
    return {left, top, right - left, bottom - top};
}

bool coverageFits(const EquationPreviewCoverage& coverage, int width, int height) {
    return coverage && width > 0 && height > 0 &&
           coverage->size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
}

bool insideOutput(const EquationPreviewRect& rect, int outputWidth, int outputHeight) {
    return !rect.empty() && rect.x >= 0 && rect.y >= 0 && rect.x + rect.width <= outputWidth &&
           rect.y + rect.height <= outputHeight;
}

// spec の無い (compile に失敗した) sequence の区間: P3-1 の評価で状態と変形だけを決める。
std::optional<EquationFrameLookup> lookupWithoutSpec(const project::EquationSequenceClipData& data,
                                                     std::int64_t sourceFrame, std::string& error) {
    const auto evaluation = project::evaluateEquationSequence(data, sourceFrame, error);
    if (!evaluation)
        return std::nullopt;
    const auto found = std::find_if(data.states.begin(), data.states.end(), [&](const auto& state) {
        return state.id == evaluation->state;
    });
    if (found == data.states.end()) {
        error = "評価した状態が sequence にありません";
        return std::nullopt;
    }
    EquationFrameLookup lookup;
    lookup.state = static_cast<std::size_t>(found - data.states.begin());
    if (evaluation->transition) {
        // P3-1 の transition は状態 i → i+1 の i 番目。
        lookup.kind = EquationFrameKind::Transition;
        lookup.transition = lookup.state;
        lookup.frame = evaluation->localFrame;
        lookup.frames = evaluation->frames;
    }
    return lookup;
}

} // namespace

std::optional<EquationPreviewTime>
equationPreviewTimeAt(const project::TimelineClip& clip, std::int64_t timelineFpsNum,
                      std::int64_t timelineFpsDen, const EquationSequenceSpec* spec,
                      std::int64_t outputFrame, std::string& error) {
    if (clip.kind != project::TimelineClipKind::EquationSequence) {
        error = "Equation Sequence の clip ではありません";
        return std::nullopt;
    }
    // output frame → source frame は P3-1 の写像 (frame 始点の切り捨て) だけを使う。
    const auto source = project::clipSourceFrameAt(clip, timelineFpsNum, timelineFpsDen,
                                                   outputFrame - clip.timelineStartFrame);
    if (!source.success) {
        error = source.error;
        return std::nullopt;
    }
    const auto lookup =
        spec ? equationSequenceFrameAt(clip.equationSequence, *spec, source.frame, error)
             : lookupWithoutSpec(clip.equationSequence, source.frame, error);
    if (!lookup)
        return std::nullopt;
    return EquationPreviewTime{source.frame, *lookup};
}

const char* equationPreviewShownKindName(EquationPreviewShownKind kind) {
    switch (kind) {
    case EquationPreviewShownKind::None:
        return "none";
    case EquationPreviewShownKind::Static:
        return "static";
    case EquationPreviewShownKind::Transition:
        return "transition";
    case EquationPreviewShownKind::Action:
        return "action";
    }
    return "unknown";
}

EquationPreviewShown resolveEquationPreview(const EquationFrameLookup& lookup,
                                            const EquationPreviewAvailability& availability) {
    const auto staticOf = [&](std::size_t state) {
        return availability.staticReady(state)
                   ? EquationPreviewShown{EquationPreviewShownKind::Static, state, 0}
                   : EquationPreviewShown{};
    };
    switch (lookup.kind) {
    case EquationFrameKind::Hold:
        return staticOf(lookup.state);
    case EquationFrameKind::HoldAction:
        // 2 層が揃わない action は見せない (半分だけの action を出さない)。
        if (availability.actionFrameReady(lookup.action, lookup.frame))
            return {EquationPreviewShownKind::Action, lookup.action, lookup.frame};
        return staticOf(lookup.state);
    case EquationFrameKind::Transition:
        // 変形を省いた代用は区間の全 frame で前の状態の静止。後の状態へは区間の後の
        // target hold の frame 0 で切り替わる (途中で切り替えない)。
        if (availability.transitionFrameReady(lookup.transition, lookup.frame))
            return {EquationPreviewShownKind::Transition, lookup.transition, lookup.frame};
        return staticOf(lookup.state);
    }
    return {};
}

std::vector<EquationPreviewLayerId>
equationPreviewCurrentLayers(const EquationFrameLookup& lookup) {
    switch (lookup.kind) {
    case EquationFrameKind::Hold:
        return {};
    case EquationFrameKind::Transition:
        return {{EquationPreviewLayerRole::TransitionFrame, lookup.transition, lookup.frame}};
    case EquationFrameKind::HoldAction:
        return {{EquationPreviewLayerRole::ActionBase, lookup.action, 0},
                {EquationPreviewLayerRole::ActionAccent, lookup.action, lookup.frame}};
    }
    return {};
}

std::vector<std::vector<EquationPreviewLayerId>>
equationPreviewUpcomingLayers(const EquationSequenceSpec& spec, const EquationPreviewTime& time,
                              std::size_t limit) {
    // 区間を source frame の順に並べる (状態 i の action を start の順、その後に変形 i)。
    struct Interval {
        bool transition = false;
        std::size_t index = 0;
        std::int64_t begin = 0;
        std::int64_t frames = 0;
    };

    std::vector<Interval> intervals;
    std::int64_t stateStart = 0;
    for (std::size_t s = 0; s < spec.states.size(); ++s) {
        for (std::size_t a = 0; a < spec.actions.size(); ++a)
            if (spec.actions[a].state == s)
                intervals.push_back(
                    {false, a, stateStart + spec.actions[a].start, spec.actions[a].duration});
        const std::int64_t holdEnd = stateStart + spec.states[s].holdFrames;
        if (s < spec.transitions.size()) {
            intervals.push_back({true, s, holdEnd, spec.transitions[s].frames});
            stateStart = holdEnd + spec.transitions[s].frames;
        }
    }
    std::vector<std::vector<EquationPreviewLayerId>> result;
    const auto add = [&](const Interval& interval, std::int64_t from) {
        for (std::int64_t i = from; i < interval.frames && result.size() < limit; ++i) {
            EquationFrameLookup lookup;
            lookup.kind =
                interval.transition ? EquationFrameKind::Transition : EquationFrameKind::HoldAction;
            lookup.transition = interval.index;
            lookup.action = interval.index;
            lookup.frame = i;
            result.push_back(equationPreviewCurrentLayers(lookup));
        }
    };
    const std::int64_t at = time.sourceFrame;
    for (std::size_t k = 0; k < intervals.size(); ++k) {
        const auto& interval = intervals[k];
        if (at >= interval.begin && at < interval.begin + interval.frames) {
            add(interval, at - interval.begin + 1); // 今の区間の残り
            if (k + 1 < intervals.size())
                add(intervals[k + 1], 0);
            return result;
        }
        if (interval.begin > at) {
            add(interval, 0); // hold の中: 次の区間
            return result;
        }
    }
    return result;
}

EquationPreviewModel::EquationPreviewModel(EquationPreviewInputs inputs)
    : inputs_(std::move(inputs)) {
    const auto& data = inputs_.clip.equationSequence;
    const std::size_t stateCount = data.states.size();
    if (inputs_.statics.size() != stateCount) {
        diagnostics_.push_back("状態の静止の数が sequence の状態の数と違います");
        inputs_.statics.resize(stateCount);
    }
    staticRects_.resize(stateCount);
    for (std::size_t s = 0; s < stateCount; ++s) {
        auto& still = inputs_.statics[s];
        if (!still)
            continue;
        int left = 0;
        int top = 0;
        if (!coverageFits(still->coverage, still->width, still->height) ||
            !math::mathRasterPlacement(still->width, still->height, inputs_.outputWidth,
                                       inputs_.outputHeight, left, top)) {
            diagnostics_.push_back("状態 " + std::to_string(s) +
                                   " の静止を出力に置けません (大きさが不正か出力より大きい)");
            still.reset();
            continue;
        }
        staticRects_[s] = EquationPreviewRect{left, top, still->width, still->height};
        rect_ = rectUnion(rect_, *staticRects_[s]);
    }

    const EquationSequenceSpec* spec = inputs_.spec ? &*inputs_.spec : nullptr;
    bool artifact = inputs_.artifactReady && spec;
    if (artifact && (inputs_.transitions.size() != spec->transitions.size() ||
                     inputs_.actions.size() != spec->actions.size())) {
        diagnostics_.push_back("artifact の区間の数が今の spec と違うため使いません");
        artifact = false;
    }
    if (!artifact) {
        inputs_.artifactReady = false;
        inputs_.transitions.clear();
        inputs_.actions.clear();
        inputs_.transitionFrames.clear();
        inputs_.actionBases.clear();
        inputs_.actionAccents.clear();
    }

    const std::size_t transitionCount = artifact ? spec->transitions.size() : 0;
    const std::size_t actionCount = artifact ? spec->actions.size() : 0;
    transitionPlacements_.resize(transitionCount);
    for (std::size_t t = 0; t < transitionCount; ++t) {
        const auto& item = inputs_.transitions[t];
        const auto& plan = spec->transitions[t];
        if (plan.fromState >= stateCount || plan.toState >= stateCount ||
            item.colors.size() != static_cast<std::size_t>(plan.frames))
            continue;
        const auto& from = inputs_.statics[plan.fromState];
        const auto& to = inputs_.statics[plan.toState];
        if (!from || !to)
            continue; // 両端の静止が無いと配置を決められない (区間は代用で見せる)
        math::MathTransformRasterPlacement placement;
        if (!math::mathTransformRasterPlacement(
                item.width, item.height, item.sourceX, item.sourceY, from->width, from->height,
                item.targetX, item.targetY, to->width, to->height, inputs_.outputWidth,
                inputs_.outputHeight, placement)) {
            diagnostics_.push_back("変形 " + std::to_string(t) +
                                   " の artifact を出力に置けないため、静止で見せます");
            continue;
        }
        transitionPlacements_[t] = placement;
        rect_ =
            rectUnion(rect_, {placement.sourceLeft, placement.sourceTop, item.width, item.height});
        rect_ =
            rectUnion(rect_, {placement.targetLeft, placement.targetTop, item.width, item.height});
    }
    actionRects_.resize(actionCount);
    for (std::size_t a = 0; a < actionCount; ++a) {
        const auto& item = inputs_.actions[a];
        const auto& plan = spec->actions[a];
        if (plan.state >= stateCount || !staticRects_[plan.state] ||
            item.accentColors.size() != static_cast<std::size_t>(plan.duration))
            continue;
        const auto& still = *staticRects_[plan.state];
        // action の artifact は静止の配置を矩形の中に含む。base と accent は同じ原点に置き、
        // 別々に中央へ寄せない。
        if (item.staticX < 0 || item.staticY < 0 || item.staticX + still.width > item.width ||
            item.staticY + still.height > item.height) {
            diagnostics_.push_back("action " + std::to_string(a) +
                                   " の artifact が状態の静止を含みません");
            continue;
        }
        const EquationPreviewRect placed{still.x - item.staticX, still.y - item.staticY, item.width,
                                         item.height};
        if (!insideOutput(placed, inputs_.outputWidth, inputs_.outputHeight)) {
            diagnostics_.push_back("action " + std::to_string(a) +
                                   " の artifact が出力に収まらないため、静止で見せます");
            continue;
        }
        actionRects_[a] = placed;
        rect_ = rectUnion(rect_, placed);
    }

    std::int64_t next = static_cast<std::int64_t>(stateCount);
    for (std::size_t t = 0; t < transitionCount; ++t) {
        transitionBase_.push_back(next);
        next += spec->transitions[t].frames;
    }
    for (std::size_t a = 0; a < actionCount; ++a) {
        actionBase_.push_back(next);
        next += spec->actions[a].duration;
    }
}

std::optional<EquationPreviewRect> EquationPreviewModel::staticRect(std::size_t state) const {
    return state < staticRects_.size() ? staticRects_[state] : std::nullopt;
}

std::optional<math::MathTransformRasterPlacement>
EquationPreviewModel::transitionPlacement(std::size_t index) const {
    return index < transitionPlacements_.size() ? transitionPlacements_[index] : std::nullopt;
}

std::optional<EquationPreviewRect> EquationPreviewModel::actionRect(std::size_t index) const {
    return index < actionRects_.size() ? actionRects_[index] : std::nullopt;
}

bool EquationPreviewModel::staticReady(std::size_t state) const {
    return state < staticRects_.size() && staticRects_[state].has_value();
}

bool EquationPreviewModel::transitionFrameReady(std::size_t transition, std::int64_t frame) const {
    if (transition >= transitionPlacements_.size() || !transitionPlacements_[transition] ||
        frame < 0 || frame >= inputs_.spec->transitions[transition].frames)
        return false;
    const auto found = inputs_.transitionFrames.find({transition, frame});
    const auto& item = inputs_.transitions[transition];
    return found != inputs_.transitionFrames.end() &&
           coverageFits(found->second, item.width, item.height);
}

bool EquationPreviewModel::actionFrameReady(std::size_t action, std::int64_t frame) const {
    if (action >= actionRects_.size() || !actionRects_[action] || frame < 0 ||
        frame >= inputs_.spec->actions[action].duration)
        return false;
    const auto& item = inputs_.actions[action];
    const auto base = inputs_.actionBases.find(action);
    const auto accent = inputs_.actionAccents.find({action, frame});
    const bool baseReady =
        base != inputs_.actionBases.end() && coverageFits(base->second, item.width, item.height);
    const bool accentReady = accent != inputs_.actionAccents.end() &&
                             coverageFits(accent->second, item.width, item.height);
    // 2 層が揃わない action は見せない (base だけ・accent だけの半分の action を出さない)。
    return baseReady && accentReady;
}

std::optional<EquationPreviewTime> EquationPreviewModel::timeAt(std::int64_t outputFrame) const {
    std::string error;
    return equationPreviewTimeAt(inputs_.clip, inputs_.timelineFpsNum, inputs_.timelineFpsDen,
                                 inputs_.spec ? &*inputs_.spec : nullptr, outputFrame, error);
}

EquationPreviewShown EquationPreviewModel::shownAt(std::int64_t outputFrame) const {
    const auto time = timeAt(outputFrame);
    if (!time)
        return {};
    return resolveEquationPreview(time->lookup, *this);
}

std::int64_t EquationPreviewModel::stateCode(const EquationPreviewShown& shown) const {
    switch (shown.kind) {
    case EquationPreviewShownKind::None:
        return -1;
    case EquationPreviewShownKind::Static:
        return static_cast<std::int64_t>(shown.index);
    case EquationPreviewShownKind::Transition:
        return transitionBase_[shown.index] + shown.frame;
    case EquationPreviewShownKind::Action:
        return actionBase_[shown.index] + shown.frame;
    }
    return -1;
}

EquationPreviewShown EquationPreviewModel::shownForCode(std::int64_t code) const {
    if (code < 0)
        return {};
    if (code < static_cast<std::int64_t>(staticRects_.size()))
        return {EquationPreviewShownKind::Static, static_cast<std::size_t>(code), 0};
    for (std::size_t t = 0; t < transitionBase_.size(); ++t)
        if (code >= transitionBase_[t] &&
            code < transitionBase_[t] + inputs_.spec->transitions[t].frames)
            return {EquationPreviewShownKind::Transition, t, code - transitionBase_[t]};
    for (std::size_t a = 0; a < actionBase_.size(); ++a)
        if (code >= actionBase_[a] && code < actionBase_[a] + inputs_.spec->actions[a].duration)
            return {EquationPreviewShownKind::Action, a, code - actionBase_[a]};
    return {};
}

void EquationPreviewModel::fill(std::int64_t code, std::uint8_t* out) const {
    // 背景は透明 (P3 の状態の背景は alpha 0 だけ)。矩形の全画素を書く。
    std::memset(out, 0,
                static_cast<std::size_t>(std::max(rect_.width, 0)) *
                    static_cast<std::size_t>(std::max(rect_.height, 0)) * 4U);
    const auto shown = shownForCode(code);
    switch (shown.kind) {
    case EquationPreviewShownKind::None:
        return;
    case EquationPreviewShownKind::Static: {
        if (!staticReady(shown.index))
            return;
        const auto& still = *inputs_.statics[shown.index];
        const auto& at = *staticRects_[shown.index];
        math::composeMathPatchAt(still.coverage->data(), still.width, still.height,
                                 {still.colorArgb, 0}, out, rect_.width, at.x - rect_.x,
                                 at.y - rect_.y);
        return;
    }
    case EquationPreviewShownKind::Transition: {
        if (!transitionFrameReady(shown.index, shown.frame))
            return;
        const auto& item = inputs_.transitions[shown.index];
        int left = 0;
        int top = 0;
        // 位置は P2 と同じ source → target の補間。色は provenance の frame の色。
        if (!math::mathTransformArtifactOriginAt(*transitionPlacements_[shown.index], shown.frame,
                                                 inputs_.spec->transitions[shown.index].frames,
                                                 left, top))
            return;
        const auto& coverage = *inputs_.transitionFrames.at({shown.index, shown.frame});
        math::composeMathPatchAt(coverage.data(), item.width, item.height,
                                 {item.colors[static_cast<std::size_t>(shown.frame)], 0}, out,
                                 rect_.width, left - rect_.x, top - rect_.y);
        return;
    }
    case EquationPreviewShownKind::Action: {
        const auto base = inputs_.actionBases.find(shown.index);
        const auto accent = inputs_.actionAccents.find({shown.index, shown.frame});
        if (!actionFrameReady(shown.index, shown.frame) || base == inputs_.actionBases.end() ||
            accent == inputs_.actionAccents.end())
            return;
        const auto& item = inputs_.actions[shown.index];
        const auto& at = *actionRects_[shown.index];
        math::composeEquationLayersAt(base->second->data(), item.baseColor, accent->second->data(),
                                      item.accentColors[static_cast<std::size_t>(shown.frame)],
                                      item.width, item.height, out, rect_.width, at.x - rect_.x,
                                      at.y - rect_.y);
        return;
    }
    }
}

} // namespace mvm::app

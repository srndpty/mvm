#include "app/equation_sequence_authoring.h"

#include "core/text_offsets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mvm::app {
namespace {
using project::BindingStatus;
using project::EquationTargetStatus;

std::int64_t roundedFrames(std::int64_t num, std::int64_t den, std::int64_t divisor) {
    if (num <= 0 || den <= 0)
        return 1;
    const auto frames = std::llround(static_cast<long double>(num) /
                                     (static_cast<long double>(den) * divisor));
    return std::max<std::int64_t>(1, frames);
}
} // namespace

const char* equationPartStatusName(EquationPartStatus status) {
    switch (status) {
    case EquationPartStatus::Bound:
        return "bound";
    case EquationPartStatus::InvalidBinding:
        return "invalid_binding";
    case EquationPartStatus::MissingTarget:
        return "missing_target";
    case EquationPartStatus::UnsupportedTexBoundary:
        return "unsupported_tex_boundary";
    case EquationPartStatus::UnsupportedEmptyTarget:
        return "unsupported_empty_target";
    }
    return "unknown";
}

std::string equationPartStatusText(EquationPartStatus status) {
    switch (status) {
    case EquationPartStatus::Bound:
        return "対応済み";
    case EquationPartStatus::InvalidBinding:
        return "範囲が無効 (式の編集で範囲が壊れました。範囲を選び直して再設定してください)";
    case EquationPartStatus::MissingTarget:
        return "部分式が見つかりません (削除済み。範囲を選んで同じ部分式として作り直せます)";
    case EquationPartStatus::UnsupportedTexBoundary:
        return "TeX の構造の途中を区切っています (この範囲は分離して描けません)";
    case EquationPartStatus::UnsupportedEmptyTarget:
        return "描く文字がありません (空白・括弧だけの範囲です)";
    }
    return "不明な状態";
}

bool equationPartRepairable(EquationPartStatus status) {
    return status != EquationPartStatus::Bound;
}

EquationPartStatus equationPartStatus(const project::EquationState& state,
                                      const project::SemanticPart& part) {
    if (part.binding.status != BindingStatus::Bound ||
        !project::equationBindingMatchesSource(state, part.binding))
        return EquationPartStatus::InvalidBinding;
    auto alone = state;
    alone.parts = {part};
    switch (compileEquationPartition(alone).failure) {
    case EquationCompileFailure::UnsupportedTexBoundary:
        return EquationPartStatus::UnsupportedTexBoundary;
    case EquationCompileFailure::UnsupportedEmptyTarget:
        return EquationPartStatus::UnsupportedEmptyTarget;
    case EquationCompileFailure::InvalidBinding:
        return EquationPartStatus::InvalidBinding;
    default:
        return EquationPartStatus::Bound;
    }
}

std::vector<EquationPartView> equationPartViews(const project::EquationSequenceClipData& data,
                                                const project::StateId& stateId) {
    std::vector<EquationPartView> views;
    const auto state = std::find_if(data.states.begin(), data.states.end(),
                                    [&](const auto& s) { return s.id == stateId; });
    if (state == data.states.end())
        return views;
    auto references = [&](EquationPartView& view) {
        for (const auto& a : data.actions)
            if (a.state == stateId && a.target == view.id)
                view.actions.push_back(a.id);
        for (const auto& t : data.transitions)
            for (const auto& pair : t.correspondence)
                if ((t.from == stateId && pair.from == view.id) ||
                    (t.to == stateId && pair.to == view.id))
                    view.correspondences.push_back(t.id);
    };
    for (const auto& p : state->parts) {
        EquationPartView view;
        view.id = p.id;
        view.label = p.label;
        view.expectedText = p.binding.expectedText;
        view.status = equationPartStatus(*state, p);
        if (view.status != EquationPartStatus::InvalidBinding) {
            const auto& source = state->equation.source;
            const auto begin =
                core::utf8ToUtf16Offset(source, static_cast<std::size_t>(p.binding.begin));
            const auto end = core::utf8ToUtf16Offset(source, static_cast<std::size_t>(p.binding.end));
            if (begin && end)
                view.rangeUtf16 = std::make_pair(*begin, *end);
        }
        references(view);
        views.push_back(std::move(view));
    }
    // 今の式の範囲を持つものを source 順に、無効は保存順のまま後ろへ。
    std::stable_sort(views.begin(), views.end(), [](const auto& a, const auto& b) {
        if (a.rangeUtf16.has_value() != b.rangeUtf16.has_value())
            return a.rangeUtf16.has_value();
        return a.rangeUtf16 && b.rangeUtf16 && a.rangeUtf16->first < b.rangeUtf16->first;
    });
    for (const auto& a : data.actions) {
        if (a.state != stateId || a.targetStatus != EquationTargetStatus::Missing)
            continue;
        if (std::any_of(views.begin(), views.end(), [&](const auto& v) { return v.id == a.target; }))
            continue;
        EquationPartView view;
        view.id = a.target;
        view.status = EquationPartStatus::MissingTarget;
        view.exists = false;
        references(view);
        views.push_back(std::move(view));
    }
    return views;
}

std::string equationCompileFailureText(EquationCompileFailure failure) {
    switch (failure) {
    case EquationCompileFailure::None:
        return "準備完了";
    case EquationCompileFailure::InvalidBinding:
        return "範囲が無効な部分式があります";
    case EquationCompileFailure::MissingPart:
        return "action の対象の部分式が見つかりません";
    case EquationCompileFailure::UnsupportedTexBoundary:
        return "TeX の構造の途中を区切る部分式があります";
    case EquationCompileFailure::PartitionConflict:
        return "部分式の範囲が重なっています";
    case EquationCompileFailure::InvalidCorrespondencePlan:
        return "変形の対応が解決できません";
    case EquationCompileFailure::UnsupportedEmptyTarget:
        return "描く文字の無い部分式があります";
    case EquationCompileFailure::BackendValidationRequired:
        return "描画の検証待ち";
    case EquationCompileFailure::InvalidSequence:
        return "sequence の構造が不正です (修復できません)";
    }
    return "不明な状態";
}

bool equationCompileFailureRepairable(EquationCompileFailure failure) {
    switch (failure) {
    case EquationCompileFailure::InvalidBinding:
    case EquationCompileFailure::MissingPart:
    case EquationCompileFailure::UnsupportedTexBoundary:
    case EquationCompileFailure::PartitionConflict:
    case EquationCompileFailure::InvalidCorrespondencePlan:
    case EquationCompileFailure::UnsupportedEmptyTarget:
        return true;
    case EquationCompileFailure::None:
    case EquationCompileFailure::BackendValidationRequired:
    case EquationCompileFailure::InvalidSequence:
        return false;
    }
    return false;
}

std::string equationBackendFailureText(math::EquationBackendFailure failure) {
    using F = math::EquationBackendFailure;
    switch (failure) {
    case F::None:
        return "検証済み";
    case F::NotValidated:
        return "未検証";
    case F::EmptyActionTarget:
        return "強調の対象に描画される文字がありません";
    case F::EmptyTransitionHandle:
        return "変形の対応の片側に描画される文字がありません";
    case F::SegmentTextMismatch:
    case F::SegmentCountMismatch:
    case F::MissingSegmentObject:
    case F::SegmentTypeMismatch:
    case F::GroupingFallback:
    case F::SharedDescendant:
    case F::AliasedPointData:
    case F::UnclaimedDescendant:
        return "描画系が部分式を式の他の部分と分けて描けません (範囲を選び直してください)";
    case F::CorruptFrame:
        return "描画結果が壊れていたため破棄しました (描き直しが必要です)";
    case F::InvalidRequest:
    case F::StructureReportMissing:
    case F::StructureReportMalformed:
    case F::StructureChangedBetweenPhases:
    case F::FrameCountMismatch:
    case F::FrameSizeMismatch:
    case F::EdgeContact:
    case F::StaticMismatch:
    case F::EndpointMismatch:
    case F::ActionMutatedState:
    case F::PulseBaseMismatch:
        return "描画結果の検証に失敗しました";
    }
    return "不明な描画の失敗";
}

bool equationBackendFailureIsContent(math::EquationBackendFailure failure) {
    using F = math::EquationBackendFailure;
    switch (failure) {
    case F::EmptyActionTarget:
    case F::EmptyTransitionHandle:
    case F::SegmentTextMismatch:
    case F::SegmentCountMismatch:
    case F::MissingSegmentObject:
    case F::SegmentTypeMismatch:
    case F::GroupingFallback:
    case F::SharedDescendant:
    case F::AliasedPointData:
    case F::UnclaimedDescendant:
        return true;
    default:
        return false;
    }
}

std::int64_t equationDefaultHoldFrames(std::int64_t fpsNum, std::int64_t fpsDen) {
    return roundedFrames(fpsNum, fpsDen, 1);
}

std::int64_t equationDefaultTransitionFrames(std::int64_t fpsNum, std::int64_t fpsDen) {
    return roundedFrames(fpsNum, fpsDen, 2);
}

std::string equationFramesText(std::int64_t frames, std::int64_t fpsNum, std::int64_t fpsDen) {
    std::string text = std::to_string(frames) + "f";
    if (fpsNum > 0 && fpsDen > 0) {
        const long double seconds = static_cast<long double>(frames) *
                                    static_cast<long double>(fpsDen) /
                                    static_cast<long double>(fpsNum);
        char buffer[48];
        std::snprintf(buffer, sizeof buffer, " (%.2f 秒)", static_cast<double>(seconds));
        text += buffer;
    }
    return text;
}

project::EquationSequenceClipData newEquationSequenceData(std::string source,
                                                          std::int64_t holdFrames,
                                                          const std::function<std::string()>& id) {
    project::EquationSequenceClipData data;
    data.states.push_back(newEquationState({}, std::move(source), holdFrames, id));
    return data;
}

project::EquationState newEquationState(const project::MathClipData& reference,
                                        std::string source, std::int64_t holdFrames,
                                        const std::function<std::string()>& id) {
    project::EquationState state;
    state.id = {id ? id() : std::string{}};
    state.equation = reference;
    state.equation.source = std::move(source);
    state.equation.backgroundColor = "#00000000";
    state.revision = id ? id() : std::string{};
    state.holdFrames = holdFrames;
    return state;
}

bool applyTrustedEquationEdits(project::EquationSequenceClipData& data,
                               const project::StateId& state,
                               const std::vector<project::TrustedEquationEdit>& edits,
                               const std::function<std::string()>& newRevision, int height,
                               std::string& error) {
    if (edits.empty() || !newRevision) {
        error = "適用する信頼済み編集がありません";
        return false;
    }
    auto candidate = data;
    for (const auto& edit : edits)
        if (!project::editEquationSourceTrusted(candidate, state, edit, newRevision(), height,
                                                error))
            return false;
    data = std::move(candidate);
    return true;
}

std::optional<std::size_t> nearestSurvivingIndex(std::size_t previousIndex, std::size_t count) {
    if (count == 0)
        return std::nullopt;
    return std::min(previousIndex, count - 1);
}
} // namespace mvm::app

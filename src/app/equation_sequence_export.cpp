#include "app/equation_sequence_export.h"

#include "project/timeline_edit.h"

#include <cstring>

namespace mvm::app {
namespace {
EquationExportReadiness failed(EquationExportFailure failure, const std::string& detail) {
    return {failure, EquationCompileFailure::None, detail};
}
} // namespace

EquationExportReadiness composeEquationExportFrame(const EquationExportSnapshot& snapshot,
                                                   std::int64_t outputFrame,
                                                   std::vector<std::uint8_t>& rgba) {
    if (!snapshot.readiness.ready())
        return snapshot.readiness;
    if (!snapshot.validate)
        return failed(EquationExportFailure::ProvenanceMissing, "描画結果の検証情報がありません");
    if (auto valid = snapshot.validate(); !valid.ready())
        return valid;
    auto inputs = snapshot.presentation;
    if (!inputs.spec || inputs.outputWidth <= 0 || inputs.outputHeight <= 0)
        return failed(EquationExportFailure::InvalidSourceRange, "数式の書き出し範囲が不正です");
    std::string error;
    const auto time =
        equationPreviewTimeAt(inputs.clip, inputs.timelineFpsNum, inputs.timelineFpsDen,
                              &*inputs.spec, outputFrame, error);
    if (!time)
        return failed(EquationExportFailure::InvalidSourceRange, error);
    // RAM 常駐は参照せず、この要求に必要な層だけを所有する。
    inputs.transitionFrames.clear();
    inputs.actionBases.clear();
    inputs.actionAccents.clear();
    for (const auto& layer : equationPreviewCurrentLayers(time->lookup)) {
        if (!snapshot.loadLayer)
            return failed(EquationExportFailure::FrameMissing, "必要な数式 frame を取得できません");
        std::vector<std::uint8_t> bytes;
        if (auto loaded = snapshot.loadLayer(layer, bytes); !loaded.ready())
            return loaded;
        auto coverage = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
        switch (layer.role) {
        case EquationPreviewLayerRole::TransitionFrame:
            inputs.transitionFrames[{layer.interval, layer.frame}] = coverage;
            break;
        case EquationPreviewLayerRole::ActionBase:
            inputs.actionBases[layer.interval] = coverage;
            break;
        case EquationPreviewLayerRole::ActionAccent:
            inputs.actionAccents[{layer.interval, layer.frame}] = coverage;
            break;
        }
    }
    EquationPreviewModel model(std::move(inputs));
    const auto& lookup = time->lookup;
    const EquationPreviewShown exact =
        lookup.kind == EquationFrameKind::Hold
            ? EquationPreviewShown{EquationPreviewShownKind::Static, lookup.state, 0}
        : lookup.kind == EquationFrameKind::Transition
            ? EquationPreviewShown{EquationPreviewShownKind::Transition, lookup.transition,
                                   lookup.frame}
            : EquationPreviewShown{EquationPreviewShownKind::Action, lookup.action, lookup.frame};
    // preview の代用は export の成功にならない。
    if (model.shownAt(outputFrame) != exact)
        return failed(EquationExportFailure::FrameMissing, "正確な数式の層または配置がありません");
    const auto rect = model.patchRect();
    std::vector<std::uint8_t> patch(static_cast<std::size_t>(rect.width) *
                                    static_cast<std::size_t>(rect.height) * 4);
    model.fill(model.stateCode(exact), patch.data());
    const int width = model.inputs().outputWidth;
    const int height = model.inputs().outputHeight;
    rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);
    for (int y = 0; y < rect.height; ++y)
        std::memcpy(
            rgba.data() + (static_cast<std::size_t>(rect.y + y) * static_cast<std::size_t>(width) +
                           static_cast<std::size_t>(rect.x)) *
                              4,
            patch.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(rect.width) * 4,
            static_cast<std::size_t>(rect.width) * 4);
    return {};
}

EquationSequenceExportPlan planEquationSequenceExport(const project::TimelineClip& clip,
                                                      std::int64_t fpsNum, std::int64_t fpsDen,
                                                      int width, int height,
                                                      const EquationExportSnapshot& snapshot,
                                                      const std::function<bool()>& cancelled) {
    EquationSequenceExportPlan plan;
    const auto compiled = compileEquationSequence(clip.equationSequence);
    if (!compiled.value) {
        auto reason = EquationExportFailure::CompileFailed;
        switch (compiled.failure) {
        case EquationCompileFailure::MissingPart:
            reason = EquationExportFailure::MissingPart;
            break;
        case EquationCompileFailure::InvalidBinding:
            reason = EquationExportFailure::InvalidBinding;
            break;
        case EquationCompileFailure::UnsupportedTexBoundary:
            reason = EquationExportFailure::UnsupportedTexBoundary;
            break;
        default:
            break;
        }
        plan.readiness = {reason, compiled.failure, "数式の内容を修復してください"};
        return plan;
    }
    auto requested = snapshot.presentation;
    requested.clip = clip;
    requested.spec = *compiled.value;
    requested.timelineFpsNum = fpsNum;
    requested.timelineFpsDen = fpsDen;
    requested.outputWidth = width;
    requested.outputHeight = height;
    plan.snapshot = snapshot.prepare ? snapshot.prepare(requested, cancelled) : snapshot;
    plan.readiness = plan.snapshot.readiness;
    if (!plan.readiness.ready())
        return plan;
    const auto& prepared = plan.snapshot;
    std::string error;
    const auto spec = equationSequenceRenderSpecFor(*compiled.value, error);
    if (!spec || *spec != prepared.renderSpec || prepared.sequenceKey.empty() ||
        prepared.sequenceKey !=
            math::equationSequenceRenderKey(*spec, prepared.toolchain, prepared.sequenceTemplate)) {
        plan.readiness = failed(EquationExportFailure::StaleSequenceKey,
                                "数式の描画結果が現在の入力と一致しません");
        return plan;
    }
    auto& inputs = plan.snapshot.presentation;
    inputs.clip = clip;
    inputs.spec = *compiled.value;
    inputs.timelineFpsNum = fpsNum;
    inputs.timelineFpsDen = fpsDen;
    inputs.outputWidth = width;
    inputs.outputHeight = height;
    project::Project timing;
    timing.timelineFpsNum = fpsNum;
    timing.timelineFpsDen = fpsDen;
    const auto duration = project::timelineClipDuration(timing, clip);
    if (!duration.success) {
        plan.readiness = failed(EquationExportFailure::InvalidSourceRange, duration.error);
        return plan;
    }
    std::vector<std::uint8_t> rgba;
    for (std::int64_t i = 0; i < duration.frame; ++i) {
        if (cancelled && cancelled()) {
            plan.readiness =
                failed(EquationExportFailure::Cancelled, "数式の書き出し準備をキャンセルしました");
            return plan;
        }
        const auto frame = clip.timelineStartFrame + i;
        const auto time =
            equationPreviewTimeAt(clip, fpsNum, fpsDen, &*compiled.value, frame, error);
        if (!time) {
            plan.readiness = failed(EquationExportFailure::InvalidSourceRange, error);
            return plan;
        }
        plan.frames.push_back(*time);
        if (auto composed = composeEquationExportFrame(plan.snapshot, frame, rgba);
            !composed.ready()) {
            plan.readiness = composed;
            return plan;
        }
    }
    return plan;
}
} // namespace mvm::app

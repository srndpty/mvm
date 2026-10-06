#ifndef MVM_PROJECT_EQUATION_SEQUENCE_EDIT_H
#define MVM_PROJECT_EQUATION_SEQUENCE_EDIT_H
#include "project/timeline_edit.h"

namespace mvm::project {
// Project の確定前に一回だけ呼ぶ。候補全体を検証して原子的に置き換える。
TimelineEditResult
editEquationSequence(Project&, const std::string& clipId,
                     const std::function<bool(EquationSequenceClipData&, std::string&)>& edit);
TimelineEditResult addEquationSequence(Project&, EquationSequenceClipData, std::string clipId,
                                       std::string name, TrackRef track, std::int64_t start);
bool copyEquationSequenceClip(const TimelineClip&, TimelineClip& copy,
                              const std::function<std::string()>& newId, std::string& error);
std::optional<EquationEvaluation> evaluateEquationClip(const TimelineClip&, core::FrameRate output,
                                                       std::int64_t clipLocalOutputFrame,
                                                       std::string& error);

struct EquationRenderability {
    bool renderable = false;
    std::vector<ActionId> unsampledActions;
    std::vector<ActionId> unresolvedActions;
    std::string error;
};

EquationRenderability equationRenderability(const TimelineClip&, core::FrameRate output,
                                            int outputHeight);
} // namespace mvm::project
#endif

#ifndef MVM_PROJECT_GRAPH_EDIT_H
#define MVM_PROJECT_GRAPH_EDIT_H
#include "project/timeline_edit.h"

namespace mvm::project {
// 全体検証の成功時だけ Project を置き換える。履歴確定は controller が一回だけ行う。
TimelineEditResult editGraph(Project&, const std::string& clipId,
                             const std::function<bool(GraphClipData&, std::string&)>&);
TimelineEditResult addGraph(Project&, std::string clipId, GraphFunctionId, std::string name,
                            TrackRef, std::int64_t start);
// 作成メニューの入口。文字・数式と同じく、start で空いている映像 track へ置く (placeStillClipAt)。
TimelineEditResult placeNewGraph(Project&, std::string clipId, GraphFunctionId, std::string name,
                                 std::int64_t start);
bool copyGraphClip(const TimelineClip&, TimelineClip&, const std::function<std::string()>&,
                   std::string& error);
TimelineEditResult setGraphSourceDuration(Project&, const std::string& clipId, std::int64_t frames);

struct GraphFrame {
    std::int64_t sourceFrame = 0;
    std::int64_t progressNumerator = 1, progressDenominator = 1;
    bool operator==(const GraphFrame&) const = default;
};

std::optional<GraphFrame> evaluateGraphClip(const TimelineClip&, core::FrameRate,
                                            std::int64_t localOutputFrame, std::string& error);
} // namespace mvm::project
#endif

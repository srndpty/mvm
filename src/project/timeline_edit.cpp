#include "project/timeline_edit.h"

#include <algorithm>
#include <utility>

namespace mvm::project {
namespace {

bool validIndex(const Project& project, int index) {
    return index >= 0 && index < static_cast<int>(project.timelineClips.size());
}

} // namespace

TimelineEditResult moveTimelineClip(Project& project, int selectedIndex, int offset) {
    TimelineEditResult result;
    if (!validIndex(project, selectedIndex)) {
        result.error = "移動する timeline clip がありません";
        return result;
    }
    if (offset != -1 && offset != 1) {
        result.error = "timeline clip の移動量が不正です";
        return result;
    }

    const int destination = selectedIndex + offset;
    if (!validIndex(project, destination)) {
        result.error = "timeline clip をこれ以上移動できません";
        return result;
    }

    std::swap(project.timelineClips[static_cast<std::size_t>(selectedIndex)],
              project.timelineClips[static_cast<std::size_t>(destination)]);
    result.success = true;
    result.selectedIndex = destination;
    return result;
}

TimelineEditResult deleteTimelineClip(Project& project, int selectedIndex) {
    TimelineEditResult result;
    if (!validIndex(project, selectedIndex)) {
        result.error = "削除する timeline clip がありません";
        return result;
    }

    project.timelineClips.erase(project.timelineClips.begin() + selectedIndex);
    result.success = true;
    if (!project.timelineClips.empty()) {
        result.selectedIndex =
            std::min(selectedIndex, static_cast<int>(project.timelineClips.size()) - 1);
    }
    return result;
}

TimelineEditResult appendManimTimelineClip(Project& project, const ManimAsset& asset) {
    TimelineEditResult result;
    if (asset.generatedVideoPath.empty() || asset.sceneName.empty()) {
        result.error = "timeline に配置できる生成済み Manim asset ではありません";
        return result;
    }
    const auto existing =
        std::find_if(project.timelineClips.begin(), project.timelineClips.end(),
                     [](const auto& clip) { return clip.kind == TimelineClipKind::Manim; });
    if (existing != project.timelineClips.end()) {
        result.error = "Manim asset はすでに timeline に配置されています";
        return result;
    }

    project.timelineClips.push_back(
        {TimelineClipKind::Manim, asset.generatedVideoPath, asset.sceneName});
    result.success = true;
    result.selectedIndex = static_cast<int>(project.timelineClips.size()) - 1;
    return result;
}

} // namespace mvm::project

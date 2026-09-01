#include "app/timeline_preview_mapping.h"

#include "project/timeline_edit.h"

namespace mvm::app {

TimelinePreviewFrameMapping mapTimelinePreviewFrame(const project::Project& project,
                                                    std::int64_t timelineFrame) {
    TimelinePreviewFrameMapping result;
    result.outputFrameNumber = timelineFrame;
    if (timelineFrame < 0) {
        result.error = "Preview timeline frameは0以上である必要があります";
        return result;
    }
    const auto active = project::activeClipsAt(project, timelineFrame);
    for (std::size_t track = 0; track < active.size(); ++track) {
        const project::TimelineClip* clip = active[track];
        if (!clip)
            continue;
        result.layers.push_back({static_cast<project::VideoTrack>(track),
                                 static_cast<int>(clip - project.timelineClips.data()), clip->id,
                                 clip->sourceInFrame + (timelineFrame - clip->timelineStartFrame)});
    }
    result.success = true;
    return result;
}

bool sameTimelinePreviewSourceSet(const TimelinePreviewFrameMapping& a,
                                  const TimelinePreviewFrameMapping& b) {
    if (!a.success || !b.success || a.layers.size() != b.layers.size())
        return false;
    for (std::size_t i = 0; i < a.layers.size(); ++i)
        if (a.layers[i].track != b.layers[i].track || a.layers[i].clipId != b.layers[i].clipId)
            return false;
    return true;
}

} // namespace mvm::app

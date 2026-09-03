#include "app/timeline_preview_mapping.h"

#include "project/timeline_edit.h"

#include <string>

namespace mvm::app {
namespace {

std::string trackLabel(const project::Project& project, project::TrackKind kind, int index) {
    const auto& tracks = project::tracksOfKind(project, kind);
    if (index < 0 || index >= static_cast<int>(tracks.size()))
        return project::defaultTrackName(kind, index);
    return tracks[static_cast<std::size_t>(index)].name;
}

} // namespace

TimelinePreviewFrameMapping mapTimelinePreviewFrame(const project::Project& project,
                                                    std::int64_t timelineFrame) {
    TimelinePreviewFrameMapping result;
    result.outputFrameNumber = timelineFrame;
    if (timelineFrame < 0) {
        result.error = "Preview timeline frameは0以上である必要があります";
        return result;
    }
    const auto active = project::activeClipsAt(project, project::TrackKind::Video, timelineFrame);
    for (std::size_t index = 0; index < active.size(); ++index) {
        const project::TimelineClip* clip = active[index];
        if (!clip)
            continue;
        // mute した video track は「黒」ではなく layer から外す。
        if (project.videoTracks[index].muted)
            continue;
        result.layers.push_back({static_cast<int>(index),
                                 static_cast<int>(clip - project.timelineClips.data()), clip->id,
                                 clip->sourceInFrame + (timelineFrame - clip->timelineStartFrame)});
    }
    if (result.layers.size() > kMaxPreviewVideoLayers) {
        result.layers.clear();
        result.error = "同時に重なる video track が " + std::to_string(kMaxPreviewVideoLayers) +
                       " 本を超えています。preview はここまでしか合成できません";
        return result;
    }
    result.success = true;
    return result;
}

bool sameTimelinePreviewSourceSet(const TimelinePreviewFrameMapping& a,
                                  const TimelinePreviewFrameMapping& b) {
    if (!a.success || !b.success || a.layers.size() != b.layers.size())
        return false;
    for (std::size_t i = 0; i < a.layers.size(); ++i) {
        if (a.layers[i].videoTrackIndex != b.layers[i].videoTrackIndex ||
            a.layers[i].clipId != b.layers[i].clipId)
            return false;
    }
    return true;
}

TimelinePreviewAudioMapping mapTimelinePreviewAudio(const project::Project& project,
                                                    std::int64_t timelineFrame) {
    TimelinePreviewAudioMapping result;
    if (timelineFrame < 0) {
        result.error = "Preview timeline frameは0以上である必要があります";
        return result;
    }
    const auto active = project::activeClipsAt(project, project::TrackKind::Audio, timelineFrame);
    for (std::size_t index = 0; index < active.size(); ++index) {
        const project::TimelineClip* clip = active[index];
        if (!clip || project.audioTracks[index].muted)
            continue;
        if (result.hasAudio) {
            result.hasAudio = false;
            result.error = "audio track " +
                           trackLabel(project, project::TrackKind::Audio, static_cast<int>(index)) +
                           " が重なっています。preview は同時に 1 本の audio しか再生できません";
            return result;
        }
        result.hasAudio = true;
        result.clipIndex = static_cast<int>(clip - project.timelineClips.data());
        result.clipId = clip->id;
        result.sourceFrameNumber = clip->sourceInFrame + (timelineFrame - clip->timelineStartFrame);
    }
    result.success = true;
    return result;
}

} // namespace mvm::app

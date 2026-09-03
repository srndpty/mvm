#include "app/timeline_preview_mapping.h"

#include "core/checked_output_timebase.h"
#include "project/timeline_edit.h"

#include <cmath>
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

AudioPreviewOffset audioPreviewSampleOffset(const project::Project& project,
                                            const project::TimelineClip& clip) {
    AudioPreviewOffset result;
    const auto sourceTimebase = core::CheckedOutputTimebase::create(
        clip.sourceFpsNum, clip.sourceFpsDen, core::kQualifiedAudioSampleRate);
    const auto timelineTimebase = core::CheckedOutputTimebase::create(
        project.timelineFpsNum, project.timelineFpsDen, core::kQualifiedAudioSampleRate);
    if (!sourceTimebase || !timelineTimebase) {
        result.error = "audio用のtimebaseを構築できません";
        return result;
    }
    const auto sourceSample = sourceTimebase.value().seekTargetSample(clip.sourceInFrame);
    const auto timelineSample = timelineTimebase.value().seekTargetSample(clip.timelineStartFrame);
    if (!sourceSample || !timelineSample) {
        result.error = "audio clipのsample位置を換算できません";
        return result;
    }
    result.success = true;
    result.sampleOffset = sourceSample.value() - timelineSample.value();
    return result;
}

AudioSourceFrameCount audioSourceFrameCount(double durationSeconds, std::int64_t fpsNum,
                                            std::int64_t fpsDen) {
    AudioSourceFrameCount result;
    if (!(durationSeconds > 0.0) || !std::isfinite(durationSeconds) || fpsNum <= 0 || fpsDen <= 0) {
        result.error = "音声素材の尺またはframe rateが不正です";
        return result;
    }
    const double frames =
        durationSeconds * static_cast<double>(fpsNum) / static_cast<double>(fpsDen);
    const double ceiled = std::ceil(frames);
    if (!(ceiled >= 1.0) || ceiled > 9.0e15) {
        result.error = "音声素材の尺をframe数へ変換できません";
        return result;
    }
    result.success = true;
    result.frameCount = static_cast<std::int64_t>(ceiled);
    return result;
}

} // namespace mvm::app

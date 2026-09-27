#include "app/timeline_preview_mapping.h"

#include "core/checked_output_timebase.h"
#include "project/timeline_edit.h"

#include <cmath>
#include <string>

namespace mvm::app {
namespace {
__extension__ using WideInteger = __int128;
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
        const auto sourceFrame =
            project::clipSourceFrameAt(*clip, project.timelineFpsNum, project.timelineFpsDen,
                                       timelineFrame - clip->timelineStartFrame);
        if (!sourceFrame.success) {
            result.layers.clear();
            result.error = clip->name + ": preview frameを素材frameへ換算できません";
            return result;
        }
        result.layers.push_back({static_cast<int>(index),
                                 static_cast<int>(clip - project.timelineClips.data()), clip->id,
                                 sourceFrame.frame});
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

PreviewVideoMapping previewVideoMappingOf(const project::TimelineClip& clip) {
    return {clip.mediaPath, clip.sourceInFrame, clip.timelineStartFrame, clip.sourceFpsNum,
            clip.sourceFpsDen};
}

bool previewVideoMappingCovers(const project::Project& project,
                               const PreviewVideoMapping& installed,
                               const project::TimelineClip& clip) {
    const PreviewVideoMapping wanted = previewVideoMappingOf(clip);
    if (installed == wanted)
        return true;
    if (installed.mediaPath != wanted.mediaPath || installed.sourceFpsNum != wanted.sourceFpsNum ||
        installed.sourceFpsDen != wanted.sourceFpsDen)
        return false;
    // source は素材 frame s を、素材の 0 frame から数えた output 位置の区間
    // [ceil((s - 1/2) R), ceil((s + 1/2) R)) に写し、timeline 上では
    // start - ceil(in R) だけずらす (R = timeline fps / 素材 fps、core/source_frame_mapping.h)。
    // 位置を素材の絶対位置で数えるので、このずらし量が同じなら in が違っても全 frame で
    // 同じ区間を指す。レーザーで分割した右半分は trim の境界も ceil(in R) で決まるため、
    // 素材 fps によらず常にこれを満たす。installed の in より前の frame は source が
    // 先頭で切っているので、in が戻る方向には使い回さない。
    const std::int64_t sourceAdvance = wanted.sourceInFrame - installed.sourceInFrame;
    if (sourceAdvance < 0)
        return false;
    const auto origin = [&](const PreviewVideoMapping& mapping) {
        return project::sourceBoundaryToTimelineBoundary(
            mapping.sourceInFrame, mapping.sourceFpsNum, mapping.sourceFpsDen,
            project.timelineFpsNum, project.timelineFpsDen);
    };
    const auto installedOrigin = origin(installed);
    const auto wantedOrigin = origin(wanted);
    if (!installedOrigin.success || !wantedOrigin.success)
        return false;
    return static_cast<WideInteger>(wanted.timelineStartFrame) - wantedOrigin.frame ==
           static_cast<WideInteger>(installed.timelineStartFrame) - installedOrigin.frame;
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
        result.layers.push_back({static_cast<int>(index),
                                 static_cast<int>(clip - project.timelineClips.data()), clip->id,
                                 clip->sourceInFrame + (timelineFrame - clip->timelineStartFrame)});
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

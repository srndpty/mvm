#include "app/timeline_preview_mapping.h"

#include "core/checked_output_timebase.h"
#include "project/clip_effects.h"
#include "project/timeline_edit.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace mvm::app {
namespace {
__extension__ using WideInteger = __int128;
} // namespace

TimelinePreviewPlan buildTimelinePreviewPlan(const project::Project& project) {
    TimelinePreviewPlan plan;
    std::vector<project::TimelineRenderSegment> video;
    std::vector<project::TimelineRenderSegment> audio;
    if (!project::timelineRenderSegments(project, project::TrackKind::Video, video, plan.error) ||
        !project::timelineRenderSegments(project, project::TrackKind::Audio, audio, plan.error))
        return plan;
    const auto entries = [&](std::vector<project::TimelineRenderSegment>& segments,
                             std::vector<TimelinePreviewPlan::Entry>& out) {
        out.reserve(segments.size());
        for (auto& segment : segments) {
            const auto duration = project::timelineClipDuration(project, segment.clip);
            if (!duration.success) {
                plan.error = segment.clip.name + ": " + duration.error;
                return false;
            }
            const auto start = segment.clip.timelineStartFrame;
            out.push_back({std::move(segment), start, start + duration.frame});
        }
        return true;
    };
    if (!entries(video, plan.video) || !entries(audio, plan.audio))
        return plan;
    std::vector<bool> usesOverLane(project.videoTracks.size(), false);
    for (const auto& entry : plan.video)
        if (entry.segment.lane > 0)
            usesOverLane[static_cast<std::size_t>(entry.segment.original.track.index)] = true;
    plan.slotBases.assign(project.videoTracks.size(), 0);
    int next = 0;
    for (std::size_t track = 0; track < plan.slotBases.size(); ++track) {
        plan.slotBases[track] = next;
        next += usesOverLane[track] ? 2 : 1;
    }
    plan.success = true;
    return plan;
}

TimelinePreviewFrameMapping mapTimelinePreviewFrame(const project::Project& project,
                                                    std::int64_t timelineFrame) {
    return mapTimelinePreviewFrame(project, buildTimelinePreviewPlan(project), timelineFrame);
}

TimelinePreviewFrameMapping mapTimelinePreviewFrame(const project::Project& project,
                                                    const TimelinePreviewPlan& plan,
                                                    std::int64_t timelineFrame) {
    TimelinePreviewFrameMapping result;
    result.outputFrameNumber = timelineFrame;
    if (timelineFrame < 0) {
        result.error = "Preview timeline frameは0以上である必要があります";
        return result;
    }
    if (!plan.success) {
        result.error = plan.error;
        return result;
    }
    for (const auto& entry : plan.video) {
        // 無効にした clip は区間に含まれない。非表示にした video track は「黒」ではなく layer から
        // 外す (下の track が見える)。
        const auto& segment = entry.segment;
        const int track = segment.original.track.index;
        if (timelineFrame < entry.start || timelineFrame >= entry.end ||
            !project::isTrackOutputEnabled(project, {project::TrackKind::Video, track}))
            continue;
        const int slot = plan.slotBases[static_cast<std::size_t>(track)] + segment.lane;
        const auto& clip = segment.clip;
        // Equation Sequence (P3-4) は数式と同じ静止画 layer として合成し、内部の区間は controller
        // の animation が output frame から決める。timeline のトランジションで素材範囲を延ばした
        // 区間は内部の時間の正 (P3-1) の外なので、preview でも未対応として拒否する。
        const bool sequence = clip.kind == project::TimelineClipKind::EquationSequence;
        if (sequence && (segment.fadeIn || segment.fadeOut ||
                         clip.timelineStartFrame != segment.original.timelineStartFrame ||
                         clip.sourceInFrame != segment.original.sourceInFrame ||
                         clip.sourceOutFrame != segment.original.sourceOutFrame)) {
            result.error = "EquationSequence と timeline のトランジションの重なりは preview "
                           "未対応です: " +
                           clip.name;
            result.layers.clear();
            result.stillLayers.clear();
            return result;
        }
        const double transitionOpacity =
            segment.fadeIn ? project::transitionProgress(*segment.fadeIn, timelineFrame) : 1.0;
        if (sequence || clip.kind == project::TimelineClipKind::Graph ||
            project::isStillClipKind(clip.kind)) {
            // 文字・画像の不透明度は書き出しと同じ区間の評価 (値・key・fade を素材 frame で
            // 数え、トランジションを掛ける) を使う。
            const auto opacity = project::renderSegmentOpacity(
                segment, project.timelineFpsNum, project.timelineFpsDen, timelineFrame);
            if (!opacity) {
                result.layers.clear();
                result.stillLayers.clear();
                result.error = clip.name + ": preview frameを素材frameへ換算できません";
                return result;
            }
            result.stillLayers.push_back(
                {track, slot, segment.clipIndex, clip.id, clip.kind, *opacity});
            continue;
        }
        const auto sourceFrame =
            project::clipSourceFrameAt(clip, project.timelineFpsNum, project.timelineFpsDen,
                                       timelineFrame - clip.timelineStartFrame);
        if (!sourceFrame.success) {
            result.layers.clear();
            result.error = clip.name + ": preview frameを素材frameへ換算できません";
            return result;
        }
        result.layers.push_back({track, slot, segment.clipIndex, clip.id, sourceFrame.frame, clip,
                                 transitionOpacity, segment.fadeIn.has_value()});
    }
    const auto bySlot = [](const auto& a, const auto& b) { return a.slot < b.slot; };
    std::stable_sort(result.layers.begin(), result.layers.end(), bySlot);
    std::stable_sort(result.stillLayers.begin(), result.stillLayers.end(), bySlot);
    if (result.layers.size() > kMaxPreviewVideoLayers) {
        result.layers.clear();
        result.stillLayers.clear();
        result.error = "同時に重なる video layer が " + std::to_string(kMaxPreviewVideoLayers) +
                       " 枚を超えています。preview はここまでしか合成できません";
        return result;
    }
    // 映像の無い frame の文字も engine が合成するので、映像の有無に関わらず上限を見る。
    if (result.layers.size() + result.stillLayers.size() > kMaxPreviewCompositionLayers) {
        result.layers.clear();
        result.stillLayers.clear();
        result.error = "同時に重なる映像・文字・画像が " +
                       std::to_string(kMaxPreviewCompositionLayers) +
                       " 枚を超えています。preview はここまでしか合成できません";
        return result;
    }
    result.success = true;
    return result;
}

void applyPreviewLayerEffects(preview::PreviewCompositionLayer& layer,
                              const project::ClipEffects& effects, double opacity,
                              std::int64_t sourceInFrame, std::int64_t sourceDurationFrames) {
    const auto mapped = project::mapClipEffects(effects);
    layer.destination = {static_cast<float>(mapped.destinationRect.x),
                         static_cast<float>(mapped.destinationRect.y),
                         static_cast<float>(mapped.destinationRect.width),
                         static_cast<float>(mapped.destinationRect.height)};
    layer.sourceRect = {
        static_cast<float>(mapped.sourceRect.x), static_cast<float>(mapped.sourceRect.y),
        static_cast<float>(mapped.sourceRect.width), static_cast<float>(mapped.sourceRect.height)};
    layer.opacity = static_cast<float>(opacity);
    layer.effectsEnabled = true;
    layer.rotationDegrees = static_cast<float>(mapped.rotationDegrees);
    layer.sourceInFrame = sourceInFrame;
    layer.sourceDurationFrames = sourceDurationFrames;
    layer.fadeInFrames = 0;
    layer.fadeOutFrames = 0;
}

std::vector<TimelinePreviewStackEntry>
previewLayerStack(const TimelinePreviewFrameMapping& mapping) {
    std::vector<TimelinePreviewStackEntry> stack;
    stack.reserve(mapping.layers.size() + mapping.stillLayers.size());
    for (std::size_t index = 0; index < mapping.layers.size(); ++index)
        stack.push_back({false, index, mapping.layers[index].slot});
    for (std::size_t index = 0; index < mapping.stillLayers.size(); ++index)
        stack.push_back({true, index, mapping.stillLayers[index].slot});
    // 1 slot に同時に載る区間は 1 つなので slot だけで順序が決まる。
    std::stable_sort(stack.begin(), stack.end(),
                     [](const auto& a, const auto& b) { return a.slot < b.slot; });
    return stack;
}

PreviewVideoMapping previewVideoMappingOf(const project::TimelineClip& clip) {
    // 検証済みの clip では timebase は必ずある。無ければ 0 のまま使い回し判定に失敗させる。
    const auto timebase = project::clipTimebase(clip).value_or(core::FrameRate{0, 1});
    const auto source = project::clipVideoSource(clip);
    return {clip.mediaPath,
            source.sourceInFrame,
            clip.timelineStartFrame,
            clip.frameHold ? source.sourceFpsNum : timebase.num,
            clip.frameHold ? source.sourceFpsDen : timebase.den,
            clip.frameHold ? clip.sourceOutFrame - clip.sourceInFrame : 0};
}

preview::PreviewSourceDescriptor previewVideoDescriptorOf(const project::Project& project,
                                                          const project::TimelineClip& clip) {
    const auto source = project::clipVideoSource(clip);
    preview::PreviewSourceDescriptor descriptor;
    descriptor.mediaPath = clip.mediaPath;
    descriptor.videoEnabled = true;
    descriptor.videoTimelineMappingEnabled = true;
    descriptor.videoSourceInFrame = source.sourceInFrame;
    descriptor.videoSourceFrameCount = source.sourceFrameCount;
    descriptor.videoTimelineStartFrame = clip.timelineStartFrame;
    descriptor.speedNum = clip.speedNum;
    descriptor.speedDen = clip.speedDen;
    if (clip.frameHold) {
        const auto duration = project::timelineClipDuration(project, clip);
        descriptor.videoHoldOutputFrames = duration.success ? duration.frame : 0;
    }
    descriptor.expectedVideoSourceFrameRate = {static_cast<std::uint32_t>(source.sourceFpsNum),
                                               static_cast<std::uint32_t>(source.sourceFpsDen)};
    return descriptor;
}

bool previewVideoMappingCovers(const project::Project& project,
                               const PreviewVideoMapping& installed,
                               const project::TimelineClip& clip) {
    const PreviewVideoMapping wanted = previewVideoMappingOf(clip);
    if (installed == wanted)
        return true;
    if (installed.holdFrames > 0 || wanted.holdFrames > 0)
        return false;
    if (installed.mediaPath != wanted.mediaPath || installed.timebaseNum != wanted.timebaseNum ||
        installed.timebaseDen != wanted.timebaseDen || wanted.timebaseNum <= 0)
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
            mapping.sourceInFrame, mapping.timebaseNum, mapping.timebaseDen, project.timelineFpsNum,
            project.timelineFpsDen);
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
        if (a.layers[i].slot != b.layers[i].slot || a.layers[i].clipId != b.layers[i].clipId)
            return false;
    }
    return true;
}

TimelinePreviewAudioMapping mapTimelinePreviewAudio(const project::Project& project,
                                                    std::int64_t timelineFrame) {
    return mapTimelinePreviewAudio(project, buildTimelinePreviewPlan(project), timelineFrame);
}

TimelinePreviewAudioMapping mapTimelinePreviewAudio(const project::Project& project,
                                                    const TimelinePreviewPlan& plan,
                                                    std::int64_t timelineFrame) {
    TimelinePreviewAudioMapping result;
    if (timelineFrame < 0) {
        result.error = "Preview timeline frameは0以上である必要があります";
        return result;
    }
    if (!plan.success) {
        result.error = plan.error;
        return result;
    }
    for (const auto& entry : plan.audio) {
        const auto& segment = entry.segment;
        const int track = segment.original.track.index;
        if (timelineFrame < entry.start || timelineFrame >= entry.end ||
            !project::isTrackOutputEnabled(project, {project::TrackKind::Audio, track}))
            continue;
        const auto& clip = segment.clip;
        result.layers.push_back({track, segment.clipIndex, clip.id,
                                 clip.sourceInFrame + (timelineFrame - clip.timelineStartFrame),
                                 segment});
    }
    std::stable_sort(result.layers.begin(), result.layers.end(), [](const auto& a, const auto& b) {
        return a.audioTrackIndex != b.audioTrackIndex
                   ? a.audioTrackIndex < b.audioTrackIndex
                   : a.segment.clip.timelineStartFrame < b.segment.clip.timelineStartFrame;
    });
    result.success = true;
    return result;
}

AudioPreviewOffset audioPreviewSampleOffset(const project::Project& project,
                                            const project::TimelineClip& clip) {
    AudioPreviewOffset result;
    // 素材側は速度で伸縮した時間軸の sample で数える。decoder は速度 s の clip を
    // 48 kHz x 1/s の密度で出すので、この時間軸では素材 in の位置が in / (f s) 秒になり、
    // 「media sample = timeline sample + offset」の定数のずらしのまま保てる。
    const auto clipRate = project::clipTimebase(clip);
    if (!clipRate) {
        result.error = "audio clipの速度とfpsの積を表せません";
        return result;
    }
    const auto sourceTimebase = core::CheckedOutputTimebase::create(
        clipRate->num, clipRate->den, core::kQualifiedAudioSampleRate);
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

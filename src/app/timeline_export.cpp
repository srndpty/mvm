#include "app/timeline_export.h"

#include "core/clip_fade.h"
#include "media/mlt/mvm_mlt_export.h"
#include "project/timeline_edit.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace mvm::app {
namespace {

std::string pathToUtf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

bool mapExportEffects(const project::TimelineClip& clip, const TimelineExportRequest& request,
                      std::int64_t timelineDuration, bool requireOverlay,
                      TimelineExportClipMapping& output, std::string& error) {
    if (project::clipEffectsAreDefault(clip.effects) && !requireOverlay)
        return true;
    const auto mapped = project::mapClipEffects(clip.effects);
    output.effectsEnabled = !project::clipEffectsAreDefault(clip.effects);
    output.cropLeft = static_cast<int>(std::lround(mapped.sourceRect.x * request.width));
    output.cropTop = static_cast<int>(std::lround(mapped.sourceRect.y * request.height));
    output.cropRight = static_cast<int>(
        std::lround((1.0 - mapped.sourceRect.x - mapped.sourceRect.width) * request.width));
    output.cropBottom = static_cast<int>(
        std::lround((1.0 - mapped.sourceRect.y - mapped.sourceRect.height) * request.height));
    output.rectX = mapped.destinationRect.x * request.width;
    output.rectY = mapped.destinationRect.y * request.height;
    output.rectWidth = mapped.destinationRect.width * request.width;
    output.rectHeight = mapped.destinationRect.height * request.height;
    output.rotationDegrees = mapped.rotationDegrees;

    const std::int64_t duration = clip.sourceOutFrame - clip.sourceInFrame;
    std::set<std::int64_t> sourcePositions{0, duration - 1};
    const auto add = [&](std::int64_t frame) {
        if (frame >= 0 && frame < duration)
            sourcePositions.insert(frame);
    };
    add(clip.effects.fadeInFrames - 1);
    add(clip.effects.fadeInFrames);
    add(duration - clip.effects.fadeOutFrames - 1);
    add(duration - clip.effects.fadeOutFrames);

    long long producerIn = 0;
    if (mvm_source_boundary_to_producer_boundary(clip.sourceInFrame, clip.sourceFpsNum,
                                                 clip.sourceFpsDen, request.fpsNum, request.fpsDen,
                                                 &producerIn) != 0) {
        error = "effect keyframeのproducer位置を変換できません";
        return false;
    }
    for (std::int64_t sourceLocal : sourcePositions) {
        if (output.opacityKeys.size() >= MVM_EXPORT_MAX_OPACITY_KEYFRAMES) {
            error = "effect opacity keyframe数が固定上限を超えました";
            return false;
        }
        long long absoluteProducer = 0;
        if (mvm_source_boundary_to_producer_boundary(
                clip.sourceInFrame + sourceLocal, clip.sourceFpsNum, clip.sourceFpsDen,
                request.fpsNum, request.fpsDen, &absoluteProducer) != 0) {
            error = "effect keyframeのproducer位置を変換できません";
            return false;
        }
        const auto localFrame =
            std::clamp<long long>(absoluteProducer - producerIn, 0, timelineDuration - 1);
        const double opacity =
            mapped.baseOpacity *
            core::clipFadeFactor(sourceLocal, duration, mapped.fadeInFrames, mapped.fadeOutFrames);
        if (!output.opacityKeys.empty() && output.opacityKeys.back().localFrame == localFrame)
            output.opacityKeys.back().opacity = opacity;
        else
            output.opacityKeys.push_back({localFrame, opacity});
    }
    return true;
}

} // namespace

TimelineExportPlan mapTimelineExportPlan(const project::Project& project,
                                         const TimelineExportRequest& request) {
    TimelineExportPlan plan;
    if (request.width <= 0 || request.height <= 0 || request.fpsNum <= 0 || request.fpsDen <= 0) {
        plan.error = "書き出しprofileが不正です";
        return plan;
    }
    const auto valid = project::validateTimeline(project);
    if (!valid.success) {
        plan.error = valid.error;
        return plan;
    }
    plan.totalDurationFrames = valid.totalFrames;
    if (static_cast<int>(project.videoTracks.size()) > kMaxExportVideoTracks) {
        for (const auto& clip : project.timelineClips) {
            if (clip.track.kind == project::TrackKind::Video &&
                clip.track.index >= kMaxExportVideoTracks) {
                plan.error = "書き出しは video track を " + std::to_string(kMaxExportVideoTracks) +
                             " 本までしか扱えません: " + clip.name;
                return plan;
            }
        }
    }
    for (const auto& clip : project.timelineClips) {
        if (clip.track.kind == project::TrackKind::Audio) {
            plan.error = "audio clip を含む timeline の書き出しは未対応です: " + clip.name;
            return plan;
        }
    }
    bool anyOverlay = false;
    std::int64_t v1Cursor = 0;
    std::vector<int> indices(project.timelineClips.size());
    for (std::size_t index = 0; index < indices.size(); ++index)
        indices[index] = static_cast<int>(index);
    std::stable_sort(indices.begin(), indices.end(), [&](int left, int right) {
        const auto& a = project.timelineClips[static_cast<std::size_t>(left)];
        const auto& b = project.timelineClips[static_cast<std::size_t>(right)];
        if (a.track.index != b.track.index)
            return a.track.index < b.track.index;
        return a.timelineStartFrame < b.timelineStartFrame;
    });
    for (const int index : indices) {
        const auto& clip = project.timelineClips[static_cast<std::size_t>(index)];
        const auto duration = project::timelineClipDuration(project, clip);
        if (!duration.success) {
            plan.error = duration.error;
            return plan;
        }
        TimelineExportClipMapping mapped;
        mapped.projectClipIndex = index;
        mapped.videoTrackIndex = clip.track.index;
        mapped.timelineStartFrame = clip.timelineStartFrame;
        mapped.timelineDurationFrames = duration.frame;
        const bool overlay = clip.track.index > 0;
        anyOverlay = anyOverlay || overlay;
        if (!overlay) {
            if (clip.timelineStartFrame != v1Cursor)
                plan.backend = TimelineExportResult::Backend::Tractor;
            v1Cursor = clip.timelineStartFrame + duration.frame;
        }
        if (!mapExportEffects(clip, request, duration.frame, overlay, mapped, plan.error))
            return plan;
        plan.clips.push_back(std::move(mapped));
    }
    if (anyOverlay)
        plan.backend = TimelineExportResult::Backend::Tractor;
    plan.success = true;
    return plan;
}

TimelineExportResult exportTimeline(const project::Project& project,
                                    const TimelineExportRequest& request) {
    TimelineExportResult result;

    if (request.outputPath.empty()) {
        result.error = "書き出し先が指定されていません";
        return result;
    }
    if (project.timelineClips.empty()) {
        result.error = "timeline に clip がありません";
        return result;
    }
    const auto plan = mapTimelineExportPlan(project, request);
    if (!plan.success) {
        result.error = plan.error;
        return result;
    }

    std::error_code pathError;
    const auto outputPath = std::filesystem::absolute(request.outputPath, pathError);
    if (pathError || outputPath.filename().empty()) {
        result.error = "書き出し先のパスが不正です";
        return result;
    }
    const auto outputDirectory = outputPath.parent_path();
    std::filesystem::create_directories(outputDirectory, pathError);
    if (pathError) {
        result.error = "書き出し先の directory を作成できません: " + pathError.message();
        return result;
    }

    // clip のパス文字列は C API へ const char* で渡すため、
    // 呼び出しが終わるまで生存させる。
    std::vector<std::string> clipPaths;
    clipPaths.reserve(project.timelineClips.size());
    for (const auto& clip : project.timelineClips) {
        if (clip.mediaPath.empty()) {
            result.error = "clip '" + clip.name + "' の media path が空です";
            return result;
        }
        clipPaths.push_back(pathToUtf8(clip.mediaPath));
    }

    std::vector<MvmExportClip> clips;
    clips.reserve(clipPaths.size());
    for (const auto& planned : plan.clips) {
        const auto index = static_cast<std::size_t>(planned.projectClipIndex);
        const auto& clip = project.timelineClips[index];
        MvmExportClip mapped{};
        mapped.path = clipPaths[index].c_str();
        mapped.source_fps_num = clip.sourceFpsNum;
        mapped.source_fps_den = clip.sourceFpsDen;
        mapped.source_in_frame = clip.sourceInFrame;
        mapped.source_out_frame = clip.sourceOutFrame;
        mapped.video_track = planned.videoTrackIndex;
        mapped.timeline_start_frame = planned.timelineStartFrame;
        mapped.timeline_duration_frames = planned.timelineDurationFrames;
        mapped.effects_enabled = planned.effectsEnabled ? 1 : 0;
        mapped.crop_left = planned.cropLeft;
        mapped.crop_top = planned.cropTop;
        mapped.crop_right = planned.cropRight;
        mapped.crop_bottom = planned.cropBottom;
        mapped.rect_x = planned.rectX;
        mapped.rect_y = planned.rectY;
        mapped.rect_width = planned.rectWidth;
        mapped.rect_height = planned.rectHeight;
        mapped.rotation_degrees = planned.rotationDegrees;
        for (const auto& plannedKey : planned.opacityKeys) {
            auto& key = mapped.opacity_keyframes[mapped.opacity_keyframe_count++];
            key.local_frame = plannedKey.localFrame;
            key.opacity = plannedKey.opacity;
        }
        clips.push_back(mapped);
    }

    const MvmExportSpec spec{
        .width = request.width,
        .height = request.height,
        .fps_num = request.fpsNum,
        .fps_den = request.fpsDen,
        .timeout_ms = request.timeoutMs,
    };

    // 一時ファイルへ書き、検証を通ってから正規名へ rename する。
    // 途中で失敗した出力を最終ファイル名で残さない。
    auto temporaryPath = outputPath;
    temporaryPath += ".mvmtmp";
    std::filesystem::remove(temporaryPath, pathError);

    const std::string temporaryUtf8 = pathToUtf8(temporaryPath);
    MvmExportResult exported{};
    char error[1024] = {0};
    const int exportStatus =
        plan.backend == TimelineExportResult::Backend::Sequential
            ? mvm_mlt_export_sequence(clips.data(), static_cast<int>(clips.size()), &spec,
                                      temporaryUtf8.c_str(), &exported, error, sizeof(error))
            : mvm_mlt_export_two_track(clips.data(), static_cast<int>(clips.size()),
                                       plan.totalDurationFrames, &spec, temporaryUtf8.c_str(),
                                       &exported, error, sizeof(error));
    if (exportStatus != 0) {
        std::filesystem::remove(temporaryPath, pathError);
        result.error = error[0] ? error : "書き出しに失敗しました";
        return result;
    }

    std::filesystem::rename(temporaryPath, outputPath, pathError);
    if (pathError) {
        std::filesystem::remove(temporaryPath, pathError);
        result.error = "書き出したファイルを正規名へ rename できません: " + pathError.message();
        return result;
    }

    result.outputPath = outputPath;
    result.frameCount = exported.frame_count;
    result.durationSec = exported.duration_sec;
    result.backend = plan.backend;
    result.playlistBlankCount = exported.playlist_blank_count;
    result.transitionCount = exported.transition_count;
    result.opaqueBlackAffineFilterCount = exported.opaque_black_affine_filter_count;
    result.success = true;
    return result;
}

} // namespace mvm::app

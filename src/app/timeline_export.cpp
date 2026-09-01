#include "app/timeline_export.h"

#include "core/clip_fade.h"
#include "media/mlt/mvm_mlt_export.h"
#include "project/timeline_edit.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <system_error>
#include <vector>

namespace mvm::app {
namespace {

std::string pathToUtf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

bool mapExportEffects(const project::TimelineClip& clip, const TimelineExportRequest& request,
                      MvmExportClip& output, std::string& error) {
    if (project::clipEffectsAreDefault(clip.effects))
        return true;
    const auto mapped = project::mapClipEffects(clip.effects);
    output.effects_enabled = 1;
    output.crop_left = static_cast<int>(std::lround(mapped.sourceRect.x * request.width));
    output.crop_top = static_cast<int>(std::lround(mapped.sourceRect.y * request.height));
    output.crop_right = static_cast<int>(
        std::lround((1.0 - mapped.sourceRect.x - mapped.sourceRect.width) * request.width));
    output.crop_bottom = static_cast<int>(
        std::lround((1.0 - mapped.sourceRect.y - mapped.sourceRect.height) * request.height));
    output.rect_x = mapped.destinationRect.x * request.width;
    output.rect_y = mapped.destinationRect.y * request.height;
    output.rect_width = mapped.destinationRect.width * request.width;
    output.rect_height = mapped.destinationRect.height * request.height;
    output.rotation_degrees = mapped.rotationDegrees;

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
        if (output.opacity_keyframe_count >= MVM_EXPORT_MAX_OPACITY_KEYFRAMES) {
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
        auto& key = output.opacity_keyframes[output.opacity_keyframe_count++];
        key.local_frame = std::max<long long>(0, absoluteProducer - producerIn);
        key.opacity =
            mapped.baseOpacity *
            core::clipFadeFactor(sourceLocal, duration, mapped.fadeInFrames, mapped.fadeOutFrames);
    }
    return true;
}

} // namespace

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
    const auto timeline = project::validateTimeline(project);
    if (!timeline.success) {
        result.error = timeline.error;
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
    for (std::size_t index = 0; index < clipPaths.size(); ++index) {
        const auto& clip = project.timelineClips[index];
        MvmExportClip mapped{};
        mapped.path = clipPaths[index].c_str();
        mapped.source_fps_num = clip.sourceFpsNum;
        mapped.source_fps_den = clip.sourceFpsDen;
        mapped.source_in_frame = clip.sourceInFrame;
        mapped.source_out_frame = clip.sourceOutFrame;
        if (!mapExportEffects(clip, request, mapped, result.error))
            return result;
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
    if (mvm_mlt_export_sequence(clips.data(), static_cast<int>(clips.size()), &spec,
                                temporaryUtf8.c_str(), &exported, error, sizeof(error)) != 0) {
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
    result.success = true;
    return result;
}

} // namespace mvm::app

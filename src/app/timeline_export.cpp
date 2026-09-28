#include "app/timeline_export.h"

#include "app/text_raster.h"
#include "media/mlt/mvm_mlt_export.h"
#include "media/still_image/still_image_decoder.h"
#include "project/timeline_edit.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <QImage>
#include <QImageWriter>
#include <QTemporaryDir>

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
    // crop は frame の寸法を変えずに範囲外を透明にする (qtcrop)。そのため affine へ渡す矩形は
    // crop 範囲ではなく、crop 前の frame 全体を置く位置にする。crop 範囲が destinationRect に
    // 重なるよう、全体を同じ倍率で拡縮して置く。
    const double width = request.width;
    const double height = request.height;
    const double scaleX = mapped.destinationRect.width / mapped.sourceRect.width;
    const double scaleY = mapped.destinationRect.height / mapped.sourceRect.height;
    double fullX = (mapped.destinationRect.x - mapped.sourceRect.x * scaleX) * width;
    double fullY = (mapped.destinationRect.y - mapped.sourceRect.y * scaleY) * height;
    const double fullWidth = scaleX * width;
    const double fullHeight = scaleY * height;
    // MLT は矩形の中心を軸に回す (docs/m7a-p0-findings.md)。preview は crop 範囲の中心で回す。
    // 中心の差 d について、全体の矩形を d - R d だけずらすと、全体の中心で回した結果が
    // crop 範囲の中心で回した結果と一致する。R は画素空間 (y 下向き) の時計回りの回転で、
    // preview の shader と同じ向き。
    if (mapped.rotationDegrees != 0.0) {
        const double radians = mapped.rotationDegrees * 3.14159265358979323846 / 180.0;
        const double cosine = std::cos(radians);
        const double sine = std::sin(radians);
        const double dx = (mapped.destinationRect.x + mapped.destinationRect.width * 0.5) * width -
                          (fullX + fullWidth * 0.5);
        const double dy =
            (mapped.destinationRect.y + mapped.destinationRect.height * 0.5) * height -
            (fullY + fullHeight * 0.5);
        fullX += dx - (cosine * dx - sine * dy);
        fullY += dy - (sine * dx + cosine * dy);
    }
    output.rectX = fullX;
    output.rectY = fullY;
    output.rectWidth = fullWidth;
    output.rectHeight = fullHeight;
    output.rotationDegrees = mapped.rotationDegrees;

    for (std::int64_t frame = 0; frame < timelineDuration; ++frame) {
        const auto sourceLocal =
            project::clipFadeSourceFrameAt(clip, request.fpsNum, request.fpsDen, frame);
        if (!sourceLocal.success) {
            error = sourceLocal.error;
            return false;
        }
        output.opacityKeys.push_back(
            {frame, project::evaluateClipOpacity(clip.effects, frame, sourceLocal.frame,
                                                 clip.sourceOutFrame - clip.sourceInFrame)});
    }
    return true;
}

} // namespace

TimelineExportPlan mapTimelineExportPlan(const project::Project& project,
                                         const TimelineExportRequest& request) {
    TimelineExportPlan plan;
    if (request.width <= 0 || request.height <= 0 || request.fpsNum <= 0 || request.fpsDen <= 0 ||
        request.videoCrf < 0 || request.videoCrf > 51 || request.fpsNum != project.timelineFpsNum ||
        request.fpsDen != project.timelineFpsDen) {
        plan.error = "書き出しprofileが不正です";
        return plan;
    }
    const auto valid = project::validateTimeline(project);
    if (!valid.success) {
        plan.error = valid.error;
        return plan;
    }
    plan.totalDurationFrames = valid.totalFrames;
    bool anyOverlay = false;
    bool anyAudio = false;
    std::int64_t v1Cursor = 0;
    std::vector<int> indices(project.timelineClips.size());
    for (std::size_t index = 0; index < indices.size(); ++index)
        indices[index] = static_cast<int>(index);
    std::stable_sort(indices.begin(), indices.end(), [&](int left, int right) {
        const auto& a = project.timelineClips[static_cast<std::size_t>(left)];
        const auto& b = project.timelineClips[static_cast<std::size_t>(right)];
        if (a.track.kind != b.track.kind)
            return a.track.kind == project::TrackKind::Video;
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
        if (duration.frame > std::numeric_limits<int>::max()) {
            plan.error = "書き出しclipのキーフレーム数が上限を超えました";
            return plan;
        }
        TimelineExportClipMapping mapped;
        mapped.projectClipIndex = index;
        mapped.audio = clip.track.kind == project::TrackKind::Audio;
        mapped.still = project::isStillClipKind(clip.kind);
        mapped.videoTrackIndex = clip.track.index;
        mapped.timelineStartFrame = clip.timelineStartFrame;
        mapped.timelineDurationFrames = duration.frame;
        const auto range = project::clipProducerRange(clip, request.fpsNum, request.fpsDen);
        if (!range.success) {
            plan.error = clip.name + ": " + range.error;
            return plan;
        }
        mapped.producerInFrame = range.begin;
        mapped.producerOutFrame = range.end;
        mapped.tailPaddingFrames = range.tailFrames;
        // 末尾の補完は tractor 経路だけが持つ。
        if (range.tailFrames > 0)
            plan.backend = TimelineExportResult::Backend::Tractor;
        if (mapped.still)
            plan.backend = TimelineExportResult::Backend::Tractor;
        if (mapped.audio) {
            anyAudio = true;
            for (std::int64_t frame = 0; frame < duration.frame; ++frame) {
                const auto sourceLocal =
                    project::clipFadeSourceFrameAt(clip, request.fpsNum, request.fpsDen, frame);
                if (!sourceLocal.success) {
                    plan.error = sourceLocal.error;
                    return plan;
                }
                mapped.gainKeys.push_back(
                    {frame, project::evaluateClipVolume(clip.effects, frame, sourceLocal.frame,
                                                        clip.sourceOutFrame - clip.sourceInFrame)});
            }
            plan.clips.push_back(std::move(mapped));
            continue;
        }
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
    if (anyOverlay || anyAudio)
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
    // 文字・画像は出力解像度の透過 PNG を stage し、qimage producer で開く。
    std::unique_ptr<QTemporaryDir> stillStaging;
    // 同じ画像素材を使う clip は同じ PNG を共有する (decode は素材ごとに 1 回)。
    std::map<std::filesystem::path, std::string> stagedImages;
    std::vector<std::string> clipPaths;
    clipPaths.reserve(project.timelineClips.size());
    const auto stagePng = [&](const QImage& image, const QString& name, std::string& staged) {
        if (!stillStaging)
            stillStaging = std::make_unique<QTemporaryDir>();
        if (!stillStaging->isValid()) {
            result.error = "文字・画像の一時 directory を作成できません";
            return false;
        }
        const QString pngPath = stillStaging->filePath(name);
        QImageWriter writer(pngPath, "PNG");
        if (!writer.write(image)) {
            result.error = "文字・画像を PNG として保存できません: " + pngPath.toStdString() +
                           ": " + writer.errorString().toStdString();
            return false;
        }
        staged = pathToUtf8(std::filesystem::path(pngPath.toStdWString()));
        return true;
    };
    for (std::size_t index = 0; index < project.timelineClips.size(); ++index) {
        const auto& clip = project.timelineClips[index];
        if (clip.kind == project::TimelineClipKind::Text) {
            QString rasterError;
            const QImage image =
                renderTextRaster(clip.text, request.width, request.height, rasterError);
            if (image.isNull()) {
                result.error = rasterError.toStdString();
                return result;
            }
            std::string staged;
            if (!stagePng(image, QString::number(index) + ".png", staged))
                return result;
            clipPaths.push_back(std::move(staged));
            continue;
        }
        if (clip.kind == project::TimelineClipKind::Image) {
            if (const auto found = stagedImages.find(clip.mediaPath); found != stagedImages.end()) {
                clipPaths.push_back(found->second);
                continue;
            }
            // preview と同じ decoder と配置 (media/still_image) を通し、同じ画素を書き出す。
            const auto decoded = media::decodeStillImage(clip.mediaPath);
            if (!decoded.success) {
                result.error = clip.name + " を読めません: " + decoded.error;
                return result;
            }
            const auto fitted =
                media::fitStillImageToRaster(decoded.image, request.width, request.height);
            if (!fitted.success) {
                result.error = clip.name + " を配置できません: " + fitted.error;
                return result;
            }
            // straight alpha の RGBA8。QImage は画素を参照するだけなので、書き終えるまで保持する。
            const QImage image(fitted.raster.rgba.data(), fitted.raster.width, fitted.raster.height,
                               fitted.raster.width * 4, QImage::Format_RGBA8888);
            std::string staged;
            if (!stagePng(image, QString::number(index) + "-image.png", staged))
                return result;
            stagedImages.emplace(clip.mediaPath, staged);
            clipPaths.push_back(std::move(staged));
            continue;
        }
        if (clip.mediaPath.empty()) {
            result.error = "clip '" + clip.name + "' の media path が空です";
            return result;
        }
        clipPaths.push_back(pathToUtf8(clip.mediaPath));
    }

    std::vector<MvmExportClip> clips;
    clips.reserve(clipPaths.size());
    std::vector<std::vector<MvmExportOpacityKeyframe>> opacityStorage;
    std::vector<std::vector<MvmExportGainKeyframe>> gainStorage;
    opacityStorage.reserve(plan.clips.size());
    gainStorage.reserve(plan.clips.size());
    for (const auto& planned : plan.clips) {
        const auto index = static_cast<std::size_t>(planned.projectClipIndex);
        const auto& clip = project.timelineClips[index];
        MvmExportClip mapped{};
        mapped.path = clipPaths[index].c_str();
        mapped.source_fps_num = clip.sourceFpsNum;
        mapped.source_fps_den = clip.sourceFpsDen;
        mapped.source_frame_count = clip.sourceFrameCount;
        mapped.source_in_frame = clip.sourceInFrame;
        mapped.source_out_frame = clip.sourceOutFrame;
        mapped.producer_in_frame = planned.producerInFrame;
        mapped.producer_out_frame = planned.producerOutFrame;
        mapped.tail_padding_frames = planned.tailPaddingFrames;
        mapped.speed_num = clip.speedNum;
        mapped.speed_den = clip.speedDen;
        mapped.is_audio = planned.audio ? 1 : 0;
        mapped.is_still_image = planned.still ? 1 : 0;
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
        auto& opacityKeys = opacityStorage.emplace_back();
        for (const auto& key : planned.opacityKeys)
            opacityKeys.push_back({key.localFrame, key.opacity});
        mapped.opacity_keyframes = opacityKeys.data();
        mapped.opacity_keyframe_count = static_cast<int>(opacityKeys.size());
        auto& gainKeys = gainStorage.emplace_back();
        for (const auto& key : planned.gainKeys)
            gainKeys.push_back({key.localFrame, key.gain});
        mapped.gain_keyframes = gainKeys.data();
        mapped.gain_keyframe_count = static_cast<int>(gainKeys.size());
        clips.push_back(mapped);
    }

    const MvmExportSpec spec{
        .width = request.width,
        .height = request.height,
        .fps_num = request.fpsNum,
        .fps_den = request.fpsDen,
        .video_crf = request.videoCrf,
        .timeout_ms = request.timeoutMs,
        .render_threads = request.renderThreads,
        .encoder_threads = request.encoderThreads,
        .progress_callback =
            [](long long completed, long long total, void* opaque) {
                const auto* exportRequest = static_cast<const TimelineExportRequest*>(opaque);
                return exportRequest->progress && exportRequest->progress(completed, total) ? 1 : 0;
            },
        .progress_opaque = const_cast<TimelineExportRequest*>(&request),
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
    if (exportStatus != MVM_EXPORT_OK) {
        std::filesystem::remove(temporaryPath, pathError);
        result.error = error[0] ? error : "書き出しに失敗しました";
        result.cancelled = exportStatus == MVM_EXPORT_CANCELLED;
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

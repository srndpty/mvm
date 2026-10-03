#include "app/timeline_export.h"

#include "app/text_raster.h"
#include "media/mlt/mvm_mlt_export.h"
#include "media/still_image/static_image.h"
#include "project/timeline_edit.h"
#include "project/timeline_render.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
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

// clip は幾何 (位置・拡大・回転・crop) を決める元の clip。不透明度は opacityAt が区間の
// local frame ごとに返す (トランジションの進み具合を含む)。
bool mapExportEffects(const project::TimelineClip& clip, const TimelineExportRequest& request,
                      std::int64_t timelineDuration, std::int64_t localOffset, bool requireOverlay,
                      const std::function<std::optional<double>(std::int64_t)>& opacityAt,
                      TimelineExportClipMapping& output, std::string& error, bool& cancelled) {
    if (project::clipEffectsAreDefault(clip.effects) && !requireOverlay)
        return true;
    const auto mapped =
        project::mapClipEffects(project::evaluateClipEffects(clip.effects, localOffset));
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
    double rectWidth = fullWidth;
    double rectHeight = fullHeight;
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
        // MLT の affine は回転してから矩形へ拡縮するので、縦横の倍率が違うと平行四辺形に
        // なる (preview は拡縮してから回す)。欲しい変換 A = R(θ)·diag(sx, sy) (画素空間、
        // 倍率は frame 全体に対する値) を、affine が表せる形へ分解する。
        // transition_affine.c は出力→素材の写像を rotate_x・shear・scale の順に
        // affine_multiply (右から転置を掛ける) で作るので、素材→出力の写像は
        // diag(sx', sy')·[[1, 0], [-t, 1]]·R(φ) (t = tan(fix_shear_x)) になる。
        // A·R(φ)^T が下三角になる φ を選べば (1 行 2 列を消す)、残りが diag と t になる。
        // 倍率が同じなら φ = θ・t = 0 になり、従来と変わらない。
        const double a = cosine * scaleX;
        const double b = -sine * scaleY;
        const double c = sine * scaleX;
        const double d = cosine * scaleY;
        const double phi = std::atan2(-b, a);
        const double cosPhi = std::cos(phi);
        const double sinPhi = std::sin(phi);
        const double scaleXAfter = a * cosPhi - b * sinPhi;
        const double scaleYAfter = c * sinPhi + d * cosPhi;
        const double lower = c * cosPhi - d * sinPhi;
        rectWidth = scaleXAfter * width;
        rectHeight = scaleYAfter * height;
        output.rotationDegrees = phi * 180.0 / 3.14159265358979323846;
        output.shearDegrees = std::atan(-lower / scaleYAfter) * 180.0 / 3.14159265358979323846;
        // 中心は変えずに、分解後の大きさで矩形を置き直す。
        fullX += (fullWidth - rectWidth) * 0.5;
        fullY += (fullHeight - rectHeight) * 0.5;
    }
    output.rectX = fullX;
    output.rectY = fullY;
    output.rectWidth = rectWidth;
    output.rectHeight = rectHeight;

    bool animatedMotion = false;
    for (const auto& channel : project::effectChannels())
        animatedMotion = animatedMotion || (channel.kind != project::ClipKeyKind::Opacity &&
                                            channel.kind != project::ClipKeyKind::Volume &&
                                            !(clip.effects.*channel.keys).empty());
    for (std::int64_t frame = 0; frame < timelineDuration; ++frame) {
        if (request.progress && frame % 256 == 0 && request.progress(0, timelineDuration)) {
            cancelled = true;
            error = "書き出し準備をキャンセルしました";
            return false;
        }
        if (animatedMotion) {
            auto evaluatedClip = clip;
            evaluatedClip.effects = project::evaluateClipEffects(clip.effects, localOffset + frame);
            TimelineExportClipMapping geometry;
            if (!mapExportEffects(evaluatedClip, request, 0, 0, true, opacityAt, geometry, error,
                                  cancelled))
                return false;
            output.motionFrames.push_back(
                {frame, evaluatedClip.effects.cropLeftPercent * request.width / 100,
                 evaluatedClip.effects.cropTopPercent * request.height / 100,
                 evaluatedClip.effects.cropRightPercent * request.width / 100,
                 evaluatedClip.effects.cropBottomPercent * request.height / 100, geometry.rectX,
                 geometry.rectY, geometry.rectWidth, geometry.rectHeight, geometry.rotationDegrees,
                 geometry.shearDegrees});
        }

        const auto opacity = opacityAt(frame);
        if (!opacity) {
            error = clip.name + ": 不透明度を評価できません";
            return false;
        }
        output.opacityKeys.push_back({frame, *opacity});
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
    std::vector<project::TimelineRenderSegment> videoSegments;
    std::vector<project::TimelineRenderSegment> audioSegments;
    if (!project::timelineRenderSegments(project, project::TrackKind::Video, videoSegments,
                                         plan.error) ||
        !project::timelineRenderSegments(project, project::TrackKind::Audio, audioSegments,
                                         plan.error))
        return plan;
    // 非表示・ミュート・他 track のソロで出力しない track の区間は外す。preview と同じ判定
    // (isTrackOutputEnabled) を使い、見聞きしたものと書き出しを食い違わせない。
    const auto dropSilenced = [&project](std::vector<project::TimelineRenderSegment>& segments) {
        const auto removed = std::erase_if(segments, [&project](const auto& segment) {
            return !project::isTrackOutputEnabled(project, segment.original.track);
        });
        return removed > 0;
    };
    const bool videoSilenced = dropSilenced(videoSegments);
    const bool audioSilenced = dropSilenced(audioSegments);
    // 映像 layer: track ごとに lane 0 を置き、トランジションのある track は lane 1 を重ねる。
    std::vector<int> layerBase(project.videoTracks.size(), 0);
    {
        std::vector<bool> usesOverLane(project.videoTracks.size(), false);
        for (const auto& segment : videoSegments)
            if (segment.lane > 0)
                usesOverLane[static_cast<std::size_t>(segment.original.track.index)] = true;
        int next = 0;
        for (std::size_t track = 0; track < layerBase.size(); ++track) {
            layerBase[track] = next;
            next += usesOverLane[track] ? 2 : 1;
        }
    }
    const auto layerOf = [&](const project::TimelineRenderSegment& segment) {
        return layerBase[static_cast<std::size_t>(segment.original.track.index)] + segment.lane;
    };
    // 無効にした clip と出力しない track の clip は区間に含まれない。尺は timeline 全体のまま
    // 保つので、穴を黒・無音で埋められる tractor にする (sequential は V1 の clip を詰めて並べる
    // だけ)。
    const bool anyDisabled =
        std::any_of(project.timelineClips.begin(), project.timelineClips.end(),
                    [](const project::TimelineClip& clip) { return !clip.enabled; });
    if (anyDisabled || videoSilenced || audioSilenced ||
        project::hasRenderedTransitions(project, project::TrackKind::Video) ||
        project::hasRenderedTransitions(project, project::TrackKind::Audio))
        plan.backend = TimelineExportResult::Backend::Tractor;

    std::vector<const project::TimelineRenderSegment*> ordered;
    for (const auto& segment : videoSegments)
        ordered.push_back(&segment);
    for (const auto& segment : audioSegments)
        ordered.push_back(&segment);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [&](const project::TimelineRenderSegment* left,
                         const project::TimelineRenderSegment* right) {
                         const auto leftKind = left->original.track.kind;
                         const auto rightKind = right->original.track.kind;
                         if (leftKind != rightKind)
                             return leftKind == project::TrackKind::Video;
                         const int leftTrack = leftKind == project::TrackKind::Video
                                                   ? layerOf(*left)
                                                   : left->original.track.index;
                         const int rightTrack = rightKind == project::TrackKind::Video
                                                    ? layerOf(*right)
                                                    : right->original.track.index;
                         if (leftTrack != rightTrack)
                             return leftTrack < rightTrack;
                         return left->clip.timelineStartFrame < right->clip.timelineStartFrame;
                     });

    bool anyOverlay = false;
    bool anyAudio = false;
    std::int64_t v1Cursor = 0;
    for (const auto* segment : ordered) {
        const auto& clip = segment->clip;
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
        mapped.projectClipIndex = segment->clipIndex;
        mapped.renderClip = clip;
        mapped.audio = clip.track.kind == project::TrackKind::Audio;
        mapped.still = project::isStillClipKind(clip.kind);
        mapped.videoTrackIndex = mapped.audio ? clip.track.index : layerOf(*segment);
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
        if (clip.frameHold)
            plan.backend = TimelineExportResult::Backend::Tractor;
        if (mapped.audio) {
            anyAudio = true;
            mapped.mixerPan = segment->mixerPan;
            for (std::int64_t frame = 0; frame < duration.frame; ++frame) {
                if (request.progress && frame % 256 == 0 &&
                    request.progress(0, plan.totalDurationFrames)) {
                    plan.cancelled = true;
                    plan.error = "書き出し準備をキャンセルしました";
                    return plan;
                }
                const auto gain = project::renderSegmentGain(
                    *segment, request.fpsNum, request.fpsDen, clip.timelineStartFrame + frame);
                if (!gain) {
                    plan.error = clip.name + ": 音量を評価できません";
                    return plan;
                }
                mapped.gainKeys.push_back({frame, *gain});
            }
            plan.clips.push_back(std::move(mapped));
            continue;
        }
        const bool overlay = mapped.videoTrackIndex > 0;
        anyOverlay = anyOverlay || overlay;
        if (!overlay) {
            if (clip.timelineStartFrame != v1Cursor)
                plan.backend = TimelineExportResult::Backend::Tractor;
            v1Cursor = clip.timelineStartFrame + duration.frame;
        }
        const auto opacityAt = [&](std::int64_t localFrame) {
            return project::renderSegmentOpacity(*segment, request.fpsNum, request.fpsDen,
                                                 clip.timelineStartFrame + localFrame);
        };
        if (!mapExportEffects(segment->original, request, duration.frame,
                              clip.timelineStartFrame - segment->original.timelineStartFrame,
                              overlay, opacityAt, mapped, plan.error, plan.cancelled))
            return plan;
        mapped.opaqueBackdrop = segment->fadeIn.has_value();
        plan.clips.push_back(std::move(mapped));
    }
    if (plan.clips.empty()) {
        plan.error = "書き出す有効なclipがありません";
        return plan;
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
        result.cancelled = plan.cancelled;
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
    // projectClipIndex → MLT へ渡す path。出力する clip (plan.clips) だけを stage する。
    // 非表示・ミュート・ソロ除外・無効の clip は出力しないので、その素材が壊れていても
    // 書き出しを失敗させない。
    std::map<std::size_t, std::string> clipPaths;
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
    for (const auto& planned : plan.clips) {
        const auto index = static_cast<std::size_t>(planned.projectClipIndex);
        if (index >= project.timelineClips.size()) {
            result.error = "書き出し計画の clip 番号が範囲外です";
            return result;
        }
        if (clipPaths.contains(index))
            continue;
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
            clipPaths.emplace(index, std::move(staged));
            continue;
        }
        if (clip.kind == project::TimelineClipKind::Image) {
            if (const auto found = stagedImages.find(clip.mediaPath); found != stagedImages.end()) {
                clipPaths.emplace(index, found->second);
                continue;
            }
            // preview と同じ authority・decoder・配置 (media/still_image) を通し、同じ画素を
            // 書き出す。取り込み後に動画やアニメーション画像へ差し替えられた素材は拒否する。
            const auto decoded = media::loadStaticImage(clip.mediaPath);
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
            clipPaths.emplace(index, std::move(staged));
            continue;
        }
        if (clip.mediaPath.empty()) {
            result.error = "clip '" + clip.name + "' の media path が空です";
            return result;
        }
        clipPaths.emplace(index, pathToUtf8(clip.mediaPath));
    }

    std::vector<MvmExportClip> clips;
    clips.reserve(plan.clips.size());
    std::vector<std::vector<MvmExportOpacityKeyframe>> opacityStorage;
    std::vector<std::vector<MvmExportGainKeyframe>> gainStorage;
    std::vector<std::vector<MvmExportMotionFrame>> motionStorage;
    motionStorage.reserve(plan.clips.size());
    opacityStorage.reserve(plan.clips.size());
    gainStorage.reserve(plan.clips.size());
    for (const auto& planned : plan.clips) {
        const auto index = static_cast<std::size_t>(planned.projectClipIndex);
        const auto& clip = planned.renderClip;
        MvmExportClip mapped{};
        mapped.path = clipPaths.at(index).c_str();
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
        mapped.preserve_pitch = clip.preservePitch ? 1 : 0;
        mapped.is_frame_hold = clip.frameHold ? 1 : 0;
        if (clip.frameHold) {
            const auto holdPosition = project::frameHoldProducerPosition(
                clip, project.timelineFpsNum, project.timelineFpsDen);
            if (!holdPosition.success) {
                result.error = holdPosition.error;
                return result;
            }
            mapped.hold_position = holdPosition.frame;
            mapped.hold_speed_num = clip.frameHold->speedNum;
            mapped.hold_speed_den = clip.frameHold->speedDen;
        }
        mapped.is_audio = planned.audio ? 1 : 0;
        mapped.is_still_image = planned.still ? 1 : 0;
        mapped.video_track = planned.videoTrackIndex;
        mapped.timeline_start_frame = planned.timelineStartFrame;
        mapped.timeline_duration_frames = planned.timelineDurationFrames;
        mapped.effects_enabled = planned.effectsEnabled ? 1 : 0;
        auto& motion = motionStorage.emplace_back();
        for (const auto& frame : planned.motionFrames)
            motion.push_back({frame.localFrame, frame.cropLeft, frame.cropTop, frame.cropRight,
                              frame.cropBottom, frame.rectX, frame.rectY, frame.rectWidth,
                              frame.rectHeight, frame.rotationDegrees, frame.shearDegrees});
        mapped.motion_frames = motion.data();
        mapped.motion_frame_count = static_cast<int>(motion.size());
        mapped.crop_left = planned.cropLeft;
        mapped.crop_top = planned.cropTop;
        mapped.crop_right = planned.cropRight;
        mapped.crop_bottom = planned.cropBottom;
        mapped.rect_x = planned.rectX;
        mapped.rect_y = planned.rectY;
        mapped.rect_width = planned.rectWidth;
        mapped.rect_height = planned.rectHeight;
        mapped.rotation_degrees = planned.rotationDegrees;
        mapped.shear_degrees = planned.shearDegrees;
        mapped.opaque_backdrop = planned.opaqueBackdrop ? 1 : 0;
        auto& opacityKeys = opacityStorage.emplace_back();
        for (const auto& key : planned.opacityKeys)
            opacityKeys.push_back({key.localFrame, key.opacity});
        mapped.opacity_keyframes = opacityKeys.data();
        mapped.opacity_keyframe_count = static_cast<int>(opacityKeys.size());
        auto& gainKeys = gainStorage.emplace_back();
        for (const auto& key : planned.gainKeys)
            gainKeys.push_back({key.localFrame, key.gain});
        mapped.mixer_pan = planned.mixerPan;
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
        // MLT hold producer は real_frame を共有する。複数 render thread で同時に読むと
        // 2 frame 目以降の画素が壊れるため、保持を含む書き出しは 1 thread に固定する。
        .render_threads = std::any_of(project.timelineClips.begin(), project.timelineClips.end(),
                                      [](const auto& clip) { return clip.frameHold.has_value(); })
                              ? 1
                              : request.renderThreads,
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

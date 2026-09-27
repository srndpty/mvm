#include "app/timeline_export.h"

#include <cstdio>
#include <cstdlib>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

mvm::project::TimelineClip clip(std::string id, int videoTrackIndex, std::int64_t start,
                                std::int64_t sourceIn, std::int64_t duration) {
    mvm::project::TimelineClip value;
    value.kind = mvm::project::TimelineClipKind::Video;
    value.id = std::move(id);
    value.name = value.id;
    value.mediaPath = value.id + ".mp4";
    value.sourceFpsNum = 60;
    value.sourceFpsDen = 1;
    value.sourceFrameCount = sourceIn + duration;
    value.sourceInFrame = sourceIn;
    value.sourceOutFrame = sourceIn + duration;
    value.timelineStartFrame = start;
    value.track = mvm::project::TrackRef{mvm::project::TrackKind::Video, videoTrackIndex};
    return value;
}

} // namespace

int main() {
    mvm::app::TimelineExportRequest request;
    request.width = 320;
    request.height = 240;

    mvm::project::Project contiguous = mvm::project::createDefaultProject();
    contiguous.timelineClips = {clip("later", 0, 10, 0, 10), clip("first", 0, 0, 5, 10)};
    const auto sequential = mvm::app::mapTimelineExportPlan(contiguous, request);
    require(sequential.success &&
                sequential.backend == mvm::app::TimelineExportResult::Backend::Sequential,
            "contiguous V1-onlyがsequential fast pathではありません");
    require(sequential.clips[0].projectClipIndex == 1 && sequential.clips[1].projectClipIndex == 0,
            "shuffled Project vectorをtimeline startで解決していません");

    auto gapProject = contiguous;
    gapProject.timelineClips[0].timelineStartFrame = 12;
    const auto gap = mvm::app::mapTimelineExportPlan(gapProject, request);
    require(gap.success && gap.backend == mvm::app::TimelineExportResult::Backend::Tractor,
            "V1-only gapがtractorを選びません");

    mvm::project::Project overlay = mvm::project::createDefaultProject();
    auto bottom = clip("bottom", 0, 0, 0, 100);
    bottom.effects.scalePercent = 80;
    auto topLate = clip("top-late", 1, 60, 20, 20);
    auto topEarly = clip("top-early", 1, 10, 30, 20);
    topEarly.effects.opacityPercent = 50;
    topEarly.effects.cropLeftPercent = 10;
    topEarly.effects.fadeInFrames = 5;
    topEarly.effects.fadeOutFrames = 5;
    overlay.timelineClips = {topLate, bottom, topEarly};
    const auto tractor = mvm::app::mapTimelineExportPlan(overlay, request);
    require(tractor.success && tractor.backend == mvm::app::TimelineExportResult::Backend::Tractor,
            "V1+V2がtractorを選びません");
    require(tractor.clips.size() == 3 && tractor.clips[0].videoTrackIndex == 0 &&
                tractor.clips[1].timelineStartFrame == 10 &&
                tractor.clips[2].timelineStartFrame == 60,
            "track/start順のmappingが不正です");
    require(tractor.clips[0].effectsEnabled, "V1 M7a effectsを失いました");
    const auto& mappedTop = tractor.clips[1];
    require(mappedTop.projectClipIndex == 2 && mappedTop.timelineStartFrame == 10 &&
                mappedTop.timelineDurationFrames == 20,
            "V2 source-in/timeline start mappingが不正です");
    require(mappedTop.cropLeft == 32 && mappedTop.opacityKeys.front().localFrame == 0 &&
                mappedTop.opacityKeys.back().localFrame == 19,
            "V2 cropまたはtransition-local key domainが不正です");
    require(mappedTop.opacityKeys.front().opacity == 0.0 &&
                mappedTop.opacityKeys.back().opacity == 0.0,
            "clipFadeFactor由来のfade端値が不正です");
    require(tractor.clips[2].opacityKeys.front().localFrame == 0 &&
                tractor.clips[2].opacityKeys.back().localFrame == 19,
            "default V2にも全尺overlay transition mappingがありません");
    require(tractor.clips[1].timelineStartFrame + tractor.clips[1].timelineDurationFrames <
                tractor.clips[2].timelineStartFrame,
            "two V2 clip間gapのmapping fixtureが成立していません");

    // 30fps素材を60fps timelineのV2へ置くと、最終素材frameはtimeline 2 frameに跨る。
    // 端keyはclip末尾(timeline-local 19)に置き、最終素材frameのopacityを保持する。
    mvm::project::Project lowFps = mvm::project::createDefaultProject();
    auto lowFpsTop = clip("low-fps-top", 1, 0, 0, 10);
    lowFpsTop.sourceFpsNum = 30;
    lowFpsTop.effects.opacityPercent = 50;
    lowFpsTop.effects.fadeInFrames = 3;
    lowFpsTop.effects.fadeOutFrames = 3;
    lowFps.timelineClips = {clip("low-fps-bottom", 0, 0, 0, 20), lowFpsTop};
    const auto lowFpsPlan = mvm::app::mapTimelineExportPlan(lowFps, request);
    require(lowFpsPlan.success && lowFpsPlan.clips.size() == 2 &&
                lowFpsPlan.clips[1].timelineDurationFrames == 20,
            "30fps V2 fixtureのmappingが成立していません");
    const auto& lowFpsKeys = lowFpsPlan.clips[1].opacityKeys;
    require(lowFpsKeys.size() >= 2 && lowFpsKeys.front().localFrame == 0 &&
                lowFpsKeys.back().localFrame == 19,
            "timelineより低fpsのV2で端keyがclip末尾に置かれません");
    // 最終素材frame(source-local 9)はtimeline-local 18から始まり、19まで同じopacityを保つ。
    require(lowFpsKeys[lowFpsKeys.size() - 2].localFrame == 18 &&
                lowFpsKeys[lowFpsKeys.size() - 2].opacity == lowFpsKeys.back().opacity &&
                lowFpsKeys.back().opacity == 0.0,
            "最終素材frameのopacityを末尾まで保持しません");

    mvm::project::Project linkedAv = mvm::project::createDefaultProject();
    auto linkedVideo = clip("linked-video", 0, 0, 0, 100);
    linkedVideo.linkGroupId = "linked-pair";
    auto linkedAudio = linkedVideo;
    linkedAudio.id = "linked-audio";
    linkedAudio.name = "linked-audio";
    linkedAudio.kind = mvm::project::TimelineClipKind::Audio;
    linkedAudio.track = {mvm::project::TrackKind::Audio, 0};
    linkedAv.timelineClips = {linkedVideo, linkedAudio};
    const auto linkedPlan = mvm::app::mapTimelineExportPlan(linkedAv, request);
    require(linkedPlan.success &&
                linkedPlan.backend == mvm::app::TimelineExportResult::Backend::Tractor &&
                linkedPlan.clips.size() == 2 && !linkedPlan.clips[0].audio &&
                linkedPlan.clips[1].audio,
            "linked audioを独立audio trackへmappingできません");

    auto mismatchedLinkedAv = linkedAv;
    mismatchedLinkedAv.timelineClips[1].sourceInFrame = 1;
    mismatchedLinkedAv.timelineClips[1].timelineStartFrame = 10;
    const auto mismatchedLinkedPlan = mvm::app::mapTimelineExportPlan(mismatchedLinkedAv, request);
    require(mismatchedLinkedPlan.success && mismatchedLinkedPlan.clips.size() == 2 &&
                mismatchedLinkedPlan.clips[1].audio &&
                mismatchedLinkedPlan.clips[1].timelineStartFrame == 10,
            "videoとstart/trimが異なるlinked audioをmappingできません");

    auto standaloneAudio = linkedAudio;
    standaloneAudio.linkGroupId.clear();
    mvm::project::Project audioOnly = mvm::project::createDefaultProject();
    audioOnly.timelineClips = {standaloneAudio};
    const auto audioOnlyPlan = mvm::app::mapTimelineExportPlan(audioOnly, request);
    require(audioOnlyPlan.success && audioOnlyPlan.clips.size() == 1 &&
                audioOnlyPlan.clips[0].audio,
            "単独audio clipを独立trackへmappingできません");
    mvm::project::Project automated = mvm::project::createDefaultProject();
    auto picture = clip("automated-video", 0, 0, 0, 11);
    picture.effects.opacityKeys = {{0, 100}, {10, 0}};
    picture.effects.fadeInFrames = 3;
    auto sound = picture;
    sound.id = "automated-audio";
    sound.kind = mvm::project::TimelineClipKind::Audio;
    sound.track = {mvm::project::TrackKind::Audio, 0};
    sound.effects = {};
    sound.effects.volumeKeys = {{0, 0}, {10, 200}};
    automated.timelineClips = {picture, sound};
    const auto automationPlan = mvm::app::mapTimelineExportPlan(automated, request);
    require(automationPlan.success && automationPlan.clips.size() == 2 &&
                automationPlan.clips[0].opacityKeys.size() == 11 &&
                automationPlan.clips[0].opacityKeys[0].opacity == 0.0 &&
                automationPlan.clips[0].opacityKeys[5].opacity == 0.5 &&
                automationPlan.clips[1].gainKeys.size() == 11 &&
                automationPlan.clips[1].gainKeys[0].gain == 0.0 &&
                automationPlan.clips[1].gainKeys[5].gain == 1.0 &&
                automationPlan.clips[1].gainKeys[10].gain == 2.0,
            "手動カーブとフェードを出力フレームへ反映できません");
    return 0;
}

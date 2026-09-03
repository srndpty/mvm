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
    return 0;
}

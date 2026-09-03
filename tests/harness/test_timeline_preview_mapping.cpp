#include "app/timeline_preview_mapping.h"

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
    mvm::project::Project project = mvm::project::createDefaultProject();
    project.timelineClips = {
        clip("v1-a", 0, 0, 100, 10),
        clip("v2-a", 1, 5, 20, 10),
        clip("v1-b", 0, 30, 200, 10),
    };

    const auto v1Only = mvm::app::mapTimelinePreviewFrame(project, 2);
    require(v1Only.success && v1Only.layers.size() == 1 && v1Only.layers[0].videoTrackIndex == 0 &&
                v1Only.layers[0].sourceFrameNumber == 102,
            "V1-only mappingが不正です");

    const auto both = mvm::app::mapTimelinePreviewFrame(project, 7);
    require(both.success && both.outputFrameNumber == 7 && both.layers.size() == 2,
            "V1+V2 mappingが不正です");
    require(both.layers[0].videoTrackIndex == 0 && both.layers[1].videoTrackIndex == 1,
            "composition orderがV1 bottom/V2 topではありません");
    require(both.layers[0].sourceFrameNumber == 107 && both.layers[1].sourceFrameNumber == 22,
            "共通output identityから異なるsource frameを計算できません");

    const auto v2Only = mvm::app::mapTimelinePreviewFrame(project, 12);
    require(v2Only.layers.size() == 1 && v2Only.layers[0].videoTrackIndex == 1,
            "V2-only mappingが不正です");
    const auto gap = mvm::app::mapTimelinePreviewFrame(project, 25);
    require(gap.success && gap.layers.empty() && gap.outputFrameNumber == 25,
            "gapを成功するempty mappingにしていません");
    const auto next = mvm::app::mapTimelinePreviewFrame(project, 30);
    require(next.layers.size() == 1 && next.layers[0].clipId == "v1-b" &&
                !mvm::app::sameTimelinePreviewSourceSet(gap, next),
            "source-set boundary changeを検出できません");
    require(
        mvm::app::sameTimelinePreviewSourceSet(both, mvm::app::mapTimelinePreviewFrame(project, 8)),
        "同一active source setを境界変更と誤認しました");
    require(!mvm::app::mapTimelinePreviewFrame(project, -1).success,
            "negative timeline frameを受理しました");
    return 0;
}

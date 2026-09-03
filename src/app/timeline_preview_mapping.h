#ifndef MVM_APP_TIMELINE_PREVIEW_MAPPING_H
#define MVM_APP_TIMELINE_PREVIEW_MAPPING_H

#include "project/project.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mvm::app {

struct TimelinePreviewLayerMapping {
    int videoTrackIndex = 0;
    int clipIndex = -1;
    std::string clipId;
    std::int64_t sourceFrameNumber = -1;
};

struct TimelinePreviewFrameMapping {
    bool success = false;
    std::int64_t outputFrameNumber = -1;
    // videoTrackIndex の昇順 (bottom -> top)。mute された track は含まない。
    std::vector<TimelinePreviewLayerMapping> layers;
    std::string error;
};

// preview が同時に合成できる video layer の上限。GPU compositor 自体は N layer を
// 描けるが、qualify して計測したのは 2 layer までなので、それを超える frame は
// 成功に見せず失敗として返す。
inline constexpr std::size_t kMaxPreviewVideoLayers = 2;

TimelinePreviewFrameMapping mapTimelinePreviewFrame(const project::Project& project,
                                                    std::int64_t timelineFrame);
bool sameTimelinePreviewSourceSet(const TimelinePreviewFrameMapping& a,
                                  const TimelinePreviewFrameMapping& b);

// preview 対象になる audio clip (mute されていない audio track のうち、
// timelineFrame に載っているもの)。現在の engine は audio source を 1 件しか
// 受理しないため、複数該当した場合は失敗として返す。
struct TimelinePreviewAudioMapping {
    bool success = false;
    bool hasAudio = false;
    int clipIndex = -1;
    std::string clipId;
    std::int64_t sourceFrameNumber = -1;
    std::string error;
};

TimelinePreviewAudioMapping mapTimelinePreviewAudio(const project::Project& project,
                                                    std::int64_t timelineFrame);

} // namespace mvm::app

#endif

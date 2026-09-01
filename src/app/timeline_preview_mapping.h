#ifndef MVM_APP_TIMELINE_PREVIEW_MAPPING_H
#define MVM_APP_TIMELINE_PREVIEW_MAPPING_H

#include "project/project.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mvm::app {

struct TimelinePreviewLayerMapping {
    project::VideoTrack track = project::VideoTrack::V1;
    int clipIndex = -1;
    std::string clipId;
    std::int64_t sourceFrameNumber = -1;
};

struct TimelinePreviewFrameMapping {
    bool success = false;
    std::int64_t outputFrameNumber = -1;
    std::vector<TimelinePreviewLayerMapping> layers;
    std::string error;
};

TimelinePreviewFrameMapping mapTimelinePreviewFrame(const project::Project& project,
                                                    std::int64_t timelineFrame);
bool sameTimelinePreviewSourceSet(const TimelinePreviewFrameMapping& a,
                                  const TimelinePreviewFrameMapping& b);

} // namespace mvm::app

#endif

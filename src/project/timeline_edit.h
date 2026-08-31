#ifndef MVM_PROJECT_TIMELINE_EDIT_H
#define MVM_PROJECT_TIMELINE_EDIT_H

#include "project/project.h"

#include <cstdint>
#include <string>

namespace mvm::project {

struct TimelineEditResult {
    bool success = false;
    int selectedIndex = -1;
    std::string error;
};

struct TimelineFrameResult {
    bool success = false;
    std::int64_t frame = 0;
    std::string error;
};

struct TimelineValidationResult {
    bool success = false;
    std::int64_t totalFrames = 0;
    std::string error;
};

enum class TrimEdge { Left, Right };

TimelineFrameResult sourceBoundaryToTimelineBoundary(std::int64_t sourceFrame,
                                                     std::int64_t sourceFpsNum,
                                                     std::int64_t sourceFpsDen,
                                                     std::int64_t timelineFpsNum,
                                                     std::int64_t timelineFpsDen);
TimelineFrameResult timelineBoundaryToSourceBoundary(std::int64_t timelineFrame,
                                                     std::int64_t sourceFpsNum,
                                                     std::int64_t sourceFpsDen,
                                                     std::int64_t timelineFpsNum,
                                                     std::int64_t timelineFpsDen);
TimelineFrameResult timelineClipDuration(const Project& project, const TimelineClip& clip);
bool sourceRateMatchesTimelineRate(const Project& project, const TimelineClip& clip);
TimelineValidationResult validateTimeline(const Project& project);
TimelineValidationResult recomputeTimelineStarts(Project& project);
int timelineClipIndexAtFrame(const Project& project, std::int64_t timelineFrame);

TimelineEditResult moveTimelineClip(Project& project, int selectedIndex, int offset);
TimelineEditResult reorderTimelineClip(Project& project, const std::string& clipId,
                                       int destinationIndex);
TimelineEditResult deleteTimelineClip(Project& project, int selectedIndex);
TimelineEditResult trimTimelineClip(Project& project, const std::string& clipId, TrimEdge edge,
                                    std::int64_t projectFrameDelta);
TimelineEditResult appendManimTimelineClip(Project& project, const ManimAsset& asset,
                                           std::string clipId, std::int64_t sourceFpsNum,
                                           std::int64_t sourceFpsDen,
                                           std::int64_t sourceFrameCount);

} // namespace mvm::project

#endif // MVM_PROJECT_TIMELINE_EDIT_H

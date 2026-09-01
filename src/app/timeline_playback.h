#ifndef MVM_APP_TIMELINE_PLAYBACK_H
#define MVM_APP_TIMELINE_PLAYBACK_H

#include "project/project.h"

#include <cstdint>
#include <string>

namespace mvm::app {

struct PlaybackFrameResult {
    bool success = false;
    std::int64_t frame = 0;
    std::string error;
};

enum class TimelinePlaybackTransition { StayInClip, SwitchClip, Finished };

struct TimelinePlaybackStep {
    bool success = false;
    TimelinePlaybackTransition transition = TimelinePlaybackTransition::StayInClip;
    std::int64_t frame = 0;
    int clipIndex = -1;
    std::string error;
};

PlaybackFrameResult timelineFrameFromElapsed(std::int64_t baseFrame,
                                             std::int64_t elapsedNanoseconds,
                                             std::int64_t timelineFpsNum,
                                             std::int64_t timelineFpsDen);

TimelinePlaybackStep evaluateTimelinePlayback(const project::Project& project, int activeClipIndex,
                                              std::int64_t candidateFrame);

bool timelinePreviewCompatible(const project::Project& project);
bool timelineCanPlay(const project::Project& project, bool busy, bool playing,
                     std::int64_t playheadFrame, std::int64_t totalTimelineFrames);

} // namespace mvm::app

#endif // MVM_APP_TIMELINE_PLAYBACK_H

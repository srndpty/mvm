#ifndef MVM_AUDIO_PREVIEW_AUDIO_VIDEO_SCHEDULER_DETAIL_H
#define MVM_AUDIO_PREVIEW_AUDIO_VIDEO_SCHEDULER_DETAIL_H

#include "media/audio_preview/audio_video_scheduler.h"

#include <limits>

namespace mvm::audio::detail {

inline FormalVideoTarget formalVideoTargetForFrame(std::int64_t mediaSample,
                                                   std::int64_t endSampleExclusive,
                                                   std::int64_t targetFrame) {
    if (mediaSample < 0 || endSampleExclusive <= 0 || mediaSample >= endSampleExclusive)
        return {};
    return {false, targetFrame};
}

inline AudioVideoScheduleDecision
scheduleVideoForTarget(std::int64_t audioMediaSample, std::int64_t lastDisplayedFrame,
                       std::int64_t lastRequestedFrame, std::int64_t videoFrameCount,
                       std::int64_t pendingSeekFrame, std::int64_t targetFrame) {
    if (audioMediaSample < 0 || lastDisplayedFrame < -1 || lastRequestedFrame < -1 ||
        videoFrameCount <= 0 || pendingSeekFrame < -1)
        return {};
    // seek completionがexact targetをpublish済みなら、最初のdisplayだけは通常再生の
    // catch-upより先に要求する。表示後にcallerがpendingをclearする。
    if (pendingSeekFrame >= 0) {
        if (pendingSeekFrame >= videoFrameCount || pendingSeekFrame <= lastDisplayedFrame)
            return {};
        return {AudioVideoScheduleAction::Request, pendingSeekFrame, 0};
    }
    if (targetFrame >= videoFrameCount)
        return {AudioVideoScheduleAction::End, targetFrame, 0};
    if (targetFrame < lastDisplayedFrame)
        return {AudioVideoScheduleAction::ClockRegression, targetFrame, 0};
    if (targetFrame == lastDisplayedFrame || targetFrame == lastRequestedFrame)
        return {AudioVideoScheduleAction::Hold, targetFrame, 0};
    const std::int64_t previous = lastRequestedFrame >= 0 ? lastRequestedFrame : lastDisplayedFrame;
    const std::int64_t skipped =
        previous >= 0 && targetFrame > previous ? targetFrame - previous - 1 : 0;
    return {skipped > 0 ? AudioVideoScheduleAction::CatchUp : AudioVideoScheduleAction::Request,
            targetFrame, skipped};
}

inline bool isVideoAheadForTarget(std::int64_t videoFrameNumber,
                                  std::int64_t audioMediaSampleAtDisplay,
                                  std::int64_t targetFrame) {
    if (videoFrameNumber < 0 || audioMediaSampleAtDisplay < 0 ||
        targetFrame == std::numeric_limits<std::int64_t>::max())
        return true;
    return videoFrameNumber > targetFrame + 1;
}

} // namespace mvm::audio::detail

#endif

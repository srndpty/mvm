#include "core/checked_output_timebase.h"
#include "media/audio_preview/audio_video_scheduler.h"
#include "media/audio_preview/audio_video_scheduler_detail.h"

namespace mvm::audio {

FormalVideoTarget formalVideoTargetForSample(std::int64_t mediaSample,
                                             std::int64_t endSampleExclusive,
                                             const core::CheckedOutputTimebase& timebase) {
    const auto frame = timebase.schedulerOutputFrame(mediaSample);
    if (!frame)
        return {};
    return detail::formalVideoTargetForFrame(mediaSample, endSampleExclusive, frame.value());
}

AudioVideoScheduleDecision
scheduleVideoForAudio(std::int64_t audioMediaSample, std::int64_t lastDisplayedFrame,
                      std::int64_t lastRequestedFrame, std::int64_t videoFrameCount,
                      std::int64_t pendingSeekFrame, const core::CheckedOutputTimebase& timebase) {
    const auto target = timebase.schedulerOutputFrame(audioMediaSample);
    if (!target)
        return {};
    return detail::scheduleVideoForTarget(audioMediaSample, lastDisplayedFrame, lastRequestedFrame,
                                          videoFrameCount, pendingSeekFrame, target.value());
}

bool isVideoAheadViolation(std::int64_t videoFrameNumber, std::int64_t audioMediaSampleAtDisplay,
                           const core::CheckedOutputTimebase& timebase) {
    const auto target = timebase.schedulerOutputFrame(audioMediaSampleAtDisplay);
    if (!target)
        return true;
    return detail::isVideoAheadForTarget(videoFrameNumber, audioMediaSampleAtDisplay,
                                         target.value());
}

} // namespace mvm::audio

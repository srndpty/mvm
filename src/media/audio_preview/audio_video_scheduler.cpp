#include "media/audio_preview/audio_video_scheduler.h"

namespace mvm::audio {

bool acceptsVideoMasterSource(VideoMasterSource source) {
    return source == VideoMasterSource::AudioDeviceClock;
}

AudioVideoScheduleDecision scheduleVideoForAudio(std::int64_t audioMediaSample,
                                                 std::int64_t lastDisplayedFrame,
                                                 std::int64_t lastRequestedFrame,
                                                 std::int64_t videoFrameCount) {
    if (audioMediaSample < 0 || lastDisplayedFrame < -1 || lastRequestedFrame < -1 ||
        videoFrameCount <= 0)
        return {};
    const std::int64_t target = audioMediaSample / kSamplesPerVideoFrame;
    if (target >= videoFrameCount)
        return {AudioVideoScheduleAction::End, target, 0};
    if (target < lastDisplayedFrame)
        return {AudioVideoScheduleAction::ClockRegression, target, 0};
    if (target == lastDisplayedFrame || target == lastRequestedFrame)
        return {AudioVideoScheduleAction::Hold, target, 0};
    const std::int64_t previous = lastRequestedFrame >= 0 ? lastRequestedFrame : lastDisplayedFrame;
    const std::int64_t skipped = previous >= 0 && target > previous ? target - previous - 1 : 0;
    return {skipped > 0 ? AudioVideoScheduleAction::CatchUp : AudioVideoScheduleAction::Request,
            target, skipped};
}

bool isVideoAheadViolation(std::int64_t videoFrameNumber, std::int64_t audioMediaSampleAtDisplay) {
    if (videoFrameNumber < 0 || audioMediaSampleAtDisplay < 0)
        return true;
    return videoFrameNumber > audioMediaSampleAtDisplay / kSamplesPerVideoFrame + 1;
}

} // namespace mvm::audio

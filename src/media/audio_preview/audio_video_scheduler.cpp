#include "media/audio_preview/audio_video_scheduler.h"

#include "media/audio_preview/audio_video_scheduler_detail.h"

namespace mvm::audio {

FormalVideoTarget formalVideoTargetForSample(std::int64_t mediaSample,
                                             std::int64_t endSampleExclusive) {
    return detail::formalVideoTargetForFrame(mediaSample, endSampleExclusive,
                                             mediaSample / kSamplesPerVideoFrame);
}

bool acceptsVideoMasterSource(VideoMasterSource source) {
    return source == VideoMasterSource::AudioDeviceClock;
}

AudioVideoScheduleDecision scheduleVideoForAudio(std::int64_t audioMediaSample,
                                                 std::int64_t lastDisplayedFrame,
                                                 std::int64_t lastRequestedFrame,
                                                 std::int64_t videoFrameCount,
                                                 std::int64_t pendingSeekFrame) {
    return detail::scheduleVideoForTarget(audioMediaSample, lastDisplayedFrame, lastRequestedFrame,
                                          videoFrameCount, pendingSeekFrame,
                                          audioMediaSample / kSamplesPerVideoFrame);
}

bool isVideoAheadViolation(std::int64_t videoFrameNumber, std::int64_t audioMediaSampleAtDisplay) {
    return detail::isVideoAheadForTarget(videoFrameNumber, audioMediaSampleAtDisplay,
                                         audioMediaSampleAtDisplay / kSamplesPerVideoFrame);
}

} // namespace mvm::audio

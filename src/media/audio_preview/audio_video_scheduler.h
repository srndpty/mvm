#ifndef MVM_AUDIO_PREVIEW_AUDIO_VIDEO_SCHEDULER_H
#define MVM_AUDIO_PREVIEW_AUDIO_VIDEO_SCHEDULER_H

#include <cstdint>

namespace mvm::audio {

constexpr std::int64_t kSamplesPerVideoFrame = 800;

enum class AudioVideoScheduleAction { Hold, Request, CatchUp, End, ClockRegression, Invalid };
enum class VideoMasterSource { AudioDeviceClock, Qpc };

bool acceptsVideoMasterSource(VideoMasterSource source);

struct AudioVideoScheduleDecision {
    AudioVideoScheduleAction action = AudioVideoScheduleAction::Invalid;
    std::int64_t targetFrame = -1;
    std::int64_t skippedFrames = 0;
};

AudioVideoScheduleDecision scheduleVideoForAudio(std::int64_t audioMediaSample,
                                                 std::int64_t lastDisplayedFrame,
                                                 std::int64_t lastRequestedFrame,
                                                 std::int64_t videoFrameCount);

bool isVideoAheadViolation(std::int64_t videoFrameNumber, std::int64_t audioMediaSampleAtDisplay);

} // namespace mvm::audio
#endif

#ifndef MVM_AUDIO_PREVIEW_AUDIO_VIDEO_SCHEDULER_H
#define MVM_AUDIO_PREVIEW_AUDIO_VIDEO_SCHEDULER_H

#include <cstdint>

namespace mvm::audio {

constexpr std::int64_t kSamplesPerVideoFrame = 800;
constexpr std::int64_t kFormalPlaybackSamples = 2'880'000;
constexpr std::int64_t kFormalPlaybackFrames = 3'600;

// formal 区間は end sample を含まない。[0, endSampleExclusive) の外では
// frame を新規 schedule しない。source の長さとは別の測定境界である。
struct FormalVideoTarget {
    bool measurementEnded = true;
    std::int64_t frameNumber = -1;
};

FormalVideoTarget formalVideoTargetForSample(std::int64_t mediaSample,
                                             std::int64_t endSampleExclusive);

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
                                                 std::int64_t videoFrameCount,
                                                 std::int64_t pendingSeekFrame = -1);

bool isVideoAheadViolation(std::int64_t videoFrameNumber, std::int64_t audioMediaSampleAtDisplay);

} // namespace mvm::audio
#endif

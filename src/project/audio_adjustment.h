#ifndef MVM_PROJECT_AUDIO_ADJUSTMENT_H
#define MVM_PROJECT_AUDIO_ADJUSTMENT_H
#include "project/clip_effects.h"

#include <string>
#include <vector>

namespace mvm::project {
struct AudioAdjustmentSettings {
    std::vector<int> voiceTracks;
    int bgmTrack = -1;
    bool normalize = true;
    bool duck = true;
    double voiceLufs = -16;
    double bgmLufs = -24;
    double reductionDb = 12;
    double thresholdDb = -35;
    int attackMs = 150;
    int holdMs = 300;
    int releaseMs = 600;
};

struct AudioDetectedRange {
    std::int64_t startSample = 0;
    std::int64_t endSample = 0;
};

bool validateAudioAdjustmentSettings(const AudioAdjustmentSettings& settings, int trackCount,
                                     std::string& error);
std::vector<AudioDetectedRange> mergeAudioDetectedRanges(std::vector<AudioDetectedRange> ranges);
std::vector<ClipKeyframe> makeDuckingKeys(const std::vector<AudioDetectedRange>& ranges,
                                          const AudioAdjustmentSettings& settings,
                                          std::int64_t clipStartFrame, std::int64_t durationFrames,
                                          std::int64_t fpsNum, std::int64_t fpsDen);
} // namespace mvm::project
#endif

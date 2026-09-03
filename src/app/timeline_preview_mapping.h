#ifndef MVM_APP_TIMELINE_PREVIEW_MAPPING_H
#define MVM_APP_TIMELINE_PREVIEW_MAPPING_H

#include "project/project.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mvm::app {

struct TimelinePreviewLayerMapping {
    int videoTrackIndex = 0;
    int clipIndex = -1;
    std::string clipId;
    std::int64_t sourceFrameNumber = -1;
};

struct TimelinePreviewFrameMapping {
    bool success = false;
    std::int64_t outputFrameNumber = -1;
    // videoTrackIndex の昇順 (bottom -> top)。mute された track は含まない。
    std::vector<TimelinePreviewLayerMapping> layers;
    std::string error;
};

// preview が同時に合成できる video layer の上限。
//
// GPU compositor 自体は N layer を描ける。この 2 という値は
// **現在の configured limit** であり、それが実測済みなのは
// `MeasuredPreviewEnvelope` の組 (60/1 × 2 video source × 2 layer × 1 audio ×
// 48kHz stereo) としてである。「2 layer が単独で qualify されている」ではない。
// 上限を超える frame は成功に見せず失敗として返す。
inline constexpr std::size_t kMaxPreviewVideoLayers = 2;

TimelinePreviewFrameMapping mapTimelinePreviewFrame(const project::Project& project,
                                                    std::int64_t timelineFrame);
bool sameTimelinePreviewSourceSet(const TimelinePreviewFrameMapping& a,
                                  const TimelinePreviewFrameMapping& b);

// preview 対象になる audio clip (mute されていない audio track のうち、
// timelineFrame に載っているもの)。現在の engine は audio source を 1 件しか
// 受理しないため、複数該当した場合は失敗として返す。
struct TimelinePreviewAudioMapping {
    bool success = false;
    bool hasAudio = false;
    int clipIndex = -1;
    std::string clipId;
    std::int64_t sourceFrameNumber = -1;
    std::string error;
};

TimelinePreviewAudioMapping mapTimelinePreviewAudio(const project::Project& project,
                                                    std::int64_t timelineFrame);

// audio source の media sample と output frame の対応ずれ。
//
//   media sample = (output frame を換算した sample) + offset
//
// sourceInFrame は素材固有の frame domain、timelineStartFrame は Project timebase
// なので、**別々の timebase で sample 化する**。両方を Project fps で換算すると、
// Project fps を変えたときに素材内の位置がずれる。
struct AudioPreviewOffset {
    bool success = false;
    std::int64_t sampleOffset = 0;
    std::string error;
};

AudioPreviewOffset audioPreviewSampleOffset(const project::Project& project,
                                            const project::TimelineClip& clip);

// audio 素材の尺を frame 数へ変換する。素材全体を含む clip を作るので ceil にする。
// floor だと末尾が最大 1 frame 切り落とされ、必ず短くなる方向へ bias する。
struct AudioSourceFrameCount {
    bool success = false;
    std::int64_t frameCount = 0;
    std::string error;
};

AudioSourceFrameCount audioSourceFrameCount(double durationSeconds, std::int64_t fpsNum,
                                            std::int64_t fpsDen);

} // namespace mvm::app

#endif

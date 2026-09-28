#ifndef MVM_APP_TIMELINE_PREVIEW_MAPPING_H
#define MVM_APP_TIMELINE_PREVIEW_MAPPING_H

#include "project/project.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mvm::app {

struct TimelinePreviewLayerMapping {
    int videoTrackIndex = 0;
    int clipIndex = -1;
    std::string clipId;
    std::int64_t sourceFrameNumber = -1;
};

// 文字 clip。decode source を持たず、静止画 layer として合成する。
struct TimelinePreviewTextLayerMapping {
    int videoTrackIndex = 0;
    int clipIndex = -1;
    std::string clipId;
    // この frame の不透明度 (0..1)。opacity の値・key・fade を評価したもの。
    // 書き出しは同じ effects を MLT の経路で評価するので、preview もここで合わせる。
    double opacity = 1.0;
};

struct TimelinePreviewFrameMapping {
    bool success = false;
    std::int64_t outputFrameNumber = -1;
    // 素材を decode する video clip。videoTrackIndex の昇順 (bottom -> top)。
    // mute された track は含まない。
    std::vector<TimelinePreviewLayerMapping> layers;
    // 文字 clip。videoTrackIndex の昇順。mute された track は含まない。
    std::vector<TimelinePreviewTextLayerMapping> textLayers;
    std::string error;
};

// preview が同時に decode する video source の上限。
//
// GPU compositor 自体は N layer を描ける。この 2 という値は
// **現在の configured limit** であり、それが実測済みなのは
// `MeasuredPreviewEnvelope` の組 (60/1 × 2 video source × 2 layer × 1 audio ×
// 48kHz stereo) としてである。「2 layer が単独で qualify されている」ではない。
// 上限を超える frame は成功に見せず失敗として返す。
inline constexpr std::size_t kMaxPreviewVideoLayers = 2;

// preview が同時に合成する layer (video + 文字) の上限。文字は decode source を
// 増やさないので video source の上限とは別に数える。V1-V3 に合わせた 3 であり、
// measured envelope (layer 2) の外なので未計測である。
// video が無い frame では文字を合成に使わない (UI 側が重ねる) ため数えない。
inline constexpr std::size_t kMaxPreviewCompositionLayers = 3;

// preview の合成順の 1 要素。text が false なら layers[index]、true なら textLayers[index]。
struct TimelinePreviewStackEntry {
    bool text = false;
    std::size_t index = 0;
    int videoTrackIndex = 0;
    bool operator==(const TimelinePreviewStackEntry&) const = default;
};

// video と文字を track の昇順 (背面 -> 前面) に並べる。書き出しと同じく
// track index だけで前後を決める。preview の重なり順はここでだけ決める。
std::vector<TimelinePreviewStackEntry>
previewLayerStack(const TimelinePreviewFrameMapping& mapping);

TimelinePreviewFrameMapping mapTimelinePreviewFrame(const project::Project& project,
                                                    std::int64_t timelineFrame);

// video preview source が timeline frame を素材 frame へ写す規則。
// descriptor へ渡した値そのもので、source を作った clip の ID は含めない。
struct PreviewVideoMapping {
    std::filesystem::path mediaPath;
    std::int64_t sourceInFrame = 0;
    std::int64_t timelineStartFrame = 0;
    // 速度込みの実効 fps (project::clipTimebase)。速度が違えば別の対応になる。
    std::int64_t timebaseNum = 0;
    std::int64_t timebaseDen = 1;
    bool operator==(const PreviewVideoMapping&) const = default;
};

PreviewVideoMapping previewVideoMappingOf(const project::TimelineClip& clip);

// installed の source で clip を表示できるか。レーザーで分割した直後のように同じ素材が
// timeline 上で連続していれば、clip が違っても timeline -> 素材の対応は同じになる。
// source は sourceInFrame より前の素材 frame を写せないので、clip 側の in が
// installed 以上であることも要求する。
bool previewVideoMappingCovers(const project::Project& project,
                               const PreviewVideoMapping& installed,
                               const project::TimelineClip& clip);
bool sameTimelinePreviewSourceSet(const TimelinePreviewFrameMapping& a,
                                  const TimelinePreviewFrameMapping& b);

struct TimelinePreviewAudioLayerMapping {
    int audioTrackIndex = 0;
    int clipIndex = -1;
    std::string clipId;
    std::int64_t sourceFrameNumber = -1;
};

// preview 対象になる全audio clip。A1から順にmixする。
struct TimelinePreviewAudioMapping {
    bool success = false;
    std::vector<TimelinePreviewAudioLayerMapping> layers;
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

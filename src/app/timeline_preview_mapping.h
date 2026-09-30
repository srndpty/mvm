#ifndef MVM_APP_TIMELINE_PREVIEW_MAPPING_H
#define MVM_APP_TIMELINE_PREVIEW_MAPPING_H

#include "preview_engine/preview_types.h"
#include "project/project.h"
#include "project/timeline_render.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mvm::app {

// 同じ track の 2 clip がトランジションで重なるので、1 track に lane を 2 本持つ。slot は
// (track, lane) を下から数えた番号で、合成順と preview source の置き場所を決める
// (書き出しの MLT layer と同じ数え方)。トランジションが無ければ slot は track の index と同じ。
struct TimelinePreviewLayerMapping {
    int videoTrackIndex = 0;
    int slot = 0;
    int clipIndex = -1;
    std::string clipId;
    std::int64_t sourceFrameNumber = -1;
    // 描画区間に合わせた clip (トランジションで延ばした素材範囲を含む)。decode source はこちらで作る。
    project::TimelineClip renderClip;
    // トランジションの進み具合 (0..1)。clip の不透明度に掛ける。トランジションの外は 1。
    double transitionOpacity = 1.0;
};

// 文字・画像 clip。decode source を持たず、静止画 layer として合成する。
struct TimelinePreviewStillLayerMapping {
    int videoTrackIndex = 0;
    int slot = 0;
    int clipIndex = -1;
    std::string clipId;
    project::TimelineClipKind kind = project::TimelineClipKind::Text;
    // この frame の不透明度 (0..1)。opacity の値・key・fade とトランジションを評価したもの。
    // 書き出しは同じ effects を MLT の経路で評価するので、preview もここで合わせる。
    double opacity = 1.0;
};

struct TimelinePreviewFrameMapping {
    bool success = false;
    std::int64_t outputFrameNumber = -1;
    // 素材を decode する video clip。videoTrackIndex の昇順 (bottom -> top)。
    // mute された track は含まない。
    std::vector<TimelinePreviewLayerMapping> layers;
    // 文字・画像 clip。videoTrackIndex の昇順。mute された track は含まない。
    std::vector<TimelinePreviewStillLayerMapping> stillLayers;
    std::string error;
};

// preview が同時に decode する video source の上限。
//
// 値は preview engine が受理する product 上限そのもの (数値は preview_types.h にだけ書く)。
// これは **configured limit** であって qualification ではない。実測済みなのは
// `MeasuredPreviewEnvelope` の組 (60/1 × 2 video source × 2 layer × 1 audio ×
// 48kHz stereo) だけである。上限を超える frame は成功に見せず失敗として返す。
inline constexpr std::size_t kMaxPreviewVideoLayers = preview::kProductMaxActiveVideoSources;

// preview が同時に合成する layer (video + 文字・画像) の上限。文字・画像は decode source を
// 増やさないので video source の上限とは別に数える。video の無い frame でも数える。
inline constexpr std::size_t kMaxPreviewCompositionLayers = preview::kProductMaxCompositionLayers;

// preview の合成順の 1 要素。still が false なら layers[index]、true なら stillLayers[index]。
struct TimelinePreviewStackEntry {
    bool still = false;
    std::size_t index = 0;
    int slot = 0;
    bool operator==(const TimelinePreviewStackEntry&) const = default;
};

// video と文字・画像を slot の昇順 (背面 -> 前面) に並べる。書き出しと同じく
// (track, lane) だけで前後を決める。preview の重なり順はここでだけ決める。
std::vector<TimelinePreviewStackEntry>
previewLayerStack(const TimelinePreviewFrameMapping& mapping);

// clip の effect (位置・拡大・回転・crop) を preview layer へ写す。opacity は Project で評価済みの
// 値 (値・key・fade) を渡し、compositor では fade を二重に掛けない。video と画像で共有する。
void applyPreviewLayerEffects(preview::PreviewCompositionLayer& layer,
                              const project::ClipEffects& effects, double opacity,
                              std::int64_t sourceInFrame, std::int64_t sourceDurationFrames);

// preview の frame 問い合わせに使う描画区間の一覧 (project::timelineRenderSegments) と、各区間の
// timeline 上の範囲・slot の起点。Project が変わったときに 1 度だけ作り、frame ごとの問い合わせは
// これを引くだけにする (区間の作り方は timelineRenderSegments に一本化したまま)。
struct TimelinePreviewPlan {
    bool success = false;
    std::string error;
    struct Entry {
        project::TimelineRenderSegment segment;
        std::int64_t start = 0;
        std::int64_t end = 0;
    };
    std::vector<Entry> video;
    std::vector<Entry> audio;
    // video track ごとの slot の起点 (トランジションのある track は lane を 2 本持つ)。
    std::vector<int> slotBases;
};

TimelinePreviewPlan buildTimelinePreviewPlan(const project::Project& project);

// plan は同じ project から作ったものを渡す。plan を持たない呼び出し側 (試験など) 向けに、
// 毎回 plan を作る版も残す。
TimelinePreviewFrameMapping mapTimelinePreviewFrame(const project::Project& project,
                                                    const TimelinePreviewPlan& plan,
                                                    std::int64_t timelineFrame);
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
    std::int64_t holdFrames = 0;
    bool operator==(const PreviewVideoMapping&) const = default;
};

PreviewVideoMapping previewVideoMappingOf(const project::TimelineClip& clip);
preview::PreviewSourceDescriptor previewVideoDescriptorOf(const project::Project& project,
                                                          const project::TimelineClip& clip);

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
    // 鳴らす区間 (トランジションで延ばした clip と、音量・クロスフェードの評価)。
    project::TimelineRenderSegment segment;
};

// preview 対象になる全audio clip。A1から順に、同じ track では開始の早い順に mix する
// (クロスフェードでは同じ track の 2 clip が重なる)。
struct TimelinePreviewAudioMapping {
    bool success = false;
    std::vector<TimelinePreviewAudioLayerMapping> layers;
    std::string error;
};

TimelinePreviewAudioMapping mapTimelinePreviewAudio(const project::Project& project,
                                                    const TimelinePreviewPlan& plan,
                                                    std::int64_t timelineFrame);
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

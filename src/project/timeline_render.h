#ifndef MVM_PROJECT_TIMELINE_RENDER_H
#define MVM_PROJECT_TIMELINE_RENDER_H

#include "project/project.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mvm::project {

// トランジションで値が 0 -> 1 (または 1 -> 0) へ変わる区間 (timeline frame)。
struct TransitionEnvelope {
    std::int64_t startFrame = 0;
    std::int64_t frames = 0;
    bool operator==(const TransitionEnvelope&) const = default;
};

// 描画・発音の単位。書き出し・シャトル/スクラブ・preview が同じ区間分けを使う。
//
// トランジションの無い clip は clip そのものが 1 区間になる。トランジションがあると clip は
// 余白の分だけ延び (outgoing は cut の後ろへ、incoming は cut の前へ)、同じ track の 2 clip が
// 重なる。映像はクロスディゾルブにするため incoming の頭の区間 [cut - before, cut + after) を
// lane 1 (outgoing の上) に切り出し、outgoing は下 (lane 0) で不透明のまま残す。incoming は
// 素材の余白 (縦横比が出力と違うときの左右・上下) を不透明な黒で埋めた出力全体の 1 枚として
// 不透明度 p で重ねる (preview の opaqueBackdrop、書き出しの attach_export_backdrop)。これで
// 余白の所も含めて A(1 - p) + B p になる (V1 では下が黒なので Premiere と同じ)。両方を
// 1 - p / p にすると中央で暗くなる (0.25 A + 0.5 B)。音声は clip ごとに 1 区間で、等パワー
// (sin / cos) の gain で重ねて加算する。
struct TimelineRenderSegment {
    int clipIndex = -1;    // project.timelineClips の index
    TimelineClip original; // effect の評価に使う元の clip
    // 区間に合わせた clip。timeline 上の位置と素材範囲が区間と一致する (素材 frame の対応は
    // original と同じ)。decode・producer の cut はこちらを使う。
    TimelineClip clip;
    int lane = 0;
    std::optional<TransitionEnvelope> fadeIn;  // この区間で 0 -> 1
    std::optional<TransitionEnvelope> fadeOut; // 音声だけ。1 -> 0
    double mixerGainDb = 0.0;
    double mixerPan = 0.0;
};

// kind の track に載る有効な clip の描画区間。無効な clip は含めず、無効な clip を含む
// トランジションは描かない (両側とも cut で切り替わる)。track の mute は呼び出し側が見る。
// Project は validateTimeline を通っていること。区間を素材 frame へ一意に換算できなければ失敗する。
bool timelineRenderSegments(const Project& project, TrackKind kind,
                            std::vector<TimelineRenderSegment>& segments, std::string& error);

// 描画に使うトランジションがあるか (無効な clip を含むものは数えない)。
bool hasRenderedTransitions(const Project& project, TrackKind kind);

// envelope の中の進み具合 (frame の中央で測る)。区間の前は 0、後は 1。
double transitionProgress(const TransitionEnvelope& envelope, std::int64_t timelineFrame);

// timelineFrameでの不透明度と音量。音量はクリップ音量にトラック音量を掛ける。元の clip の effect
// (key・フェード) を、 延ばした区間では clip
// の端の値のまま評価し、トランジションの進み具合を掛ける。
std::optional<double> renderSegmentOpacity(const TimelineRenderSegment& segment,
                                           std::int64_t timelineFpsNum, std::int64_t timelineFpsDen,
                                           std::int64_t timelineFrame);
std::optional<double> renderSegmentGain(const TimelineRenderSegment& segment,
                                        std::int64_t timelineFpsNum, std::int64_t timelineFpsDen,
                                        std::int64_t timelineFrame);

} // namespace mvm::project

#endif // MVM_PROJECT_TIMELINE_RENDER_H

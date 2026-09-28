#ifndef MVM_PROJECT_TIMELINE_EDIT_H
#define MVM_PROJECT_TIMELINE_EDIT_H

#include "core/source_frame_mapping.h"
#include "project/project.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mvm::project {

struct TimelineEditResult {
    bool success = false;
    int selectedIndex = -1;
    std::string error;
};

struct TimelineFrameResult {
    bool success = false;
    std::int64_t frame = 0;
    std::string error;
};

struct TimelineValidationResult {
    bool success = false;
    std::int64_t totalFrames = 0;
    std::string error;
};

enum class ClipKeyKind { Opacity, Volume };

struct ClipKeyEditPreview {
    bool success = false;
    ClipEffects effects;
    std::int64_t frame = 0;
    std::string error;
};

// originalFrame が負なら追加。ドラッグ表示と確定の両方がこの候補計算を使う。
ClipKeyEditPreview previewClipKeyEdit(const Project& project, const std::string& clipId,
                                      ClipKeyKind kind, std::int64_t originalFrame,
                                      std::int64_t requestedFrame, double requestedPercent);
TimelineEditResult editClipKey(Project& project, const std::string& clipId, ClipKeyKind kind,
                               std::int64_t originalFrame, std::int64_t requestedFrame,
                               double requestedPercent);
// frame のキーを消す。最後のキーを消すと線は effects の基準値へ戻る。
TimelineEditResult deleteClipKey(Project& project, const std::string& clipId, ClipKeyKind kind,
                                 std::int64_t frame);

enum class TrimEdge { Left, Right };

// リンクされた clip (video / audio の 1 組) の相手にも同じ編集を適用するか。
// Premiere の「リンクされた選択」に相当する。既定の操作は Linked で、
// Alt を押しながらの操作は Single (操作した clip だけ) になる。
enum class LinkMode { Linked, Single };

TimelineFrameResult sourceBoundaryToTimelineBoundary(std::int64_t sourceFrame,
                                                     std::int64_t sourceFpsNum,
                                                     std::int64_t sourceFpsDen,
                                                     std::int64_t timelineFpsNum,
                                                     std::int64_t timelineFpsDen);
TimelineFrameResult timelineBoundaryToSourceBoundary(std::int64_t timelineFrame,
                                                     std::int64_t sourceFpsNum,
                                                     std::int64_t sourceFpsDen,
                                                     std::int64_t timelineFpsNum,
                                                     std::int64_t timelineFpsDen);
// clip の実効 fps (素材 fps × 速度、約分済み)。速度 s の clip は素材 fps が f s の clip と
// 同じに扱えるので、timeline との換算はすべてこれを使う。実際の素材 fps を使ってよいのは
// decode の検証と素材 frame の秒換算だけである。積を int64 で表せなければ nullopt。
std::optional<core::FrameRate> clipTimebase(const TimelineClip& clip);
// clipTimebase を使う境界換算。素材境界 -> timeline は ceil、逆は floor。
TimelineFrameResult clipSourceBoundaryToTimeline(const TimelineClip& clip, std::int64_t sourceFrame,
                                                 std::int64_t timelineFpsNum,
                                                 std::int64_t timelineFpsDen);
TimelineFrameResult clipTimelineBoundaryToSource(const TimelineClip& clip,
                                                 std::int64_t timelineFrame,
                                                 std::int64_t timelineFpsNum,
                                                 std::int64_t timelineFpsDen);
TimelineFrameResult timelineClipDuration(const Project& project, const TimelineClip& clip);
// clip 先頭から clipLocalFrame 番目の timeline frame が表示する素材 frame (素材の絶対 frame)。
// core::sourceFrameAtOutputPosition の四捨五入で、書き出し (MLT) と同じ frame を返す。
// 丸めで sourceOutFrame (trim で外した次の frame) を返すことがあるのも MLT と同じである。
// 素材の末尾を越える分だけは最終 frame に止める。
TimelineFrameResult clipSourceFrameAt(const TimelineClip& clip, std::int64_t timelineFpsNum,
                                      std::int64_t timelineFpsDen, std::int64_t clipLocalFrame);

// 書き出しの producer に渡す cut の範囲。位置は素材の 0 frame から数えた output 位置
// (core/source_frame_mapping.h) で、[begin, end) が clipSourceFrameAt と同じ frame を出す。
// 素材の末尾で、丸めると存在しない frame を指す位置は cut に含めない。その分の
// tailFrames は最終 frame を繰り返して埋める (clipSourceFrameAt の最終 frame への止め方と同じ)。
struct ClipProducerRange {
    bool success = false;
    std::int64_t begin = 0;
    std::int64_t end = 0;
    std::int64_t tailFrames = 0;
    std::string error;
};

ClipProducerRange clipProducerRange(const TimelineClip& clip, std::int64_t timelineFpsNum,
                                    std::int64_t timelineFpsDen);
// フェードと音量カーブの評価に渡す素材 local frame (clipSourceFrameAt - in)。
// 丸めで素材範囲の外を指す分は [0, out - in) に収める。
TimelineFrameResult clipFadeSourceFrameAt(const TimelineClip& clip, std::int64_t timelineFpsNum,
                                          std::int64_t timelineFpsDen, std::int64_t clipLocalFrame);
bool sourceRateMatchesTimelineRate(const Project& project, const TimelineClip& clip);
TimelineValidationResult validateTimeline(const Project& project);

int timelineClipIndexAt(const Project& project, TrackRef track, std::int64_t timelineFrame);
const TimelineClip* activeClipAt(const Project& project, TrackRef track,
                                 std::int64_t timelineFrame);
// index が track index と一致する配列を返す。clip が無い track は nullptr。
// 合成順は index 昇順で bottom -> top。
std::vector<const TimelineClip*> activeClipsAt(const Project& project, TrackKind kind,
                                               std::int64_t timelineFrame);
TimelineFrameResult timelineEndFrame(const Project& project);
TimelineFrameResult timelineTrackEndFrame(const Project& project, TrackRef track);

TimelineEditResult moveClip(Project& project, const std::string& clipId, TrackRef destinationTrack,
                            std::int64_t newStartFrame);
// anchor clip の移動量を選択 clip 全体へ適用する。Linked ならリンク相手も同じ時間差で
// 移動する。全変更を一つの candidate として検証する。
TimelineEditResult moveClips(Project& project, const std::vector<std::string>& clipIds,
                             const std::string& anchorClipId, TrackRef destinationTrack,
                             std::int64_t newStartFrame, LinkMode linkMode);
// track 末尾へ追加する。clip.track と clip.timelineStartFrame はここで確定させる。
TimelineEditResult appendTimelineClip(Project& project, TimelineClip clip, TrackRef track);
// 指定位置へ配置する。既存 clip と重なる場合は fail-closed にする。
TimelineEditResult placeTimelineClipAt(Project& project, TimelineClip clip, TrackRef track,
                                       std::int64_t timelineStartFrame);
// 再生ヘッドで最上位の active clip より上の非 mute 映像 track に文字を置く。
// V1～V3 に空きが無ければ Project を変えずに失敗する。
TimelineEditResult placeTextClipAt(Project& project, TimelineClip clip,
                                   std::int64_t timelineStartFrame);
// link済みvideo/audioを一つのcandidateへ追加し、Project invariantを満たした状態だけをcommitする。
TimelineEditResult placeLinkedAvPairAt(Project& project, TimelineClip video, TrackRef videoTrack,
                                       TimelineClip audio, TrackRef audioTrack,
                                       std::int64_t timelineStartFrame);
TimelineEditResult deleteTimelineClip(Project& project, int selectedIndex);
TimelineEditResult unlinkTimelineClip(Project& project, const std::string& clipId);
// clip の端のドラッグの種類。動かせる範囲の決め方が違う。
//   Trim   : 素材の範囲と 1 frame 以上の尺。left 端は timeline 先頭より前へ出さない
//   Ripple : Trim と同じだが、left 端でも clip の開始位置は動かない
//   Roll   : 操作した clip と接している clip の両方で素材の範囲と 1 frame 以上の尺
enum class EdgeEditKind { Trim, Ripple, Roll };

// 端のドラッグ量 (project frame) を、動かせる範囲で止めた値にする。Linked ならリンク相手も
// 同じ量を動かせる範囲に止める。Premiere と同じく、素材の端を越えるドラッグは失敗させずに
// 端で止める。trim / リップル / ローリングの確定と drag 中の表示の両方がこれを使う。
TimelineFrameResult clampEdgeEdit(const Project& project, const std::string& clipId, TrimEdge edge,
                                  EdgeEditKind kind, std::int64_t projectFrameDelta,
                                  LinkMode linkMode);

// Linked ならリンク相手の同じ側の端も同じ量だけ動かす。動かせる範囲を越える量は
// clampEdgeEdit で止め、1 frame も動かせなければ失敗する。
TimelineEditResult trimTimelineClip(Project& project, const std::string& clipId, TrimEdge edge,
                                    std::int64_t projectFrameDelta, LinkMode linkMode);

// --- レート調整 (Premiere の Rate Stretch) --------------------------------
// 端をドラッグして、素材範囲 (in/out) を変えずに速度を変えて尺を伸縮する。反対側の端は動かさず、
// リップルも上書きもしない。Linked ならリンク相手にも同じ速度を適用し、同じ側の端を動かす
// (相手の速度がもともと違えば失敗する)。
//
// 尺 D にしたい clip の速度は s = (out - in) R / D (R = timeline fps / 素材 fps) とする。
// このとき実効 fps は (out - in) timeline fps / D で、尺はちょうど D になる。

// 端のドラッグ量 (project frame) を、伸縮できる範囲で止めた値にする。止める条件は
// 速度の範囲 (kMin/kMaxClipSpeedPercent)、1 frame 以上の尺、同じ track の隣の clip、
// timeline 先頭で、Linked ならリンク相手の条件も含める。確定と drag 中の表示の両方が使う。
TimelineFrameResult clampRateEdit(const Project& project, const std::string& clipId, TrimEdge edge,
                                  std::int64_t projectFrameDelta, LinkMode linkMode);

// drag 中の表示。clampRateEdit で止めた量で伸縮した結果を、対象 clip (Linked ならリンク相手も)
// ごとに返す。確定 (rateStretchTimelineClip) と同じ candidate から作るので、リンク相手の
// 尺が操作した clip と違っても、表示と確定が食い違わない (相手は同じ速度で自分の尺になる)。
struct RateStretchPreviewClip {
    std::string clipId;
    std::int64_t timelineStartFrame = 0;
    std::int64_t durationFrames = 0;
    std::int64_t speedNum = 1;
    std::int64_t speedDen = 1;
    // 開始 / 終端が現在の位置から動く frame 数 (drag 中の表示用)。
    std::int64_t startDelta = 0;
    std::int64_t endDelta = 0;
};

struct RateStretchPreview {
    bool success = false;
    // 操作した clip の端を実際に動かす量 (clampRateEdit の結果)。
    std::int64_t appliedDelta = 0;
    std::vector<RateStretchPreviewClip> clips;
    std::string error;
};

RateStretchPreview previewRateStretch(const Project& project, const std::string& clipId,
                                      TrimEdge edge, std::int64_t projectFrameDelta,
                                      LinkMode linkMode);
// clampRateEdit で止めた量で伸縮する。1 frame も伸縮できなければ失敗する。
TimelineEditResult rateStretchTimelineClip(Project& project, const std::string& clipId,
                                           TrimEdge edge, std::int64_t projectFrameDelta,
                                           LinkMode linkMode);

// --- Premiere 風の編集ツール ---------------------------------------------
// いずれも candidate 全体を validateTimeline で検証し、失敗時は Project を変更しない。

// frame で clip を 2 つに分割する (レーザーツール)。frame は clip の内側 (start < frame < end)
// でなければならない。Linked でリンク相手も frame を内側に含むなら同時に分割し、右半分どうしを
// newId() で作った新しい link group で結ぶ。片方だけを切ったら右半分は未リンクになる。
// フェードは左半分が in、右半分が out を引き継ぐ。
// selectedIndex は最初に指定した clip の左半分を指す。
TimelineEditResult splitTimelineClips(Project& project, const std::vector<std::string>& clipIds,
                                      std::int64_t frame, const std::function<std::string()>& newId,
                                      LinkMode linkMode);
// frame を内側に含む clip (全 track)。Shift+クリックの全 track 分割に使う。
std::vector<std::string> clipIdsSpanningFrame(const Project& project, std::int64_t frame);

// リップルトリム。trim した尺の増減だけ、trim した clip の track で後ろにある clip
// (とそのリンク相手) をずらす。left 端を trim しても clip の開始位置は動かない。
// Linked ならリンク相手も同じ量 trim し、相手の track の後ろもずらす。
TimelineEditResult rippleTrimTimelineClip(Project& project, const std::string& clipId,
                                          TrimEdge edge, std::int64_t projectFrameDelta,
                                          LinkMode linkMode);
// ローリング編集。clip の edge と接している隣の clip の境界を一緒に動かし、
// 2 clip の合計尺を保つ。接している clip が無ければ失敗する。
// Linked ならリンク相手の編集点も動かす。相手が編集点を持たない (L/J カット) なら動かさない。
TimelineEditResult rollTimelineEdit(Project& project, const std::string& clipId, TrimEdge edge,
                                    std::int64_t projectFrameDelta, LinkMode linkMode);
// スリップ。timeline 上の位置と素材上の長さを保ったまま in/out を同じだけずらす。
// 素材の範囲を超える分は素材の端で止める。1 frame もずらせなければ失敗する。
// Linked ならリンク相手も同じ量ずらし、全員がずらせる範囲で止める。
TimelineEditResult slipTimelineClip(Project& project, const std::string& clipId,
                                    std::int64_t projectFrameDelta, LinkMode linkMode);
// スライド。clip を尺を変えずに横へ動かし、接している前後の clip の out / in を
// 追従させる。操作した clip に接している前後の clip が無ければ失敗する (単なる移動に
// しない)。Linked ならリンク相手も同じようにスライドし、相手に接している clip があれば
// それも追従させる。
TimelineEditResult slideTimelineClip(Project& project, const std::string& clipId,
                                     std::int64_t projectFrameDelta, LinkMode linkMode);
// スライド量を、前の clip の out・後ろの clip の in を動かせる範囲 (素材の範囲と 1 frame 以上の
// 尺)、timeline 先頭、接していない clip との空白に止めた値にする。Linked ならリンク相手の
// 制約も含める。操作した clip に前後の clip が無ければ失敗する。
// slideTimelineClip の確定と drag 中の表示の両方がこれを使う。
TimelineFrameResult clampSlideEdit(const Project& project, const std::string& clipId,
                                   std::int64_t projectFrameDelta, LinkMode linkMode);

enum class SelectDirection { Forward, Backward };

// トラックの選択ツール。Forward は frame 以降に掛かる clip (end > frame)、Backward は
// frame 以前に掛かる clip (start <= frame)。track を指定するとその track だけに絞る。
std::vector<std::string> clipIdsFromFrame(const Project& project, std::int64_t frame,
                                          SelectDirection direction,
                                          std::optional<TrackRef> track = std::nullopt);
TimelineEditResult appendManimTimelineClipAt(Project& project, const ManimAsset& asset,
                                             std::string clipId, std::int64_t sourceFpsNum,
                                             std::int64_t sourceFpsDen,
                                             std::int64_t sourceFrameCount,
                                             std::int64_t timelineStartFrame, TrackRef track);

// 解像度とtimeline fpsを一つのtransactionで変更する。既存clipのsource domainは
// 変更せず、timelineStartFrameだけをwall-clock位置が保たれるよう新fpsへ換算する。
TimelineEditResult setProjectVideoSettings(Project& project, int width, int height,
                                           std::int64_t fpsNum, std::int64_t fpsDen);

// fpsだけを変更する互換入口。変換規則はsetProjectVideoSettingsと同じ。
TimelineEditResult setTimelineFrameRate(Project& project, std::int64_t fpsNum, std::int64_t fpsDen);

// track 編集。clip が載っている track は削除させない (暗黙に clip を消さない)。
TimelineEditResult addTrack(Project& project, TrackKind kind);
TimelineEditResult removeTrack(Project& project, TrackRef track);
TimelineEditResult setTrackMuted(Project& project, TrackRef track, bool muted);

struct TimelineGap {
    bool found = false;
    std::int64_t start = 0;
    std::int64_t end = 0;
    std::string error;
};

// track 上の timelineFrame を含む「clip が無く、後ろに clip がある」区間を返す。
// 終端より後ろの空白は詰める対象が無いので found=false とする。
TimelineGap gapAt(const Project& project, TrackRef track, std::int64_t timelineFrame);
// gapAt が返す区間を閉じ、後続 clip を左へ詰める (ripple delete)。
TimelineEditResult rippleDeleteGap(Project& project, TrackRef track, std::int64_t timelineFrame);

} // namespace mvm::project

#endif // MVM_PROJECT_TIMELINE_EDIT_H

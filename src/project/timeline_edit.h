#ifndef MVM_PROJECT_TIMELINE_EDIT_H
#define MVM_PROJECT_TIMELINE_EDIT_H

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
TimelineFrameResult timelineClipDuration(const Project& project, const TimelineClip& clip);
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

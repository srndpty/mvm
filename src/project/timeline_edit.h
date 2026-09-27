#ifndef MVM_PROJECT_TIMELINE_EDIT_H
#define MVM_PROJECT_TIMELINE_EDIT_H

#include "project/project.h"

#include <cstdint>
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
// anchor clip の移動量を選択 clip 全体へ適用する。リンク相手も同じ時間差で移動し、
// 全変更を一つの candidate として検証する。
TimelineEditResult moveClips(Project& project, const std::vector<std::string>& clipIds,
                             const std::string& anchorClipId, TrackRef destinationTrack,
                             std::int64_t newStartFrame);
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
TimelineEditResult trimTimelineClip(Project& project, const std::string& clipId, TrimEdge edge,
                                    std::int64_t projectFrameDelta);
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

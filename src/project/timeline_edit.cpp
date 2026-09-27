#include "project/timeline_edit.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace mvm::project {
namespace {

bool validIndex(const Project& project, int index) {
    return index >= 0 && index < static_cast<int>(project.timelineClips.size());
}

bool checkedMultiply(std::uint64_t left, std::uint64_t right, std::uint64_t& result) {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
        return false;
    result = left * right;
    return true;
}

TimelineFrameResult convertBoundary(std::int64_t frame, std::int64_t fromNum, std::int64_t fromDen,
                                    std::int64_t toNum, std::int64_t toDen, bool roundUp) {
    TimelineFrameResult result;
    if (frame < 0 || fromNum <= 0 || fromDen <= 0 || toNum <= 0 || toDen <= 0) {
        result.error = "frame または timebase が不正です";
        return result;
    }

    std::uint64_t factors[3] = {static_cast<std::uint64_t>(frame),
                                static_cast<std::uint64_t>(toNum),
                                static_cast<std::uint64_t>(fromDen)};
    std::uint64_t divisors[2] = {static_cast<std::uint64_t>(toDen),
                                 static_cast<std::uint64_t>(fromNum)};
    for (auto& divisor : divisors) {
        for (auto& factor : factors) {
            const std::uint64_t common = std::gcd(factor, divisor);
            factor /= common;
            divisor /= common;
        }
    }

    std::uint64_t numerator = 1;
    for (const auto factor : factors) {
        if (!checkedMultiply(numerator, factor, numerator)) {
            result.error = "frame timebase 変換が overflow しました";
            return result;
        }
    }
    std::uint64_t denominator = 1;
    for (const auto divisor : divisors) {
        if (!checkedMultiply(denominator, divisor, denominator) || denominator == 0) {
            result.error = "frame timebase 変換が overflow しました";
            return result;
        }
    }
    std::uint64_t converted = numerator / denominator;
    if (roundUp && numerator % denominator != 0)
        ++converted;
    if (converted > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        result.error = "frame timebase 変換が int64 範囲外です";
        return result;
    }
    result.success = true;
    result.frame = static_cast<std::int64_t>(converted);
    return result;
}

int indexOfId(const Project& project, const std::string& id) {
    for (std::size_t index = 0; index < project.timelineClips.size(); ++index) {
        if (project.timelineClips[index].id == id)
            return static_cast<int>(index);
    }
    return -1;
}

struct TimelineInterval {
    TrackRef track;
    std::int64_t start = 0;
    std::int64_t end = 0;
    const TimelineClip* clip = nullptr;
};

// clip の timeline 区間 [start, end)。duration の失敗はそのまま返す。
bool clipInterval(const Project& project, const TimelineClip& clip, std::int64_t& start,
                  std::int64_t& end, std::string& error) {
    const auto duration = timelineClipDuration(project, clip);
    if (!duration.success) {
        error = clip.name + ": " + duration.error;
        return false;
    }
    if (clip.timelineStartFrame < 0 ||
        clip.timelineStartFrame > std::numeric_limits<std::int64_t>::max() - duration.frame) {
        error = "timeline duration が overflow しました";
        return false;
    }
    start = clip.timelineStartFrame;
    end = start + duration.frame;
    return true;
}

// clip の片端を project frame 単位で動かす。検証は呼び出し側が candidate 全体に対して行う。
// left 端を動かしても右端 (clip の終端) は timeline 上で動かない。
bool trimClipBoundary(const Project& project, TimelineClip& clip, TrimEdge edge,
                      std::int64_t projectFrameDelta, std::string& error) {
    const auto originalDuration = timelineClipDuration(project, clip);
    if (!originalDuration.success) {
        error = originalDuration.error;
        return false;
    }
    const std::int64_t originalStart = clip.timelineStartFrame;
    const std::int64_t original = edge == TrimEdge::Left ? clip.sourceInFrame : clip.sourceOutFrame;
    const auto timelineBoundary =
        sourceBoundaryToTimelineBoundary(original, clip.sourceFpsNum, clip.sourceFpsDen,
                                         project.timelineFpsNum, project.timelineFpsDen);
    if (!timelineBoundary.success ||
        (projectFrameDelta < 0 && timelineBoundary.frame < -projectFrameDelta) ||
        (projectFrameDelta > 0 &&
         timelineBoundary.frame > std::numeric_limits<std::int64_t>::max() - projectFrameDelta)) {
        error = timelineBoundary.success ? "trim delta が範囲外です" : timelineBoundary.error;
        return false;
    }
    const auto sourceBoundary = timelineBoundaryToSourceBoundary(
        timelineBoundary.frame + projectFrameDelta, clip.sourceFpsNum, clip.sourceFpsDen,
        project.timelineFpsNum, project.timelineFpsDen);
    if (!sourceBoundary.success) {
        error = sourceBoundary.error;
        return false;
    }
    if (edge == TrimEdge::Left) {
        clip.sourceInFrame = sourceBoundary.frame;
        const auto newDuration = timelineClipDuration(project, clip);
        if (!newDuration.success) {
            error = newDuration.error;
            return false;
        }
        const std::int64_t startDelta = originalDuration.frame - newDuration.frame;
        if ((startDelta < 0 && originalStart < -startDelta) ||
            (startDelta > 0 &&
             originalStart > std::numeric_limits<std::int64_t>::max() - startDelta)) {
            error = "left trim 後の timeline start が範囲外です";
            return false;
        }
        clip.timelineStartFrame = originalStart + startDelta;
    } else {
        clip.sourceOutFrame = sourceBoundary.frame;
    }
    return true;
}

} // namespace

TimelineFrameResult sourceBoundaryToTimelineBoundary(std::int64_t sourceFrame,
                                                     std::int64_t sourceFpsNum,
                                                     std::int64_t sourceFpsDen,
                                                     std::int64_t timelineFpsNum,
                                                     std::int64_t timelineFpsDen) {
    return convertBoundary(sourceFrame, sourceFpsNum, sourceFpsDen, timelineFpsNum, timelineFpsDen,
                           true);
}

TimelineFrameResult timelineBoundaryToSourceBoundary(std::int64_t timelineFrame,
                                                     std::int64_t sourceFpsNum,
                                                     std::int64_t sourceFpsDen,
                                                     std::int64_t timelineFpsNum,
                                                     std::int64_t timelineFpsDen) {
    return convertBoundary(timelineFrame, timelineFpsNum, timelineFpsDen, sourceFpsNum,
                           sourceFpsDen, false);
}

TimelineFrameResult timelineClipDuration(const Project& project, const TimelineClip& clip) {
    const auto begin =
        sourceBoundaryToTimelineBoundary(clip.sourceInFrame, clip.sourceFpsNum, clip.sourceFpsDen,
                                         project.timelineFpsNum, project.timelineFpsDen);
    if (!begin.success)
        return begin;
    const auto end =
        sourceBoundaryToTimelineBoundary(clip.sourceOutFrame, clip.sourceFpsNum, clip.sourceFpsDen,
                                         project.timelineFpsNum, project.timelineFpsDen);
    if (!end.success)
        return end;
    TimelineFrameResult result;
    if (end.frame <= begin.frame) {
        result.error = "clip の timeline duration が 1 frame 未満です";
        return result;
    }
    result.success = true;
    result.frame = end.frame - begin.frame;
    return result;
}

bool sourceRateMatchesTimelineRate(const Project& project, const TimelineClip& clip) {
    if (clip.sourceFpsNum <= 0 || clip.sourceFpsDen <= 0 || project.timelineFpsNum <= 0 ||
        project.timelineFpsDen <= 0)
        return false;
    const auto sourceDivisor = std::gcd(clip.sourceFpsNum, clip.sourceFpsDen);
    const auto timelineDivisor = std::gcd(project.timelineFpsNum, project.timelineFpsDen);
    return clip.sourceFpsNum / sourceDivisor == project.timelineFpsNum / timelineDivisor &&
           clip.sourceFpsDen / sourceDivisor == project.timelineFpsDen / timelineDivisor;
}

TimelineValidationResult validateTimeline(const Project& project) {
    TimelineValidationResult result;
    if (project.schemaVersion != kProjectSchemaVersion) {
        result.error = "Project schema_version が " + std::to_string(kProjectSchemaVersion) +
                       " ではありません";
        return result;
    }
    if (!isConfigurableTimelineFrameRate(project.timelineFpsNum, project.timelineFpsDen)) {
        result.error = "Project timeline FPS が対応外です";
        return result;
    }
    // 永続化された fps は約分済みの 1 つの表現だけを authority にする。
    // 120/2 と 60/1 が両方存在すると、UI の一致判定も比較も二重定義になる。
    if (!isCanonicalFrameRate(project.timelineFpsNum, project.timelineFpsDen)) {
        result.error = "Project timeline FPS が約分されていません";
        return result;
    }
    if (!isValidProjectOutputSize(project.outputWidth, project.outputHeight)) {
        result.error = "Project output size が不正です";
        return result;
    }
    if (project.videoTracks.empty()) {
        result.error = "video track が 1 本もありません";
        return result;
    }
    std::unordered_set<std::string> trackNames;
    for (const auto kind : {TrackKind::Video, TrackKind::Audio}) {
        for (const auto& track : tracksOfKind(project, kind)) {
            if (track.name.empty() || !trackNames.insert(track.name).second) {
                result.error = "track 名が空または重複しています";
                return result;
            }
        }
    }
    std::unordered_set<std::string> ids;

    struct LinkGroupSummary {
        int count = 0;
        bool hasVideo = false;
        bool hasAudio = false;
    };

    std::unordered_map<std::string, LinkGroupSummary> linkGroups;
    std::vector<TimelineInterval> intervals;
    intervals.reserve(project.timelineClips.size());
    std::int64_t totalEnd = 0;
    for (const auto& clip : project.timelineClips) {
        if (clip.id.empty() || !ids.insert(clip.id).second) {
            result.error = "timeline clip ID が空または重複しています";
            return result;
        }
        if (clip.mediaPath.empty() || clip.name.empty()) {
            result.error = "timeline clip の path または name が空です";
            return result;
        }
        if (clip.sourceFpsNum <= 0 || clip.sourceFpsDen <= 0 || clip.sourceFrameCount <= 0 ||
            clip.sourceInFrame < 0 || clip.sourceOutFrame <= clip.sourceInFrame ||
            clip.sourceOutFrame > clip.sourceFrameCount) {
            result.error = "timeline clip の source range または FPS が不正です: " + clip.name;
            return result;
        }
        std::string effectsError;
        if (!validateClipEffects(clip.effects, clip.sourceOutFrame - clip.sourceInFrame,
                                 effectsError)) {
            result.error = clip.name + ": " + effectsError;
            return result;
        }
        if (!isValidTrackRef(project, clip.track)) {
            result.error = "timeline clip の track が存在しません: " + clip.name;
            return result;
        }
        if (!clipKindFitsTrackKind(clip.kind, clip.track.kind)) {
            result.error = "timeline clip の種別と track 種別が一致しません: " + clip.name;
            return result;
        }
        if (clip.timelineStartFrame < 0) {
            result.error = "timeline clip の start frame が負です: " + clip.name;
            return result;
        }
        if (!clip.linkGroupId.empty()) {
            auto& group = linkGroups[clip.linkGroupId];
            ++group.count;
            group.hasAudio = group.hasAudio || clip.kind == TimelineClipKind::Audio;
            group.hasVideo = group.hasVideo || clip.kind != TimelineClipKind::Audio;
            if (group.count > 2 || (group.count == 2 && (!group.hasAudio || !group.hasVideo))) {
                result.error =
                    "clipリンクはvideo/audioの1組である必要があります: " + clip.linkGroupId;
                return result;
            }
        }
        std::int64_t start = 0;
        std::int64_t end = 0;
        if (!clipInterval(project, clip, start, end, result.error))
            return result;
        for (const auto& interval : intervals) {
            if (interval.track == clip.track && start < interval.end && interval.start < end) {
                result.error =
                    "同じ track の timeline clip が重複しています: " + interval.clip->name + " / " +
                    clip.name;
                return result;
            }
        }
        intervals.push_back({clip.track, start, end, &clip});
        totalEnd = std::max(totalEnd, end);
    }
    for (const auto& [linkGroupId, group] : linkGroups) {
        if (group.count != 2 || !group.hasAudio || !group.hasVideo) {
            result.error = "clipリンクはvideo/audioの1組である必要があります: " + linkGroupId;
            return result;
        }
    }
    result.success = true;
    result.totalFrames = totalEnd;
    return result;
}

int timelineClipIndexAt(const Project& project, TrackRef track, std::int64_t timelineFrame) {
    const TimelineClip* clip = activeClipAt(project, track, timelineFrame);
    return clip ? static_cast<int>(clip - project.timelineClips.data()) : -1;
}

const TimelineClip* activeClipAt(const Project& project, TrackRef track,
                                 std::int64_t timelineFrame) {
    if (!isValidTrackRef(project, track) || timelineFrame < 0)
        return nullptr;
    for (const auto& clip : project.timelineClips) {
        if (!(clip.track == track))
            continue;
        std::int64_t start = 0;
        std::int64_t end = 0;
        std::string ignored;
        if (!clipInterval(project, clip, start, end, ignored))
            continue;
        if (timelineFrame >= start && timelineFrame < end)
            return &clip;
    }
    return nullptr;
}

std::vector<const TimelineClip*> activeClipsAt(const Project& project, TrackKind kind,
                                               std::int64_t timelineFrame) {
    const auto& tracks = tracksOfKind(project, kind);
    std::vector<const TimelineClip*> active(tracks.size(), nullptr);
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        active[index] =
            activeClipAt(project, TrackRef{kind, static_cast<int>(index)}, timelineFrame);
    }
    return active;
}

TimelineFrameResult timelineEndFrame(const Project& project) {
    const auto validation = validateTimeline(project);
    return {validation.success, validation.totalFrames, validation.error};
}

TimelineFrameResult timelineTrackEndFrame(const Project& project, TrackRef track) {
    TimelineFrameResult result;
    if (!isValidTrackRef(project, track)) {
        result.error = "track が存在しません";
        return result;
    }
    std::int64_t end = 0;
    for (const auto& clip : project.timelineClips) {
        if (!(clip.track == track))
            continue;
        std::int64_t start = 0;
        std::int64_t clipEnd = 0;
        if (!clipInterval(project, clip, start, clipEnd, result.error))
            return result;
        end = std::max(end, clipEnd);
    }
    result.success = true;
    result.frame = end;
    return result;
}

TimelineEditResult moveClip(Project& project, const std::string& clipId, TrackRef destinationTrack,
                            std::int64_t newStartFrame) {
    return moveClips(project, {clipId}, clipId, destinationTrack, newStartFrame, LinkMode::Linked);
}

TimelineEditResult moveClips(Project& project, const std::vector<std::string>& clipIds,
                             const std::string& anchorClipId, TrackRef destinationTrack,
                             std::int64_t newStartFrame, LinkMode linkMode) {
    TimelineEditResult result;
    if (clipIds.empty() || !isValidTrackRef(project, destinationTrack) || newStartFrame < 0) {
        result.error = "timeline clip の移動先 track または start frame が不正です";
        return result;
    }
    Project candidate = project;
    const int index = indexOfId(candidate, anchorClipId);
    if (!validIndex(candidate, index)) {
        result.error = "移動する timeline clip がありません";
        return result;
    }
    const auto& anchor = candidate.timelineClips[static_cast<std::size_t>(index)];
    if (!clipKindFitsTrackKind(anchor.kind, destinationTrack.kind)) {
        result.error = "この clip はその種別の track へ移動できません";
        return result;
    }

    std::unordered_set<std::string> movedIds;
    std::unordered_set<std::string> linkGroups;
    for (const auto& clipId : clipIds) {
        const int selectedIndex = indexOfId(candidate, clipId);
        if (!validIndex(candidate, selectedIndex)) {
            result.error = "移動する timeline clip がありません";
            return result;
        }
        const auto& selected = candidate.timelineClips[static_cast<std::size_t>(selectedIndex)];
        movedIds.insert(selected.id);
        if (linkMode == LinkMode::Linked && !selected.linkGroupId.empty())
            linkGroups.insert(selected.linkGroupId);
    }
    if (!movedIds.contains(anchorClipId)) {
        result.error = "anchor clip が選択に含まれていません";
        return result;
    }
    for (const auto& clip : candidate.timelineClips) {
        if (!clip.linkGroupId.empty() && linkGroups.contains(clip.linkGroupId))
            movedIds.insert(clip.id);
    }

    std::int64_t minimumStartFrame = std::numeric_limits<std::int64_t>::max();
    for (const auto& clip : candidate.timelineClips) {
        if (movedIds.contains(clip.id))
            minimumStartFrame = std::min(minimumStartFrame, clip.timelineStartFrame);
    }

    const std::int64_t oldStartFrame = anchor.timelineStartFrame;
    const std::int64_t requestedDelta = newStartFrame - oldStartFrame;
    // anchorだけを0へ丸めると、より左にある選択clipやリンク相手が負になる。
    // グループ全体の最左端が0に接する位置で止め、全clipへ同じdeltaを適用する。
    const std::int64_t delta = requestedDelta < 0 && minimumStartFrame < -requestedDelta
                                   ? -minimumStartFrame
                                   : requestedDelta;
    const int trackDelta = destinationTrack.index - anchor.track.index;
    for (auto& clip : candidate.timelineClips) {
        if (!movedIds.contains(clip.id))
            continue;
        if (delta > 0 &&
            clip.timelineStartFrame > std::numeric_limits<std::int64_t>::max() - delta) {
            result.error = "選択clipの移動先が範囲外です";
            return result;
        }
        clip.timelineStartFrame += delta;
        if (clip.track.kind == destinationTrack.kind) {
            const int destinationIndex = clip.track.index + trackDelta;
            const TrackRef translatedTrack{clip.track.kind, destinationIndex};
            if (!isValidTrackRef(candidate, translatedTrack)) {
                result.error = "選択clipの移動先trackが範囲外です";
                return result;
            }
            clip.track = translatedTrack;
        }
    }
    const auto validation = validateTimeline(candidate);
    if (!validation.success) {
        result.error = validation.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    result.selectedIndex = index;
    return result;
}

TimelineEditResult appendTimelineClip(Project& project, TimelineClip clip, TrackRef track) {
    TimelineEditResult result;
    if (!isValidTrackRef(project, track)) {
        result.error = "追加先の track が存在しません";
        return result;
    }
    if (!clipKindFitsTrackKind(clip.kind, track.kind)) {
        result.error = "clip 種別と track 種別が一致しません";
        return result;
    }
    const auto start = timelineTrackEndFrame(project, track);
    if (!start.success) {
        result.error = start.error;
        return result;
    }
    Project candidate = project;
    clip.track = track;
    clip.timelineStartFrame = start.frame;
    candidate.timelineClips.push_back(std::move(clip));
    const auto validation = validateTimeline(candidate);
    if (!validation.success) {
        result.error = validation.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    result.selectedIndex = static_cast<int>(project.timelineClips.size()) - 1;
    return result;
}

TimelineEditResult deleteTimelineClip(Project& project, int selectedIndex) {
    TimelineEditResult result;
    if (!validIndex(project, selectedIndex)) {
        result.error = "削除する timeline clip がありません";
        return result;
    }
    Project candidate = project;
    const std::string linkGroupId =
        candidate.timelineClips[static_cast<std::size_t>(selectedIndex)].linkGroupId;
    if (linkGroupId.empty()) {
        candidate.timelineClips.erase(candidate.timelineClips.begin() + selectedIndex);
    } else {
        std::erase_if(candidate.timelineClips,
                      [&](const TimelineClip& clip) { return clip.linkGroupId == linkGroupId; });
    }
    const auto valid = validateTimeline(candidate);
    if (!valid.success) {
        result.error = valid.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    if (!project.timelineClips.empty())
        result.selectedIndex =
            std::min(selectedIndex, static_cast<int>(project.timelineClips.size()) - 1);
    return result;
}

TimelineEditResult placeTimelineClipAt(Project& project, TimelineClip clip, TrackRef track,
                                       std::int64_t timelineStartFrame) {
    TimelineEditResult result;
    if (!isValidTrackRef(project, track) || timelineStartFrame < 0) {
        result.error = "追加先の track または start frame が不正です";
        return result;
    }
    if (!clipKindFitsTrackKind(clip.kind, track.kind)) {
        result.error = "clip 種別と track 種別が一致しません";
        return result;
    }
    Project candidate = project;
    clip.track = track;
    clip.timelineStartFrame = timelineStartFrame;
    candidate.timelineClips.push_back(std::move(clip));
    const auto validation = validateTimeline(candidate);
    if (!validation.success) {
        result.error = validation.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    result.selectedIndex = static_cast<int>(project.timelineClips.size()) - 1;
    return result;
}

TimelineEditResult placeLinkedAvPairAt(Project& project, TimelineClip video, TrackRef videoTrack,
                                       TimelineClip audio, TrackRef audioTrack,
                                       std::int64_t timelineStartFrame) {
    TimelineEditResult result;
    if (!isValidTrackRef(project, videoTrack) || !isValidTrackRef(project, audioTrack) ||
        timelineStartFrame < 0) {
        result.error = "リンクclipの追加先trackまたはstart frameが不正です";
        return result;
    }
    if (video.kind == TimelineClipKind::Audio || audio.kind != TimelineClipKind::Audio ||
        videoTrack.kind != TrackKind::Video || audioTrack.kind != TrackKind::Audio ||
        video.linkGroupId.empty() || video.linkGroupId != audio.linkGroupId) {
        result.error = "リンクclipは同じlink IDを持つvideo/audioの1組である必要があります";
        return result;
    }

    Project candidate = project;
    video.track = videoTrack;
    video.timelineStartFrame = timelineStartFrame;
    audio.track = audioTrack;
    audio.timelineStartFrame = timelineStartFrame;
    const int videoIndex = static_cast<int>(candidate.timelineClips.size());
    candidate.timelineClips.push_back(std::move(video));
    candidate.timelineClips.push_back(std::move(audio));
    const auto validation = validateTimeline(candidate);
    if (!validation.success) {
        result.error = validation.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    result.selectedIndex = videoIndex;
    return result;
}

TimelineEditResult unlinkTimelineClip(Project& project, const std::string& clipId) {
    TimelineEditResult result;
    Project candidate = project;
    const int index = indexOfId(candidate, clipId);
    if (!validIndex(candidate, index)) {
        result.error = "リンク解除する timeline clip がありません";
        return result;
    }
    const std::string linkGroupId =
        candidate.timelineClips[static_cast<std::size_t>(index)].linkGroupId;
    if (linkGroupId.empty()) {
        result.error = "選択した clip はリンクされていません";
        return result;
    }
    for (auto& clip : candidate.timelineClips) {
        if (clip.linkGroupId == linkGroupId)
            clip.linkGroupId.clear();
    }
    const auto valid = validateTimeline(candidate);
    if (!valid.success) {
        result.error = valid.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    result.selectedIndex = index;
    return result;
}

namespace {

// 編集の対象になる clip。clip 自身を先頭に置き、Linked ならリンク相手を後ろへ足す。
std::vector<int> editTargets(const Project& project, int index, LinkMode linkMode) {
    std::vector<int> targets{index};
    const std::string& group = project.timelineClips[static_cast<std::size_t>(index)].linkGroupId;
    if (linkMode == LinkMode::Single || group.empty())
        return targets;
    for (std::size_t other = 0; other < project.timelineClips.size(); ++other) {
        if (static_cast<int>(other) != index && project.timelineClips[other].linkGroupId == group)
            targets.push_back(static_cast<int>(other));
    }
    return targets;
}

bool isTarget(const std::vector<int>& targets, std::size_t index) {
    return std::find(targets.begin(), targets.end(), static_cast<int>(index)) != targets.end();
}

// clip の edge と接している同じ track の clip。見つからなければ -1。
int adjacentClipIndex(const Project& project, int index, TrimEdge edge, std::string& error) {
    const auto& clip = project.timelineClips[static_cast<std::size_t>(index)];
    std::int64_t start = 0;
    std::int64_t end = 0;
    if (!clipInterval(project, clip, start, end, error))
        return -1;
    for (std::size_t other = 0; other < project.timelineClips.size(); ++other) {
        if (static_cast<int>(other) == index)
            continue;
        const auto& candidate = project.timelineClips[other];
        if (!(candidate.track == clip.track))
            continue;
        std::int64_t otherStart = 0;
        std::int64_t otherEnd = 0;
        if (!clipInterval(project, candidate, otherStart, otherEnd, error))
            return -1;
        if ((edge == TrimEdge::Right && otherStart == end) ||
            (edge == TrimEdge::Left && otherEnd == start))
            return static_cast<int>(other);
    }
    return -1;
}

// marked に印を付けた clip のリンク相手にも印を付ける。link は横移動を同期する契約である。
void includeLinkedCounterparts(const Project& project, std::vector<bool>& marked) {
    std::unordered_set<std::string> linkGroups;
    for (std::size_t index = 0; index < marked.size(); ++index) {
        const auto& clip = project.timelineClips[index];
        if (marked[index] && !clip.linkGroupId.empty())
            linkGroups.insert(clip.linkGroupId);
    }
    for (std::size_t index = 0; index < marked.size(); ++index) {
        const auto& clip = project.timelineClips[index];
        if (!clip.linkGroupId.empty() && linkGroups.contains(clip.linkGroupId))
            marked[index] = true;
    }
}

bool shiftStart(TimelineClip& clip, std::int64_t delta, std::string& error) {
    if ((delta < 0 && clip.timelineStartFrame < -delta) ||
        (delta > 0 && clip.timelineStartFrame > std::numeric_limits<std::int64_t>::max() - delta)) {
        error = "clip の移動先が範囲外です: " + clip.name;
        return false;
    }
    clip.timelineStartFrame += delta;
    return true;
}

TimelineEditResult commitCandidate(Project& project, Project candidate, int selectedIndex) {
    TimelineEditResult result;
    const auto valid = validateTimeline(candidate);
    if (!valid.success) {
        result.error = valid.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    result.selectedIndex = selectedIndex;
    return result;
}

// outgoing (edge=Right なら clip 自身) と incoming の境界を動かす。
// 接している clip が無ければ neighborFound=false で何もしない。
bool rollEditPoint(Project& candidate, int index, TrimEdge edge, std::int64_t projectFrameDelta,
                   bool& neighborFound, std::string& error) {
    const int neighbor = adjacentClipIndex(candidate, index, edge, error);
    neighborFound = neighbor >= 0;
    if (!neighborFound)
        return error.empty();
    const int outgoingIndex = edge == TrimEdge::Right ? index : neighbor;
    const int incomingIndex = edge == TrimEdge::Right ? neighbor : index;
    auto& outgoing = candidate.timelineClips[static_cast<std::size_t>(outgoingIndex)];
    auto& incoming = candidate.timelineClips[static_cast<std::size_t>(incomingIndex)];
    if (!trimClipBoundary(candidate, outgoing, TrimEdge::Right, projectFrameDelta, error) ||
        !trimClipBoundary(candidate, incoming, TrimEdge::Left, projectFrameDelta, error))
        return false;
    std::int64_t outgoingStart = 0;
    std::int64_t outgoingEnd = 0;
    std::int64_t incomingStart = 0;
    std::int64_t incomingEnd = 0;
    if (!clipInterval(candidate, outgoing, outgoingStart, outgoingEnd, error) ||
        !clipInterval(candidate, incoming, incomingStart, incomingEnd, error))
        return false;
    // 素材 fps が timeline と異なると frame 境界の丸めで隙間ができうる。
    // 合計尺を保つという契約を守れないので黙って受理しない。
    if (outgoingEnd != incomingStart) {
        error = "境界を素材 frame に揃えられないためローリング編集できません";
        return false;
    }
    return true;
}

// clip の in / out を timeline 上で動かせる範囲 [lower, upper] (project frame)。
bool slipRange(const Project& project, const TimelineClip& clip, std::int64_t& lower,
               std::int64_t& upper, std::string& error) {
    const auto in =
        sourceBoundaryToTimelineBoundary(clip.sourceInFrame, clip.sourceFpsNum, clip.sourceFpsDen,
                                         project.timelineFpsNum, project.timelineFpsDen);
    const auto out =
        sourceBoundaryToTimelineBoundary(clip.sourceOutFrame, clip.sourceFpsNum, clip.sourceFpsDen,
                                         project.timelineFpsNum, project.timelineFpsDen);
    const auto end = sourceBoundaryToTimelineBoundary(clip.sourceFrameCount, clip.sourceFpsNum,
                                                      clip.sourceFpsDen, project.timelineFpsNum,
                                                      project.timelineFpsDen);
    if (!in.success || !out.success || !end.success) {
        error = !in.success ? in.error : (!out.success ? out.error : end.error);
        return false;
    }
    lower = -in.frame;
    upper = end.frame - out.frame;
    return true;
}

// in / out を timeline 上で projectFrameDelta だけずらす。素材上の長さ (out - in) は保ち、
// 素材の範囲を超える分は端で止める。in が動いたら moved=true。
bool slipClipSource(const Project& project, TimelineClip& clip, std::int64_t projectFrameDelta,
                    bool& moved, std::string& error) {
    const auto inBoundary =
        sourceBoundaryToTimelineBoundary(clip.sourceInFrame, clip.sourceFpsNum, clip.sourceFpsDen,
                                         project.timelineFpsNum, project.timelineFpsDen);
    if (!inBoundary.success) {
        error = inBoundary.error;
        return false;
    }
    if (projectFrameDelta > 0 &&
        inBoundary.frame > std::numeric_limits<std::int64_t>::max() - projectFrameDelta) {
        error = "スリップ量が範囲外です";
        return false;
    }
    // 素材の先頭より前は存在しないので、timeline 境界の段階で 0 に丸めておく。
    const std::int64_t shiftedBoundary =
        projectFrameDelta < 0 && inBoundary.frame < -projectFrameDelta
            ? 0
            : inBoundary.frame + projectFrameDelta;
    const auto shiftedIn =
        timelineBoundaryToSourceBoundary(shiftedBoundary, clip.sourceFpsNum, clip.sourceFpsDen,
                                         project.timelineFpsNum, project.timelineFpsDen);
    if (!shiftedIn.success) {
        error = shiftedIn.error;
        return false;
    }
    // 素材 fps が timeline と異なる場合、timeline 上の尺は frame 境界の丸めで 1 frame
    // 変わりうる。重なりは validateTimeline が拒否する。
    const std::int64_t sourceLength = clip.sourceOutFrame - clip.sourceInFrame;
    const std::int64_t newIn =
        std::clamp<std::int64_t>(shiftedIn.frame, 0, clip.sourceFrameCount - sourceLength);
    moved = newIn != clip.sourceInFrame;
    clip.sourceInFrame = newIn;
    clip.sourceOutFrame = newIn + sourceLength;
    return true;
}

} // namespace

TimelineEditResult trimTimelineClip(Project& project, const std::string& clipId, TrimEdge edge,
                                    std::int64_t projectFrameDelta, LinkMode linkMode) {
    TimelineEditResult result;
    Project candidate = project;
    const int index = indexOfId(candidate, clipId);
    if (!validIndex(candidate, index)) {
        result.error = "trim する timeline clip がありません";
        return result;
    }
    for (const int target : editTargets(candidate, index, linkMode)) {
        if (!trimClipBoundary(candidate, candidate.timelineClips[static_cast<std::size_t>(target)],
                              edge, projectFrameDelta, result.error))
            return result;
    }
    return commitCandidate(project, std::move(candidate), index);
}

TimelineEditResult splitTimelineClips(Project& project, const std::vector<std::string>& clipIds,
                                      std::int64_t frame, const std::function<std::string()>& newId,
                                      LinkMode linkMode) {
    TimelineEditResult result;
    if (clipIds.empty() || !newId) {
        result.error = "分割する timeline clip がありません";
        return result;
    }
    Project candidate = project;
    const std::size_t originalCount = candidate.timelineClips.size();
    std::vector<bool> requested(originalCount, false);
    for (const auto& id : clipIds) {
        const int index = indexOfId(candidate, id);
        if (!validIndex(candidate, index)) {
            result.error = "分割する timeline clip がありません";
            return result;
        }
        const auto& clip = candidate.timelineClips[static_cast<std::size_t>(index)];
        std::int64_t start = 0;
        std::int64_t end = 0;
        if (!clipInterval(candidate, clip, start, end, result.error))
            return result;
        if (frame <= start || frame >= end) {
            result.error = "分割位置が clip の内側にありません: " + clip.name;
            return result;
        }
        requested[static_cast<std::size_t>(index)] = true;
    }
    // リンク相手は、Linked で、かつ分割位置を内側に含むときだけ一緒に切る。
    std::vector<bool> split = requested;
    if (linkMode == LinkMode::Linked)
        includeLinkedCounterparts(candidate, split);
    for (std::size_t index = 0; index < originalCount; ++index) {
        if (!split[index] || requested[index])
            continue;
        std::int64_t start = 0;
        std::int64_t end = 0;
        if (!clipInterval(candidate, candidate.timelineClips[index], start, end, result.error))
            return result;
        split[index] = frame > start && frame < end;
    }

    // link group の両方を切ったときだけ、右半分どうしを新しい group で結ぶ。
    // 片方だけを切った場合、右半分は相手を持たないので未リンクにする。
    std::unordered_map<std::string, int> splitPerGroup;
    for (std::size_t index = 0; index < originalCount; ++index) {
        const auto& clip = candidate.timelineClips[index];
        if (split[index] && !clip.linkGroupId.empty())
            ++splitPerGroup[clip.linkGroupId];
    }
    std::unordered_map<std::string, std::string> rightLinkGroups;
    for (const auto& [group, count] : splitPerGroup) {
        if (count == 2)
            rightLinkGroups[group] = newId();
    }

    for (std::size_t index = 0; index < originalCount; ++index) {
        if (!split[index])
            continue;
        TimelineClip left = candidate.timelineClips[index];
        TimelineClip right = left;
        std::int64_t start = 0;
        std::int64_t end = 0;
        if (!clipInterval(candidate, left, start, end, result.error) ||
            !trimClipBoundary(candidate, left, TrimEdge::Right, frame - end, result.error) ||
            !trimClipBoundary(candidate, right, TrimEdge::Left, frame - start, result.error))
            return result;
        // 両半分とも同じ timeline 境界から換算するので、素材上の境界は一致するはずである。
        if (left.sourceOutFrame != right.sourceInFrame) {
            result.error = "分割位置を素材 frame へ一意に換算できません: " + left.name;
            return result;
        }
        left.effects.fadeOutFrames = 0;
        left.effects.fadeInFrames =
            std::min(left.effects.fadeInFrames, left.sourceOutFrame - left.sourceInFrame);
        right.effects.fadeInFrames = 0;
        right.effects.fadeOutFrames =
            std::min(right.effects.fadeOutFrames, right.sourceOutFrame - right.sourceInFrame);
        right.id = newId();
        if (right.id.empty() || right.id == left.id) {
            result.error = "分割後の clip ID を作れません";
            return result;
        }
        if (!right.linkGroupId.empty()) {
            const auto found = rightLinkGroups.find(right.linkGroupId);
            right.linkGroupId = found == rightLinkGroups.end() ? std::string{} : found->second;
        }
        candidate.timelineClips[index] = std::move(left);
        candidate.timelineClips.push_back(std::move(right));
    }
    return commitCandidate(project, std::move(candidate), indexOfId(project, clipIds.front()));
}

std::vector<std::string> clipIdsSpanningFrame(const Project& project, std::int64_t frame) {
    std::vector<std::string> ids;
    for (const auto& clip : project.timelineClips) {
        std::int64_t start = 0;
        std::int64_t end = 0;
        std::string ignored;
        if (clipInterval(project, clip, start, end, ignored) && frame > start && frame < end)
            ids.push_back(clip.id);
    }
    return ids;
}

TimelineEditResult rippleTrimTimelineClip(Project& project, const std::string& clipId,
                                          TrimEdge edge, std::int64_t projectFrameDelta,
                                          LinkMode linkMode) {
    TimelineEditResult result;
    Project candidate = project;
    const int index = indexOfId(candidate, clipId);
    if (!validIndex(candidate, index)) {
        result.error = "リップルトリムする timeline clip がありません";
        return result;
    }
    const std::vector<int> targets = editTargets(candidate, index, linkMode);

    // trim した各 clip の track で、その clip の元の終端以降にある clip を後ろへ波及させる。
    struct RippleSource {
        TrackRef track;
        std::int64_t originalEnd = 0;
    };

    std::vector<RippleSource> sources;
    std::int64_t shift = 0;
    for (std::size_t order = 0; order < targets.size(); ++order) {
        auto& clip = candidate.timelineClips[static_cast<std::size_t>(targets[order])];
        std::int64_t originalStart = 0;
        std::int64_t originalEnd = 0;
        if (!clipInterval(candidate, clip, originalStart, originalEnd, result.error) ||
            !trimClipBoundary(candidate, clip, edge, projectFrameDelta, result.error))
            return result;
        // left 端を動かしても clip は元の開始位置に留め、尺の変化を後ろへ波及させる。
        clip.timelineStartFrame = originalStart;
        std::int64_t newStart = 0;
        std::int64_t newEnd = 0;
        if (!clipInterval(candidate, clip, newStart, newEnd, result.error))
            return result;
        // リンク相手と後ろのずれ方が違うと同期が崩れる。素材 fps の丸めで食い違うなら拒否する。
        if (order == 0)
            shift = newEnd - originalEnd;
        else if (newEnd - originalEnd != shift) {
            result.error = "リンク相手と尺の変化量が一致しないためリップルトリムできません";
            return result;
        }
        sources.push_back({clip.track, originalEnd});
    }

    std::vector<bool> shifted(candidate.timelineClips.size(), false);
    for (std::size_t other = 0; other < candidate.timelineClips.size(); ++other) {
        const auto& following = candidate.timelineClips[other];
        for (const auto& source : sources) {
            if (following.track == source.track &&
                following.timelineStartFrame >= source.originalEnd)
                shifted[other] = true;
        }
    }
    includeLinkedCounterparts(candidate, shifted);
    // trim した clip 自身は開始位置を保つ。Single のときのリンク相手も動かさない。
    for (std::size_t other = 0; other < shifted.size(); ++other) {
        const auto& clip = candidate.timelineClips[other];
        if (isTarget(targets, other) ||
            (!clip.linkGroupId.empty() &&
             clip.linkGroupId ==
                 candidate.timelineClips[static_cast<std::size_t>(index)].linkGroupId))
            shifted[other] = false;
    }
    for (std::size_t other = 0; other < shifted.size(); ++other) {
        if (shifted[other] && !shiftStart(candidate.timelineClips[other], shift, result.error))
            return result;
    }
    return commitCandidate(project, std::move(candidate), index);
}

TimelineEditResult rollTimelineEdit(Project& project, const std::string& clipId, TrimEdge edge,
                                    std::int64_t projectFrameDelta, LinkMode linkMode) {
    TimelineEditResult result;
    Project candidate = project;
    const int index = indexOfId(candidate, clipId);
    if (!validIndex(candidate, index)) {
        result.error = "ローリング編集する timeline clip がありません";
        return result;
    }
    const std::vector<int> targets = editTargets(candidate, index, linkMode);
    for (std::size_t order = 0; order < targets.size(); ++order) {
        bool neighborFound = false;
        if (!rollEditPoint(candidate, targets[order], edge, projectFrameDelta, neighborFound,
                           result.error))
            return result;
        // 操作した clip には接している clip が必要。リンク相手は L カット / J カットで
        // 編集点を持たないことがあり、その場合は相手の編集点を動かさない。
        if (order == 0 && !neighborFound) {
            result.error = "ローリング編集には接している隣の clip が必要です";
            return result;
        }
    }
    return commitCandidate(project, std::move(candidate), index);
}

TimelineEditResult slipTimelineClip(Project& project, const std::string& clipId,
                                    std::int64_t projectFrameDelta, LinkMode linkMode) {
    TimelineEditResult result;
    Project candidate = project;
    const int index = indexOfId(candidate, clipId);
    if (!validIndex(candidate, index)) {
        result.error = "スリップする timeline clip がありません";
        return result;
    }
    const std::vector<int> targets = editTargets(candidate, index, linkMode);
    // リンク相手と同じ量だけずらさないと同期が崩れる。全員がずらせる範囲へ先に丸める。
    std::int64_t lower = std::numeric_limits<std::int64_t>::min();
    std::int64_t upper = std::numeric_limits<std::int64_t>::max();
    for (const int target : targets) {
        std::int64_t clipLower = 0;
        std::int64_t clipUpper = 0;
        if (!slipRange(candidate, candidate.timelineClips[static_cast<std::size_t>(target)],
                       clipLower, clipUpper, result.error))
            return result;
        lower = std::max(lower, clipLower);
        upper = std::min(upper, clipUpper);
    }
    const std::int64_t delta = std::clamp(projectFrameDelta, std::min(lower, upper), upper);
    bool anyMoved = false;
    for (const int target : targets) {
        bool moved = false;
        if (!slipClipSource(candidate, candidate.timelineClips[static_cast<std::size_t>(target)],
                            delta, moved, result.error))
            return result;
        anyMoved = anyMoved || moved;
    }
    if (!anyMoved) {
        result.error = "素材の端に達しているためスリップできません";
        return result;
    }
    return commitCandidate(project, std::move(candidate), index);
}

TimelineEditResult slideTimelineClip(Project& project, const std::string& clipId,
                                     std::int64_t projectFrameDelta, LinkMode linkMode) {
    TimelineEditResult result;
    Project candidate = project;
    const int index = indexOfId(candidate, clipId);
    if (!validIndex(candidate, index)) {
        result.error = "スライドする timeline clip がありません";
        return result;
    }
    if (projectFrameDelta == 0) {
        result.error = "スライド量が 0 です";
        return result;
    }
    const std::vector<int> targets = editTargets(candidate, index, linkMode);
    std::vector<bool> slid(candidate.timelineClips.size(), false);
    for (const int target : targets)
        slid[static_cast<std::size_t>(target)] = true;

    // 隣接関係は移動前の配置で決める。全部決めてから書き換える。
    struct Neighbors {
        int previous = -1;
        int next = -1;
    };

    std::vector<Neighbors> neighbors(candidate.timelineClips.size());
    for (std::size_t slide = 0; slide < slid.size(); ++slide) {
        if (!slid[slide])
            continue;
        neighbors[slide].previous =
            adjacentClipIndex(candidate, static_cast<int>(slide), TrimEdge::Left, result.error);
        if (!result.error.empty())
            return result;
        neighbors[slide].next =
            adjacentClipIndex(candidate, static_cast<int>(slide), TrimEdge::Right, result.error);
        if (!result.error.empty())
            return result;
    }
    for (std::size_t slide = 0; slide < slid.size(); ++slide) {
        if (!slid[slide])
            continue;
        if (!shiftStart(candidate.timelineClips[slide], projectFrameDelta, result.error))
            return result;
        const int previous = neighbors[slide].previous;
        if (previous >= 0 && !slid[static_cast<std::size_t>(previous)] &&
            !trimClipBoundary(candidate,
                              candidate.timelineClips[static_cast<std::size_t>(previous)],
                              TrimEdge::Right, projectFrameDelta, result.error))
            return result;
        const int next = neighbors[slide].next;
        if (next >= 0 && !slid[static_cast<std::size_t>(next)] &&
            !trimClipBoundary(candidate, candidate.timelineClips[static_cast<std::size_t>(next)],
                              TrimEdge::Left, projectFrameDelta, result.error))
            return result;
    }
    return commitCandidate(project, std::move(candidate), index);
}

std::vector<std::string> clipIdsFromFrame(const Project& project, std::int64_t frame,
                                          SelectDirection direction,
                                          std::optional<TrackRef> track) {
    std::vector<std::string> ids;
    for (const auto& clip : project.timelineClips) {
        if (track && !(clip.track == *track))
            continue;
        std::int64_t start = 0;
        std::int64_t end = 0;
        std::string ignored;
        if (!clipInterval(project, clip, start, end, ignored))
            continue;
        if (direction == SelectDirection::Forward ? end > frame : start <= frame)
            ids.push_back(clip.id);
    }
    return ids;
}

TimelineEditResult appendManimTimelineClipAt(Project& project, const ManimAsset& asset,
                                             std::string clipId, std::int64_t sourceFpsNum,
                                             std::int64_t sourceFpsDen,
                                             std::int64_t sourceFrameCount,
                                             std::int64_t timelineStartFrame, TrackRef track) {
    TimelineEditResult result;
    if (asset.generatedVideoPath.empty() || asset.sceneName.empty() || clipId.empty()) {
        result.error = "timeline に配置できる生成済み Manim asset ではありません";
        return result;
    }
    if (!isValidTrackRef(project, track) || track.kind != TrackKind::Video) {
        result.error = "Manim clip の配置先 video track が存在しません";
        return result;
    }
    const auto existing =
        std::find_if(project.timelineClips.begin(), project.timelineClips.end(),
                     [](const auto& clip) { return clip.kind == TimelineClipKind::Manim; });
    if (existing != project.timelineClips.end()) {
        result.error = "Manim asset はすでに timeline に配置されています";
        return result;
    }
    Project candidate = project;
    candidate.timelineClips.push_back({TimelineClipKind::Manim,
                                       asset.generatedVideoPath,
                                       asset.sceneName,
                                       std::move(clipId),
                                       sourceFpsNum,
                                       sourceFpsDen,
                                       sourceFrameCount,
                                       0,
                                       sourceFrameCount,
                                       timelineStartFrame,
                                       {},
                                       track,
                                       {}});
    const auto valid = validateTimeline(candidate);
    if (!valid.success) {
        result.error = valid.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    result.selectedIndex = static_cast<int>(project.timelineClips.size()) - 1;
    return result;
}

TimelineEditResult setTimelineFrameRate(Project& project, std::int64_t fpsNum,
                                        std::int64_t fpsDen) {
    return setProjectVideoSettings(project, project.outputWidth, project.outputHeight, fpsNum,
                                   fpsDen);
}

TimelineEditResult setProjectVideoSettings(Project& project, int width, int height,
                                           std::int64_t fpsNum, std::int64_t fpsDen) {
    TimelineEditResult result;
    if (!isValidProjectOutputSize(width, height)) {
        result.error = "Project output size が不正です";
        return result;
    }
    if (!isConfigurableTimelineFrameRate(fpsNum, fpsDen) || !isCanonicalFrameRate(fpsNum, fpsDen)) {
        result.error = "対応していない timeline frame rate です";
        return result;
    }
    if (project.outputWidth == width && project.outputHeight == height &&
        project.timelineFpsNum == fpsNum && project.timelineFpsDen == fpsDen) {
        result.success = true;
        return result;
    }
    Project candidate = project;
    if (project.timelineFpsNum != fpsNum || project.timelineFpsDen != fpsDen) {
        for (auto& clip : candidate.timelineClips) {
            const auto converted =
                sourceBoundaryToTimelineBoundary(clip.timelineStartFrame, project.timelineFpsNum,
                                                 project.timelineFpsDen, fpsNum, fpsDen);
            if (!converted.success) {
                result.error =
                    "clip開始位置を新しいtimeline frame rateへ変換できません: " + clip.name;
                return result;
            }
            clip.timelineStartFrame = converted.frame;
        }
    }
    candidate.outputWidth = width;
    candidate.outputHeight = height;
    candidate.timelineFpsNum = fpsNum;
    candidate.timelineFpsDen = fpsDen;
    const auto valid = validateTimeline(candidate);
    if (!valid.success) {
        result.error = "Project設定変更後のtimelineが不正です: " + valid.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    return result;
}

TimelineEditResult addTrack(Project& project, TrackKind kind) {
    TimelineEditResult result;
    Project candidate = project;
    auto& tracks = tracksOfKind(candidate, kind);
    const int index = static_cast<int>(tracks.size());
    tracks.push_back(Track{defaultTrackName(kind, index), false});
    const auto valid = validateTimeline(candidate);
    if (!valid.success) {
        result.error = valid.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    result.selectedIndex = index;
    return result;
}

TimelineEditResult removeTrack(Project& project, TrackRef track) {
    TimelineEditResult result;
    if (!isValidTrackRef(project, track)) {
        result.error = "削除する track が存在しません";
        return result;
    }
    if (track.kind == TrackKind::Video && project.videoTracks.size() <= 1) {
        result.error = "video track は最低 1 本必要です";
        return result;
    }
    for (const auto& clip : project.timelineClips) {
        if (clip.track == track) {
            result.error = "clip が載っている track は削除できません: " + clip.name;
            return result;
        }
    }
    Project candidate = project;
    auto& tracks = tracksOfKind(candidate, track.kind);
    tracks.erase(tracks.begin() + track.index);
    // 削除位置より後ろの track index を詰め、既定名も振り直す。
    for (std::size_t index = 0; index < tracks.size(); ++index)
        tracks[index].name = defaultTrackName(track.kind, static_cast<int>(index));
    for (auto& clip : candidate.timelineClips) {
        if (clip.track.kind == track.kind && clip.track.index > track.index)
            --clip.track.index;
    }
    const auto valid = validateTimeline(candidate);
    if (!valid.success) {
        result.error = valid.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    result.selectedIndex = track.index;
    return result;
}

TimelineEditResult setTrackMuted(Project& project, TrackRef track, bool muted) {
    TimelineEditResult result;
    if (!isValidTrackRef(project, track)) {
        result.error = "track が存在しません";
        return result;
    }
    tracksOfKind(project, track.kind)[static_cast<std::size_t>(track.index)].muted = muted;
    result.success = true;
    result.selectedIndex = track.index;
    return result;
}

TimelineGap gapAt(const Project& project, TrackRef track, std::int64_t timelineFrame) {
    TimelineGap gap;
    if (!isValidTrackRef(project, track) || timelineFrame < 0) {
        gap.error = "track または frame が不正です";
        return gap;
    }
    std::int64_t gapStart = 0;
    std::int64_t gapEnd = std::numeric_limits<std::int64_t>::max();
    bool hasFollowing = false;
    for (const auto& clip : project.timelineClips) {
        if (!(clip.track == track))
            continue;
        std::int64_t start = 0;
        std::int64_t end = 0;
        if (!clipInterval(project, clip, start, end, gap.error))
            return gap;
        if (timelineFrame >= start && timelineFrame < end) {
            gap.error = "その位置には clip があります";
            return gap;
        }
        if (end <= timelineFrame)
            gapStart = std::max(gapStart, end);
        if (start > timelineFrame) {
            gapEnd = std::min(gapEnd, start);
            hasFollowing = true;
        }
    }
    if (!hasFollowing) {
        gap.error = "後続 clip が無いため詰める gap がありません";
        return gap;
    }
    gap.found = true;
    gap.start = gapStart;
    gap.end = gapEnd;
    return gap;
}

TimelineEditResult rippleDeleteGap(Project& project, TrackRef track, std::int64_t timelineFrame) {
    TimelineEditResult result;
    const TimelineGap gap = gapAt(project, track, timelineFrame);
    if (!gap.found) {
        result.error = gap.error;
        return result;
    }
    const std::int64_t shift = gap.end - gap.start;
    if (shift <= 0) {
        result.error = "詰める空白がありません";
        return result;
    }
    Project candidate = project;
    // ripple 対象を先に確定させる。link は横移動を同期する契約なので、対象 clip の
    // counterpart も同じ shift へ含めないと linked A/V の相対位置が壊れる。
    const std::size_t clipCount = candidate.timelineClips.size();
    std::vector<bool> shifted(clipCount, false);
    for (std::size_t index = 0; index < clipCount; ++index) {
        const auto& clip = candidate.timelineClips[index];
        shifted[index] = clip.track == track && clip.timelineStartFrame >= gap.end;
    }
    includeLinkedCounterparts(candidate, shifted);
    for (std::size_t index = 0; index < clipCount; ++index) {
        if (!shifted[index])
            continue;
        auto& clip = candidate.timelineClips[index];
        if (clip.timelineStartFrame < shift) {
            result.error = "リンクclipの移動先が範囲外です";
            return result;
        }
        clip.timelineStartFrame -= shift;
    }
    const auto valid = validateTimeline(candidate);
    if (!valid.success) {
        result.error = valid.error;
        return result;
    }
    project = std::move(candidate);
    result.success = true;
    return result;
}

} // namespace mvm::project

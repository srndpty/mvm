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
    if (project.schemaVersion != 3) {
        result.error = "Project schema_version が 3 ではありません";
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
    if (project.outputWidth <= 0 || project.outputHeight <= 0) {
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
    TimelineEditResult result;
    if (!isValidTrackRef(project, destinationTrack) || newStartFrame < 0) {
        result.error = "timeline clip の移動先 track または start frame が不正です";
        return result;
    }
    Project candidate = project;
    const int index = indexOfId(candidate, clipId);
    if (!validIndex(candidate, index)) {
        result.error = "移動する timeline clip がありません";
        return result;
    }
    auto& clip = candidate.timelineClips[static_cast<std::size_t>(index)];
    if (!clipKindFitsTrackKind(clip.kind, destinationTrack.kind)) {
        result.error = "この clip はその種別の track へ移動できません";
        return result;
    }
    const std::int64_t oldStartFrame = clip.timelineStartFrame;
    const std::string linkGroupId = clip.linkGroupId;
    clip.track = destinationTrack;
    clip.timelineStartFrame = newStartFrame;
    if (!linkGroupId.empty()) {
        const std::int64_t delta = newStartFrame - oldStartFrame;
        for (auto& linked : candidate.timelineClips) {
            if (linked.id == clipId || linked.linkGroupId != linkGroupId)
                continue;
            if ((delta < 0 && linked.timelineStartFrame < -delta) ||
                (delta > 0 &&
                 linked.timelineStartFrame > std::numeric_limits<std::int64_t>::max() - delta)) {
                result.error = "リンクclipの移動先が範囲外です";
                return result;
            }
            linked.timelineStartFrame += delta;
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

TimelineEditResult trimTimelineClip(Project& project, const std::string& clipId, TrimEdge edge,
                                    std::int64_t projectFrameDelta) {
    TimelineEditResult result;
    Project candidate = project;
    const int index = indexOfId(candidate, clipId);
    if (!validIndex(candidate, index)) {
        result.error = "trim する timeline clip がありません";
        return result;
    }
    auto& clip = candidate.timelineClips[static_cast<std::size_t>(index)];
    const auto originalDuration = timelineClipDuration(candidate, clip);
    if (!originalDuration.success) {
        result.error = originalDuration.error;
        return result;
    }
    const std::int64_t originalStart = clip.timelineStartFrame;
    const std::int64_t original = edge == TrimEdge::Left ? clip.sourceInFrame : clip.sourceOutFrame;
    const auto timelineBoundary =
        sourceBoundaryToTimelineBoundary(original, clip.sourceFpsNum, clip.sourceFpsDen,
                                         candidate.timelineFpsNum, candidate.timelineFpsDen);
    if (!timelineBoundary.success ||
        (projectFrameDelta < 0 && timelineBoundary.frame < -projectFrameDelta) ||
        (projectFrameDelta > 0 &&
         timelineBoundary.frame > std::numeric_limits<std::int64_t>::max() - projectFrameDelta)) {
        result.error =
            timelineBoundary.success ? "trim delta が範囲外です" : timelineBoundary.error;
        return result;
    }
    const auto sourceBoundary = timelineBoundaryToSourceBoundary(
        timelineBoundary.frame + projectFrameDelta, clip.sourceFpsNum, clip.sourceFpsDen,
        candidate.timelineFpsNum, candidate.timelineFpsDen);
    if (!sourceBoundary.success) {
        result.error = sourceBoundary.error;
        return result;
    }
    if (edge == TrimEdge::Left) {
        clip.sourceInFrame = sourceBoundary.frame;
        const auto newDuration = timelineClipDuration(candidate, clip);
        if (!newDuration.success) {
            result.error = newDuration.error;
            return result;
        }
        const std::int64_t startDelta = originalDuration.frame - newDuration.frame;
        if ((startDelta < 0 && originalStart < -startDelta) ||
            (startDelta > 0 &&
             originalStart > std::numeric_limits<std::int64_t>::max() - startDelta)) {
            result.error = "left trim 後の timeline start が範囲外です";
            return result;
        }
        clip.timelineStartFrame = originalStart + startDelta;
    } else {
        clip.sourceOutFrame = sourceBoundary.frame;
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
    TimelineEditResult result;
    if (!isConfigurableTimelineFrameRate(fpsNum, fpsDen) || !isCanonicalFrameRate(fpsNum, fpsDen)) {
        result.error = "対応していない timeline frame rate です";
        return result;
    }
    if (project.timelineFpsNum == fpsNum && project.timelineFpsDen == fpsDen) {
        result.success = true;
        return result;
    }
    if (!project.timelineClips.empty()) {
        result.error = "clip がある Project の frame rate は変更できません。"
                       "新規 Project を作ってから frame rate を選んでください";
        return result;
    }
    Project candidate = project;
    candidate.timelineFpsNum = fpsNum;
    candidate.timelineFpsDen = fpsDen;
    const auto valid = validateTimeline(candidate);
    if (!valid.success) {
        result.error = valid.error;
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
    for (auto& clip : candidate.timelineClips) {
        if (clip.track == track && clip.timelineStartFrame >= gap.end)
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

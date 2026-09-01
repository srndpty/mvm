#include "project/timeline_edit.h"

#include <algorithm>
#include <limits>
#include <numeric>
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

bool validVideoTrack(VideoTrack track) {
    return track == VideoTrack::V1 || track == VideoTrack::V2;
}

struct TimelineInterval {
    VideoTrack track = VideoTrack::V1;
    std::int64_t start = 0;
    std::int64_t end = 0;
    const TimelineClip* clip = nullptr;
};

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
    if (project.schemaVersion != 2) {
        result.error = "Project schema_version が 2 ではありません";
        return result;
    }
    if (project.timelineFpsNum <= 0 || project.timelineFpsDen <= 0) {
        result.error = "Project timeline FPS が不正です";
        return result;
    }
    std::unordered_set<std::string> ids;
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
        if (!validVideoTrack(clip.videoTrack)) {
            result.error = "timeline clip の video_track が不正です: " + clip.name;
            return result;
        }
        if (clip.timelineStartFrame < 0) {
            result.error = "timeline clip の start frame が負です: " + clip.name;
            return result;
        }
        const auto duration = timelineClipDuration(project, clip);
        if (!duration.success) {
            result.error = clip.name + ": " + duration.error;
            return result;
        }
        if (clip.timelineStartFrame > std::numeric_limits<std::int64_t>::max() - duration.frame) {
            result.error = "timeline duration が overflow しました";
            return result;
        }
        const std::int64_t end = clip.timelineStartFrame + duration.frame;
        for (const auto& interval : intervals) {
            if (interval.track == clip.videoTrack && clip.timelineStartFrame < interval.end &&
                interval.start < end) {
                result.error =
                    "同じ video track の timeline clip が重複しています: " + interval.clip->name +
                    " / " + clip.name;
                return result;
            }
        }
        intervals.push_back({clip.videoTrack, clip.timelineStartFrame, end, &clip});
        totalEnd = std::max(totalEnd, end);
    }
    result.success = true;
    result.totalFrames = totalEnd;
    return result;
}

TimelineValidationResult recomputeTimelineStarts(Project& project) {
    std::int64_t start = 0;
    for (auto& clip : project.timelineClips) {
        clip.timelineStartFrame = start;
        const auto duration = timelineClipDuration(project, clip);
        if (!duration.success)
            return {false, 0, clip.name + ": " + duration.error};
        if (start > std::numeric_limits<std::int64_t>::max() - duration.frame)
            return {false, 0, "timeline duration が overflow しました"};
        start += duration.frame;
    }
    return validateTimeline(project);
}

int timelineClipIndexAtFrame(const Project& project, std::int64_t timelineFrame) {
    if (timelineFrame < 0)
        return -1;
    for (std::size_t index = 0; index < project.timelineClips.size(); ++index) {
        const auto& clip = project.timelineClips[index];
        const auto duration = timelineClipDuration(project, clip);
        if (duration.success && timelineFrame >= clip.timelineStartFrame &&
            timelineFrame < clip.timelineStartFrame + duration.frame)
            return static_cast<int>(index);
    }
    return -1;
}

const TimelineClip* activeClipAt(const Project& project, VideoTrack track,
                                 std::int64_t timelineFrame) {
    if (!validVideoTrack(track) || timelineFrame < 0)
        return nullptr;
    for (const auto& clip : project.timelineClips) {
        if (clip.videoTrack != track)
            continue;
        const auto duration = timelineClipDuration(project, clip);
        if (!duration.success || clip.timelineStartFrame < 0 ||
            clip.timelineStartFrame > std::numeric_limits<std::int64_t>::max() - duration.frame) {
            continue;
        }
        if (timelineFrame >= clip.timelineStartFrame &&
            timelineFrame < clip.timelineStartFrame + duration.frame) {
            return &clip;
        }
    }
    return nullptr;
}

std::array<const TimelineClip*, 2> activeClipsAt(const Project& project,
                                                 std::int64_t timelineFrame) {
    return {activeClipAt(project, VideoTrack::V1, timelineFrame),
            activeClipAt(project, VideoTrack::V2, timelineFrame)};
}

TimelineFrameResult timelineEndFrame(const Project& project) {
    const auto validation = validateTimeline(project);
    return {validation.success, validation.totalFrames, validation.error};
}

TimelineFrameResult timelineTrackEndFrame(const Project& project, VideoTrack track) {
    TimelineFrameResult result;
    if (!validVideoTrack(track)) {
        result.error = "video track が不正です";
        return result;
    }
    std::int64_t end = 0;
    for (const auto& clip : project.timelineClips) {
        if (clip.videoTrack != track)
            continue;
        const auto duration = timelineClipDuration(project, clip);
        if (!duration.success) {
            result.error = clip.name + ": " + duration.error;
            return result;
        }
        if (clip.timelineStartFrame < 0 ||
            clip.timelineStartFrame > std::numeric_limits<std::int64_t>::max() - duration.frame) {
            result.error = "timeline duration が overflow しました";
            return result;
        }
        end = std::max(end, clip.timelineStartFrame + duration.frame);
    }
    result.success = true;
    result.frame = end;
    return result;
}

TimelineEditResult moveClip(Project& project, const std::string& clipId,
                            VideoTrack destinationTrack, std::int64_t newStartFrame) {
    TimelineEditResult result;
    if (!validVideoTrack(destinationTrack) || newStartFrame < 0) {
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
    clip.videoTrack = destinationTrack;
    clip.timelineStartFrame = newStartFrame;
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

TimelineEditResult appendVideoTimelineClip(Project& project, TimelineClip clip) {
    TimelineEditResult result;
    if (clip.kind != TimelineClipKind::Video) {
        result.error = "Add Video に指定された clip kind が不正です";
        return result;
    }
    const auto start = timelineTrackEndFrame(project, VideoTrack::V1);
    if (!start.success) {
        result.error = start.error;
        return result;
    }
    Project candidate = project;
    clip.videoTrack = VideoTrack::V1;
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

TimelineEditResult reorderTimelineClip(Project& project, const std::string& clipId,
                                       int destinationIndex) {
    TimelineEditResult result;
    const int sourceIndex = indexOfId(project, clipId);
    if (!validIndex(project, sourceIndex) || !validIndex(project, destinationIndex)) {
        result.error = "timeline clip の移動先が不正です";
        return result;
    }
    auto clip = std::move(project.timelineClips[static_cast<std::size_t>(sourceIndex)]);
    project.timelineClips.erase(project.timelineClips.begin() + sourceIndex);
    project.timelineClips.insert(project.timelineClips.begin() + destinationIndex, std::move(clip));
    const auto valid = recomputeTimelineStarts(project);
    if (!valid.success) {
        result.error = valid.error;
        return result;
    }
    result.success = true;
    result.selectedIndex = destinationIndex;
    return result;
}

TimelineEditResult moveTimelineClip(Project& project, int selectedIndex, int offset) {
    if (!validIndex(project, selectedIndex))
        return {false, -1, "移動する timeline clip がありません"};
    if (offset != -1 && offset != 1)
        return {false, -1, "timeline clip の移動量が不正です"};
    const int destination = selectedIndex + offset;
    if (!validIndex(project, destination))
        return {false, -1, "timeline clip をこれ以上移動できません"};
    return reorderTimelineClip(
        project, project.timelineClips[static_cast<std::size_t>(selectedIndex)].id, destination);
}

TimelineEditResult deleteTimelineClip(Project& project, int selectedIndex) {
    TimelineEditResult result;
    if (!validIndex(project, selectedIndex)) {
        result.error = "削除する timeline clip がありません";
        return result;
    }
    Project candidate = project;
    candidate.timelineClips.erase(candidate.timelineClips.begin() + selectedIndex);
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

namespace {

TimelineEditResult appendManimClipOnTrack(Project& project, const ManimAsset& asset,
                                          std::string clipId, std::int64_t sourceFpsNum,
                                          std::int64_t sourceFpsDen, std::int64_t sourceFrameCount,
                                          std::int64_t timelineStartFrame, VideoTrack track) {
    TimelineEditResult result;
    if (asset.generatedVideoPath.empty() || asset.sceneName.empty() || clipId.empty()) {
        result.error = "timeline に配置できる生成済み Manim asset ではありません";
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
                                       track});
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

} // namespace

TimelineEditResult appendManimTimelineClip(Project& project, const ManimAsset& asset,
                                           std::string clipId, std::int64_t sourceFpsNum,
                                           std::int64_t sourceFpsDen,
                                           std::int64_t sourceFrameCount) {
    const auto start = timelineTrackEndFrame(project, VideoTrack::V1);
    if (!start.success)
        return {false, -1, start.error};
    return appendManimClipOnTrack(project, asset, std::move(clipId), sourceFpsNum, sourceFpsDen,
                                  sourceFrameCount, start.frame, VideoTrack::V1);
}

TimelineEditResult appendManimTimelineClipAt(Project& project, const ManimAsset& asset,
                                             std::string clipId, std::int64_t sourceFpsNum,
                                             std::int64_t sourceFpsDen,
                                             std::int64_t sourceFrameCount,
                                             std::int64_t timelineStartFrame) {
    return appendManimClipOnTrack(project, asset, std::move(clipId), sourceFpsNum, sourceFpsDen,
                                  sourceFrameCount, timelineStartFrame, VideoTrack::V2);
}

} // namespace mvm::project

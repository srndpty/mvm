#include "app/timeline_playback.h"

#include "project/timeline_edit.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <optional>

namespace mvm::app {
namespace {

bool checkedMultiply(std::uint64_t left, std::uint64_t right, std::uint64_t& result) {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
        return false;
    result = left * right;
    return true;
}

} // namespace

PlaybackFrameResult timelineFrameFromElapsed(std::int64_t baseFrame,
                                             std::int64_t elapsedNanoseconds,
                                             std::int64_t timelineFpsNum,
                                             std::int64_t timelineFpsDen) {
    PlaybackFrameResult result;
    if (baseFrame < 0 || elapsedNanoseconds < 0 || timelineFpsNum <= 0 || timelineFpsDen <= 0) {
        result.error = "再生clockのframeまたはtimebaseが不正です";
        return result;
    }

    std::uint64_t factors[2] = {static_cast<std::uint64_t>(elapsedNanoseconds),
                                static_cast<std::uint64_t>(timelineFpsNum)};
    std::uint64_t divisors[2] = {1'000'000'000ULL, static_cast<std::uint64_t>(timelineFpsDen)};
    for (auto& divisor : divisors) {
        for (auto& factor : factors) {
            const std::uint64_t common = std::gcd(factor, divisor);
            factor /= common;
            divisor /= common;
        }
    }

    std::uint64_t numerator = 0;
    std::uint64_t denominator = 0;
    if (!checkedMultiply(factors[0], factors[1], numerator) ||
        !checkedMultiply(divisors[0], divisors[1], denominator) || denominator == 0) {
        result.error = "再生clockのframe変換がoverflowしました";
        return result;
    }
    const std::uint64_t advanced = numerator / denominator;
    if (advanced > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        baseFrame >
            std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(advanced)) {
        result.error = "再生clockのframe変換がint64範囲外です";
        return result;
    }
    result.success = true;
    result.frame = baseFrame + static_cast<std::int64_t>(advanced);
    return result;
}

PlaybackFrameResult timelineShuttleFrameFromElapsed(std::int64_t baseFrame,
                                                    std::int64_t elapsedNanoseconds,
                                                    std::int64_t timelineFpsNum,
                                                    std::int64_t timelineFpsDen, int rate,
                                                    std::int64_t lastFrame) {
    PlaybackFrameResult result;
    if (rate == 0 || rate < -16 || rate > 16 || (std::abs(rate) & (std::abs(rate) - 1)) != 0 ||
        baseFrame < 0 || lastFrame < 0 || baseFrame > lastFrame || timelineFpsNum <= 0 ||
        timelineFpsNum > std::numeric_limits<std::int64_t>::max() / std::abs(rate)) {
        result.error = "シャトルの速度またはframe範囲が不正です";
        return result;
    }
    const auto advanced = timelineFrameFromElapsed(0, elapsedNanoseconds,
                                                   timelineFpsNum * std::abs(rate), timelineFpsDen);
    if (!advanced.success)
        return advanced;
    result.success = true;
    result.frame = rate > 0 ? baseFrame + std::min(advanced.frame, lastFrame - baseFrame)
                            : baseFrame - std::min(advanced.frame, baseFrame);
    return result;
}

PlaybackFrameResult adjacentTimelineEditPoint(const project::Project& project,
                                              std::int64_t playheadFrame, int direction,
                                              std::int64_t lastFrame) {
    PlaybackFrameResult result;
    if ((direction != -1 && direction != 1) || playheadFrame < 0 || lastFrame < 0 ||
        playheadFrame > lastFrame) {
        result.error = "編集点の探索方向またはframe範囲が不正です";
        return result;
    }
    std::optional<std::int64_t> nearest;
    for (const auto& clip : project.timelineClips) {
        const auto duration = project::timelineClipDuration(project, clip);
        if (!duration.success || duration.frame <= 0 || clip.timelineStartFrame < 0 ||
            clip.timelineStartFrame > std::numeric_limits<std::int64_t>::max() - duration.frame) {
            result.error = duration.success ? "clipの編集点が不正です" : duration.error;
            return result;
        }
        const std::int64_t edges[2] = {
            clip.timelineStartFrame, std::min(clip.timelineStartFrame + duration.frame, lastFrame)};
        for (const auto edge : edges) {
            if (direction > 0 && edge > playheadFrame && (!nearest || edge < *nearest))
                nearest = edge;
            if (direction < 0 && edge < playheadFrame && (!nearest || edge > *nearest))
                nearest = edge;
        }
    }
    for (const auto marker : project.timelineMarkers) {
        if (marker < 0 || marker > lastFrame) {
            result.error = "マーカーの編集点が不正です";
            return result;
        }
        if (direction > 0 && marker > playheadFrame && (!nearest || marker < *nearest))
            nearest = marker;
        if (direction < 0 && marker < playheadFrame && (!nearest || marker > *nearest))
            nearest = marker;
    }
    if (!nearest) {
        result.error = direction > 0 ? "次の編集点はありません" : "前の編集点はありません";
        return result;
    }
    result.success = true;
    result.frame = *nearest;
    return result;
}

std::optional<int> nextShuttleRate(int currentRate, int direction) {
    if ((direction != -1 && direction != 1) || currentRate < -16 || currentRate > 16 ||
        (currentRate != 0 && (std::abs(currentRate) & (std::abs(currentRate) - 1)) != 0))
        return std::nullopt;
    if (currentRate == 0)
        return direction;
    if ((currentRate > 0) == (direction > 0))
        return direction * std::min(16, std::abs(currentRate) * 2);
    if (std::abs(currentRate) == 1)
        return 0;
    return (currentRate > 0 ? 1 : -1) * (std::abs(currentRate) / 2);
}

std::optional<std::int64_t> timelineShuttleSampleAt(std::int64_t baseSample, int rate,
                                                    std::int64_t outputSample) {
    if (baseSample < 0 || outputSample < 0 ||
        (rate != -4 && rate != -2 && rate != -1 && rate != 1 && rate != 2 && rate != 4) ||
        outputSample > std::numeric_limits<std::int64_t>::max() / std::abs(rate))
        return std::nullopt;
    const std::int64_t delta = outputSample * std::abs(rate);
    if (rate > 0) {
        if (baseSample > std::numeric_limits<std::int64_t>::max() - delta)
            return std::nullopt;
        return baseSample + delta;
    }
    if (baseSample < delta)
        return std::nullopt;
    return baseSample - delta;
}

TimelinePlaybackStep evaluateTimelinePlayback(const project::Project& project, int activeClipIndex,
                                              std::int64_t candidateFrame) {
    TimelinePlaybackStep result;
    if (activeClipIndex < 0 || activeClipIndex >= static_cast<int>(project.timelineClips.size()) ||
        candidateFrame < 0) {
        result.error = "再生中のtimeline clipまたはframeが不正です";
        return result;
    }

    const auto& active = project.timelineClips[static_cast<std::size_t>(activeClipIndex)];
    if (candidateFrame < active.timelineStartFrame) {
        result.error = "再生clockがactive clipの先頭より前です";
        return result;
    }
    const auto duration = project::timelineClipDuration(project, active);
    if (!duration.success ||
        active.timelineStartFrame > std::numeric_limits<std::int64_t>::max() - duration.frame) {
        result.error = duration.success ? "再生中clipの終端がoverflowしました" : duration.error;
        return result;
    }
    const std::int64_t clipEnd = active.timelineStartFrame + duration.frame;
    result.success = true;
    if (candidateFrame < clipEnd) {
        result.transition = TimelinePlaybackTransition::StayInClip;
        result.frame = candidateFrame;
        result.clipIndex = activeClipIndex;
        return result;
    }

    const int nextIndex = activeClipIndex + 1;
    if (nextIndex < static_cast<int>(project.timelineClips.size())) {
        result.transition = TimelinePlaybackTransition::SwitchClip;
        result.frame =
            project.timelineClips[static_cast<std::size_t>(nextIndex)].timelineStartFrame;
        result.clipIndex = nextIndex;
        return result;
    }

    result.transition = TimelinePlaybackTransition::Finished;
    result.frame = clipEnd;
    result.clipIndex = activeClipIndex;
    return result;
}

bool timelinePreviewCompatible(const project::Project& project) {
    for (const auto& clip : project.timelineClips) {
        if (!project::timelineClipDuration(project, clip).success)
            return false;
    }
    return true;
}

bool timelineCanPlay(const project::Project& project, bool busy, bool playing,
                     std::int64_t playheadFrame, std::int64_t totalTimelineFrames) {
    return !busy && !playing && !project.timelineClips.empty() && playheadFrame >= 0 &&
           playheadFrame < totalTimelineFrames && timelinePreviewCompatible(project);
}

} // namespace mvm::app

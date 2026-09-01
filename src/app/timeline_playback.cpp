#include "app/timeline_playback.h"

#include "project/timeline_edit.h"

#include <limits>
#include <numeric>

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
        if (!project::sourceRateMatchesTimelineRate(project, clip))
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

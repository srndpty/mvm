#include "shuttle_audio_mix.h"

#include "app/timeline_playback.h"
#include "app/timeline_preview_mapping.h"
#include "core/checked_output_timebase.h"
#include "media/audio_preview/audio_clock.h"
#include "project/timeline_edit.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace mvm::app {

bool isShuttleAudibleClip(const project::Project& project, const project::TimelineClip& clip) {
    return clip.enabled && clip.track.kind == project::TrackKind::Audio &&
           project::isTrackOutputEnabled(project, clip.track);
}

bool hasShuttleAudibleClip(const project::Project& project) {
    return std::any_of(
        project.timelineClips.begin(), project.timelineClips.end(),
        [&project](const auto& clip) { return isShuttleAudibleClip(project, clip); });
}

bool planShuttleAudio(const project::Project& project, int rate, std::int64_t baseFrame,
                      ShuttleAudioPlan& plan, std::string& error) {
    if (rate != -4 && rate != -2 && rate != -1 && rate != 1 && rate != 2 && rate != 4) {
        error = "シャトル音声の再生速度が不正です";
        return false;
    }
    const auto timebase = core::CheckedOutputTimebase::create(
        project.timelineFpsNum, project.timelineFpsDen, audio::kInternalSampleRate);
    if (!timebase) {
        error = "シャトル音声のtimebaseを作成できません";
        return false;
    }
    const auto timeline = project::validateTimeline(project);
    const auto base = timebase.value().seekTargetSample(baseFrame);
    const auto end = timeline.success ? timebase.value().seekTargetSample(timeline.totalFrames)
                                      : decltype(base){};
    if (!timeline.success || !base || !end || base.value() < 0 || end.value() <= 0 ||
        base.value() >= end.value()) {
        error = "シャトル音声のtimeline範囲が不正です";
        return false;
    }
    ShuttleAudioPlan next;
    next.baseSample = base.value();
    next.endSample = end.value();
    next.rate = rate;
    next.timelineFpsNum = project.timelineFpsNum;
    next.timelineFpsDen = project.timelineFpsDen;
    std::vector<project::TimelineRenderSegment> segments;
    if (!project::timelineRenderSegments(project, project::TrackKind::Audio, segments, error))
        return false;
    for (const auto& segment : segments) {
        const auto& clip = segment.clip;
        if (!isShuttleAudibleClip(project, segment.original))
            continue;
        const auto duration = project::timelineClipDuration(project, clip);
        if (!duration.success) {
            error = duration.error;
            return false;
        }
        const auto startSample = timebase.value().seekTargetSample(clip.timelineStartFrame);
        const auto endSample =
            timebase.value().seekTargetSample(clip.timelineStartFrame + duration.frame);
        const auto offset = audioPreviewSampleOffset(project, clip);
        if (!startSample || !endSample || !offset.success) {
            error = "シャトル音声のclip位置を換算できません";
            return false;
        }
        const auto utf8Path = clip.mediaPath.u8string();
        next.clips.push_back(
            {std::string(reinterpret_cast<const char*>(utf8Path.data()), utf8Path.size()),
             startSample.value(), endSample.value(), offset.sampleOffset, clip, segment});
    }
    plan = std::move(next);
    return true;
}

bool mixShuttleBlock(const ShuttleAudioPlan& plan, std::int64_t outputStart,
                     std::int64_t sampleCount, const ShuttleSourceReader& read,
                     std::vector<float>& pcm, std::string& error) {
    // 64bit では SIZE_MAX / channels が int64 の最大値を超え上限にならないので、
    // vector が実際に持てる要素数で判定する。
    if (sampleCount <= 0 || static_cast<std::uint64_t>(sampleCount) >
                                pcm.max_size() / static_cast<std::size_t>(audio::kInternalChannels)) {
        error = "シャトル音声のblock sample数が不正です";
        return false;
    }
    pcm.assign(static_cast<std::size_t>(sampleCount) * audio::kInternalChannels, 0.0F);
    const auto timebase = core::CheckedOutputTimebase::create(
        plan.timelineFpsNum, plan.timelineFpsDen, audio::kInternalSampleRate);
    if (!timebase) {
        error = "シャトル音声の音量カーブを換算できません";
        return false;
    }
    for (std::size_t clipIndex = 0; clipIndex < plan.clips.size(); ++clipIndex) {
        const auto& clip = plan.clips[clipIndex];
        // 出力 i に対応する素材 sample。clip の外なら無し。
        const auto sourceAt = [&](std::int64_t i) -> std::optional<std::int64_t> {
            const auto timelineSample =
                timelineShuttleSampleAt(plan.baseSample, plan.rate, outputStart + i);
            if (!timelineSample || *timelineSample < clip.timelineStartSample ||
                *timelineSample >= clip.timelineEndSample || *timelineSample >= plan.endSample)
                return std::nullopt;
            return *timelineSample + clip.sourceOffset;
        };
        std::int64_t first = std::numeric_limits<std::int64_t>::max();
        std::int64_t last = -1;
        for (std::int64_t i = 0; i < sampleCount; ++i) {
            const auto sourceSample = sourceAt(i);
            if (!sourceSample)
                continue;
            if (*sourceSample < 0) {
                error = "シャトル音声の素材sample位置が負です";
                return false;
            }
            first = std::min(first, *sourceSample);
            last = std::max(last, *sourceSample);
        }
        if (last < 0)
            continue;
        std::vector<float> source;
        if (!read(clipIndex, first, last - first + 1, source, error))
            return false;
        if (source.size() !=
            static_cast<std::size_t>(last - first + 1) * audio::kInternalChannels) {
            error = "シャトル音声の素材sample数が要求と一致しません";
            return false;
        }
        for (std::int64_t i = 0; i < sampleCount; ++i) {
            const auto sourceSample = sourceAt(i);
            if (!sourceSample)
                continue;
            const auto index =
                static_cast<std::size_t>(*sourceSample - first) * audio::kInternalChannels;
            const auto output = static_cast<std::size_t>(i) * audio::kInternalChannels;
            const auto timelineSample = timelineShuttleSampleAt(plan.baseSample, plan.rate,
                                                                outputStart + i);
            if (!timelineSample) {
                error = "シャトル音声のtimeline sampleを換算できません";
                return false;
            }
            // frame へ換算できない位置を別の位置 (clip 先頭など) の gain で鳴らさない。
            const auto frame = timebase.value().schedulerOutputFrame(*timelineSample);
            if (!frame) {
                error = "シャトル音声のtimeline frameを換算できません";
                return false;
            }
            const auto evaluated = project::renderSegmentGain(
                clip.segment, plan.timelineFpsNum, plan.timelineFpsDen, frame.value());
            if (!evaluated) {
                error = "シャトル音声の音量カーブ位置が不正です";
                return false;
            }
            const float gain = static_cast<float>(*evaluated);
            pcm[output] += source[index] * gain;
            pcm[output + 1] += source[index + 1] * gain;
        }
    }
    for (auto& sample : pcm)
        sample = std::clamp(sample, -1.0F, 1.0F);
    return true;
}

} // namespace mvm::app

#include "shuttle_audio_playback.h"

#include "app/timeline_playback.h"
#include "app/timeline_preview_mapping.h"
#include "core/checked_output_timebase.h"
#include "project/timeline_edit.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace mvm::app {
namespace {
constexpr audio::SourceId kOutputSource{1};
constexpr audio::SourceGeneration kOutputGeneration{1};
constexpr audio::AudioResourceEpoch kOutputEpoch{1};
constexpr std::int64_t kBlockSamples = 12000;
} // namespace

ShuttleAudioPlayback::ShuttleAudioPlayback()
    : queue_(kOutputSource, kOutputGeneration), sink_(queue_, clock_) {}

ShuttleAudioPlayback::~ShuttleAudioPlayback() {
    stop();
}

bool ShuttleAudioPlayback::start(const project::Project& project, int rate, std::int64_t baseFrame,
                                 float volume, std::string& error) {
    if (running_ ||
        (rate != -4 && rate != -2 && rate != -1 && rate != 1 && rate != 2 && rate != 4)) {
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
    baseSample_ = base.value();
    endSample_ = end.value();
    rate_ = rate;
    for (const auto& clip : project.timelineClips) {
        if (clip.track.kind != project::TrackKind::Audio ||
            project.audioTracks[static_cast<std::size_t>(clip.track.index)].muted)
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
        clips_.push_back(
            {std::string(reinterpret_cast<const char*>(utf8Path.data()), utf8Path.size()),
             startSample.value(), endSample.value(), offset.sampleOffset, nullptr});
    }
    if (!sink_.open(error, volume))
        return false;
    running_ = true;
    producer_ = std::thread(&ShuttleAudioPlayback::produce, this);
    if (!sink_.play(0, kOutputGeneration, error)) {
        stop();
        return false;
    }
    return true;
}

void ShuttleAudioPlayback::stop() {
    running_ = false;
    queue_.stop();
    if (producer_.joinable())
        producer_.join();
    sink_.stop();
    clips_.clear();
}

std::int64_t ShuttleAudioPlayback::elapsedSamples() const {
    return clock_.snapshot().mediaSamplePosition;
}

std::string ShuttleAudioPlayback::error() const {
    std::lock_guard lock(errorMutex_);
    return error_;
}

bool ShuttleAudioPlayback::readSamples(Clip& clip, std::int64_t first, std::int64_t count,
                                       std::vector<float>& pcm, std::string& error) {
    if (!clip.worker) {
        clip.worker = std::make_unique<audio::AudioDecodeWorker>(
            audio::SourceId{static_cast<std::uint64_t>(&clip - clips_.data()) + 2});
        if (!clip.worker->start(clip.path, error))
            return false;
    }
    audio::SourceGeneration generation = clip.worker->queue().generation();
    if (clip.nextSample != first) {
        clip.worker->pause();
        audio::AudioSeekTicket ticket;
        if (clip.worker->requestSeek(first, ticket, error) !=
            audio::AudioSeekRequestResult::Accepted) {
            error = error.empty() ? "シャトル音声のseekを要求できません" : error;
            return false;
        }
        audio::AudioSeekCompletion completion;
        for (int attempts = 0; running_ && attempts < 25; ++attempts) {
            const auto result = clip.worker->waitSeek(ticket, 200, completion);
            if (result == audio::AudioSeekWaitResult::Ready)
                break;
            if (result != audio::AudioSeekWaitResult::Timeout) {
                error = "シャトル音声のseek結果が失効しました";
                return false;
            }
        }
        if (!running_)
            return false;
        if (!completion.completed) {
            error = completion.error.empty() ? "シャトル音声のseekがタイムアウトしました"
                                             : completion.error;
            return false;
        }
        generation = completion.seekGeneration;
    }
    clip.worker->play();
    pcm.assign(static_cast<std::size_t>(count) * audio::kInternalChannels, 0.0F);
    std::int64_t consumed = 0;
    int waits = 0;
    while (running_ && consumed < count) {
        const auto result = clip.worker->queue().consume(
            pcm.data() + consumed * audio::kInternalChannels, first + consumed,
            std::min<std::int64_t>(count - consumed, 2048), generation);
        consumed += result.audioSamples;
        if (consumed == count)
            break;
        if (result.shortageKind == audio::AudioShortageKind::TerminalEof)
            break;
        if (result.audioSamples > 0) {
            waits = 0;
            continue;
        }
        if (++waits > 25 || !clip.worker->queue().waitForSamples(1, 200)) {
            error = "シャトル音声のdecodeが必要なsampleを供給できません";
            return false;
        }
    }
    clip.nextSample = first + count;
    return running_;
}

bool ShuttleAudioPlayback::fillBlock(std::int64_t outputStart, std::vector<float>& pcm,
                                     std::string& error) {
    pcm.assign(static_cast<std::size_t>(kBlockSamples) * audio::kInternalChannels, 0.0F);
    for (auto& clip : clips_) {
        std::int64_t first = std::numeric_limits<std::int64_t>::max();
        std::int64_t last = -1;
        for (std::int64_t i = 0; i < kBlockSamples; ++i) {
            const auto timelineSample =
                timelineShuttleSampleAt(baseSample_, rate_, outputStart + i);
            if (!timelineSample || *timelineSample < clip.timelineStartSample ||
                *timelineSample >= clip.timelineEndSample || *timelineSample >= endSample_)
                continue;
            const auto sourceSample = *timelineSample + clip.sourceOffset;
            if (sourceSample < 0) {
                error = "シャトル音声の素材sample位置が負です";
                return false;
            }
            first = std::min(first, sourceSample);
            last = std::max(last, sourceSample);
        }
        if (last < 0)
            continue;
        if (rate_ > 0 && clip.nextSample >= 0 && first >= clip.nextSample &&
            first - clip.nextSample < rate_)
            first = clip.nextSample;
        std::vector<float> source;
        if (!readSamples(clip, first, last - first + 1, source, error))
            return false;
        for (std::int64_t i = 0; i < kBlockSamples; ++i) {
            const auto timelineSample =
                timelineShuttleSampleAt(baseSample_, rate_, outputStart + i);
            if (!timelineSample || *timelineSample < clip.timelineStartSample ||
                *timelineSample >= clip.timelineEndSample || *timelineSample >= endSample_)
                continue;
            const auto index =
                static_cast<std::size_t>(*timelineSample + clip.sourceOffset - first) *
                audio::kInternalChannels;
            const auto output = static_cast<std::size_t>(i) * audio::kInternalChannels;
            pcm[output] += source[index];
            pcm[output + 1] += source[index + 1];
        }
    }
    for (auto& sample : pcm)
        sample = std::clamp(sample, -1.0F, 1.0F);
    return true;
}

void ShuttleAudioPlayback::produce() {
    std::int64_t next = 0;
    while (running_) {
        if (!queue_.waitUntilBelow(audio::kQueueTargetSamples, 50) ||
            !queue_.waitForSpace(kBlockSamples, 50))
            continue;
        std::vector<float> pcm;
        std::string error;
        if (!fillBlock(next, pcm, error)) {
            if (running_) {
                std::lock_guard lock(errorMutex_);
                error_ = error.empty() ? "シャトル音声を生成できません" : error;
            }
            break;
        }
        std::uint64_t audible = 0;
        for (const float sample : pcm) {
            if (std::abs(sample) > 0.00001F)
                ++audible;
        }
        nonSilentSamples_.fetch_add(audible, std::memory_order_relaxed);
        audio::AudioChunk chunk;
        chunk.sourceId = kOutputSource;
        chunk.sourceGeneration = kOutputGeneration;
        chunk.resourceEpoch = kOutputEpoch;
        chunk.startSample = next;
        chunk.sampleCount = kBlockSamples;
        chunk.sampleRate = audio::kInternalSampleRate;
        chunk.channels = audio::kInternalChannels;
        chunk.pcm = std::make_shared<std::vector<float>>(std::move(pcm));
        if (queue_.push(std::move(chunk)) != audio::AudioQueuePushResult::Accepted)
            break;
        next += kBlockSamples;
    }
}

} // namespace mvm::app

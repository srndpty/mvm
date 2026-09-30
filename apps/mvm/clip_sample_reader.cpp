#include "clip_sample_reader.h"

#include <algorithm>

namespace mvm::app {
namespace {
// decode 待ちは短く区切って running を見る。scrub の release で stop が
// 待たされないようにするため。打ち切るまでの時間は区切る前と同じにする。
constexpr int kWaitPollMs = 20;
constexpr int kSeekWaitLimitMs = 5000;
constexpr int kSampleStallLimitMs = 200;
} // namespace

void ClipSampleReaders::reset(std::size_t clipCount, std::int64_t maxForwardSkip) {
    maxForwardSkip_ = maxForwardSkip;
    readers_.clear();
    readers_.resize(clipCount);
}

bool ClipSampleReaders::read(const ShuttleAudioClip& source, std::size_t clipIndex,
                             std::int64_t first, std::int64_t count, std::vector<float>& pcm,
                             const std::atomic<bool>& running, std::string& error) {
    if (clipIndex >= readers_.size()) {
        error = "音声clipの読み出し先がありません";
        return false;
    }
    auto& clip = readers_[clipIndex];
    if (!clip.worker) {
        clip.worker = std::make_unique<audio::AudioDecodeWorker>(
            audio::SourceId{static_cast<std::uint64_t>(clipIndex) + 2});
        // 素材 sample は通常再生と同じく速度で伸縮した時間軸で数える (sourceOffset も同じ)。
        const auto& timelineClip = source.clip;
        if (!clip.worker->setPlaybackSpeed(timelineClip.speedNum, timelineClip.speedDen,
                                           timelineClip.preservePitch, error) ||
            !clip.worker->start(source.path, error))
            return false;
    }
    // 前回の続きから近い要求は seek せずに続きから読み、要求より前の分を捨てる。
    const std::int64_t skipped =
        clip.nextSample >= 0 && first > clip.nextSample && first - clip.nextSample < maxForwardSkip_
            ? first - clip.nextSample
            : 0;
    first -= skipped;
    count += skipped;
    audio::SourceGeneration generation = clip.worker->queue().generation();
    if (clip.nextSample != first) {
        clip.worker->pause();
        audio::AudioSeekTicket ticket;
        if (clip.worker->requestSeek(first, ticket, error) !=
            audio::AudioSeekRequestResult::Accepted) {
            error = error.empty() ? "音声のseekを要求できません" : error;
            return false;
        }
        audio::AudioSeekCompletion completion;
        seekWaits_.fetch_add(1, std::memory_order_relaxed);
        for (int attempts = 0; running && attempts < kSeekWaitLimitMs / kWaitPollMs; ++attempts) {
            const auto result = clip.worker->waitSeek(ticket, kWaitPollMs, completion);
            if (result == audio::AudioSeekWaitResult::Ready)
                break;
            if (result != audio::AudioSeekWaitResult::Timeout) {
                error = "音声のseek結果が失効しました";
                return false;
            }
        }
        if (!running)
            return false;
        if (!completion.completed) {
            error =
                completion.error.empty() ? "音声のseekがタイムアウトしました" : completion.error;
            return false;
        }
        generation = completion.seekGeneration;
    }
    clip.worker->play();
    pcm.assign(static_cast<std::size_t>(count) * audio::kInternalChannels, 0.0F);
    std::int64_t consumed = 0;
    int waits = 0;
    while (running && consumed < count) {
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
        if (++waits > 25) {
            error = "音声のdecodeが必要なsampleを供給できません";
            return false;
        }
        int stalledMs = 0;
        while (running && !clip.worker->queue().waitForSamples(1, kWaitPollMs)) {
            stalledMs += kWaitPollMs;
            if (stalledMs >= kSampleStallLimitMs) {
                error = "音声のdecodeが必要なsampleを供給できません";
                return false;
            }
        }
    }
    clip.nextSample = first + count;
    pcm.erase(pcm.begin(), pcm.begin() + skipped * audio::kInternalChannels);
    return running;
}

} // namespace mvm::app

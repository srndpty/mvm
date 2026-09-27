#include "shuttle_audio_playback.h"

#include <algorithm>
#include <cmath>

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
    if (running_) {
        error = "シャトル音声はすでに再生中です";
        return false;
    }
    if (!planShuttleAudio(project, rate, baseFrame, plan_, error))
        return false;
    if (plan_.clips.empty()) {
        error = "シャトル音声で鳴らすaudio clipがありません";
        return false;
    }
    readers_.clear();
    readers_.resize(plan_.clips.size());
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
    readers_.clear();
}

std::int64_t ShuttleAudioPlayback::elapsedSamples() const {
    return clock_.snapshot().mediaSamplePosition;
}

std::string ShuttleAudioPlayback::error() const {
    std::lock_guard lock(errorMutex_);
    return error_;
}

bool ShuttleAudioPlayback::readSamples(std::size_t clipIndex, std::int64_t first,
                                       std::int64_t count, std::vector<float>& pcm,
                                       std::string& error) {
    auto& clip = readers_[clipIndex];
    if (!clip.worker) {
        clip.worker = std::make_unique<audio::AudioDecodeWorker>(
            audio::SourceId{static_cast<std::uint64_t>(clipIndex) + 2});
        if (!clip.worker->start(plan_.clips[clipIndex].path, error))
            return false;
    }
    // 前進シャトルでは前 block の続きから rate 未満しか離れていないので、
    // seek せずに続きから読み、要求より前の分を捨てる。
    const std::int64_t skipped =
        plan_.rate > 0 && clip.nextSample >= 0 && first > clip.nextSample &&
                first - clip.nextSample < plan_.rate
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
    pcm.erase(pcm.begin(), pcm.begin() + skipped * audio::kInternalChannels);
    return running_;
}

void ShuttleAudioPlayback::produce() {
    std::int64_t next = 0;
    while (running_) {
        if (!queue_.waitUntilBelow(audio::kQueueTargetSamples, 50) ||
            !queue_.waitForSpace(kBlockSamples, 50))
            continue;
        std::vector<float> pcm;
        std::string error;
        const auto read = [this](std::size_t clipIndex, std::int64_t first, std::int64_t count,
                                 std::vector<float>& source, std::string& readError) {
            return readSamples(clipIndex, first, count, source, readError);
        };
        if (!mixShuttleBlock(plan_, next, kBlockSamples, read, pcm, error)) {
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

#include "media/audio_preview/audio_decode_worker.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace mvm::audio {
namespace {

std::string ffError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    return text;
}

std::int64_t qpcNow() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

double qpcMilliseconds(std::int64_t begin, std::int64_t end) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(end - begin) * 1000.0 / frequency.QuadPart;
}
} // namespace

AudioDecodeWorker::AudioDecodeWorker(SourceId sourceId, std::int64_t queueHardMaxSamples)
    : sourceId_(sourceId), queue_(sourceId, generation_, queueHardMaxSamples) {
    metrics_.sourceId = sourceId_;
    metrics_.sourceGeneration = generation_;
    metrics_.resourceEpoch = resourceEpoch_;
}

AudioDecodeWorker::~AudioDecodeWorker() {
    stop();
}

bool AudioDecodeWorker::openInput(const std::string& path, std::string& error) {
    int result = avformat_open_input(&format_, path.c_str(), nullptr, nullptr);
    if (result < 0) {
        error = "音声入力を開けません: " + ffError(result);
        return false;
    }
    result = avformat_find_stream_info(format_, nullptr);
    if (result < 0) {
        error = "音声 stream 情報を取得できません: " + ffError(result);
        return false;
    }
    streamIndex_ = av_find_best_stream(format_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIndex_ < 0) {
        error = "音声 stream がありません";
        return false;
    }
    AVStream* stream = format_->streams[streamIndex_];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        error = "音声 decoder が見つかりません";
        return false;
    }
    codec_ = avcodec_alloc_context3(decoder);
    if (!codec_) {
        error = "音声 decoder context を確保できません";
        return false;
    }
    if ((result = avcodec_parameters_to_context(codec_, stream->codecpar)) < 0 ||
        (result = avcodec_open2(codec_, decoder, nullptr)) < 0) {
        error = "音声 decoder を初期化できません: " + ffError(result);
        return false;
    }
    AVChannelLayout outputLayout = AV_CHANNEL_LAYOUT_STEREO;
    result = swr_alloc_set_opts2(&resampler_, &outputLayout, AV_SAMPLE_FMT_FLT, kInternalSampleRate,
                                 &codec_->ch_layout, codec_->sample_fmt, codec_->sample_rate, 0,
                                 nullptr);
    if (result < 0 || !resampler_ || (result = swr_init(resampler_)) < 0) {
        error = "音声 format converter を初期化できません: " + ffError(result);
        return false;
    }
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!frame_ || !packet_) {
        error = "音声 decode buffer を確保できません";
        return false;
    }
    metrics_.sourceFormat = {codec_->sample_rate, codec_->ch_layout.nb_channels,
                             av_get_sample_fmt_name(codec_->sample_fmt)};
    metrics_.open = true;
    return true;
}

bool AudioDecodeWorker::start(const std::string& utf8Path, std::string& error) {
    std::lock_guard lock(mutex_);
    if (running_) {
        error = "AudioDecodeWorker は既に実行中です";
        return false;
    }
    if (sourceId_.value == 0 || utf8Path.empty()) {
        error = "source id または入力 path が無効です";
        return false;
    }
    if (!openInput(utf8Path, error)) {
        closeInput();
        return false;
    }
    queue_.restart();
    running_ = true;
    playing_ = false;
    joined_ = false;
    metrics_.running = true;
    metrics_.joined = false;
    thread_ = std::thread(&AudioDecodeWorker::run, this);
    return true;
}

void AudioDecodeWorker::play() {
    {
        std::lock_guard lock(mutex_);
        // stop 済みの worker を playing へ戻さない。
        if (!running_.load(std::memory_order_acquire))
            return;
        playing_ = true;
        metrics_.playing = true;
    }
    wake_.notify_all();
}

void AudioDecodeWorker::pause() {
    {
        std::lock_guard lock(mutex_);
        playing_ = false;
        metrics_.playing = false;
    }
}

void AudioDecodeWorker::stop() {
    {
        std::lock_guard lock(mutex_);
        running_ = false;
        playing_ = false;
        metrics_.running = false;
        metrics_.playing = false;
    }
    queue_.stop();
    wake_.notify_all();
    seekDone_.notify_all();
    if (thread_.joinable())
        thread_.join();
    joined_ = true;
    {
        std::lock_guard lock(mutex_);
        metrics_.joined = true;
        seekOutstanding_ = false;
        closeInput();
    }
}

AudioSeekRequestResult AudioDecodeWorker::requestSeek(std::int64_t sample, AudioSeekTicket& ticket,
                                                      std::string& error) {
    {
        std::lock_guard lock(mutex_);
        if (!running_)
            return AudioSeekRequestResult::RejectedStopped;
        if (sample < 0) {
            error = "seek sample は 0 以上でなければなりません";
            return AudioSeekRequestResult::RejectedInvalid;
        }
        if (seekOutstanding_)
            return AudioSeekRequestResult::RejectedBusy;
        ticket = {++nextSeekId_, sample};
        seekTicket_ = ticket;
        pendingSeek_ = true;
        seekOutstanding_ = true;
        seekCompletion_ = {};
    }
    wake_.notify_all();
    return AudioSeekRequestResult::Accepted;
}

AudioSeekWaitResult AudioDecodeWorker::waitSeek(const AudioSeekTicket& ticket, int timeoutMs,
                                                AudioSeekCompletion& completion) {
    std::unique_lock lock(mutex_);
    if (!seekOutstanding_ || ticket.requestId != seekTicket_.requestId)
        return AudioSeekWaitResult::StaleTicket;
    if (!seekDone_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
            return !running_ || (seekCompletion_.requestId == ticket.requestId);
        })) {
        ++metrics_.seekTimeoutCount;
        return AudioSeekWaitResult::Timeout;
    }
    if (seekCompletion_.requestId != ticket.requestId)
        return AudioSeekWaitResult::StaleTicket;
    completion = seekCompletion_;
    seekOutstanding_ = false;
    return AudioSeekWaitResult::Ready;
}

bool AudioDecodeWorker::decodeOne(AudioChunk& chunk, std::string& error) {
    AVStream* stream = format_->streams[streamIndex_];
    for (;;) {
        int result = avcodec_receive_frame(codec_, frame_);
        if (result == 0) {
            const std::int64_t delay = swr_get_delay(resampler_, codec_->sample_rate);
            const int capacity = static_cast<int>(av_rescale_rnd(
                delay + frame_->nb_samples, kInternalSampleRate, codec_->sample_rate, AV_ROUND_UP));
            auto pcm = std::make_shared<std::vector<float>>(static_cast<std::size_t>(capacity) *
                                                            kInternalChannels);
            std::uint8_t* output[] = {reinterpret_cast<std::uint8_t*>(pcm->data())};
            const int converted = swr_convert(
                resampler_, output, capacity,
                const_cast<const std::uint8_t**>(frame_->extended_data), frame_->nb_samples);
            if (converted < 0) {
                error = "音声 sample 変換に失敗しました: " + ffError(converted);
                av_frame_unref(frame_);
                return false;
            }
            pcm->resize(static_cast<std::size_t>(converted) * kInternalChannels);
            const std::int64_t pts = frame_->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) {
                error = "音声 frame に PTS がありません";
                av_frame_unref(frame_);
                return false;
            }
            const std::int64_t start =
                av_rescale_q(pts, stream->time_base, AVRational{1, kInternalSampleRate});
            chunk = {sourceId_,
                     generation_,
                     resourceEpoch_,
                     start,
                     converted,
                     pts,
                     {stream->time_base.num, stream->time_base.den},
                     kInternalSampleRate,
                     kInternalChannels,
                     std::move(pcm),
                     0};
            const std::int64_t decodedEnd = chunk.startSample + chunk.sampleCount;
            std::int64_t actualDecodedEnd = decodedEnd;
            {
                std::lock_guard lock(mutex_);
                metrics_.actualLastDecodedSampleExclusive =
                    std::max(metrics_.actualLastDecodedSampleExclusive, decodedEnd);
                actualDecodedEnd = metrics_.actualLastDecodedSampleExclusive;
            }
            if (attribution_)
                attribution_->context.actualAudioEndExclusive.store(actualDecodedEnd,
                                                                    std::memory_order_release);
            av_frame_unref(frame_);
            return true;
        }
        if (result == AVERROR_EOF) {
            std::int64_t actualDecodedEnd = -1;
            {
                std::lock_guard lock(mutex_);
                metrics_.eof = true;
                actualDecodedEnd = metrics_.actualLastDecodedSampleExclusive;
            }
            if (attribution_)
                attribution_->context.audioDecoderEof.store(true, std::memory_order_release);
            if (!queue_.markEndOfStream(generation_, actualDecodedEnd))
                error = "current generationのaudio EOF authorityを確定できません";
            return false;
        }
        if (result != AVERROR(EAGAIN)) {
            error = "音声 frame decode に失敗しました: " + ffError(result);
            return false;
        }
        if (demuxEof_) {
            avcodec_send_packet(codec_, nullptr);
            continue;
        }
        for (;;) {
            result = av_read_frame(format_, packet_);
            if (result == AVERROR_EOF) {
                demuxEof_ = true;
                avcodec_send_packet(codec_, nullptr);
                break;
            }
            if (result < 0) {
                error = "音声 packet 読み込みに失敗しました: " + ffError(result);
                return false;
            }
            if (packet_->stream_index != streamIndex_) {
                av_packet_unref(packet_);
                continue;
            }
            result = avcodec_send_packet(codec_, packet_);
            av_packet_unref(packet_);
            if (result < 0 && result != AVERROR(EAGAIN)) {
                error = "音声 packet decode に失敗しました: " + ffError(result);
                return false;
            }
            break;
        }
    }
}

AudioSeekCompletion AudioDecodeWorker::executeSeek(const AudioSeekTicket& ticket) {
    AudioSeekCompletion completion;
    completion.requestId = ticket.requestId;
    completion.requestedSample = ticket.targetSample;
    const std::int64_t begin = qpcNow();
    AVStream* stream = format_->streams[streamIndex_];
    const std::int64_t timestamp =
        av_rescale_q(ticket.targetSample, AVRational{1, kInternalSampleRate}, stream->time_base);
    const int result = avformat_seek_file(format_, streamIndex_, INT64_MIN, timestamp, timestamp,
                                          AVSEEK_FLAG_BACKWARD);
    if (result < 0) {
        completion.error = "音声 seek に失敗しました: " + ffError(result);
        return completion;
    }
    avcodec_flush_buffers(codec_);
    swr_close(resampler_);
    if (swr_init(resampler_) < 0) {
        completion.error = "seek 後の resampler reset に失敗しました";
        return completion;
    }
    demuxEof_ = false;
    ++generation_.value;
    queue_.setGeneration(generation_);
    {
        std::lock_guard lock(mutex_);
        metrics_.sourceGeneration = generation_;
        metrics_.eof = false;
        metrics_.actualLastDecodedSampleExclusive = -1;
    }
    if (attribution_) {
        attribution_->context.audioDecoderEof.store(false, std::memory_order_release);
        attribution_->context.actualAudioEndExclusive.store(-1, std::memory_order_release);
    }
    for (;;) {
        AudioChunk chunk;
        std::string error;
        if (!decodeOne(chunk, error)) {
            completion.error = error.empty() ? "seek target まで decode できません" : error;
            return completion;
        }
        const std::int64_t end = chunk.startSample + chunk.sampleCount;
        if (end <= ticket.targetSample) {
            completion.discardedPrerollSamples += chunk.sampleCount;
            continue;
        }
        if (chunk.startSample < ticket.targetSample) {
            const std::int64_t trim = ticket.targetSample - chunk.startSample;
            chunk.startSample += trim;
            chunk.offsetSamples += static_cast<std::size_t>(trim);
            chunk.sampleCount -= trim;
            completion.discardedPrerollSamples += trim;
        }
        if (chunk.startSample != ticket.targetSample) {
            completion.error = "最初の output sample が requested sample と一致しません";
            return completion;
        }
        completion.firstOutputSample = chunk.startSample;
        completion.seekGeneration = generation_;
        if (queue_.push(std::move(chunk)) != AudioQueuePushResult::Accepted) {
            completion.error = "seek target chunk を queue へ投入できません";
            return completion;
        }
        completion.completed = true;
        completion.readyQpc = qpcNow();
        completion.latencyMs = qpcMilliseconds(begin, completion.readyQpc);
        std::lock_guard lock(mutex_);
        metrics_.discardedPrerollSamples += completion.discardedPrerollSamples;
        return completion;
    }
}

void AudioDecodeWorker::run() {
    while (running_) {
        AudioSeekTicket seek;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return !running_ || pendingSeek_ || playing_; });
            if (!running_)
                break;
            if (pendingSeek_) {
                seek = seekTicket_;
                pendingSeek_ = false;
            }
        }
        if (seek.requestId != 0) {
            AudioSeekCompletion completion = executeSeek(seek);
            {
                std::lock_guard lock(mutex_);
                seekCompletion_ = std::move(completion);
            }
            seekDone_.notify_all();
            continue;
        }
        if (!queue_.waitUntilBelow(kQueueTargetSamples, 50) || !queue_.waitForSpace(2048, 50)) {
            if (!running_)
                break;
            std::lock_guard lock(mutex_);
            ++metrics_.backpressureWaitCount;
            continue;
        }
        AudioChunk chunk;
        std::string error;
        if (!decodeOne(chunk, error)) {
            if (!error.empty())
                fail(error);
            playing_ = false;
            continue;
        }
        if (chunk.startSample + chunk.sampleCount <= 0)
            continue;
        if (chunk.startSample < 0) {
            const std::int64_t trim = -chunk.startSample;
            chunk.startSample = 0;
            chunk.offsetSamples += static_cast<std::size_t>(trim);
            chunk.sampleCount -= trim;
        }
        const auto count = chunk.sampleCount;
        const auto result = queue_.push(std::move(chunk));
        if (result == AudioQueuePushResult::RejectedOverflow)
            continue;
        if (result != AudioQueuePushResult::Accepted) {
            fail("decoded audio chunk を queue が拒否しました");
            continue;
        }
        std::lock_guard lock(mutex_);
        ++metrics_.decodedChunkCount;
        metrics_.decodedSampleCount += static_cast<std::uint64_t>(count);
    }
}

void AudioDecodeWorker::fail(const std::string& error) {
    std::lock_guard lock(mutex_);
    metrics_.fatal = true;
    ++metrics_.decodeErrorCount;
    metrics_.lastError = error;
}

AudioDecoderSnapshot AudioDecodeWorker::snapshot() const {
    std::lock_guard lock(mutex_);
    AudioDecoderSnapshot result = metrics_;
    result.running = running_;
    result.playing = playing_;
    result.joined = joined_;
    return result;
}

void AudioDecodeWorker::closeInput() {
    av_packet_free(&packet_);
    av_frame_free(&frame_);
    swr_free(&resampler_);
    avcodec_free_context(&codec_);
    avformat_close_input(&format_);
    streamIndex_ = -1;
    demuxEof_ = false;
    metrics_.open = false;
}

} // namespace mvm::audio

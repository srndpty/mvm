#include "media/audio_preview/audio_decode_worker.h"

#include "core/checked_integer.h"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

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

__extension__ using WideInteger = __int128;

WideInteger gcdWide(WideInteger a, WideInteger b) {
    while (b != 0) {
        const WideInteger r = a % b;
        a = b;
        b = r;
    }
    return a;
}

// numerator / denominator を int で表せる有理数にする。約分して収まればそのまま、
// 収まらなければ連分数で分母・分子とも INT_MAX 以下の最良近似にする
// (相対誤差は 1e-18 程度で、1 時間の再生でも 1 sample に満たない)。
bool resamplerRates(WideInteger numerator, WideInteger denominator, int& input, int& output) {
    if (numerator <= 0 || denominator <= 0)
        return false;
    const WideInteger common = gcdWide(numerator, denominator);
    numerator /= common;
    denominator /= common;
    const WideInteger limit = std::numeric_limits<int>::max();
    if (numerator <= limit && denominator <= limit) {
        input = static_cast<int>(numerator);
        output = static_cast<int>(denominator);
        return true;
    }
    WideInteger h0 = 0, h1 = 1, k0 = 1, k1 = 0;
    WideInteger a = numerator, b = denominator;
    while (b != 0) {
        const WideInteger q = a / b;
        const WideInteger h2 = q * h1 + h0;
        const WideInteger k2 = q * k1 + k0;
        if (h2 > limit || k2 > limit)
            break;
        h0 = h1;
        h1 = h2;
        k0 = k1;
        k1 = k2;
        const WideInteger r = a % b;
        a = b;
        b = r;
    }
    if (h1 <= 0 || k1 <= 0)
        return false;
    input = static_cast<int>(h1);
    output = static_cast<int>(k1);
    return true;
}

double qpcMilliseconds(std::int64_t begin, std::int64_t end) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(end - begin) * 1000.0 / static_cast<double>(frequency.QuadPart);
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
    // 速度 s = p / q の素材を「rate が素材 rate x s の入力」として 48 kHz へ変換すると、
    // 出力は伸縮した時間軸の sample になる。rate は整数比 (素材 rate x p) : (48000 x q)。
    // ピッチ保持は等速の 48 kHz へ変換してから stretcher で伸縮する。速度 1/1 (約分済みなので
    // p == q) では伸縮しないので stretcher を通さない。書き出しも 1/1 では timewarp を
    // 通らないため、preview だけに余計な DSP と遅延が入らないようにする。
    const bool stretch = preservePitch_ && speedNum_ != speedDen_;
    if (codec_->sample_rate <= 0 ||
        !resamplerRates(static_cast<WideInteger>(codec_->sample_rate) * (stretch ? 1 : speedNum_),
                        static_cast<WideInteger>(kInternalSampleRate) * (stretch ? 1 : speedDen_),
                        resamplerInputRate_, resamplerOutputRate_)) {
        error = "音声の再生速度をresamplerのrateへ換算できません";
        return false;
    }
    AVChannelLayout outputLayout = AV_CHANNEL_LAYOUT_STEREO;
    result = swr_alloc_set_opts2(&resampler_, &outputLayout, AV_SAMPLE_FMT_FLT,
                                 resamplerOutputRate_, &codec_->ch_layout, codec_->sample_fmt,
                                 resamplerInputRate_, 0, nullptr);
    if (result < 0 || !resampler_ || (result = swr_init(resampler_)) < 0) {
        error = "音声 format converter を初期化できません: " + ffError(result);
        return false;
    }
    if (stretch) {
        // seek は伸縮後の 48 kHz の位置を素材 rate の位置へ戻す: x (素材 rate x p) / (48000 x q)。
        // 積を int64 で作ると巨大な約分済み速度で signed overflow になるので、WideInteger で
        // 作って約分し、int64 に収まらなければ開始を拒否する (seek の途中で失敗させない)。
        WideInteger seekNum = static_cast<WideInteger>(codec_->sample_rate) * speedNum_;
        WideInteger seekDen = static_cast<WideInteger>(kInternalSampleRate) * speedDen_;
        const WideInteger common = gcdWide(seekNum, seekDen);
        seekNum /= common;
        seekDen /= common;
        if (seekNum > std::numeric_limits<std::int64_t>::max() ||
            seekDen > std::numeric_limits<std::int64_t>::max()) {
            error = "ピッチ保持の速度を seek 位置の換算へ表せません";
            return false;
        }
        pitchSeekNum_ = static_cast<std::int64_t>(seekNum);
        pitchSeekDen_ = static_cast<std::int64_t>(seekDen);
        stretcher_ = std::make_unique<PitchPreservingStretcher>(static_cast<double>(speedDen_) /
                                                                static_cast<double>(speedNum_));
        if (!stretcher_->valid()) {
            error = "ピッチ保持処理を初期化できません";
            return false;
        }
        pitchNextOutputSample_ = -1;
        pitchExpectedEndSample_ = -1;
        pitchFlushDone_ = false;
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

bool AudioDecodeWorker::setPlaybackSpeed(std::int64_t speedNum, std::int64_t speedDen,
                                         bool preservePitch, std::string& error) {
    std::lock_guard lock(mutex_);
    if (running_ || speedNum <= 0 || speedDen <= 0) {
        error = "音声の再生速度が不正、または開始後です";
        return false;
    }
    speedNum_ = speedNum;
    speedDen_ = speedDen;
    preservePitch_ = preservePitch;
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
    int invalidPackets = 0;
    for (;;) {
        int result = avcodec_receive_frame(codec_, frame_);
        if (result == 0) {
            const std::int64_t pts = frame_->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) {
                error = "音声 frame に PTS がありません";
                av_frame_unref(frame_);
                return false;
            }
            // container 上の非ゼロ開始PTSを素材内sample 0へ正規化する。
            // MPEG-PS等は先頭audio PTSが0ではないため、絶対PTSをsampleへ変換すると
            // seek(0)の最初のchunkがsample 1000前後から始まり、exact seek契約を破る。
            const std::int64_t streamStart =
                stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
            std::int64_t relativePts = 0;
            if (!core::checkedSubtract(pts, streamStart, relativePts)) {
                error = "音声PTSからstream開始PTSを減算できません";
                av_frame_unref(frame_);
                return false;
            }
            const std::int64_t inputSample =
                av_rescale_q(relativePts, stream->time_base, AVRational{1, codec_->sample_rate});
            if (inputSample > std::numeric_limits<std::int64_t>::max() / resamplerOutputRate_ ||
                inputSample < std::numeric_limits<std::int64_t>::min() / resamplerOutputRate_) {
                error = "音声 PTS をresampler timestampへ換算できません";
                av_frame_unref(frame_);
                return false;
            }
            // swr_next_pts は内部delayを含む「次に出るsample」の時刻を返す。
            // 入力PTSを直接48kHzへ丸めると、44.1kHz等の変換で先頭chunkだけが
            // delay分短くなり、次chunkとの間に偽の隙間ができる。
            // resampler から見た入力 rate は素材 rate x 速度なので、sample 数はそのまま渡す。
            const std::int64_t nextOutputTimestamp =
                swr_next_pts(resampler_, inputSample * resamplerOutputRate_);
            if (nextOutputTimestamp == std::numeric_limits<std::int64_t>::min()) {
                error = "resamplerの出力timestampを取得できません";
                av_frame_unref(frame_);
                return false;
            }
            std::int64_t start = av_rescale(nextOutputTimestamp, 1, resamplerInputRate_);
            const std::int64_t delay = swr_get_delay(resampler_, resamplerInputRate_);
            const int capacity =
                static_cast<int>(av_rescale_rnd(delay + frame_->nb_samples, resamplerOutputRate_,
                                                resamplerInputRate_, AV_ROUND_UP));
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
            int outputCount = converted;
            if (stretcher_) {
                pitchExpectedEndSample_ =
                    av_rescale_rnd(start + converted, speedDen_, speedNum_, AV_ROUND_NEAR_INF);
                if (pitchNextOutputSample_ < 0) {
                    pitchNextOutputSample_ =
                        av_rescale_rnd(start, speedDen_, speedNum_, AV_ROUND_DOWN);
                }
                auto stretched =
                    stretcher_->process(pcm->data(), static_cast<std::size_t>(converted), false);
                pcm = std::make_shared<std::vector<float>>(std::move(stretched));
                start = pitchNextOutputSample_;
                outputCount = static_cast<int>(pcm->size() / kInternalChannels);
                pitchNextOutputSample_ += outputCount;
                if (outputCount == 0) {
                    av_frame_unref(frame_);
                    continue;
                }
            }
            chunk = {sourceId_,
                     generation_,
                     resourceEpoch_,
                     start,
                     outputCount,
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
            if (stretcher_ && !pitchFlushDone_) {
                pitchFlushDone_ = true;
                auto stretched = stretcher_->finish();
                if (pitchExpectedEndSample_ >= pitchNextOutputSample_) {
                    const auto remaining = pitchExpectedEndSample_ - pitchNextOutputSample_;
                    stretched.resize(std::min(
                        stretched.size(), static_cast<std::size_t>(remaining) * kInternalChannels));
                }
                if (!stretched.empty()) {
                    const int count = static_cast<int>(stretched.size() / kInternalChannels);
                    auto pcm = std::make_shared<std::vector<float>>(std::move(stretched));
                    chunk = {sourceId_,
                             generation_,
                             resourceEpoch_,
                             pitchNextOutputSample_,
                             count,
                             0,
                             {1, 1},
                             kInternalSampleRate,
                             kInternalChannels,
                             std::move(pcm),
                             0};
                    pitchNextOutputSample_ += count;
                    return true;
                }
            }
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
            // MPEG-PSはseek直後のpacketがaudio frame境界より前から始まる場合がある。
            // decoderが同期を取り直せるよう少数の壊れた先頭packetだけを読み飛ばす。
            // 上限なしで成功扱いにはせず、素材破損時はfail-closedにする。
            if (result == AVERROR_INVALIDDATA && ++invalidPackets <= 32)
                continue;
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
    // 伸縮した時間軸の出力 sample を素材の入力 sample へ戻してから時刻にする。
    // ピッチ保持では resampler が素材 rate -> 48 kHz の等速なので、速度に加えて
    // 48 kHz -> 素材 rate の換算も要る (速度だけ戻すと 44.1 kHz 素材で位置が後ろへずれ、
    // preroll の 1 秒を超えると要求位置より後ろから decode してしまう)。
    const std::int64_t inputSample =
        stretcher_
            ? av_rescale_rnd(ticket.targetSample, pitchSeekNum_, pitchSeekDen_, AV_ROUND_DOWN)
            : av_rescale(ticket.targetSample, resamplerInputRate_, resamplerOutputRate_);
    const std::int64_t relativeTimestamp =
        av_rescale_q(inputSample, AVRational{1, codec_->sample_rate}, stream->time_base);
    const std::int64_t streamStart = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
    std::int64_t timestamp = 0;
    if (!core::checkedAdd(relativeTimestamp, streamStart, timestamp)) {
        completion.error = "音声seek timestampにstream開始PTSを加算できません";
        return completion;
    }
    // 圧縮audioでは要求位置そのものがpacket/frame境界とは限らない。
    // 少し前へseekしてdecodeしながら要求sampleまで捨てることで、MPEG-PSでも
    // 要求位置より後の最初の有効frameへ飛んでexact seekを破ることを防ぐ。
    const std::int64_t prerollTimestamp = av_rescale_q(1, AVRational{1, 1}, stream->time_base);
    std::int64_t seekTimestamp = timestamp;
    std::int64_t earlierTimestamp = 0;
    if (core::checkedSubtract(timestamp, prerollTimestamp, earlierTimestamp))
        seekTimestamp = std::max(streamStart, earlierTimestamp);
    const int result = avformat_seek_file(format_, streamIndex_, INT64_MIN, seekTimestamp,
                                          seekTimestamp, AVSEEK_FLAG_BACKWARD);
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
    if (stretcher_)
        stretcher_->reset();
    pitchNextOutputSample_ = -1;
    pitchExpectedEndSample_ = -1;
    pitchFlushDone_ = false;
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
            completion.error =
                "最初の output sample が requested sample と一致しません: requested=" +
                std::to_string(ticket.targetSample) +
                " actual=" + std::to_string(chunk.startSample);
            return completion;
        }
        completion.firstOutputSample = chunk.startSample;
        completion.seekGeneration = generation_;
        if (completion.discardedPrerollSamples < 0) {
            completion.error = "seek preroll sample数が負になりました";
            return completion;
        }
        if (queue_.push(std::move(chunk)) != AudioQueuePushResult::Accepted) {
            completion.error = "seek target chunk を queue へ投入できません";
            return completion;
        }
        completion.completed = true;
        completion.readyQpc = qpcNow();
        completion.latencyMs = qpcMilliseconds(begin, completion.readyQpc);
        std::lock_guard lock(mutex_);
        metrics_.discardedPrerollSamples +=
            static_cast<std::uint64_t>(completion.discardedPrerollSamples);
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
        const auto end = chunk.startSample + count;
        const auto result = queue_.push(std::move(chunk));
        if (result == AudioQueuePushResult::RejectedOverflow)
            continue;
        if (result != AudioQueuePushResult::Accepted) {
            fail("decoded audio chunk を queue が拒否しました");
            continue;
        }
        if (stretcher_ && pitchFlushDone_) {
            if (!queue_.markEndOfStream(generation_, end))
                fail("ピッチ保持後の音声終端を確定できません");
            playing_ = false;
            std::lock_guard lock(mutex_);
            metrics_.eof = true;
            metrics_.actualLastDecodedSampleExclusive = end;
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
    stretcher_.reset();
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

#include "media/audio_waveform/audio_waveform_decoder.h"

#include "core/checked_integer.h"

#include <memory>
#include <vector>

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

struct FormatCloser {
    void operator()(AVFormatContext* context) const { avformat_close_input(&context); }
};

struct CodecCloser {
    void operator()(AVCodecContext* context) const { avcodec_free_context(&context); }
};

struct ResamplerCloser {
    void operator()(SwrContext* context) const { swr_free(&context); }
};

struct FrameCloser {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};

struct PacketCloser {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};

bool cancelled(const std::atomic<bool>* cancel) {
    return cancel && cancel->load(std::memory_order_relaxed);
}

class WaveformDecoder final {
public:
    bool open(const std::string& utf8Path, std::string& error);
    bool run(const std::atomic<bool>* cancel, core::WaveformPeaks& peaks, bool& wasCancelled,
             std::string& error);

private:
    bool drainFrames(std::string& error);
    bool convertFrame(std::string& error);

    std::unique_ptr<AVFormatContext, FormatCloser> format_;
    std::unique_ptr<AVCodecContext, CodecCloser> codec_;
    std::unique_ptr<SwrContext, ResamplerCloser> resampler_;
    std::unique_ptr<AVFrame, FrameCloser> frame_;
    std::unique_ptr<AVPacket, PacketCloser> packet_;
    int streamIndex_ = -1;
    int channels_ = 0;
    std::vector<std::vector<float>> planes_;
    core::WaveformPeakBuilder builder_;
};

bool WaveformDecoder::open(const std::string& utf8Path, std::string& error) {
    AVFormatContext* format = nullptr;
    int result = avformat_open_input(&format, utf8Path.c_str(), nullptr, nullptr);
    if (result < 0) {
        error = "音声入力を開けません: " + ffError(result);
        return false;
    }
    format_.reset(format);
    result = avformat_find_stream_info(format_.get(), nullptr);
    if (result < 0) {
        error = "音声 stream 情報を取得できません: " + ffError(result);
        return false;
    }
    streamIndex_ = av_find_best_stream(format_.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIndex_ < 0) {
        error = "音声 stream がありません";
        return false;
    }
    // 動画付き素材では video packet の demux だけでも重い。audio 以外は読まない。
    for (unsigned index = 0; index < format_->nb_streams; ++index) {
        if (static_cast<int>(index) != streamIndex_)
            format_->streams[index]->discard = AVDISCARD_ALL;
    }
    AVStream* stream = format_->streams[streamIndex_];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        error = "音声 decoder が見つかりません";
        return false;
    }
    codec_.reset(avcodec_alloc_context3(decoder));
    if (!codec_) {
        error = "音声 decoder context を確保できません";
        return false;
    }
    if ((result = avcodec_parameters_to_context(codec_.get(), stream->codecpar)) < 0 ||
        (result = avcodec_open2(codec_.get(), decoder, nullptr)) < 0) {
        error = "音声 decoder を初期化できません: " + ffError(result);
        return false;
    }
    channels_ = codec_->ch_layout.nb_channels;
    if (channels_ <= 0 || codec_->sample_rate <= 0) {
        error = "音声の channel 数または sample rate が不正です";
        return false;
    }
    // channel 数と rate は保ち、sample format だけを planar float へそろえる。
    SwrContext* resampler = nullptr;
    result = swr_alloc_set_opts2(&resampler, &codec_->ch_layout, AV_SAMPLE_FMT_FLTP,
                                 codec_->sample_rate, &codec_->ch_layout, codec_->sample_fmt,
                                 codec_->sample_rate, 0, nullptr);
    resampler_.reset(resampler);
    if (result < 0 || !resampler_ || (result = swr_init(resampler_.get())) < 0) {
        error = "音声 format converter を初期化できません: " + ffError(result);
        return false;
    }
    frame_.reset(av_frame_alloc());
    packet_.reset(av_packet_alloc());
    if (!frame_ || !packet_) {
        error = "音声 decode buffer を確保できません";
        return false;
    }
    planes_.assign(static_cast<std::size_t>(channels_), {});
    return builder_.reset(codec_->sample_rate, channels_,
                          core::waveformSamplesPerPeak(codec_->sample_rate), error);
}

bool WaveformDecoder::convertFrame(std::string& error) {
    const std::int64_t pts = frame_->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) {
        error = "音声 frame に PTS がありません";
        return false;
    }
    // preview と同じく、container 上の非ゼロ開始 PTS を素材内 sample 0 へ正規化する。
    AVStream* stream = format_->streams[streamIndex_];
    const std::int64_t streamStart = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
    std::int64_t relativePts = 0;
    if (!core::checkedSubtract(pts, streamStart, relativePts)) {
        error = "音声PTSからstream開始PTSを減算できません";
        return false;
    }
    const std::int64_t startSample =
        av_rescale_q(relativePts, stream->time_base, AVRational{1, codec_->sample_rate});

    // rate を変えないので delay は 0 のはずだが、converter の内部 buffer を仮定しない。
    const int capacity =
        static_cast<int>(swr_get_delay(resampler_.get(), codec_->sample_rate)) + frame_->nb_samples;
    std::vector<std::uint8_t*> output(static_cast<std::size_t>(channels_));
    for (int channel = 0; channel < channels_; ++channel) {
        auto& plane = planes_[static_cast<std::size_t>(channel)];
        plane.resize(static_cast<std::size_t>(capacity));
        output[static_cast<std::size_t>(channel)] = reinterpret_cast<std::uint8_t*>(plane.data());
    }
    const int converted =
        swr_convert(resampler_.get(), output.data(), capacity,
                    const_cast<const std::uint8_t**>(frame_->extended_data), frame_->nb_samples);
    if (converted < 0) {
        error = "音声 sample 変換に失敗しました: " + ffError(converted);
        return false;
    }
    std::vector<const float*> planes(static_cast<std::size_t>(channels_));
    for (int channel = 0; channel < channels_; ++channel)
        planes[static_cast<std::size_t>(channel)] =
            planes_[static_cast<std::size_t>(channel)].data();
    return builder_.addPlanar(startSample, planes.data(), converted, error);
}

bool WaveformDecoder::drainFrames(std::string& error) {
    for (;;) {
        const int result = avcodec_receive_frame(codec_.get(), frame_.get());
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            return true;
        if (result < 0) {
            error = "音声 frame decode に失敗しました: " + ffError(result);
            return false;
        }
        const bool converted = convertFrame(error);
        av_frame_unref(frame_.get());
        if (!converted)
            return false;
    }
}

bool WaveformDecoder::run(const std::atomic<bool>* cancel, core::WaveformPeaks& peaks,
                          bool& wasCancelled, std::string& error) {
    wasCancelled = false;
    int invalidPackets = 0;
    for (;;) {
        if (cancelled(cancel)) {
            wasCancelled = true;
            error = "波形の生成を中断しました";
            return false;
        }
        int result = av_read_frame(format_.get(), packet_.get());
        if (result == AVERROR_EOF)
            break;
        if (result < 0) {
            error = "音声 packet 読み込みに失敗しました: " + ffError(result);
            return false;
        }
        if (packet_->stream_index != streamIndex_) {
            av_packet_unref(packet_.get());
            continue;
        }
        result = avcodec_send_packet(codec_.get(), packet_.get());
        av_packet_unref(packet_.get());
        // preview の decoder と同じく、少数の壊れた packet だけは読み飛ばす。
        // 上限なしで成功扱いにはしない。
        if (result == AVERROR_INVALIDDATA && ++invalidPackets <= 32)
            continue;
        if (result < 0) {
            error = "音声 packet decode に失敗しました: " + ffError(result);
            return false;
        }
        if (!drainFrames(error))
            return false;
    }
    const int flushed = avcodec_send_packet(codec_.get(), nullptr);
    if (flushed < 0 && flushed != AVERROR_EOF) {
        error = "音声 decoder を flush できません: " + ffError(flushed);
        return false;
    }
    if (!drainFrames(error))
        return false;
    return builder_.finish(peaks, error);
}

} // namespace

AudioWaveformResult decodeAudioWaveform(const std::string& utf8Path,
                                        const std::atomic<bool>* cancel) {
    AudioWaveformResult result;
    if (utf8Path.empty()) {
        result.error = "音声入力の path が空です";
        return result;
    }
    WaveformDecoder decoder;
    if (!decoder.open(utf8Path, result.error))
        return result;
    result.success = decoder.run(cancel, result.peaks, result.cancelled, result.error);
    return result;
}

} // namespace mvm::audio

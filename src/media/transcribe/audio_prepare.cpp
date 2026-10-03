#include "media/transcribe/transcribe.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <numeric>
#include <string_view>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace mvm::transcribe {
namespace {
struct FormatCloser {
    void operator()(AVFormatContext* p) const { avformat_close_input(&p); }
};

struct CodecCloser {
    void operator()(AVCodecContext* p) const { avcodec_free_context(&p); }
};

struct FrameCloser {
    void operator()(AVFrame* p) const { av_frame_free(&p); }
};

struct PacketCloser {
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};

struct SwrCloser {
    void operator()(SwrContext* p) const { swr_free(&p); }
};

bool stopped(const std::atomic<bool>* cancel) {
    return cancel && cancel->load();
}

std::string utf8(const std::filesystem::path& path) {
    auto text = path.u8string();
    return {text.begin(), text.end()};
}

// 区間の開始の手前へ seek してからデコードする。seek の手前の余白 (preroll) を少し読み、
// timestamp で正確な開始位置まで捨てる。
constexpr std::int64_t kSeekPrerollMs = 2000;

// 1 回分の準備。allowSeek なら区間の開始の手前へ seek する。seek が開始位置を越えた場合
// (overshoot) は false を返して overshoot を立てる。呼び出し側は seek せずにやり直す。
bool preparePass(const Request& request, const std::atomic<bool>* cancel, bool allowSeek,
                 std::vector<float>& samples, std::string& error, PrepareStats& stats,
                 bool& overshoot) {
    overshoot = false;
    if (request.sourceBeginMs < 0 ||
        (request.sourceEndMs != -1 && request.sourceEndMs <= request.sourceBeginMs)) {
        error = "文字起こしの素材区間が不正です";
        return false;
    }
    AVFormatContext* raw = avformat_alloc_context();
    if (!raw) {
        error = "音声入力の準備に失敗しました";
        return false;
    }
    raw->interrupt_callback = {
        [](void* opaque) { return stopped(static_cast<const std::atomic<bool>*>(opaque)) ? 1 : 0; },
        const_cast<std::atomic<bool>*>(cancel)};
    const auto path = utf8(request.mediaPath);
    if (avformat_open_input(&raw, path.c_str(), nullptr, nullptr) < 0) {
        error = "文字起こしの音声入力を開けません";
        return false;
    }
    std::unique_ptr<AVFormatContext, FormatCloser> format(raw);
    if (avformat_find_stream_info(raw, nullptr) < 0) {
        error = "音声情報を取得できません";
        return false;
    }
    const int index = av_find_best_stream(raw, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (index < 0) {
        error = "対象素材に音声がありません";
        return false;
    }
    auto* stream = raw->streams[index];
    const auto* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    std::unique_ptr<AVCodecContext, CodecCloser> codec(avcodec_alloc_context3(decoder));
    if (!decoder || !codec || avcodec_parameters_to_context(codec.get(), stream->codecpar) < 0 ||
        avcodec_open2(codec.get(), decoder, nullptr) < 0) {
        error = "音声デコーダーを開けません";
        return false;
    }
    if (codec->sample_rate <= 0 || codec->ch_layout.nb_channels <= 0) {
        error = "音声のレート・チャンネルが不正です";
        return false;
    }
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    SwrContext* swrRaw = nullptr;
    if (swr_alloc_set_opts2(&swrRaw, &mono, AV_SAMPLE_FMT_FLT, 16000, &codec->ch_layout,
                            codec->sample_fmt, codec->sample_rate, 0, nullptr) < 0) {
        swr_free(&swrRaw);
        error = "音声変換器を作成できません";
        return false;
    }
    std::unique_ptr<SwrContext, SwrCloser> swr(swrRaw);
    if (swr_init(swr.get()) < 0) {
        error = "音声変換器を初期化できません";
        return false;
    }
    std::unique_ptr<AVFrame, FrameCloser> frame(av_frame_alloc());
    std::unique_ptr<AVPacket, PacketCloser> packet(av_packet_alloc());
    if (!frame || !packet) {
        error = "音声バッファを確保できません";
        return false;
    }
    // 先頭からデコードしてリサンプラーの遅延を揃え、指定区間だけを保持する。
    if (request.sourceBeginMs > std::numeric_limits<std::int64_t>::max() / 16 ||
        request.sourceEndMs > std::numeric_limits<std::int64_t>::max() / 16) {
        error = "音声の時刻が範囲外です";
        return false;
    }
    const auto first = request.sourceBeginMs * 16;
    const auto last = request.sourceEndMs < 0 ? std::numeric_limits<std::int64_t>::max()
                                              : request.sourceEndMs * 16;
    std::int64_t cursor = 0;
    std::vector<float> output;
    std::int64_t origin = stream->start_time;
    // 長い素材の後半だけを使う clip で、先頭から区間まで全部デコードしない。
    // mp3 は索引が無いと seek 先の時刻が推定になり (可変ビットレート)、字幕の時刻がずれうる。
    // mp3 のデコードは十分に速い (85 分を約 1.7 秒) ので、mp3 は先頭から読む。
    // 先頭の時刻が分からない素材も、seek 後の位置を素材の時刻へ換算できないので先頭から読む。
    // WAV は先頭の時刻を持たないが、PCM の timestamp は定義上 0 から始まる。
    bool seekPending = false;
    const std::string_view formatName(raw->iformat->name);
    if (origin == AV_NOPTS_VALUE && formatName == "wav")
        origin = 0;
    const bool seekableFormat =
        formatName.find("mp3") == std::string_view::npos && origin != AV_NOPTS_VALUE;
    if (allowSeek && seekableFormat && request.sourceBeginMs > kSeekPrerollMs) {
        const auto target = origin + av_rescale_q(request.sourceBeginMs - kSeekPrerollMs,
                                                  AVRational{1, 1000}, stream->time_base);
        if (avformat_seek_file(raw, index, std::numeric_limits<std::int64_t>::min(), target, target,
                               AVSEEK_FLAG_BACKWARD) >= 0) {
            avcodec_flush_buffers(codec.get());
            seekPending = true;
            stats.seeked = true;
        }
    }
    const auto append = [&](const float* data, int count) {
        const auto a = std::max(cursor, first), b = std::min(cursor + count, last);
        if (b > a)
            output.insert(output.end(), data + (a - cursor), data + (b - cursor));
        cursor += count;
        stats.decodedSamples += count;
        return output.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max());
    };
    const auto drain = [&]() {
        int code = 0;
        while ((code = avcodec_receive_frame(codec.get(), frame.get())) >= 0) {
            if (stopped(cancel))
                return false;
            if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                if (origin == AV_NOPTS_VALUE)
                    origin = frame->best_effort_timestamp;
                // 先頭から読んだ場合、16kHz の各サンプルは素材の 0 番目のサンプルを起点とする
                // 格子の上にある。seek 後はリサンプラーが途中のサンプルから始まるので、格子が
                // サンプル未満ずれて値が変わる (実測: 48kHz の AAC で最大差 0.07)。格子が揃う
                // 位置 (48kHz なら 3 の倍数) まで無音を前へ足す。無音は捨てる余白の中に入る。
                if (seekPending) {
                    const auto rate = codec->sample_rate;
                    const auto step = rate / std::gcd(rate, 16000);
                    const auto sourceIndex = av_rescale_q(frame->best_effort_timestamp - origin,
                                                          stream->time_base, AVRational{1, rate});
                    const auto pad = ((sourceIndex % step) + step) % step;
                    if (pad > 0 && swr_inject_silence(swr.get(), static_cast<int>(pad)) < 0)
                        return false;
                }
                const auto desired = av_rescale_q(frame->best_effort_timestamp - origin,
                                                  stream->time_base, AVRational{1, 16000}) -
                                     swr_get_delay(swr.get(), 16000);
                // seek 後の最初の frame の位置から数え始める。区間の開始を越えていたら、
                // 開始の手前が欠けるので seek せずにやり直させる (時刻をずらさない)。
                if (seekPending) {
                    seekPending = false;
                    if (desired > first) {
                        overshoot = true;
                        return false;
                    }
                    cursor = std::max<std::int64_t>(0, desired);
                }
                // 時刻の隙間を無音で保ち、認識後の字幕が素材の時刻からずれないようにする。
                if (desired > cursor + 1) {
                    float silence[4096]{};
                    while (cursor < desired) {
                        if (stopped(cancel) ||
                            !append(silence, static_cast<int>(
                                                 std::min<std::int64_t>(4096, desired - cursor))))
                            return false;
                    }
                } else if (desired < cursor - 2) {
                    error = "音声のタイムスタンプが逆行しています";
                    return false;
                }
            }
            const auto capacity =
                av_rescale_rnd(swr_get_delay(swr.get(), codec->sample_rate) + frame->nb_samples,
                               16000, codec->sample_rate, AV_ROUND_UP);
            if (capacity > std::numeric_limits<int>::max())
                return false;
            std::vector<float> converted(static_cast<std::size_t>(capacity));
            std::uint8_t* target = reinterpret_cast<std::uint8_t*>(converted.data());
            const int count = swr_convert(swr.get(), &target, static_cast<int>(capacity),
                                          const_cast<const std::uint8_t**>(frame->extended_data),
                                          frame->nb_samples);
            av_frame_unref(frame.get());
            if (count < 0 || !append(converted.data(), count))
                return false;
        }
        return code == AVERROR(EAGAIN) || code == AVERROR_EOF;
    };
    int readCode = 0;
    while (cursor < last && (readCode = av_read_frame(raw, packet.get())) >= 0) {
        if (stopped(cancel)) {
            error = "文字起こしをキャンセルしました";
            return false;
        }
        if (packet->stream_index == index) {
            if (avcodec_send_packet(codec.get(), packet.get()) < 0 || !drain()) {
                if (!overshoot)
                    error = "音声をデコードできません";
                return false;
            }
        }
        av_packet_unref(packet.get());
    }
    if (cursor < last) {
        if (readCode != AVERROR_EOF || avcodec_send_packet(codec.get(), nullptr) < 0 || !drain()) {
            error = "音声の終端をデコードできません";
            return false;
        }
        while (true) {
            float tail[4096];
            auto* target = reinterpret_cast<std::uint8_t*>(tail);
            const int count = swr_convert(swr.get(), &target, 4096, nullptr, 0);
            if (count < 0 || !append(tail, std::max(0, count))) {
                error = "音声変換の終端処理に失敗しました";
                return false;
            }
            if (!count)
                break;
        }
    }
    if (stopped(cancel)) {
        error = "文字起こしをキャンセルしました";
        return false;
    }
    if (output.empty()) {
        error = "認識する音声サンプルがありません";
        return false;
    }
    samples = std::move(output);
    return true;
}
} // namespace

bool prepareAudio(const Request& request, const std::atomic<bool>* cancel,
                  std::vector<float>& samples, std::string& error, PrepareStats* stats) {
    PrepareStats local;
    auto& counted = stats ? *stats : local;
    counted = {};
    bool overshoot = false;
    if (preparePass(request, cancel, true, samples, error, counted, overshoot))
        return true;
    if (!overshoot)
        return false;
    // seek が区間の開始を越えた。先頭から読み直す (仕事は増えるが時刻は正確)。
    error.clear();
    return preparePass(request, cancel, false, samples, error, counted, overshoot);
}
} // namespace mvm::transcribe

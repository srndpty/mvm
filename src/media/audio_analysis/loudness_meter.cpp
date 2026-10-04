#include "media/audio_analysis/loudness_meter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
}

namespace mvm::audio {
struct LoudnessMeter::State {
    AVFilterGraph* graph = nullptr;

    struct Channel {
        AVFilterContext* source = nullptr;
        AVFilterContext* sink = nullptr;
        AVFilterContext* meter = nullptr;
        bool observedOutput = false;
    };

    Channel loudness, peak;
    AVFrame* output = av_frame_alloc();
    std::int64_t samples = 0;
    bool nonzero = false;
    bool finished = false;

    ~State() {
        av_frame_free(&output);
        avfilter_graph_free(&graph);
    }

    bool drain(Channel& channel, std::string& error) {
        int status = 0;
        while ((status = av_buffersink_get_frame(channel.sink, output)) >= 0) {
            channel.observedOutput = true;
            av_frame_unref(output);
        }
        if (status != AVERROR(EAGAIN) && status != AVERROR_EOF) {
            error = "ラウドネス測定の出力を取得できません";
            return false;
        }
        return true;
    }

    bool create(Channel& channel, const char* prefix, const char* options, std::string& error) {
        const std::string name(prefix);
        if (avfilter_graph_create_filter(
                &channel.source, avfilter_get_by_name("abuffer"), (name + "Source").c_str(),
                "time_base=1/48000:sample_rate=48000:sample_fmt=flt:channel_layout=stereo", nullptr,
                graph) < 0 ||
            avfilter_graph_create_filter(&channel.meter, avfilter_get_by_name("ebur128"),
                                         (name + "Meter").c_str(), options, nullptr, graph) < 0 ||
            avfilter_graph_create_filter(&channel.sink, avfilter_get_by_name("abuffersink"),
                                         (name + "Sink").c_str(), nullptr, nullptr, graph) < 0 ||
            avfilter_link(channel.source, 0, channel.meter, 0) < 0 ||
            avfilter_link(channel.meter, 0, channel.sink, 0) < 0) {
            error = "ラウドネス測定の入出力を作れません";
            return false;
        }
        return true;
    }

    bool feed(Channel& channel, const float* stereo, std::size_t frames, std::string& error) {
        AVFrame* frame = av_frame_alloc();
        if (!frame) {
            error = "測定する音声のメモリを確保できません";
            return false;
        }
        frame->format = AV_SAMPLE_FMT_FLT;
        frame->sample_rate = 48000;
        av_channel_layout_default(&frame->ch_layout, 2);
        frame->nb_samples = static_cast<int>(frames);
        frame->pts = samples;
        int status = av_frame_get_buffer(frame, 0);
        if (status >= 0) {
            std::memcpy(frame->data[0], stereo, frames * 2 * sizeof(float));
            status =
                av_buffersrc_add_frame_flags(channel.source, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
        }
        av_frame_free(&frame);
        if (status < 0) {
            error = "ラウドネス測定へ音声を渡せません";
            return false;
        }
        return drain(channel, error);
    }
};

LoudnessMeter::LoudnessMeter() = default;
LoudnessMeter::~LoudnessMeter() = default;

bool LoudnessMeter::start(std::string& error) {
    state_ = std::make_unique<State>();
    auto& s = *state_;
    s.graph = avfilter_graph_alloc();
    if (!s.graph || !s.output) {
        error = "ラウドネス測定のメモリを確保できません";
        return false;
    }
    s.graph->nb_threads = 1;
    const auto* source = avfilter_get_by_name("abuffer");
    const auto* sink = avfilter_get_by_name("abuffersink");
    if (!source || !sink || !avfilter_get_by_name("ebur128")) {
        error = "必須の FFmpeg フィルター ebur128 がありません";
        return false;
    }
    if (!s.create(s.loudness, "loudness", "metadata=0:peak=none:framelog=quiet", error) ||
        !s.create(s.peak, "peak", "metadata=0:peak=true:framelog=quiet", error) ||
        avfilter_graph_config(s.graph, nullptr) < 0) {
        error = "ラウドネス・true peak の測定フィルターを構成できません";
        return false;
    }
    double value = 0;
    if (av_opt_get_double(s.loudness.meter, "integrated", AV_OPT_SEARCH_CHILDREN, &value) < 0 ||
        av_opt_get_double(s.peak.meter, "true_peak", AV_OPT_SEARCH_CHILDREN, &value) < 0) {
        error = "FFmpeg に高精度のラウドネス・true peak 取得機能がありません";
        return false;
    }
    return true;
}

bool LoudnessMeter::push(const float* stereo, std::size_t frames, std::string& error) {
    if (!state_ || !state_->loudness.source || state_->finished || !stereo || frames == 0 ||
        frames > 48000) {
        error = "ラウドネス測定の PCM または状態が不正です";
        return false;
    }
    auto& s = *state_;
    for (std::size_t i = 0; i < frames * 2; ++i) {
        if (!std::isfinite(stereo[i])) {
            error = "測定する音声に有限値ではない sample があります";
            return false;
        }
        s.nonzero = s.nonzero || stereo[i] != 0;
    }
    if (!s.feed(s.loudness, stereo, frames, error) || !s.feed(s.peak, stereo, frames, error))
        return false;
    s.samples += static_cast<std::int64_t>(frames);
    return true;
}

bool LoudnessMeter::finish(LoudnessMeasurement& result, std::string& error) {
    if (!state_ || !state_->loudness.source || state_->finished || state_->samples == 0) {
        error = "ラウドネスを測定する音声がありません";
        return false;
    }
    auto& s = *state_;
    const auto measuredSamples = s.samples;
    s.finished = true;
    if (av_buffersrc_add_frame_flags(s.loudness.source, nullptr, 0) < 0 ||
        !s.drain(s.loudness, error)) {
        if (error.empty())
            error = "ラウドネス測定を完了できません";
        return false;
    }
    double integrated = std::numeric_limits<double>::quiet_NaN();
    if (!s.loudness.observedOutput || av_opt_get_double(s.loudness.meter, "integrated",
                                                        AV_OPT_SEARCH_CHILDREN, &integrated) < 0) {
        error = "ラウドネスの測定値が欠けています";
        return false;
    }
    // oversampling の遅延を吐き出す。補う無音は積分ラウドネスと使用区間に含めない。
    const std::array<float, 1024> tail{};
    if (!s.feed(s.peak, tail.data(), tail.size() / 2, error))
        return false;
    if (av_buffersrc_add_frame_flags(s.peak.source, nullptr, 0) < 0 || !s.drain(s.peak, error)) {
        if (error.empty())
            error = "ラウドネス測定を完了できません";
        return false;
    }
    result = {};
    result.samples = measuredSamples;
    // metadata の振幅値は小数 3 桁へ丸められるため、ピーク制限には使わない。
    double peakDb = std::numeric_limits<double>::quiet_NaN();
    if (!s.peak.observedOutput ||
        av_opt_get_double(s.peak.meter, "true_peak", AV_OPT_SEARCH_CHILDREN, &peakDb) < 0) {
        error = "ラウドネスまたは true peak の測定値が欠けています";
        return false;
    }
    // 400 ms の積分窓とゲートに達しない音声も「測定不能」として区別する。
    if (!s.nonzero || measuredSamples < 19200 || (std::isfinite(integrated) && integrated <= -70))
        return true;
    if (!std::isfinite(integrated) || !std::isfinite(peakDb)) {
        error = "ラウドネスまたは true peak の測定値が欠けています";
        return false;
    }
    result.measurable = true;
    result.integratedLufs = integrated;
    result.truePeakDb = peakDb;
    return true;
}
} // namespace mvm::audio

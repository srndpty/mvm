#include "media/gpu_preview/transition_probe_reference.h"

#include "media/gpu_preview/phase4_composition_catalog.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libavutil/sha.h>
}

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

namespace mvm::gpu {
namespace {

struct FrameSamples {
    Rgba8 tl;
    Rgba8 br;
    Rgba8 center;
};

double samplePlane(const uint8_t* data, int stride, int width, int height, double u, double v) {
    const double px = std::clamp(u * width - 0.5, 0.0, static_cast<double>(width - 1));
    const double py = std::clamp(v * height - 0.5, 0.0, static_cast<double>(height - 1));
    const int x0 = static_cast<int>(std::floor(px));
    const int y0 = static_cast<int>(std::floor(py));
    const int x1 = std::min(x0 + 1, width - 1);
    const int y1 = std::min(y0 + 1, height - 1);
    const double fx = px - x0;
    const double fy = py - y0;
    const auto value = [&](int x, int y) { return static_cast<double>(data[y * stride + x]); };
    return (value(x0, y0) * (1.0 - fx) + value(x1, y0) * fx) * (1.0 - fy) +
           (value(x0, y1) * (1.0 - fx) + value(x1, y1) * fx) * fy;
}

Rgba8 convert709(double y, double u, double v) {
    return bt709Limited(y, u, v);
}

Rgba8 sampleYuv420(const AVFrame* frame, double u, double v) {
    const double y =
        samplePlane(frame->data[0], frame->linesize[0], frame->width, frame->height, u, v);
    const int cw = (frame->width + 1) / 2;
    const int ch = (frame->height + 1) / 2;
    const double uu = samplePlane(frame->data[1], frame->linesize[1], cw, ch, u, v);
    const double vv = samplePlane(frame->data[2], frame->linesize[2], cw, ch, u, v);
    return convert709(y, uu, vv);
}

bool sha256File(const std::string& path, std::string& result, std::string& err) {
    const std::filesystem::path nativePath(reinterpret_cast<const char8_t*>(path.c_str()));
    std::ifstream stream(nativePath, std::ios::binary);
    if (!stream) {
        err = "CPU reference fixtureをhash用に開けません: " + path;
        return false;
    }
    AVSHA* sha = av_sha_alloc();
    if (!sha || av_sha_init(sha, 256) < 0) {
        av_free(sha);
        err = "CPU reference fixture SHA-256を初期化できません";
        return false;
    }
    std::array<char, 1 << 16> buffer{};
    while (stream) {
        stream.read(buffer.data(), buffer.size());
        const auto count = stream.gcount();
        if (count > 0)
            av_sha_update(sha, reinterpret_cast<const uint8_t*>(buffer.data()),
                          static_cast<size_t>(count));
    }
    std::array<uint8_t, 32> digest{};
    av_sha_final(sha, digest.data());
    av_free(sha);
    std::ostringstream text;
    text << std::hex << std::setfill('0');
    for (uint8_t byte : digest)
        text << std::setw(2) << static_cast<int>(byte);
    result = text.str();
    return true;
}

bool decodeSamples(const std::string& path, const std::set<long long>& targets,
                   std::map<long long, FrameSamples>& result, std::string& err) {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    auto cleanup = [&] {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
    };
    if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(format, nullptr) < 0) {
        err = "CPU reference fixtureを開けません: " + path;
        cleanup();
        return false;
    }
    const int stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream < 0) {
        err = "CPU reference fixtureにvideo streamがありません";
        cleanup();
        return false;
    }
    const AVCodecParameters* parameters = format->streams[stream]->codecpar;
    const AVCodec* decoder = avcodec_find_decoder(parameters->codec_id);
    codec = decoder ? avcodec_alloc_context3(decoder) : nullptr;
    if (!codec || avcodec_parameters_to_context(codec, parameters) < 0 ||
        avcodec_open2(codec, decoder, nullptr) < 0) {
        err = "CPU reference software decoderを開始できません";
        cleanup();
        return false;
    }
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!packet || !frame) {
        err = "CPU reference decode bufferを確保できません";
        cleanup();
        return false;
    }
    long long decoded = 0;
    const long long maximum = *targets.rbegin();
    auto receive = [&]() -> bool {
        for (;;) {
            const int rc = avcodec_receive_frame(codec, frame);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
                return true;
            if (rc < 0) {
                err = "CPU reference frame decodeに失敗しました";
                return false;
            }
            if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P) {
                err = "CPU reference fixtureがplanar YUV420ではありません";
                return false;
            }
            if (targets.contains(decoded)) {
                constexpr double tlU = (480.0 + 0.5) / 1920.0;
                constexpr double tlV = (270.0 + 0.5) / 1080.0;
                constexpr double brU = (1440.0 + 0.5) / 1920.0;
                constexpr double brV = (810.0 + 0.5) / 1080.0;
                constexpr double centerU = (480.0 + 0.5) / 960.0;
                constexpr double centerV = (270.0 + 0.5) / 540.0;
                result.emplace(decoded, FrameSamples{sampleYuv420(frame, tlU, tlV),
                                                     sampleYuv420(frame, brU, brV),
                                                     sampleYuv420(frame, centerU, centerV)});
            }
            ++decoded;
            av_frame_unref(frame);
        }
    };
    while (decoded <= maximum && av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == stream) {
            if (avcodec_send_packet(codec, packet) < 0 || !receive()) {
                av_packet_unref(packet);
                cleanup();
                return false;
            }
        }
        av_packet_unref(packet);
    }
    if (decoded <= maximum) {
        avcodec_send_packet(codec, nullptr);
        if (!receive()) {
            cleanup();
            return false;
        }
    }
    cleanup();
    if (result.size() != targets.size()) {
        err = "CPU reference候補frameをすべてdecodeできませんでした";
        return false;
    }
    return true;
}

} // namespace

std::optional<Rgba8> phase4ExpectedProbe(CompositionStateId state, TransitionProbePoint point,
                                         Rgba8 aTl, Rgba8 aBr, Rgba8 bCenter) {
    if (state == kPhase4S0)
        return point == TransitionProbePoint::TL ? aTl : straightAlphaBlend(bCenter, aBr, 0.75);
    if (state == kPhase4S1)
        return point == TransitionProbePoint::TL ? straightAlphaBlend(bCenter, aTl, 0.75) : aBr;
    if (state == kPhase4S2)
        return point == TransitionProbePoint::TL ? straightAlphaBlend(bCenter, aTl, 0.50) : aBr;
    if (state == kPhase4S3)
        return point == TransitionProbePoint::TL ? aTl : straightAlphaBlend(bCenter, aBr, 0.50);
    return std::nullopt;
}

const Phase4ProbeExpected* Phase4CpuReferenceSet::find(long long boundary, long long outputFrame,
                                                       TransitionProbePoint point) const {
    const auto found = std::find_if(candidates.begin(), candidates.end(), [&](const auto& value) {
        return value.boundary == boundary && value.outputFrame == outputFrame &&
               value.point == point;
    });
    return found == candidates.end() ? nullptr : &*found;
}

bool buildPhase4CpuReferences(Phase4ScheduleKind kind, const std::string& sourceA,
                              const std::string& sourceB, const std::string& expectedShaA,
                              const std::string& expectedShaB, Phase4CpuReferenceSet& output,
                              std::string& err) {
    output = {};
    if (!sha256File(sourceA, output.fixtureASha256, err) ||
        !sha256File(sourceB, output.fixtureBSha256, err))
        return false;
    if (output.fixtureASha256 != expectedShaA || output.fixtureBSha256 != expectedShaB) {
        err = "CPU reference fixture SHA-256が固定manifestと一致しません";
        return false;
    }
    const auto entries = phase4ScheduleEntries(kind);
    std::set<long long> targets;
    for (size_t i = 1; i < entries.size(); ++i)
        for (long long frame = entries[i].boundaryOutputFrame;
             frame <= entries[i].boundaryOutputFrame + 2; ++frame)
            targets.insert(frame);
    std::map<long long, FrameSamples> a;
    std::map<long long, FrameSamples> b;
    if (!decodeSamples(sourceA, targets, a, err) || !decodeSamples(sourceB, targets, b, err))
        return false;
    const auto schedule = phase4Schedule(kind);
    if (!schedule) {
        err = "canonical Phase 4 scheduleを構築できません";
        return false;
    }
    for (size_t i = 1; i < entries.size(); ++i) {
        const long long boundary = entries[i].boundaryOutputFrame;
        const auto state = schedule->resolve(boundary);
        if (!state) {
            err = "canonical Phase 4 scheduleからprobe stateを解決できません";
            return false;
        }
        for (long long frame = boundary; frame <= boundary + 2; ++frame) {
            const auto tl = phase4ExpectedProbe(*state, TransitionProbePoint::TL, a.at(frame).tl,
                                                a.at(frame).br, b.at(frame).center);
            const auto br = phase4ExpectedProbe(*state, TransitionProbePoint::BR, a.at(frame).tl,
                                                a.at(frame).br, b.at(frame).center);
            if (!tl || !br) {
                err = "canonical Phase 4 stateのprobe expectationを解決できません";
                return false;
            }
            output.candidates.push_back({boundary, frame, *state, TransitionProbePoint::TL, *tl});
            output.candidates.push_back({boundary, frame, *state, TransitionProbePoint::BR, *br});
        }
    }
    return true;
}

} // namespace mvm::gpu

#include "media/gpu_preview/ffmpeg_d3d11_decoder.h"

#include "media/gpu_preview/color_metadata.h"
#include "media/gpu_preview/timebase.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <atomic>
#include <cstdio>
#include <string>

namespace mvm::gpu {

const char* toString(DecodeStatus s) {
    switch (s) {
    case DecodeStatus::Ok:
        return "ok";
    case DecodeStatus::Eof:
        return "eof";
    case DecodeStatus::Again:
        return "again";
    case DecodeStatus::Error:
        return "error";
    }
    return "unknown";
}

const char* toString(OpenFailure f) {
    switch (f) {
    case OpenFailure::None:
        return "none";
    case OpenFailure::CannotOpenFile:
        return "cannot_open_file";
    case OpenFailure::NoVideoStream:
        return "no_video_stream";
    case OpenFailure::NoHardwareDecoder:
        return "no_hardware_decoder";
    case OpenFailure::HardwareContextFailed:
        return "hardware_context_failed";
    case OpenFailure::CodecOpenFailed:
        return "codec_open_failed";
    case OpenFailure::DeviceMismatch:
        return "device_mismatch";
    }
    return "unknown";
}

int expectedHwPixelFormatValue() {
    return static_cast<int>(AV_PIX_FMT_D3D11);
}

int chooseHwPixelFormat(const int* candidates, int count) {
    if (!candidates)
        return static_cast<int>(AV_PIX_FMT_NONE);
    for (int i = 0; i < count; i++) {
        if (candidates[i] == static_cast<int>(AV_PIX_FMT_NONE))
            break;
        if (candidates[i] == static_cast<int>(AV_PIX_FMT_D3D11))
            return static_cast<int>(AV_PIX_FMT_D3D11);
    }
    // software 形式へ落ちない。落ちると「絵は出るが zero-copy ではない」
    // という、最も気づきにくい失敗になる。
    return static_cast<int>(AV_PIX_FMT_NONE);
}

bool isExpectedHwFormat(int avPixelFormat) {
    return avPixelFormat == static_cast<int>(AV_PIX_FMT_D3D11);
}

namespace {

// resource / composition epoch は **プロセス全体で単調増加させる。**
//
// decoder インスタンスごとのカウンタにすると、別の decoder がどれも epoch 1 になり、
// SRV cache の key (epoch, texture, ...) が衝突する。
// texture のアドレスは pool 解放後に再利用されるので、衝突すると
// **前の pool 用の SRV を新しい pool のフレームに使ってしまう。**
//
// 実際に open/close soak で発覚した: 旧 epoch と判定されず 1 件も retire されず、
// SRV cache が 100 cycle で 400 entry まで増え続けた。
unsigned long long allocateResourceEpoch() {
    static std::atomic<unsigned long long> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::string avErr(const char* what, int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof buf);
    return std::string(what) + " に失敗しました: " + buf;
}

// software 側の画素形式 -> D3D11 の decode 出力形式。
//
// **codecpar->format は yuv420p である。** D3D11VA の出力は NV12 なので、
// 「nv12 でなければ非対応」と書くと H.264 が全部弾かれる (実際に弾かれた)。
// 判断材料は subsampling とビット深度であり、平面配置ではない。
GpuPixelFormat toGpuPixelFormat(AVPixelFormat swFormat) {
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(swFormat);
    if (!d || d->nb_components < 3)
        return GpuPixelFormat::Unknown;
    // D3D11VA が扱うのは 4:2:0 だけ。4:2:2 / 4:4:4 は P1 の対象外。
    if (d->log2_chroma_w != 1 || d->log2_chroma_h != 1)
        return GpuPixelFormat::Unknown;

    const int depth = d->comp[0].depth;
    if (depth <= 8)
        return GpuPixelFormat::NV12;
    if (depth <= 10)
        return GpuPixelFormat::P010;
    return GpuPixelFormat::Unknown;
}

// decode 結果そのものから形式を取る。open 時の推定より確実。
GpuPixelFormat pixelFormatOfHwFrame(const AVFrame* frame, GpuPixelFormat fallback) {
    if (!frame->hw_frames_ctx)
        return fallback;
    const auto* fc = reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
    const GpuPixelFormat f = toGpuPixelFormat(fc->sw_format);
    return f == GpuPixelFormat::Unknown ? fallback : f;
}

enum AVPixelFormat getFormatCallback(AVCodecContext* /*ctx*/, const enum AVPixelFormat* fmts) {
    // AVPixelFormat の配列を int として渡し、判定は共有ロジックへ委ねる。
    // ここに判定を書くと、テストで検査している経路と実行経路が別物になる。
    int buf[32];
    int n = 0;
    for (; fmts && fmts[n] != AV_PIX_FMT_NONE && n < 31; n++)
        buf[n] = static_cast<int>(fmts[n]);
    buf[n] = static_cast<int>(AV_PIX_FMT_NONE);
    return static_cast<AVPixelFormat>(chooseHwPixelFormat(buf, n + 1));
}

} // namespace

// --------------------------------------------------------------------------

struct FFmpegD3D11Decoder::Impl {
    SharedD3D11Device& shared;
    ReadbackCounters& counters;

    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    AVBufferRef* hwDevice = nullptr;
    AVPacket* packet = nullptr;
    int streamIndex = -1;

    VideoStreamInfo info;
    OpenFailure failure = OpenFailure::None;
    AdapterInfo decodeAdapter;
    unsigned long long decodeDevice = 0;

    // source 単位の seek / flush 世代。
    SourceGeneration generation{1};
    // open ごとに進む resource epoch。decode pool・SRV cache・texture の世代。
    // **composition epoch ではない (P1.2 §2)。**
    ResourceEpoch resourceEpoch{0};
    // この decoder が担当する source。インスタンスごとに一意。
    SourceId sourceId{};
    bool eofSent = false;
    bool eofReached = false;

    long long decoded = 0;
    long long swRejects = 0;
    long long decodeErrors = 0;
    // 1 回目の seek で行き過ぎ、戻して測り直した回数。
    // 0 なら「戻しは一度も要らなかった」。隠さずに出す。
    long long seekBackoffs = 0;

    // seek() が先読みしたフレーム。次の requestFrame がこれを返す。
    DecodedGpuFrame pending;
    bool hasPending = false;

    Impl(SharedD3D11Device& d, SourceId id, ReadbackCounters& c)
        : shared(d), counters(c), sourceId(id) {}

    ~Impl() { teardown(); }

    void teardown() {
        pending = DecodedGpuFrame{};
        hasPending = false;
        if (packet) {
            av_packet_free(&packet);
            packet = nullptr;
        }
        if (codec) {
            avcodec_free_context(&codec);
            codec = nullptr;
        }
        if (fmt) {
            avformat_close_input(&fmt);
            fmt = nullptr;
        }
        if (hwDevice) {
            av_buffer_unref(&hwDevice);
            hwDevice = nullptr;
        }
        streamIndex = -1;
        eofSent = eofReached = false;
    }

    bool createHwDevice(std::string& err);
    DecodeStatus pull(DecodedGpuFrame& out, std::string& err);
    bool wrapFrame(AVFrame* frame, DecodedGpuFrame& out, std::string& err);
};

bool FFmpegD3D11Decoder::Impl::createHwDevice(std::string& err) {
    if (!shared.valid()) {
        err = "共有 D3D11 device が未初期化です";
        return false;
    }

    hwDevice = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hwDevice) {
        err = "AVHWDeviceContext (D3D11VA) を確保できませんでした";
        return false;
    }

    auto* devCtx = reinterpret_cast<AVHWDeviceContext*>(hwDevice->data);
    auto* d3d = static_cast<AVD3D11VADeviceContext*>(devCtx->hwctx);

    // Qt の device をそのまま渡す。ここが P1 の主要仮説の実体である。
    // AVHWDeviceContext は解放時に必ず Release() するので、AddRef しておく。
    d3d->device = shared.device();
    d3d->device_context = shared.context();
    d3d->device->AddRef();
    d3d->device_context->AddRef();
    // video_device / video_context は FFmpeg に導出させる。

    // decode 出力 texture から shader resource view を作れるようにする。
    // **これが無いと表示手段が CPU readback しか残らない。**
    d3d->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    d3d->MiscFlags = 0;

    // decode thread と render thread を同じ lock で直列化する。
    // FFmpeg は再帰的にロックするので recursive_mutex を渡している。
    d3d->lock = &D3D11Lock::lockCallback;
    d3d->unlock = &D3D11Lock::unlockCallback;
    d3d->lock_ctx = &shared.lock();

    const int rc = av_hwdevice_ctx_init(hwDevice);
    if (rc < 0) {
        err = avErr("av_hwdevice_ctx_init(D3D11VA)", rc);
        av_buffer_unref(&hwDevice);
        hwDevice = nullptr;
        return false;
    }
    return true;
}

bool FFmpegD3D11Decoder::Impl::wrapFrame(AVFrame* frame, DecodedGpuFrame& out, std::string& err) {
    if (!isExpectedHwFormat(frame->format)) {
        // software frame は受け取らない。ここを通すと zero-copy が崩れる。
        swRejects++;
        const char* name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
        err = std::string("hardware frame ではありません (format=") + (name ? name : "?") +
              ")。software decode へは落ちません";
        return false;
    }

    auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    const auto index = static_cast<unsigned int>(reinterpret_cast<intptr_t>(frame->data[1]));
    if (!texture) {
        err = "decode 結果に ID3D11Texture2D がありません";
        return false;
    }

    // 実体を見る。設定値ではなく、返ってきた texture がどの device のものかを見る。
    if (!decodeAdapter.valid) {
        ID3D11Device* owner = nullptr;
        texture->GetDevice(&owner);
        if (!owner) {
            err = "decode texture の device を取得できませんでした";
            return false;
        }
        std::string qerr;
        const bool ok = queryAdapterInfo(owner, decodeAdapter, qerr);
        const bool samePointer = (owner == shared.device());
        decodeDevice = reinterpret_cast<unsigned long long>(owner);
        owner->Release();
        if (!ok) {
            err = "decode texture の adapter を取得できませんでした: " + qerr;
            return false;
        }
        if (!samePointer && !decodeAdapter.sameAdapterAs(shared.adapter())) {
            failure = OpenFailure::DeviceMismatch;
            err = "decode texture が Qt とは別の adapter に属しています";
            return false;
        }
    }

    long long pts = frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE)
        pts = frame->pts;
    if (pts == AV_NOPTS_VALUE)
        pts = kNoPts;

    out = DecodedGpuFrame{};
    out.pts = pts;
    out.timeBase = info.timeBase;
    out.frameNumber = ptsToFrameNumber(pts, info.startPts, info.timeBase, info.frameRate);
    out.width = frame->width;
    out.height = frame->height;
    out.pixelFormat = pixelFormatOfHwFrame(frame, info.pixelFormat);
    out.texture = texture;
    out.arrayIndex = index;

    const ColorDecision cd =
        decideColor(static_cast<int>(frame->colorspace), static_cast<int>(frame->color_range),
                    frame->width, frame->height);
    out.colorSpace = cd.space;
    out.colorRange = cd.range;
    out.colorSpaceInferred = cd.spaceInferred;
    out.colorRangeInferred = cd.rangeInferred;
    out.sourceId = sourceId;
    out.sourceGeneration = generation;
    out.resourceEpoch = resourceEpoch;

    // lifetime token: AVFrame の所有権をここへ移す。
    // token が生きている間、この texture slice は再利用されない。
    AVFrame* owned = frame;
    out.lifetime = FrameLifetimeToken(static_cast<void*>(owned), [](void* p) {
        auto* f = static_cast<AVFrame*>(p);
        av_frame_free(&f);
    });

    if (out.frameNumber < 0) {
        err = "PTS から frame number を決定できませんでした";
        return false;
    }
    return true;
}

DecodeStatus FFmpegD3D11Decoder::Impl::pull(DecodedGpuFrame& out, std::string& err) {
    if (!codec)
        return DecodeStatus::Error;

    for (;;) {
        AVFrame* frame = av_frame_alloc();
        if (!frame) {
            err = "AVFrame を確保できませんでした";
            return DecodeStatus::Error;
        }

        int rc = avcodec_receive_frame(codec, frame);
        if (rc == 0) {
            const bool ok = wrapFrame(frame, out, err);
            if (!ok) {
                // wrapFrame が lifetime を作る前に失敗した場合だけ解放が要る。
                if (!out.lifetime)
                    av_frame_free(&frame);
                decodeErrors++;
                return DecodeStatus::Error;
            }
            decoded++;
            return DecodeStatus::Ok;
        }
        av_frame_free(&frame);

        if (rc == AVERROR_EOF) {
            eofReached = true;
            return DecodeStatus::Eof;
        }
        if (rc != AVERROR(EAGAIN)) {
            decodeErrors++;
            err = avErr("avcodec_receive_frame", rc);
            return DecodeStatus::Error;
        }

        // 出力が無いので packet を送る。
        if (eofSent)
            continue; // すでに drain 中。次の receive で EOF になる。

        rc = av_read_frame(fmt, packet);
        if (rc == AVERROR_EOF) {
            avcodec_send_packet(codec, nullptr);
            eofSent = true;
            continue;
        }
        if (rc < 0) {
            decodeErrors++;
            err = avErr("av_read_frame", rc);
            return DecodeStatus::Error;
        }

        if (packet->stream_index != streamIndex) {
            av_packet_unref(packet);
            continue;
        }

        rc = avcodec_send_packet(codec, packet);
        av_packet_unref(packet);
        if (rc < 0 && rc != AVERROR(EAGAIN)) {
            decodeErrors++;
            err = avErr("avcodec_send_packet", rc);
            return DecodeStatus::Error;
        }
    }
}

// --------------------------------------------------------------------------

FFmpegD3D11Decoder::FFmpegD3D11Decoder(SharedD3D11Device& device, SourceId sourceId,
                                       ReadbackCounters* counters)
    : impl_(std::make_unique<Impl>(device, sourceId,
                                   counters ? *counters : globalReadbackCounters())) {}

FFmpegD3D11Decoder::~FFmpegD3D11Decoder() = default;

bool FFmpegD3D11Decoder::open(const std::string& utf8Path, std::string& err) {
    close();
    Impl& d = *impl_;
    d.failure = OpenFailure::None;

    // FFmpeg は Windows でも UTF-8 の path を受け取る。
    // ここで ANSI へ変換すると日本語パスが壊れる (Phase 0 の V10 と同じ扱い)。
    int rc = avformat_open_input(&d.fmt, utf8Path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        d.failure = OpenFailure::CannotOpenFile;
        err = avErr("avformat_open_input", rc);
        return false;
    }

    rc = avformat_find_stream_info(d.fmt, nullptr);
    if (rc < 0) {
        d.failure = OpenFailure::CannotOpenFile;
        err = avErr("avformat_find_stream_info", rc);
        close();
        return false;
    }

    const AVCodec* decoder = nullptr;
    rc = av_find_best_stream(d.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (rc < 0 || !decoder) {
        d.failure = OpenFailure::NoVideoStream;
        err = "映像 stream が見つかりません";
        close();
        return false;
    }
    d.streamIndex = rc;
    AVStream* st = d.fmt->streams[d.streamIndex];

    // D3D11VA の hwaccel を持つ config を探す。無ければ **落とさずに落ちる**。
    const AVCodecHWConfig* hw = nullptr;
    for (int i = 0;; i++) {
        const AVCodecHWConfig* c = avcodec_get_hw_config(decoder, i);
        if (!c)
            break;
        if (c->device_type == AV_HWDEVICE_TYPE_D3D11VA &&
            (c->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
            c->pix_fmt == AV_PIX_FMT_D3D11) {
            hw = c;
            break;
        }
    }
    if (!hw) {
        d.failure = OpenFailure::NoHardwareDecoder;
        err = std::string("codec '") + decoder->name +
              "' に D3D11VA の hardware decoder がありません。"
              "P1 は software decode へフォールバックしません";
        close();
        return false;
    }

    if (!d.createHwDevice(err)) {
        d.failure = OpenFailure::HardwareContextFailed;
        close();
        return false;
    }

    d.codec = avcodec_alloc_context3(decoder);
    if (!d.codec) {
        d.failure = OpenFailure::CodecOpenFailed;
        err = "AVCodecContext を確保できませんでした";
        close();
        return false;
    }
    rc = avcodec_parameters_to_context(d.codec, st->codecpar);
    if (rc < 0) {
        d.failure = OpenFailure::CodecOpenFailed;
        err = avErr("avcodec_parameters_to_context", rc);
        close();
        return false;
    }

    d.codec->pkt_timebase = st->time_base;
    d.codec->get_format = getFormatCallback;
    d.codec->hw_device_ctx = av_buffer_ref(d.hwDevice);
    // 表示側は GPU 完了まで frame を retire queue に保持する (P1.1 §1)。
    // その分だけ pool に余裕が要る。足りないと decoder が「空きが無い」で止まる。
    //
    // **「直近 N 枚を retain する」方式は P1.1 で廃止した。** 保持期間は
    // GPU の完了 serial が決めるので、8 は「実測で足りている上限側の余裕」であって
    // retain 深さではない。retirement_depth_peak を JSON で観測している
    // (実測では 2)。
    d.codec->extra_hw_frames = 8;
    d.codec->thread_count = 1; // hwaccel では frame thread を使わない

    rc = avcodec_open2(d.codec, decoder, nullptr);
    if (rc < 0) {
        d.failure = OpenFailure::CodecOpenFailed;
        err = avErr("avcodec_open2", rc);
        close();
        return false;
    }

    d.packet = av_packet_alloc();
    if (!d.packet) {
        d.failure = OpenFailure::CodecOpenFailed;
        err = "AVPacket を確保できませんでした";
        close();
        return false;
    }

    // --- stream info -------------------------------------------------------
    VideoStreamInfo vi;
    vi.width = d.codec->width;
    vi.height = d.codec->height;
    vi.codecName = decoder->name;
    vi.hwaccelName = "d3d11va";

    AVRational fr = st->r_frame_rate;
    if (fr.num <= 0 || fr.den <= 0)
        fr = st->avg_frame_rate;
    vi.frameRate = Rational{fr.num, fr.den};
    vi.timeBase = Rational{st->time_base.num, st->time_base.den};
    vi.startPts = (st->start_time == AV_NOPTS_VALUE) ? 0 : st->start_time;

    if (st->nb_frames > 0) {
        vi.frameCount = st->nb_frames;
    } else if (st->duration != AV_NOPTS_VALUE && vi.frameRate.valid() && vi.timeBase.valid()) {
        vi.frameCount = ptsToFrameNumber(
            st->start_time == AV_NOPTS_VALUE ? st->duration : st->start_time + st->duration,
            vi.startPts, vi.timeBase, vi.frameRate);
    } else {
        vi.frameCount = -1; // 不明。0 と書かない
    }

    // sw_pix_fmt は hwaccel が確定させる。まだ未確定なら codecpar から推測する。
    AVPixelFormat sw = d.codec->sw_pix_fmt;
    if (sw == AV_PIX_FMT_NONE)
        sw = static_cast<AVPixelFormat>(st->codecpar->format);
    vi.pixelFormat = toGpuPixelFormat(sw);
    if (vi.pixelFormat == GpuPixelFormat::Unknown) {
        // 8bit 4:2:0 / 10bit 4:2:0 以外は D3D11VA の decode 出力にならない。
        // ここへ来るのは 4:2:2 / 4:4:4 などで、P1 の対象外である。
        d.failure = OpenFailure::NoHardwareDecoder;
        const char* name = av_get_pix_fmt_name(sw);
        err = std::string("D3D11VA が扱えない画素形式です: ") + (name ? name : "?");
        close();
        return false;
    }

    const ColorDecision cd =
        decideColor(static_cast<int>(st->codecpar->color_space),
                    static_cast<int>(st->codecpar->color_range), vi.width, vi.height);
    vi.colorSpace = cd.space;
    vi.colorRange = cd.range;

    if (!vi.frameRate.valid() || !vi.timeBase.valid()) {
        d.failure = OpenFailure::CannotOpenFile;
        err = "frame rate または time base が不正です";
        close();
        return false;
    }

    d.info = vi;
    d.generation.value++;
    // open ごとに resource_epoch を進める (§4)。
    // 前の open で作った SRV / decode pool は別世代のものとして扱う。
    d.resourceEpoch = ResourceEpoch{allocateResourceEpoch()};
    return true;
}

DecodeStatus FFmpegD3D11Decoder::requestFrame(DecodedGpuFrame& out, std::string& err) {
    Impl& d = *impl_;
    if (!d.codec) {
        err = "decoder が open されていません";
        return DecodeStatus::Error;
    }
    if (d.hasPending) {
        out = d.pending;
        d.pending = DecodedGpuFrame{};
        d.hasPending = false;
        return DecodeStatus::Ok;
    }
    if (d.eofReached)
        return DecodeStatus::Eof;
    return d.pull(out, err);
}

bool FFmpegD3D11Decoder::seek(long long frameNumber, std::string& err) {
    Impl& d = *impl_;
    if (!d.codec) {
        err = "decoder が open されていません";
        return false;
    }
    if (frameNumber < 0) {
        err = "負の frame number へは seek できません";
        return false;
    }

    // time base / fps が不正なら、この先の PTS 計算はすべて無意味になる。
    // 先に 1 度だけ確かめる (実際の seek 先は下のループが計算する)。
    if (frameNumberToPts(frameNumber, d.info.startPts, d.info.timeBase, d.info.frameRate) ==
        kNoPts) {
        err = "frame number から PTS を作れませんでした";
        return false;
    }

    // --- 行き過ぎたら戻して測り直す --------------------------------------
    //
    // [事実] AVSEEK_FLAG_BACKWARD は「目標以前のキーフレームへ飛ぶ」ことを
    // 保証しない。mp4 の index は DTS で並んでおり、B frame があると
    // PTS != DTS になる。その結果、飛んだ先のキーフレームの **表示時刻が
    // 目標より後**になることがある。
    //
    // 実測: benchmark の 1080p60 HEVC で、frame 299 を要求すると 300 に着地した。
    // 1000 点のランダム seek のうち 65 点 (6.5%) が同じ形で外れた。
    // 同じ time_base / fps の H.264 では 1 件も起きない。B frame 構造の差である。
    //
    // 「>= なら成功」にすると、この 1 frame ずれを成功として飲み込む。
    // 飲み込めば marker 検査も通ってしまい、**編集点が 1 frame ずれる**という
    // 最も気づきにくい形の不具合になる。したがって:
    //
    //   - 着地は **完全一致のみ** を成功とする
    //   - 行き過ぎたら seek 先を手前へ下げて decode し直す
    //   - 先頭まで戻っても届かなければ失敗として報告する (黙って近い値を返さない)
    //
    // 戻し幅は 1 秒相当から 4 倍ずつ広げる。
    const long long fpsRound =
        (d.info.frameRate.num + d.info.frameRate.den / 2) / d.info.frameRate.den;
    const long long oneSecond = fpsRound > 0 ? fpsRound : 60;

    long long backoff = 0;
    for (int attempt = 0; attempt < 6; attempt++) {
        long long seekFrame = frameNumber - backoff;
        if (seekFrame < 0)
            seekFrame = 0;
        const long long seekPts =
            frameNumberToPts(seekFrame, d.info.startPts, d.info.timeBase, d.info.frameRate);

        const int rc = av_seek_frame(d.fmt, d.streamIndex, seekPts, AVSEEK_FLAG_BACKWARD);
        if (rc < 0) {
            err = avErr("av_seek_frame", rc);
            return false;
        }

        // **seek 後は必ず flush する。** しないと seek 前のフレームが
        // decoder の内部バッファから出てきて、seek が当たったように見える。
        flush();
        if (attempt > 0)
            d.seekBackoffs++;

        bool overshot = false;
        for (;;) {
            DecodedGpuFrame f;
            std::string derr;
            const DecodeStatus st = d.pull(f, derr);
            if (st == DecodeStatus::Eof) {
                err =
                    "seek 先が素材の末尾を超えています (frame " + std::to_string(frameNumber) + ")";
                return false;
            }
            if (st != DecodeStatus::Ok) {
                err = derr.empty() ? "seek 中の decode に失敗しました" : derr;
                return false;
            }
            if (f.frameNumber == frameNumber) {
                d.pending = f;
                d.hasPending = true;
                return true;
            }
            if (f.frameNumber > frameNumber) {
                overshot = true;
                break;
            }
            // f はここで破棄される (lifetime token が解放する)
        }

        if (!overshot)
            break;
        if (seekFrame == 0) {
            err =
                "先頭から decode しても frame " + std::to_string(frameNumber) + " へ到達できません";
            return false;
        }
        backoff = (backoff == 0) ? oneSecond : backoff * 4;
    }

    err = "frame " + std::to_string(frameNumber) + " へ着地できませんでした";
    return false;
}

void FFmpegD3D11Decoder::flush() {
    Impl& d = *impl_;
    if (!d.codec)
        return;
    avcodec_flush_buffers(d.codec);
    d.pending = DecodedGpuFrame{};
    d.hasPending = false;
    d.eofSent = false;
    d.eofReached = false;
    // 表示側が「seek 前のフレーム」を弾けるように generation を進める。
    d.generation.value++;
}

void FFmpegD3D11Decoder::close() {
    impl_->teardown();
    impl_->info = VideoStreamInfo{};
    impl_->decodeAdapter = AdapterInfo{};
    impl_->decodeDevice = 0;
}

const VideoStreamInfo& FFmpegD3D11Decoder::info() const {
    return impl_->info;
}

SourceId FFmpegD3D11Decoder::sourceId() const {
    return impl_->sourceId;
}

SourceGeneration FFmpegD3D11Decoder::sourceGeneration() const {
    return impl_->generation;
}

ResourceEpoch FFmpegD3D11Decoder::resourceEpoch() const {
    return impl_->resourceEpoch;
}

OpenFailure FFmpegD3D11Decoder::openFailure() const {
    return impl_->failure;
}

const AdapterInfo& FFmpegD3D11Decoder::decodeAdapter() const {
    return impl_->decodeAdapter;
}

unsigned long long FFmpegD3D11Decoder::decodeDevicePointer() const {
    return impl_->decodeDevice;
}

long long FFmpegD3D11Decoder::decodedFrameCount() const {
    return impl_->decoded;
}

long long FFmpegD3D11Decoder::softwareFrameRejectCount() const {
    return impl_->swRejects;
}

long long FFmpegD3D11Decoder::decodeErrorCount() const {
    return impl_->decodeErrors;
}

long long FFmpegD3D11Decoder::seekBackoffCount() const {
    return impl_->seekBackoffs;
}

} // namespace mvm::gpu

// mvm Phase 1 / P1 - 実 decode を伴う検査 CLI
//
// Qt も window も使わない。D3D11 device を自前で作り、
// FFmpeg (D3D11VA) の decode 結果が本当に GPU texture として出てくるか、
// marker が一致するか、CPU full-frame readback が 0 のままかを検査する。
//
// Qt との device 共有そのものは preview_spike が検査する
// (QRhi から device を取るのは window が要るため)。
// ここでは **decode 側の契約**だけを短時間で見る。
//
// 終了コード:
//   0 正常
//   2 使い方の誤り
//   3 検査結果が期待と違う
//   4 素材を開けない (壊れている / 映像が無い)
//   5 D3D11 device を用意できない
//   6 その codec に hardware decoder が無い
//
// 「0 以外なら失敗」ではなく、**意図した理由で失敗したこと**を照合できるように
// 理由ごとに分けている (Phase 0 の scripts/expect-exit.ps1 と同じ方針)。

#include "core/mvm_marker.h"
#include "media/gpu_preview/d3d11_shared_device.h"
#include "media/gpu_preview/ffmpeg_d3d11_decoder.h"
#include "media/gpu_preview/nv12_converter.h"
#include "media/gpu_preview/readback_counter.h"
#include "util/mvm_win_utf8.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace mvm::gpu;

namespace {

constexpr int kOk = 0;
constexpr int kUsage = 2;
constexpr int kMismatch = 3;
constexpr int kCannotOpen = 4;
constexpr int kNoDevice = 5;
constexpr int kNoHwDecoder = 6;

constexpr int kBandWidth = mvm::marker::kCellSize * mvm::marker::kCellCount; // 1216
constexpr int kBandHeight = mvm::marker::kCellSize;                          // 64

int fail(const std::string& msg, int code) {
    std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
    return code;
}

// 検査用の D3D11 device。Qt の device ではないが、
// decode 側の契約を見るには十分である (device 共有の検査は preview_spike)。
class OwnedDevice {
public:
    ~OwnedDevice() {
        shared.release();
        if (context_)
            context_->Release();
        if (device_)
            device_->Release();
    }

    bool create(std::string& err) {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL got{};
        const HRESULT hr =
            D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                              D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                              levels, 2, D3D11_SDK_VERSION, &device_, &got, &context_);
        if (FAILED(hr)) {
            char buf[128];
            std::snprintf(buf, sizeof buf, "D3D11CreateDevice に失敗 (0x%08lX)",
                          static_cast<unsigned long>(hr));
            err = buf;
            return false;
        }
        return shared.adopt(device_, context_, err);
    }

    SharedD3D11Device shared;

private:
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
};

int openFailureExit(OpenFailure f) {
    switch (f) {
    case OpenFailure::NoHardwareDecoder:
        return kNoHwDecoder;
    case OpenFailure::CannotOpenFile:
    case OpenFailure::NoVideoStream:
        return kCannotOpen;
    default:
        return kMismatch;
    }
}

// --------------------------------------------------------------------------
// decode: 先頭から連番でフレームが出てくること
// --------------------------------------------------------------------------
int cmdDecode(const std::string& media, int count) {
    OwnedDevice dev;
    std::string err;
    if (!dev.create(err))
        return fail(err, kNoDevice);

    FFmpegD3D11Decoder dec(dev.shared);
    if (!dec.open(media, err))
        return fail(err, openFailureExit(dec.openFailure()));

    std::fprintf(stdout, "codec=%s %dx%d frames=%lld pixfmt=%s space=%s range=%s\n",
                 dec.info().codecName.c_str(), dec.info().width, dec.info().height,
                 dec.info().frameCount, toString(dec.info().pixelFormat),
                 toString(dec.info().colorSpace), toString(dec.info().colorRange));

    for (int i = 0; i < count; i++) {
        DecodedGpuFrame f;
        const DecodeStatus st = dec.requestFrame(f, err);
        if (st != DecodeStatus::Ok)
            return fail(std::string("frame ") + std::to_string(i) + " が " + toString(st) +
                            (err.empty() ? "" : ": " + err),
                        kMismatch);
        if (f.frameNumber != i)
            return fail("frame number が連番ではありません: " + std::to_string(f.frameNumber),
                        kMismatch);
        if (!f.texture)
            return fail("decode 結果に texture がありません", kMismatch);
        if (f.pixelFormat != GpuPixelFormat::NV12 && f.pixelFormat != GpuPixelFormat::P010)
            return fail("hardware 形式ではありません", kMismatch);
        if (!f.lifetime)
            return fail("lifetime token がありません", kMismatch);

        // decode texture が SRV を作れる状態であること。
        // BindFlags の設定が効いていなければ、ここで分かる。
        D3D11_TEXTURE2D_DESC td{};
        f.texture->GetDesc(&td);
        if ((td.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0)
            return fail("decode texture に D3D11_BIND_SHADER_RESOURCE がありません", kMismatch);
    }

    if (dec.softwareFrameRejectCount() != 0)
        return fail("software frame を受け取りました", kMismatch);
    if (globalReadbackCounters().fullFrameReadbacks() != 0)
        return fail("CPU full-frame readback が発生しました", kMismatch);

    std::fprintf(stdout, "OK decode %d frames (full-frame readback=0)\n", count);
    return kOk;
}

// --------------------------------------------------------------------------
// seek: flush が効いていること
// --------------------------------------------------------------------------
int cmdSeek(const std::string& media, const std::vector<long long>& targets) {
    OwnedDevice dev;
    std::string err;
    if (!dev.create(err))
        return fail(err, kNoDevice);

    FFmpegD3D11Decoder dec(dev.shared);
    if (!dec.open(media, err))
        return fail(err, openFailureExit(dec.openFailure()));

    // seek 前に少し進めておく。decoder の内部バッファを埋めた状態で
    // seek しないと「flush が無くても通る」テストになる。
    for (int i = 0; i < 5; i++) {
        DecodedGpuFrame f;
        if (dec.requestFrame(f, err) != DecodeStatus::Ok)
            return fail("事前 decode に失敗: " + err, kMismatch);
    }

    unsigned long long prevGen = dec.generation();
    for (long long t : targets) {
        if (!dec.seek(t, err))
            return fail("seek " + std::to_string(t) + " に失敗: " + err, kMismatch);

        const unsigned long long gen = dec.generation();
        if (gen <= prevGen)
            return fail("seek で generation が進んでいません", kMismatch);
        prevGen = gen;

        DecodedGpuFrame f;
        if (dec.requestFrame(f, err) != DecodeStatus::Ok)
            return fail("seek 後の decode に失敗: " + err, kMismatch);
        if (f.frameNumber != t)
            return fail("seek 先が違います: 要求 " + std::to_string(t) + " / 着地 " +
                            std::to_string(f.frameNumber),
                        kMismatch);
        if (f.generation != gen)
            return fail("frame の generation が decoder と一致しません", kMismatch);

        // 続きが連番で出ること (flush 後の状態が正しいこと)
        if (dec.requestFrame(f, err) == DecodeStatus::Ok && f.frameNumber != t + 1)
            return fail("seek 後の続きが連番ではありません: " + std::to_string(f.frameNumber),
                        kMismatch);
    }

    std::fprintf(stdout, "OK seek %zu 点\n", targets.size());
    return kOk;
}

// --------------------------------------------------------------------------
// marker: 要求フレームと焼き込みマーカーが一致すること
// --------------------------------------------------------------------------
int cmdMarker(const std::string& media, const std::vector<long long>& frames) {
    OwnedDevice dev;
    std::string err;
    if (!dev.create(err))
        return fail(err, kNoDevice);

    FFmpegD3D11Decoder dec(dev.shared);
    if (!dec.open(media, err))
        return fail(err, openFailureExit(dec.openFailure()));

    ReadbackCounters counters;
    Nv12Converter conv;
    if (!conv.initialize(dev.shared, &counters, err))
        return fail("変換パスを初期化できません: " + err, kMismatch);

    int checked = 0;
    for (long long t : frames) {
        if (dec.info().frameCount > 0 && t >= dec.info().frameCount) {
            // 素材に無いフレームは黙って飛ばさない。飛ばしたことを出す。
            std::fprintf(stdout, "skip frame %lld (素材は %lld frame)\n", t, dec.info().frameCount);
            continue;
        }
        if (!dec.seek(t, err))
            return fail("seek " + std::to_string(t) + " に失敗: " + err, kMismatch);

        DecodedGpuFrame f;
        if (dec.requestFrame(f, err) != DecodeStatus::Ok)
            return fail("decode に失敗: " + err, kMismatch);

        std::vector<unsigned char> rgba;
        if (!conv.readMarkerBand(f, kBandWidth, kBandHeight, rgba, err))
            return fail("marker 帯を読めません: " + err, kMismatch);

        const mvm::marker::MarkerRead r = mvm::marker::readMarkerWithCellSize(
            rgba.data(), kBandWidth, kBandHeight, mvm::marker::kCellSize);
        if (!r.syncOk)
            return fail("marker の同期が取れません (frame " + std::to_string(t) + ", luma " +
                            std::to_string(r.lumaMin) + ".." + std::to_string(r.lumaMax) + ")",
                        kMismatch);
        if (r.value != t)
            return fail("marker 不一致: 要求 " + std::to_string(t) + " / 読み取り " +
                            std::to_string(r.value),
                        kMismatch);
        checked++;
    }

    if (checked == 0)
        return fail("検査対象が 0 件でした", kMismatch);

    // 帯 readback は band を増やし、full-frame は増やさない。
    if (counters.fullFrameReadbacks() != 0)
        return fail("CPU full-frame readback が発生しました", kMismatch);
    if (counters.markerBandReadbacks() != checked)
        return fail("marker 帯 readback の回数が検査数と一致しません", kMismatch);

    std::fprintf(stdout, "OK marker %d 点 (full-frame readback=%lld / band=%lld / gpu copy=%lld)\n",
                 checked, counters.fullFrameReadbacks(), counters.markerBandReadbacks(),
                 counters.gpuCopies());
    return kOk;
}

// --------------------------------------------------------------------------
// no-shader-bind: BindFlags の検査が効いていることの negative test
// --------------------------------------------------------------------------
// D3D11_BIND_SHADER_RESOURCE を持たない NV12 texture を変換パスへ渡す。
// 「SRV が作れないので CPU へ落とす」という退避をしていないことを確認する。
// この検査が無ければ、BindFlags 設定を外しても気づけない。
int cmdNoShaderBind() {
    OwnedDevice dev;
    std::string err;
    if (!dev.create(err))
        return fail(err, kNoDevice);

    ReadbackCounters counters;
    Nv12Converter conv;
    if (!conv.initialize(dev.shared, &counters, err))
        return fail("変換パスを初期化できません: " + err, kMismatch);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = 1920;
    td.Height = 1080;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = 0; // **わざと SHADER_RESOURCE を外す**

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev.shared.device()->CreateTexture2D(&td, nullptr, &tex)))
        return fail("検査用 NV12 texture を作れません", kMismatch);

    DecodedGpuFrame f;
    f.frameNumber = 0;
    f.pts = 0;
    f.timeBase = Rational{1, 60};
    f.width = 1920;
    f.height = 1080;
    f.pixelFormat = GpuPixelFormat::NV12;
    f.texture = tex;
    f.colorSpace = ColorSpace::BT709;
    f.colorRange = ColorRange::Limited;
    f.lifetime = FrameLifetimeToken(reinterpret_cast<void*>(1), [](void*) {});

    std::vector<unsigned char> rgba;
    const bool ok = conv.readMarkerBand(f, kBandWidth, kBandHeight, rgba, err);
    tex->Release();

    if (ok)
        return fail("SHADER_RESOURCE 無しの texture を受け入れてしまいました", kMismatch);
    if (counters.fullFrameReadbacks() != 0)
        return fail("失敗時に CPU readback へ退避しています", kMismatch);

    std::fprintf(stdout, "OK BindFlags 検査が効いています: %s\n", err.c_str());
    return kOk;
}

std::vector<long long> parseList(const char* s) {
    std::vector<long long> out;
    const char* p = s;
    while (*p) {
        char* end = nullptr;
        const long long v = std::strtoll(p, &end, 10);
        if (end == p)
            break;
        out.push_back(v);
        p = end;
        while (*p == ',' || *p == ' ')
            p++;
    }
    return out;
}

void usage() {
    std::fprintf(stderr, "使い方: mvm_test_gpu_decode <command> [args]\n"
                         "  decode <media> [count]        先頭から連番で decode できること\n"
                         "  seek <media> <f1,f2,...>      seek + flush が効いていること\n"
                         "  marker <media> <f1,f2,...>    marker 代表点が一致すること\n"
                         "  no-shader-bind                BindFlags 検査の negative test\n");
}

} // namespace

int main(int, char**) {
    // Windows の argv は ANSI コードページである。UTF-8 として扱うと
    // 日本語パスで avformat_open_input が "Invalid argument" で落ちる
    // (実際に落ちた)。Phase 0 と同じく GetCommandLineW から取り直す。
    mvm_enable_utf8_console();
    int argc = 0;
    char** argv = mvm_win_get_utf8_args(&argc);
    if (!argv) {
        std::fprintf(stderr, "コマンドライン引数を取得できませんでした\n");
        return kUsage;
    }

    struct ArgvGuard {
        char** v;
        int n;

        ~ArgvGuard() { mvm_win_free_utf8_args(v, n); }
    } guard{argv, argc};

    if (argc < 2) {
        usage();
        return kUsage;
    }
    const std::string cmd = argv[1];

    if (cmd == "no-shader-bind")
        return cmdNoShaderBind();

    if (cmd == "decode") {
        if (argc < 3) {
            usage();
            return kUsage;
        }
        const int count = (argc >= 4) ? std::atoi(argv[3]) : 5;
        if (count <= 0) {
            std::fprintf(stderr, "count は 1 以上にしてください\n");
            return kUsage;
        }
        return cmdDecode(argv[2], count);
    }

    if (cmd == "seek" || cmd == "marker") {
        if (argc < 4) {
            usage();
            return kUsage;
        }
        const std::vector<long long> list = parseList(argv[3]);
        if (list.empty()) {
            std::fprintf(stderr, "フレーム番号のリストが空です\n");
            return kUsage;
        }
        return (cmd == "seek") ? cmdSeek(argv[2], list) : cmdMarker(argv[2], list);
    }

    usage();
    return kUsage;
}

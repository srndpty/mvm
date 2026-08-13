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
#include "media/gpu_preview/decode_worker.h"
#include "media/gpu_preview/ffmpeg_d3d11_decoder.h"
#include "media/gpu_preview/gpu_completion.h"
#include "media/gpu_preview/nv12_converter.h"
#include "media/gpu_preview/readback_counter.h"
#include "util/mvm_win_utf8.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <psapi.h>
#include <sstream>
#include <string>
#include <thread>
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

    ID3D11Device* rawDevice() const { return device_; }

    ID3D11DeviceContext* rawContext() const { return context_; }

private:
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
};

// --------------------------------------------------------------------------
// color fixture manifest (行指向。scripts/make-color-fixtures.ps1 が生成)
// --------------------------------------------------------------------------
struct ColorPatchSpec {
    int index = 0;
    int yuv[3] = {0, 0, 0};
    int expected[3] = {0, 0, 0};
};

struct ColorFixture {
    std::string id;
    std::string relativePath;
    std::string matrix;
    std::string range;
    bool gate = false;
    int depth = 8;
    int patchSize = 0;
    int patchCount = 0;
    int tolerance = 0;
    std::vector<ColorPatchSpec> patches;
};

bool readWholeFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool parseColorFixture(const std::string& text, const std::string& id, ColorFixture& out,
                       std::string& err) {
    std::istringstream in(text);
    std::string line;
    bool found = false;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ls(line);
        std::string kind, rowId;
        ls >> kind >> rowId;
        if (rowId != id)
            continue;
        if (kind == "asset") {
            std::string gate;
            ls >> out.relativePath >> gate >> out.matrix >> out.range >> out.depth >>
                out.patchSize >> out.patchCount >> out.tolerance;
            out.id = id;
            out.gate = (gate == "gate");
            found = true;
        } else if (kind == "patch") {
            ColorPatchSpec p;
            ls >> p.index >> p.yuv[0] >> p.yuv[1] >> p.yuv[2] >> p.expected[0] >> p.expected[1] >>
                p.expected[2];
            out.patches.push_back(p);
        }
    }
    if (!found) {
        err = "manifest に fixture '" + id + "' がありません";
        return false;
    }
    if (out.patches.empty()) {
        // 「patch が 0 件なのに合格」を起こさない。
        err = "fixture '" + id + "' に patch がありません";
        return false;
    }
    return true;
}

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

    FFmpegD3D11Decoder dec(dev.shared, SourceId{1});
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

    FFmpegD3D11Decoder dec(dev.shared, SourceId{1});
    if (!dec.open(media, err))
        return fail(err, openFailureExit(dec.openFailure()));

    // seek 前に少し進めておく。decoder の内部バッファを埋めた状態で
    // seek しないと「flush が無くても通る」テストになる。
    for (int i = 0; i < 5; i++) {
        DecodedGpuFrame f;
        if (dec.requestFrame(f, err) != DecodeStatus::Ok)
            return fail("事前 decode に失敗: " + err, kMismatch);
    }

    SourceGeneration prevGen = dec.sourceGeneration();
    for (long long t : targets) {
        if (!dec.seek(t, err))
            return fail("seek " + std::to_string(t) + " に失敗: " + err, kMismatch);

        const SourceGeneration gen = dec.sourceGeneration();
        if (!(prevGen < gen))
            return fail("seek で generation が進んでいません", kMismatch);
        prevGen = gen;

        DecodedGpuFrame f;
        if (dec.requestFrame(f, err) != DecodeStatus::Ok)
            return fail("seek 後の decode に失敗: " + err, kMismatch);
        if (f.frameNumber != t)
            return fail("seek 先が違います: 要求 " + std::to_string(t) + " / 着地 " +
                            std::to_string(f.frameNumber),
                        kMismatch);
        if (f.sourceGeneration != gen)
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
// exact seek product contract
// --------------------------------------------------------------------------
int cmdSeekContract(const std::string& media, const std::vector<long long>& targets, int repeats) {
    OwnedDevice dev;
    std::string err;
    if (!dev.create(err))
        return fail(err, kNoDevice);

    FFmpegD3D11Decoder dec(dev.shared, SourceId{1});
    if (!dec.open(media, err))
        return fail(err, openFailureExit(dec.openFailure()));
    if (dec.info().frameCount <= 0)
        return fail("frame count が不明な素材では exact seek contract を検査できません",
                    kMismatch);

    SourceGeneration generation = dec.sourceGeneration();
    DecodedGpuFrame retained;
    int checked = 0;

    for (int repeat = 0; repeat < repeats; ++repeat) {
        for (long long target : targets) {
            if (target < 0 || target >= dec.info().frameCount)
                return fail("contract target が素材範囲外です", kUsage);

            if (!dec.seek(target, err))
                return fail("exact seek に失敗: " + err, kMismatch);
            if (!(generation < dec.sourceGeneration()))
                return fail("seek/flush で generation が進みませんでした", kMismatch);
            generation = dec.sourceGeneration();

            DecodedGpuFrame exactFrame;
            if (dec.requestFrame(exactFrame, err) != DecodeStatus::Ok)
                return fail("seek 後の exact frame を取得できません: " + err, kMismatch);
            if (exactFrame.frameNumber != target)
                return fail("exact seek が要求 frame へ完全一致しませんでした", kMismatch);
            if (exactFrame.sourceGeneration != generation)
                return fail("exact frame の generation が decoder と一致しません", kMismatch);

            // 前の seek で得た AVFrame token を保持したまま unrelated seek を完了しても、
            // hardware texture の ownership が壊れていないことを確認する。
            if (retained.lifetime) {
                D3D11_TEXTURE2D_DESC desc{};
                retained.texture->GetDesc(&desc);
                if (desc.Width == 0 || desc.Height == 0)
                    return fail("seek を跨いで保持した hardware frame が無効です", kMismatch);
            }
            retained = exactFrame;

            if (target + 1 < dec.info().frameCount) {
                DecodedGpuFrame nextFrame;
                if (dec.requestFrame(nextFrame, err) != DecodeStatus::Ok ||
                    nextFrame.frameNumber != target + 1)
                    return fail("seek 後の decoder state が連番を維持していません", kMismatch);
            }
            ++checked;
        }
    }

    // EOS の次を success や近傍 frame に縮退させない。失敗後にも先頭への
    // unrelated seek が完全一致し、stale frame が残らないことを検査する。
    const long long unavailable = dec.info().frameCount;
    std::string unavailableError;
    if (dec.seek(unavailable, unavailableError))
        return fail("素材末尾の次の frame を受理しました", kMismatch);
    if (unavailableError.empty())
        return fail("unavailable target が理由を報告しませんでした", kMismatch);

    if (!dec.seek(0, err))
        return fail("unavailable target 後の reset seek に失敗: " + err, kMismatch);
    DecodedGpuFrame zero;
    if (dec.requestFrame(zero, err) != DecodeStatus::Ok || zero.frameNumber != 0)
        return fail("unavailable target 後に stale frame が残りました", kMismatch);

    std::fprintf(stdout, "OK exact seek contract %d 点 (EOS、generation、lifetime)\n", checked);
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

    FFmpegD3D11Decoder dec(dev.shared, SourceId{1});
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

// --------------------------------------------------------------------------
// snapshot-race: GUI 側の getter が decoder 内部を無排他で読まないこと (§2)
// --------------------------------------------------------------------------
// TSAN は Windows / MSYS2 では使えない。代わりに **決定論的な回数**だけ
// 別スレッドから snapshot() を叩き、返ってきた値の内部整合を検査する。
//
// P1 の実装は info() / decodeAdapter() / lastError() が
// decode thread の書き換える実体への const 参照を返していた。
// std::string の書き換え中に読めば壊れた文字列が見えるので、
// 「codec 名が既知の文字列か」「幅が一定か」「decode 数が単調か」で検出できる。
int cmdSnapshotRace(const std::string& media, int iterations) {
    OwnedDevice dev;
    std::string err;
    if (!dev.create(err))
        return fail(err, kNoDevice);

    PreviewState state;
    if (!state.device.adopt(dev.rawDevice(), dev.rawContext(), err))
        return fail("state へ device を adopt できません: " + err, kNoDevice);
    state.deviceReady.store(true, std::memory_order_release);

    DecodeWorker worker(state);
    if (!worker.start(media, err))
        return fail("worker を開始できません: " + err, kCannotOpen);

    const DecoderSnapshot base = worker.snapshot();
    if (!base.open || base.info.width <= 0)
        return fail("open 直後の snapshot が不正です", kMismatch);

    std::atomic<bool> stop{false};
    std::atomic<long long> checks{0};
    std::atomic<long long> violations{0};
    std::string violationText;
    std::mutex violationMutex;

    auto note = [&](const std::string& what) {
        violations.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> g(violationMutex);
        if (violationText.empty())
            violationText = what;
    };

    std::thread reader([&] {
        long long lastDecoded = -1;
        while (!stop.load(std::memory_order_relaxed)) {
            const DecoderSnapshot s = worker.snapshot();
            checks.fetch_add(1, std::memory_order_relaxed);

            if (s.open) {
                if (s.info.width != base.info.width || s.info.height != base.info.height)
                    note("解像度が途中で変わりました (torn read)");
                if (s.info.codecName.empty() || s.info.codecName.size() > 32)
                    note("codec 名の長さが不正です (torn read)");
                for (char c : s.info.codecName)
                    if (c < 0x20 || c > 0x7e)
                        note("codec 名に印字不能文字があります (torn read)");
                if (s.decodedFrameCount < lastDecoded)
                    note("decodedFrameCount が減少しました");
                lastDecoded = s.decodedFrameCount;
            }
            for (char c : s.lastError)
                if (c == ' ')
                    note("lastError に NUL があります (torn read)");
        }
    });

    // decode を回しながら、表示側の役 (取り出して displayed にする) も務める。
    worker.play();
    for (int i = 0; i < iterations; i++) {
        DecodedGpuFrame f;
        if (state.queue.takeForDisplay(f))
            state.queue.noteDisplayed(f);
        if ((i % 64) == 0) {
            double ms = 0.0;
            std::string serr;
            worker.seekBlocking(i % 200, ms, serr); // seek も混ぜる (generation が動く)
        }
    }

    stop.store(true);
    reader.join();
    worker.stop();

    // 停止後の snapshot は「閉じた」状態を返すこと。
    const DecoderSnapshot after = worker.snapshot();
    if (after.open)
        return fail("stop 後も open のままです", kMismatch);

    std::fprintf(stdout, "snapshot 検査 %lld 回 / 違反 %lld 件\n", checks.load(),
                 violations.load());
    if (checks.load() <= 0)
        return fail("snapshot を 1 回も検査していません", kMismatch);
    if (violations.load() != 0)
        return fail("snapshot の整合が壊れました: " + violationText, kMismatch);

    std::fprintf(stdout, "OK snapshot-race\n");
    return kOk;
}

// --------------------------------------------------------------------------
// soak: open / close を繰り返しても resource が増え続けないこと (§4)
// --------------------------------------------------------------------------
// decoder を開き直すと decode pool の texture がまるごと入れ替わる。
// SRV cache が epoch を跨いで増え続ければ、長時間の編集で VRAM を食い潰す。
//
// **同一プロセス**で H.264 / HEVC を交互に開閉し、
//   - marker mismatch 0
//   - device lost 0
//   - handle 数 first / last
//   - PrivateUsage first / peak / last
//   - SRV cache entry が plateau すること
// を見る。PrivateUsage が増えた場合、機構を断定せず生データを残す。
int cmdSoak(const std::string& mediaA, const std::string& mediaB, int cycles,
            GpuCompletionBackend preferred) {
    OwnedDevice dev;
    std::string err;
    if (!dev.create(err))
        return fail(err, kNoDevice);

    ReadbackCounters counters;
    Nv12Converter conv;
    if (!conv.initialize(dev.shared, &counters, err))
        return fail("変換パスを初期化できません: " + err, kMismatch);

    GpuCompletionTracker completion;
    if (!completion.initialize(dev.shared, err, preferred))
        return fail("GPU 完了追跡を初期化できません: " + err, kMismatch);
    if (preferred == GpuCompletionBackend::EventQuery &&
        completion.backend() != GpuCompletionBackend::EventQuery)
        return fail("event query backend を強制できませんでした", kMismatch);
    GpuRetirementQueue retirement;
    std::fprintf(stdout, "gpu_completion_backend=%s\n", toString(completion.backend()));

    // 表示先の代わりに使う小さな offscreen RT。
    ID3D11Texture2D* rt = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = 640;
        td.Height = 360;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev.shared.device()->CreateTexture2D(&td, nullptr, &rt)) ||
            FAILED(dev.shared.device()->CreateRenderTargetView(rt, nullptr, &rtv)))
            return fail("soak 用の render target を作れません", kMismatch);
    }

    auto handleCount = [] {
        DWORD n = 0;
        GetProcessHandleCount(GetCurrentProcess(), &n);
        return static_cast<long long>(n);
    };
    auto privateUsage = [] {
        PROCESS_MEMORY_COUNTERS_EX pm{};
        pm.cb = sizeof pm;
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm),
                             sizeof pm);
        return static_cast<long long>(pm.PrivateUsage);
    };

    long long handlesFirst = 0, privFirst = 0, privPeak = 0;
    long long handlesMid = 0, privMid = 0;
    size_t srvPlateau = 0;
    long long markerMismatch = 0;
    const float clear[4] = {0, 0, 0, 1};
    const int kBandW = kBandWidth;
    const int kBandH = kBandHeight;

    for (int cycle = 0; cycle < cycles; cycle++) {
        const std::string& media = (cycle % 2 == 0) ? mediaA : mediaB;
        FFmpegD3D11Decoder dec(dev.shared, SourceId{static_cast<unsigned long long>(cycle + 1)},
                               &counters);
        if (!dec.open(media, err))
            return fail("cycle " + std::to_string(cycle) + " の open に失敗: " + err,
                        openFailureExit(dec.openFailure()));

        // 新しい epoch。旧 epoch の SRV は GPU 完了後に解放させる。
        conv.retireEntriesNotInEpoch(dec.resourceEpoch(), retirement);

        for (int i = 0; i < 4; i++) {
            DecodedGpuFrame f;
            if (dec.requestFrame(f, err) != DecodeStatus::Ok)
                return fail("cycle " + std::to_string(cycle) + " の decode に失敗: " + err,
                            kMismatch);
            if (!conv.drawToRenderTarget(f, rtv, 640, 360, true, clear, err))
                return fail("cycle " + std::to_string(cycle) + " の描画に失敗: " + err, kMismatch);
            const SubmissionResult sub = completion.signalSubmission();
            if (!sub.tracked())
                return fail("cycle " + std::to_string(cycle) +
                                " で GPU submission を追跡できませんでした",
                            kMismatch);
            conv.stampSubmissionSerial(sub.serial);
            retirement.retire(sub.serial, f.lifetime);
            retirement.poll(completion.polledCompletedSerial());

            if (i == 0) {
                std::vector<unsigned char> rgba;
                if (!conv.readMarkerBand(f, kBandW, kBandH, rgba, err))
                    return fail("cycle " + std::to_string(cycle) +
                                    " の marker 読み取りに失敗: " + err,
                                kMismatch);
                const mvm::marker::MarkerRead r = mvm::marker::readMarkerWithCellSize(
                    rgba.data(), kBandW, kBandH, mvm::marker::kCellSize);
                if (!r.syncOk || r.value != f.frameNumber)
                    markerMismatch++;
            }
        }

        long reason = 0;
        if (dev.shared.deviceLost(reason))
            return fail("cycle " + std::to_string(cycle) + " で device lost", kMismatch);

        if (cycle == 0) {
            handlesFirst = handleCount();
            privFirst = privateUsage();
        }
        const long long pu = privateUsage();
        if (pu > privPeak)
            privPeak = pu;
        // 後半 (定常状態) の SRV entry 数を plateau とみなす。
        // handle / PrivateUsage も中間点を採る。first と last だけでは
        // 「線形に増えている」のか「頭打ちになった」のかが分からない。
        if (cycle == cycles / 2) {
            srvPlateau = conv.srvCacheEntries();
            handlesMid = handleCount();
            privMid = privateUsage();
        }
    }

    // 終了時は有限 timeout で drain する。timeout は fail-closed。
    // **終端で 1 度だけ** flush する (毎 frame ではない)。
    completion.flushForShutdown();
    const bool drained = retirement.drain([&] { return completion.polledCompletedSerial(); }, 2000);
    const long long leftover = static_cast<long long>(retirement.releaseWithoutCompletion());

    const long long handlesLast = handleCount();
    const long long privLast = privateUsage();
    const size_t srvLast = conv.srvCacheEntries();

    rtv->Release();
    rt->Release();

    std::fprintf(stdout,
                 "cycles=%d\n"
                 "marker_mismatch=%lld\n"
                 "handles_first=%lld handles_mid=%lld handles_last=%lld\n"
                 "private_usage_first=%lld mid=%lld peak=%lld last=%lld\n"
                 "srv_cache_plateau=%zu srv_cache_last=%zu retired_srv_entries=%lld\n"
                 "srv_cache_texture_groups=%zu\n"
                 "retirement_drained=%s leftover=%lld timeout_count=%lld\n"
                 "frames_released_before_completion=%lld\n"
                 "cpu_full_frame_readback=%lld marker_band_readback=%lld\n",
                 cycles, markerMismatch, handlesFirst, handlesMid, handlesLast, privFirst, privMid,
                 privPeak, privLast, srvPlateau, srvLast, conv.retiredSrvEntries(),
                 conv.srvCacheTextureGroups(), drained ? "true" : "false", leftover,
                 retirement.retirementTimeoutCount(), retirement.payloadsReleasedBeforeCompletion(),
                 counters.fullFrameReadbacks(), counters.markerBandReadbacks());

    if (markerMismatch != 0)
        return fail("marker 不一致が " + std::to_string(markerMismatch) + " 件", kMismatch);
    if (counters.fullFrameReadbacks() != 0)
        return fail("CPU full-frame readback が発生しました", kMismatch);
    if (completion.untrackedSubmissionCount() != 0)
        return fail("追跡できない submission が " +
                        std::to_string(completion.untrackedSubmissionCount()) + " 件ありました",
                    kMismatch);
    if (completion.fatal())
        return fail("GPU 完了追跡が壊れました: " + completion.fatalReason(), kMismatch);
    if (!drained)
        return fail("GPU retirement を timeout 内に drain できませんでした", kMismatch);
    if (leftover != 0)
        return fail("drain 後に未完了の retirement が残っています", kMismatch);
    // **SRV cache が epoch を跨いで増え続けないこと。**
    // decode pool は 1 epoch あたり有界なので、cycle 数に比例して増えたら漏れている。
    // plateau (中盤) と last (終盤) を比べ、増えていたら失敗させる。
    if (srvLast > srvPlateau)
        return fail("SRV cache が増え続けています (中盤 " + std::to_string(srvPlateau) +
                        " -> 終盤 " + std::to_string(srvLast) + ")",
                    kMismatch);

    std::fprintf(stdout, "OK soak %d cycles\n", cycles);
    return kOk;
}

// --------------------------------------------------------------------------
// color: 既知の YUV patch が期待 RGB へ変換されること (§6)
// --------------------------------------------------------------------------
// marker 一致は color correctness の証拠にならない。
// marker は白 235 / 黒 16 なので、係数が多少ずれても読めてしまう。
// ここでは **既知の YUV** を **表示と同じ shader** で RGB 化して照合する。
//
// 期待値は manifest (scripts/make-color-fixtures.ps1 が標準式から独立に計算) から読む。
// 実装の coefficientsFor を呼んで期待値を作ると、実装のバグを追認してしまう。
int cmdColor(const std::string& manifestPath, const std::string& id) {
    std::string jsonText;
    if (!readWholeFile(manifestPath, jsonText))
        return fail("manifest を読めません: " + manifestPath, kCannotOpen);

    ColorFixture fx;
    std::string perr;
    if (!parseColorFixture(jsonText, id, fx, perr))
        return fail(perr, kUsage);

    // manifest からの相対パスで素材を探す
    std::string dir = manifestPath;
    const size_t slash = dir.find_last_of("/\\");
    dir = (slash == std::string::npos) ? std::string(".") : dir.substr(0, slash);
    const std::string media = dir + "/" + fx.relativePath;

    OwnedDevice dev;
    std::string err;
    if (!dev.create(err))
        return fail(err, kNoDevice);

    FFmpegD3D11Decoder dec(dev.shared, SourceId{1});
    if (!dec.open(media, err))
        return fail(err, openFailureExit(dec.openFailure()));

    ReadbackCounters counters;
    Nv12Converter conv;
    if (!conv.initialize(dev.shared, &counters, err))
        return fail("変換パスを初期化できません: " + err, kMismatch);

    DecodedGpuFrame f;
    if (dec.requestFrame(f, err) != DecodeStatus::Ok)
        return fail("decode に失敗: " + err, kMismatch);

    const int patchW = fx.patchSize * fx.patchCount;
    const int patchH = fx.patchSize;
    std::vector<unsigned char> rgba;
    if (!conv.readColorPatches(f, patchW, patchH, rgba, err))
        return fail("color patch を読めません: " + err, kMismatch);

    std::fprintf(stdout, "%s: matrix=%s range=%s depth=%d / 実際に選ばれた space=%s range=%s%s%s\n",
                 id.c_str(), fx.matrix.c_str(), fx.range.c_str(), fx.depth, toString(f.colorSpace),
                 toString(f.colorRange), f.colorSpaceInferred ? " (space は推定)" : "",
                 f.colorRangeInferred ? " (range は推定)" : "");

    int worst = 0;
    int mismatches = 0;
    for (const auto& p : fx.patches) {
        // patch 中心付近の 16x16 を平均する。境界は chroma の滲みが乗るので避ける。
        const int cx = p.index * fx.patchSize + fx.patchSize / 2;
        const int cy = fx.patchSize / 2;
        long sum[3] = {0, 0, 0};
        int n = 0;
        for (int y = cy - 8; y < cy + 8; y++) {
            for (int x = cx - 8; x < cx + 8; x++) {
                const size_t o = (static_cast<size_t>(y) * static_cast<size_t>(patchW) +
                                  static_cast<size_t>(x)) *
                                 4u;
                sum[0] += rgba[o];
                sum[1] += rgba[o + 1];
                sum[2] += rgba[o + 2];
                n++;
            }
        }
        const int actual[3] = {static_cast<int>(sum[0] / n), static_cast<int>(sum[1] / n),
                               static_cast<int>(sum[2] / n)};
        const int d[3] = {actual[0] - p.expected[0], actual[1] - p.expected[1],
                          actual[2] - p.expected[2]};
        const int maxd = std::max(std::max(std::abs(d[0]), std::abs(d[1])), std::abs(d[2]));
        if (maxd > worst)
            worst = maxd;
        const bool ok = maxd <= fx.tolerance;
        if (!ok)
            mismatches++;
        std::fprintf(stdout,
                     "  patch %d yuv=(%3d,%3d,%3d) 期待=(%3d,%3d,%3d) 実測=(%3d,%3d,%3d) "
                     "差=(%+d,%+d,%+d) %s\n",
                     p.index, p.yuv[0], p.yuv[1], p.yuv[2], p.expected[0], p.expected[1],
                     p.expected[2], actual[0], actual[1], actual[2], d[0], d[1], d[2],
                     ok ? "OK" : "**NG**");
    }

    if (counters.fullFrameReadbacks() != 0)
        return fail("CPU full-frame readback が発生しました", kMismatch);
    if (fx.patches.empty())
        return fail("検査対象の patch が 0 件でした", kMismatch);
    if (mismatches != 0)
        return fail(std::to_string(mismatches) + " 件の patch が許容誤差 " +
                        std::to_string(fx.tolerance) + " を超えました (最大差 " +
                        std::to_string(worst) + ")",
                    kMismatch);

    std::fprintf(stdout, "OK color %zu patch (最大差 %d / 許容 %d)\n", fx.patches.size(), worst,
                 fx.tolerance);
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
                         "  seek-contract <media> <list> [repeat] exact seek 契約を検査\n"
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

    if (cmd == "color") {
        if (argc < 4) {
            usage();
            return kUsage;
        }
        return cmdColor(argv[2], argv[3]);
    }

    if (cmd == "snapshot-race") {
        if (argc < 4) {
            usage();
            return kUsage;
        }
        const int iters = std::atoi(argv[3]);
        if (iters <= 0) {
            std::fprintf(stderr, "iterations は 1 以上にしてください\n");
            return kUsage;
        }
        return cmdSnapshotRace(argv[2], iters);
    }

    if (cmd == "soak") {
        if (argc < 5) {
            usage();
            return kUsage;
        }
        const int cycles = std::atoi(argv[4]);
        if (cycles <= 0) {
            std::fprintf(stderr, "cycles は 1 以上にしてください\n");
            return kUsage;
        }
        // 5 番目の引数で backend を強制できる (fallback 経路を実際に走らせる)。
        GpuCompletionBackend preferred = GpuCompletionBackend::Fence;
        if (argc >= 6) {
            const std::string b = argv[5];
            if (b == "event_query")
                preferred = GpuCompletionBackend::EventQuery;
            else if (b != "fence") {
                std::fprintf(stderr, "backend は fence か event_query です\n");
                return kUsage;
            }
        }
        return cmdSoak(argv[2], argv[3], cycles, preferred);
    }

    if (cmd == "seek" || cmd == "marker" || cmd == "seek-contract") {
        if (argc < 4) {
            usage();
            return kUsage;
        }
        const std::vector<long long> list = parseList(argv[3]);
        if (list.empty()) {
            std::fprintf(stderr, "フレーム番号のリストが空です\n");
            return kUsage;
        }
        if (cmd == "seek-contract") {
            const int repeats = argc >= 5 ? std::atoi(argv[4]) : 1;
            if (repeats <= 0) {
                std::fprintf(stderr, "repeat は 1 以上にしてください\n");
                return kUsage;
            }
            return cmdSeekContract(argv[2], list, repeats);
        }
        return (cmd == "seek") ? cmdSeek(argv[2], list) : cmdMarker(argv[2], list);
    }

    usage();
    return kUsage;
}

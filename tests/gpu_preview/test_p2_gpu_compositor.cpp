// mvm Phase 1 / P2-C1 - offscreen D3D11 GPU compositor 統合検査
#include "core/mvm_marker.h"
#include "media/gpu_preview/gpu_compositor.h"
#include "media/gpu_preview/source_decode_worker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace mvm::gpu;

namespace {
constexpr int kW = 1920;
constexpr int kH = 1080;
constexpr int kMarkerW = mvm::marker::kCellSize * mvm::marker::kCellCount;
constexpr int kMarkerH = mvm::marker::kCellSize;

int fail(const std::string& text, int code = 3) {
    std::fprintf(stderr, "FAIL: %s\n", text.c_str());
    return code;
}

class OwnedDevice {
public:
    ~OwnedDevice() {
        shared.release();
        if (context)
            context->Release();
        if (device)
            device->Release();
    }

    bool create(std::string& err) {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selected{};
        const HRESULT rc =
            D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                              D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                              levels, 2, D3D11_SDK_VERSION, &device, &selected, &context);
        if (FAILED(rc)) {
            err = "D3D11 hardware deviceを生成できません";
            return false;
        }
        return shared.adopt(device, context, err);
    }

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    SharedD3D11Device shared;
};

struct OwnedNv12 {
    ID3D11Texture2D* texture = nullptr;

    ~OwnedNv12() {
        if (texture)
            texture->Release();
    }
};

bool makeNv12(ID3D11Device* device, int width, int height, unsigned char yLeft, unsigned char uLeft,
              unsigned char vLeft, unsigned char yRight, unsigned char uRight, unsigned char vRight,
              OwnedNv12& out, std::string& err) {
    const size_t rowBytes = static_cast<size_t>(width);
    std::vector<unsigned char> bytes(rowBytes * static_cast<size_t>(height) * 3 / 2);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            bytes[static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x)] =
                x < width / 2 ? yLeft : yRight;
    unsigned char* uv = bytes.data() + rowBytes * static_cast<size_t>(height);
    for (int y = 0; y < height / 2; ++y)
        for (int x = 0; x < width; x += 2) {
            const bool left = x < width / 2;
            uv[static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x)] = left ? uLeft : uRight;
            uv[static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x + 1)] =
                left ? vLeft : vRight;
        }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{bytes.data(), static_cast<UINT>(width),
                                static_cast<UINT>(bytes.size())};
    const HRESULT rc = device->CreateTexture2D(&td, &init, &out.texture);
    if (FAILED(rc)) {
        err = "deterministic NV12 textureを生成できません";
        return false;
    }
    return true;
}

DecodedGpuFrame fixtureFrame(OwnedNv12& texture, SourceId source, ResourceEpoch epoch,
                             ColorSpace space = ColorSpace::BT709) {
    DecodedGpuFrame f;
    f.frameNumber = 0;
    f.pts = 0;
    f.timeBase = {1, 60};
    f.width = 16;
    f.height = 16;
    f.pixelFormat = GpuPixelFormat::NV12;
    f.texture = texture.texture;
    f.colorSpace = space;
    f.colorRange = ColorRange::Limited;
    f.sourceId = source;
    f.sourceGeneration = {1};
    f.resourceEpoch = epoch;
    f.lifetime = std::make_shared<int>(1);
    return f;
}

struct Rgb {
    int r;
    int g;
    int b;
};

Rgb expected709(int y, int u, int v) {
    // shader helperを使わない、BT.709 limitedの独立した標準式。
    const double c = y - 16.0, d = u - 128.0, e = v - 128.0;
    auto q = [](double value) { return std::clamp(static_cast<int>(std::lround(value)), 0, 255); };
    return {q(1.164383 * c + 1.792741 * e), q(1.164383 * c - 0.213249 * d - 0.532909 * e),
            q(1.164383 * c + 2.112402 * d)};
}

Rgb expected601(int y, int u, int v) {
    const double c = y - 16.0, d = u - 128.0, e = v - 128.0;
    auto q = [](double value) { return std::clamp(static_cast<int>(std::lround(value)), 0, 255); };
    return {q(1.164383 * c + 1.596027 * e), q(1.164383 * c - 0.391762 * d - 0.812968 * e),
            q(1.164383 * c + 2.017232 * d)};
}

Rgb blend(Rgb source, Rgb destination, double opacity) {
    auto q = [opacity](int s, int d) {
        return static_cast<int>(std::lround(s * opacity + d * (1.0 - opacity)));
    };
    return {q(source.r, destination.r), q(source.g, destination.g), q(source.b, destination.b)};
}

bool probeEquals(GpuCompositor& c, int x, int y, Rgb expected, std::string& err) {
    std::vector<unsigned char> rgba;
    if (!c.readOutputProbe(x, y, 1, 1, rgba, err))
        return false;
    for (int channel = 0; channel < 3; ++channel) {
        const int want[] = {expected.r, expected.g, expected.b};
        if (std::abs(static_cast<int>(rgba[static_cast<size_t>(channel)]) - want[channel]) > 3) {
            err = "output probe RGBが±3を超えました (位置 " + std::to_string(x) + "," +
                  std::to_string(y) + ")";
            return false;
        }
    }
    if (rgba[3] != 255) {
        err = "preview output alphaがopaqueではありません";
        return false;
    }
    return true;
}

ComposedFrame composition(const DecodedGpuFrame& a, const DecodedGpuFrame& b, float opacity,
                          RectF uv = {0, 0, 1, 1}) {
    return {
        a.frameNumber,
        {1},
        {{a, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0f, 0}, {b, {0.5f, 0.5f, 0.5f, 0.5f}, uv, opacity, 1}}};
}

bool runFixtureCases(GpuCompositor& compositor, OwnedDevice& owned, std::string& err) {
    OwnedNv12 aTexture, bTexture;
    if (!makeNv12(owned.device, 16, 16, 81, 90, 240, 81, 90, 240, aTexture, err) ||
        !makeNv12(owned.device, 16, 16, 145, 54, 34, 41, 240, 110, bTexture, err))
        return false;
    const DecodedGpuFrame a = fixtureFrame(aTexture, {1}, {1});
    const DecodedGpuFrame b = fixtureFrame(bTexture, {2}, {2});
    const Rgb rgbA = expected709(81, 90, 240);
    const Rgb rgbBLeft = expected709(145, 54, 34);
    const Rgb rgbBRight = expected709(41, 240, 110);

    std::fprintf(stderr, "[compositor_two_layers]\n[compositor_clear_once]\n"
                         "[compositor_z_order]\n[compositor_destination_rect]\n");
    if (!compositor.compose(composition(a, b, 0.75f), err) ||
        !probeEquals(compositor, 100, 100, rgbA, err) ||
        !probeEquals(compositor, 1500, 800, blend(rgbBRight, rgbA, 0.75), err) ||
        !probeEquals(compositor, 960, 540, blend(rgbBLeft, rgbA, 0.75), err) ||
        !probeEquals(compositor, 959, 539, rgbA, err))
        return false;

    std::fprintf(stderr, "[compositor_opacity]\n");
    for (float opacity : {0.5f, 0.0f, 1.0f}) {
        if (!compositor.compose(composition(a, b, opacity), err) ||
            !probeEquals(compositor, 1500, 800, blend(rgbBRight, rgbA, opacity), err))
            return false;
    }

    std::fprintf(stderr, "[compositor_source_uv]\n");
    if (!compositor.compose(composition(a, b, 1.0f, {0.5f, 0, 0.5f, 1}), err) ||
        !probeEquals(compositor, 1500, 800, rgbBRight, err))
        return false;

    std::fprintf(stderr, "[compositor_color_metadata_per_source]\n");
    // A=BT.601、B=BT.709を同一compositionへ入れ、各source metadataが独立に
    // constant bufferへ反映されることを標準式の期待値で検査する。
    auto a601 = a;
    a601.colorSpace = ColorSpace::BT601;
    const Rgb rgbA601 = expected601(81, 90, 240);
    if (!compositor.compose(composition(a601, b, 0.75f), err) ||
        !probeEquals(compositor, 100, 100, rgbA601, err) ||
        !probeEquals(compositor, 1500, 800, blend(rgbBRight, rgbA601, 0.75), err))
        return false;

    std::fprintf(stderr, "[compositor_actual_device_validation]\n"
                         "[compositor_device_mismatch_negative]\n");
    OwnedDevice foreign;
    OwnedNv12 foreignTexture;
    if (!foreign.create(err) ||
        !makeNv12(foreign.device, 16, 16, 145, 54, 34, 145, 54, 34, foreignTexture, err))
        return false;
    const DecodedGpuFrame foreignFrame = fixtureFrame(foreignTexture, {3}, {3});
    const auto before = compositor.counters();
    if (compositor.compose(composition(a, foreignFrame, 0.75f), err)) {
        err = "foreign device layerを拒否しませんでした";
        return false;
    }
    if (compositor.counters().deviceMismatchCount != before.deviceMismatchCount + 1 ||
        compositor.counters().clearCount != before.clearCount ||
        compositor.counters().layerDrawCount != before.layerDrawCount) {
        err = "device mismatchでGPU draw開始前にfail-closedになりませんでした";
        return false;
    }
    err.clear();
    return true;
}

bool runMarkers(GpuCompositor& compositor, OwnedDevice& owned, ReadbackCounters& readbacks,
                const std::string& pathA, const std::string& pathB, std::string& err) {
    std::fprintf(stderr, "[compositor_marker_a]\n[compositor_marker_b]\n");
    SourceDecodeWorker workerA({101}, owned.shared, readbacks, 2);
    SourceDecodeWorker workerB({102}, owned.shared, readbacks, 2);
    if (!workerA.start(pathA, err) || !workerB.start(pathB, err))
        return false;
    const long long targets[] = {0, 1, 137, 299, 600, 1799, 3599};
    for (long long target : targets) {
        double elapsed = 0;
        if (!workerA.seekBlocking(target, elapsed, err) ||
            !workerB.seekBlocking(target, elapsed, err))
            return false;
        DecodedGpuFrame a, b;
        if (!workerA.buffer().takeExact(target, a) || !workerB.buffer().takeExact(target, b)) {
            err = "marker対象のexact frameを取得できません";
            return false;
        }
        for (const DecodedGpuFrame* frame : {&a, &b}) {
            std::vector<unsigned char> rgba;
            if (!compositor.readSourceMarker(*frame, kMarkerW, kMarkerH, rgba, err))
                return false;
            const auto marker = mvm::marker::readMarkerAuto(rgba.data(), kMarkerW, kMarkerH);
            if (!marker.syncOk || marker.value != target) {
                err = "GpuCompositor入力textureのmarkerがrequested frameと違います";
                return false;
            }
        }
        if (!compositor.compose(composition(a, b, 0.75f), err))
            return false;
    }
    workerA.stop();
    workerB.stop();
    if (!workerA.joined() || !workerB.joined()) {
        err = "decoder完全停止を確認できません";
        return false;
    }
    return true;
}

bool checkInvariants(const GpuCompositorCounters& c, std::string& err) {
    if (c.layerDrawCount != c.compositionDrawnCount * 2 ||
        c.clearCount != c.compositionDrawnCount ||
        c.gpuSubmissionCount != c.compositionDrawnCount || c.untrackedSubmissionCount != 0 ||
        c.completionPollFailureCount != 0 || c.retirementDepthAfterDrain != 0 ||
        c.payloadsReleasedBeforeCompletion != 0 || c.retirementTimeoutCount != 0) {
        err = "GpuCompositor counter invariantが成立しません";
        return false;
    }
    return true;
}

bool runLatentFailureCases(OwnedDevice& owned, ReadbackCounters& readbacks, std::string& err) {
    std::fprintf(stderr, "[compositor_initialize_rollback_negative]\n");
    for (auto fault :
         {GpuCompositorInitializeFault::Completion, GpuCompositorInitializeFault::TargetTexture,
          GpuCompositorInitializeFault::TargetRtv, GpuCompositorInitializeFault::TargetSrv}) {
        GpuCompositor compositor;
        compositor.setTestFaults({fault, -1});
        if (compositor.initialize(owned.shared, readbacks, kW, kH, err) || compositor.ready()) {
            err = "initialize fault後にready/resourceが残りました";
            return false;
        }
        compositor.setTestFaults({});
        err.clear();
        if (!compositor.initialize(owned.shared, readbacks, kW, kH, err) ||
            !compositor.shutdown(10000, err)) {
            err = "initialize rollback後に再initializeできません: " + err;
            return false;
        }
    }

    std::fprintf(stderr, "[compositor_partial_gpu_issue_negative]\n"
                         "[compositor_fatal_gate_negative]\n");
    OwnedNv12 aTexture, bTexture;
    if (!makeNv12(owned.device, 16, 16, 81, 90, 240, 81, 90, 240, aTexture, err) ||
        !makeNv12(owned.device, 16, 16, 145, 54, 34, 145, 54, 34, bTexture, err))
        return false;
    auto a = fixtureFrame(aTexture, {1}, {20});
    auto b = fixtureFrame(bTexture, {2}, {21});
    GpuCompositor compositor;
    if (!compositor.initialize(owned.shared, readbacks, kW, kH, err))
        return false;
    compositor.setTestFaults({GpuCompositorInitializeFault::None, 1});
    if (compositor.compose(composition(a, b, 0.75f), err) || !compositor.fatal() ||
        compositor.counters().partialGpuIssueFailureCount != 1 ||
        compositor.counters().gpuSubmissionCount != 1) {
        err = "partial issueをtracked retirement付きfatalへ遷移できませんでした";
        return false;
    }
    const long long draws = compositor.counters().layerDrawCount;
    if (compositor.compose(composition(a, b, 0.75f), err) ||
        compositor.counters().composeAfterFatalRejectedCount != 1 ||
        compositor.counters().layerDrawCount != draws) {
        err = "fatal後のcomposeがGPU command発行前に拒否されませんでした";
        return false;
    }
    return compositor.shutdown(10000, err);
}

int run(const std::string& pathA, const std::string& pathB) {
    OwnedDevice owned;
    ReadbackCounters readbacks;
    std::string err;
    if (!owned.create(err))
        return fail(err, 5);

    if (!runLatentFailureCases(owned, readbacks, err))
        return fail(err);

    GpuCompositor fence;
    if (!fence.initialize(owned.shared, readbacks, kW, kH, err))
        return fail(err);
    if (fence.completionBackend() != GpuCompletionBackend::Fence)
        return fail("fence backendを実走できませんでした");
    if (!runFixtureCases(fence, owned, err) ||
        !runMarkers(fence, owned, readbacks, pathA, pathB, err))
        return fail(err);
    std::fprintf(stderr, "[compositor_output_probe]\n[compositor_aggregate_lifetime]\n"
                         "[compositor_submission_single_serial]\n[compositor_retirement]\n"
                         "[compositor_event_query]\n[compositor_shutdown_drain]\n");
    if (!fence.shutdown(10000, err) || !checkInvariants(fence.counters(), err))
        return fail(err);
    const auto fc = fence.counters();

    // fence対応機でもevent_queryを強制し、2 layer / submission / drainを実走する。
    GpuCompositor eventQuery;
    OwnedNv12 aTexture, bTexture;
    if (!makeNv12(owned.device, 16, 16, 81, 90, 240, 81, 90, 240, aTexture, err) ||
        !makeNv12(owned.device, 16, 16, 145, 54, 34, 145, 54, 34, bTexture, err) ||
        !eventQuery.initialize(owned.shared, readbacks, kW, kH, err,
                               GpuCompletionBackend::EventQuery))
        return fail(err);
    if (eventQuery.completionBackend() != GpuCompletionBackend::EventQuery)
        return fail("event_query backendを強制できませんでした");
    auto a = fixtureFrame(aTexture, {1}, {10});
    auto b = fixtureFrame(bTexture, {2}, {11});
    if (!eventQuery.compose(composition(a, b, 0.75f), err) || !eventQuery.shutdown(10000, err) ||
        !checkInvariants(eventQuery.counters(), err))
        return fail(err);
    const auto ec = eventQuery.counters();

    if (readbacks.fullFrameReadbacks() != 0 || readbacks.markerBandReadbacks() != 14)
        return fail("readback counterが契約と一致しません");
    std::fprintf(stdout,
                 "composition_requested_count=%lld\ncomposition_drawn_count=%lld\n"
                 "layer_draw_count=%lld\nclear_count=%lld\ndevice_mismatch_count=%lld\n"
                 "gpu_submission_count=%lld\nuntracked_submission_count=%lld\n"
                 "completion_poll_failure_count=%lld\nretirement_depth_peak=%zu\n"
                 "retirement_depth_after_drain=%zu\npayloads_released_before_completion=%lld\n"
                 "retirement_timeout_count=%lld\ncpu_full_frame_readback_count=%lld\n"
                 "marker_band_readback_count=%lld\noutput_probe_readback_count=%lld\n"
                 "marker_a_mismatch=0\nmarker_b_mismatch=0\noutput_probe_mismatch=0\n"
                 "partial_pair_consume_count=0\nfence_composition_count=%lld\n"
                 "event_query_composition_count=%lld\nevent_query_gpu_submission_count=%lld\n"
                 "event_query_untracked_submission_count=%lld\n"
                 "event_query_completion_poll_failure_count=%lld\n"
                 "event_query_retirement_depth_after_drain=%zu\n"
                 "event_query_payloads_released_before_completion=%lld\n"
                 "event_query_timeout_count=%lld\nsoftware_fallback_count=0\n"
                 "OK P2-C1 offscreen D3D11 GPU compositor\n",
                 fc.compositionRequestedCount, fc.compositionDrawnCount, fc.layerDrawCount,
                 fc.clearCount, fc.deviceMismatchCount, fc.gpuSubmissionCount,
                 fc.untrackedSubmissionCount, fc.completionPollFailureCount, fc.retirementDepthPeak,
                 fc.retirementDepthAfterDrain, fc.payloadsReleasedBeforeCompletion,
                 fc.retirementTimeoutCount, readbacks.fullFrameReadbacks(),
                 readbacks.markerBandReadbacks(), readbacks.outputProbeReadbacks(),
                 fc.compositionDrawnCount, ec.compositionDrawnCount, ec.gpuSubmissionCount,
                 ec.untrackedSubmissionCount, ec.completionPollFailureCount,
                 ec.retirementDepthAfterDrain, ec.payloadsReleasedBeforeCompletion,
                 ec.retirementTimeoutCount);
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3)
        return fail("使い方: mvm_test_p2_gpu_compositor <h264> <hevc>", 2);
    return run(argv[1], argv[2]);
}

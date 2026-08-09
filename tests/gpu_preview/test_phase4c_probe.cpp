#include "media/gpu_preview/gpu_compositor.h"
#include "media/gpu_preview/transition_probe.h"
#include "media/gpu_preview/transition_probe_reference.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace mvm::gpu;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(3);
    }
}

void candidateFrameSelection() {
    TransitionProbeSelector selector({200, 400});
    require(!selector.select(199), "boundary前を選択しました");
    require(selector.select(201) == 200, "drop後のfirst actual frameでboundary 200を選べません");
    require(!selector.select(202), "同じtransitionを再選択しました");
    require(selector.select(402) == 400, "候補b+2を選べません");
    require(selector.selectedCount() == 2, "選択transition数が2ではありません");
}

void duplicateSuppression() {
    TransitionProbeSelector selector({200});
    require(selector.select(200) == 200, "最初のtransitionを選択できません");
    for (long long frame : {200LL, 201LL, 202LL, 500LL})
        require(!selector.select(frame), "同じtransitionを複数回選択しました");
}

void expectedLocationsAndOpacity() {
    const Rgba8 aTl{10, 20, 30, 255};
    const Rgba8 aBr{100, 110, 120, 255};
    const Rgba8 b{210, 220, 230, 255};
    require(phase4ExpectedProbe({0}, TransitionProbePoint::TL, aTl, aBr, b) == aTl,
            "S0 TLはA-onlyではありません");
    require(phase4ExpectedProbe({0}, TransitionProbePoint::BR, aTl, aBr, b) ==
                straightAlphaBlend(b, aBr, 0.75),
            "S0 BRのopacityが0.75ではありません");
    require(phase4ExpectedProbe({1}, TransitionProbePoint::TL, aTl, aBr, b) ==
                straightAlphaBlend(b, aTl, 0.75),
            "S1 TLのopacityが0.75ではありません");
    require(phase4ExpectedProbe({1}, TransitionProbePoint::BR, aTl, aBr, b) == aBr,
            "S1 BRはA-onlyではありません");
    require(phase4ExpectedProbe({2}, TransitionProbePoint::TL, aTl, aBr, b) ==
                straightAlphaBlend(b, aTl, 0.50),
            "S2 TLのopacityが0.50ではありません");
    require(phase4ExpectedProbe({3}, TransitionProbePoint::BR, aTl, aBr, b) ==
                straightAlphaBlend(b, aBr, 0.50),
            "S3 BRのopacityが0.50ではありません");
}

void bt709Reference() {
    require(bt709Limited(81, 90, 240) == Rgba8{255, 24, 0, 255},
            "BT.709 limited標準式の期待値が違います");
}

void straightAlpha() {
    require(straightAlphaBlend({200, 100, 50, 255}, {100, 50, 0, 255}, 0.75) ==
                Rgba8{175, 88, 38, 255},
            "straight alpha 0.75が違います");
    require(straightAlphaBlend({200, 100, 50, 255}, {100, 50, 0, 255}, 0.50) ==
                Rgba8{150, 75, 25, 255},
            "straight alpha 0.50が違います");
}

void toleranceAndAlpha() {
    const Rgba8 expected{100, 110, 120, 255};
    require(probeWithinTolerance({103, 107, 123, 255}, expected), "RGB ±3境界を拒否しました");
    require(!probeWithinTolerance({104, 110, 120, 255}, expected), "RGB +4を受理しました");
    require(!probeWithinTolerance({100, 110, 120, 254}, expected), "alpha 254を受理しました");
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
        const HRESULT hr =
            D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                              D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                              levels, 2, D3D11_SDK_VERSION, &device, &selected, &context);
        if (FAILED(hr)) {
            err = "D3D11 hardware deviceを生成できません";
            return false;
        }
        return shared.adopt(device, context, err);
    }

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    SharedD3D11Device shared;
};

void asyncGpuCopy() {
    std::string err;
    OwnedDevice owned;
    require(owned.create(err), err.c_str());
    const unsigned char pixels[8] = {10, 20, 30, 255, 40, 50, 60, 255};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 2;
    desc.Height = 1;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA initial{pixels, 8, 8};
    ID3D11Texture2D* texture = nullptr;
    require(SUCCEEDED(owned.device->CreateTexture2D(&desc, &initial, &texture)),
            "RGBA8 test targetを生成できません");

    AsyncTransitionProbeReadback readback;
    ReadbackCounters readbackCounters;
    GpuCompositor compositor;
    require(compositor.initialize(owned.shared, readbackCounters, 2, 1, err), err.c_str());
    const auto fullFrameCopiesBefore = compositor.counters().fullFrameGpuCopyCount;
    require(readback.initialize(owned.shared, err), err.c_str());
    unsigned long long tlTicket = 0, brTicket = 0;
    require(readback.issue(texture, {200, 200, {1}, {8}, TransitionProbePoint::TL, 0, 0, {}},
                           tlTicket, err),
            err.c_str());
    require(readback.issue(texture, {200, 200, {1}, {8}, TransitionProbePoint::BR, 1, 0, {}},
                           brTicket, err),
            err.c_str());
    readback.setTestDeferCompletionPollOnce();
    std::vector<TransitionProbeResult> results;
    require(readback.poll(results, err), err.c_str());
    require(results.empty() && readback.counters().completedCount == 0,
            "completion前にMap/result取得しました");
    for (int i = 0; results.size() < 2 && i < 1000; ++i) {
        require(readback.poll(results, err), err.c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(results.size() == 2, "async 1x1 copyが完了しませんでした");
    require(results[0].request.point == TransitionProbePoint::TL &&
                results[0].rgba == std::array<unsigned char, 4>{10, 20, 30, 255},
            "TL probe identity/RGBAが違います");
    require(results[1].request.point == TransitionProbePoint::BR &&
                results[1].rgba == std::array<unsigned char, 4>{40, 50, 60, 255},
            "BR probe identity/RGBAが違います");
    std::vector<TransitionProbeResult> drained;
    require(readback.drain(5000, drained, err), err.c_str());
    const auto counters = readback.counters();
    require(counters.renderThreadBlockingWaitCount == 0 && counters.pendingAfterDrainCount == 0 &&
                counters.completionFailureCount == 0 && counters.untrackedSubmissionCount == 0,
            "async probeのcompletion/drain counterが0ではありません");
    require(compositor.counters().fullFrameGpuCopyCount == fullFrameCopiesBefore,
            "async 1x1 probeがfull-frame GPU copy counterを増やしました");
    readback.release();
    require(compositor.shutdown(5000, err), err.c_str());
    texture->Release();
}

struct Case {
    const char* name;
    void (*run)();
};

const Case cases[] = {{"CandidateFrameSelection", candidateFrameSelection},
                      {"DuplicateTransitionSuppression", duplicateSuppression},
                      {"ExpectedLocationsAndOpacity", expectedLocationsAndOpacity},
                      {"Bt709Reference", bt709Reference},
                      {"StraightAlpha", straightAlpha},
                      {"ToleranceAndAlpha", toleranceAndAlpha},
                      {"AsyncGpuCopy", asyncGpuCopy}};
} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "FixtureReference") {
        Phase4CpuReferenceSet references;
        std::string error;
        if (!buildPhase4SmokeCpuReferences(
                argv[2], argv[3],
                "d398114c38806f39670df51dfabb0095d462cbc35286ea1467901d4007cf0308",
                "fe7cd1a45d101d363cb3930497601efd41dd55fd36c194acf8f24e3e4728b479", references,
                error)) {
            std::fprintf(stderr, "FAIL: %s\n", error.c_str());
            return 3;
        }
        require(references.candidates.size() == 12, "CPU reference候補が12件ではありません");
        for (const auto& candidate : references.candidates)
            std::fprintf(stderr, "reference boundary=%lld frame=%lld point=%s rgba=%d,%d,%d,%d\n",
                         candidate.boundary, candidate.outputFrame,
                         transitionProbePointName(candidate.point), candidate.rgba.r,
                         candidate.rgba.g, candidate.rgba.b, candidate.rgba.a);
        return 0;
    }
    if (argc != 2) {
        std::fprintf(stderr, "使い方: mvm_test_phase4c_probe <test-name>\n");
        return 2;
    }
    for (const auto& test : cases) {
        if (test.name == std::string(argv[1])) {
            test.run();
            std::fprintf(stderr, "PASS: %s\n", test.name);
            return 0;
        }
    }
    std::fprintf(stderr, "未知testです: %s\n", argv[1]);
    return 2;
}

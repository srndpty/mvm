#include "media/gpu_preview/gpu_compositor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mvm::gpu {
namespace {
template<class T>
void safeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::string hrText(const char* what, HRESULT hr) {
    char text[160];
    std::snprintf(text, sizeof text, "%sに失敗しました (HRESULT=0x%08lX)", what,
                  static_cast<unsigned long>(hr));
    return text;
}
} // namespace

GpuCompositor::~GpuCompositor() {
    if (ready_) {
        std::string ignored;
        shutdown(5000, ignored);
    }
    releaseTarget();
}

void GpuCompositor::releaseTarget() {
    safeRelease(targetSrv_);
    safeRelease(targetRtv_);
    safeRelease(target_);
}

bool GpuCompositor::initialize(SharedD3D11Device& device, ReadbackCounters& readbacks, int width,
                               int height, std::string& err, GpuCompletionBackend backend) {
    if (ready_ || !device.valid() || width <= 0 || height <= 0) {
        err = "GpuCompositorの初期化引数が不正です";
        return false;
    }
    shared_ = &device;
    width_ = width;
    height_ = height;
    if (!converter_.initialize(device, &readbacks, err) ||
        !completion_.initialize(device, err, backend))
        return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT rc = device.device()->CreateTexture2D(&td, nullptr, &target_);
    if (SUCCEEDED(rc))
        rc = device.device()->CreateRenderTargetView(target_, nullptr, &targetRtv_);
    if (SUCCEEDED(rc))
        rc = device.device()->CreateShaderResourceView(target_, nullptr, &targetSrv_);
    if (FAILED(rc)) {
        err = hrText("offscreen render targetの生成", rc);
        releaseTarget();
        return false;
    }
    ready_ = true;
    return true;
}

bool GpuCompositor::validateAllLayers(const ComposedFrame& frame, std::string& err) {
    if (frame.layers.size() != 2) {
        err = "P2-C1 compositionは2 layer必須です";
        return false;
    }
    for (const auto& layer : frame.layers) {
        if (!layer.frame.valid() || layer.destination.width <= 0 || layer.destination.height <= 0 ||
            layer.sourceUv.width <= 0 || layer.sourceUv.height <= 0 || layer.opacity < 0.0f ||
            layer.opacity > 1.0f) {
            err = "composition layerの値が不正です";
            return false;
        }
        ID3D11Device* owner = nullptr;
        layer.frame.texture->GetDevice(&owner);
        const bool same = owner == shared_->device();
        if (owner)
            owner->Release();
        if (!same) {
            ++counters_.deviceMismatchCount;
            err = "layer textureの実owner deviceがshared deviceと一致しません";
            return false;
        }
    }
    return true;
}

bool GpuCompositor::compose(const ComposedFrame& frame, std::string& err) {
    ++counters_.compositionRequestedCount;
    if (!ready_) {
        err = "GpuCompositorが初期化されていません";
        return false;
    }
    // validation passはGPU command発行より必ず先に完了させる。
    if (!validateAllLayers(frame, err))
        return false;
    {
        std::lock_guard<D3D11Lock> guard(shared_->lock());
        const float clear[4] = {0, 0, 0, 1};
        shared_->context()->ClearRenderTargetView(targetRtv_, clear);
        ++counters_.clearCount;
    }
    for (const auto& layer : frame.layers) {
        const FitRect destination{
            static_cast<int>(std::lround(layer.destination.x * static_cast<float>(width_))),
            static_cast<int>(std::lround(layer.destination.y * static_cast<float>(height_))),
            static_cast<int>(std::lround(layer.destination.width * static_cast<float>(width_))),
            static_cast<int>(std::lround(layer.destination.height * static_cast<float>(height_)))};
        const float uv[4] = {layer.sourceUv.x, layer.sourceUv.y, layer.sourceUv.width,
                             layer.sourceUv.height};
        if (!converter_.drawLayer(layer.frame, targetRtv_, destination, uv, layer.opacity, true,
                                  err))
            return false;
        ++counters_.layerDrawCount;
    }
    const SubmissionResult submission = completion_.signalSubmission();
    if (!submission.tracked()) {
        ++counters_.untrackedSubmissionCount;
        retirement_.retire(kNeverCompletingSerial, aggregateLifetime(frame));
        err = "composition submissionをGPU完了trackerで追跡できません";
        return false;
    }
    ++counters_.gpuSubmissionCount;
    ++counters_.compositionDrawnCount;
    converter_.stampSubmissionSerial(submission.serial);
    retirement_.retire(submission.serial, aggregateLifetime(frame));
    counters_.retirementDepthPeak = retirement_.depthPeak();
    return poll(err);
}

bool GpuCompositor::poll(std::string& err) {
    const CompletionPollResult result = completion_.polledCompleted();
    if (result.status != CompletionPollStatus::Ok) {
        ++counters_.completionPollFailureCount;
        err = "GPU completion pollに失敗しました";
        return false;
    }
    retirement_.poll(result.completed);
    return true;
}

bool GpuCompositor::shutdown(int timeoutMs, std::string& err) {
    if (!ready_)
        return true;
    completion_.flushForShutdown();
    const bool drained =
        retirement_.drain([this] { return completion_.polledCompletedSerial(); }, timeoutMs);
    counters_.retirementDepthAfterDrain = retirement_.depthCurrent();
    counters_.retirementDepthPeak = retirement_.depthPeak();
    counters_.payloadsReleasedBeforeCompletion = retirement_.payloadsReleasedBeforeCompletion();
    counters_.retirementTimeoutCount = retirement_.retirementTimeoutCount();
    if (!drained) {
        err = "GPU retirementを有限時間でdrainできませんでした";
        return false;
    }
    converter_.release();
    completion_.release();
    releaseTarget();
    ready_ = false;
    return true;
}

bool GpuCompositor::readOutputProbe(int x, int y, int width, int height,
                                    std::vector<unsigned char>& rgba, std::string& err) {
    return converter_.readOutputProbe(target_, x, y, width, height, rgba, err);
}

bool GpuCompositor::readSourceMarker(const DecodedGpuFrame& frame, int width, int height,
                                     std::vector<unsigned char>& rgba, std::string& err) {
    return converter_.readMarkerBand(frame, width, height, rgba, err);
}

} // namespace mvm::gpu

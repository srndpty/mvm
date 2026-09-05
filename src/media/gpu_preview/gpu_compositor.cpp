#include "media/gpu_preview/gpu_compositor.h"

#include "media/gpu_preview/qpc_clock.h"

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

void GpuCompositor::rollbackInitialize() {
    releaseTarget();
    completion_.release();
    converter_.release();
    shared_ = nullptr;
    width_ = height_ = 0;
    ready_ = false;
    fatal_ = false;
    fatalReason_.clear();
}

void GpuCompositor::enterFatal(const std::string& reason) {
    if (!fatal_) {
        fatal_ = true;
        fatalReason_ = reason;
    }
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
    if (!converter_.initialize(device, &readbacks, err)) {
        rollbackInitialize();
        return false;
    }
    if (testFaults_.initialize == GpuCompositorInitializeFault::Completion ||
        !completion_.initialize(device, err, backend)) {
        if (err.empty())
            err = "test fault: completion initialize";
        rollbackInitialize();
        return false;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT rc = testFaults_.initialize == GpuCompositorInitializeFault::TargetTexture
                     ? E_FAIL
                     : device.device()->CreateTexture2D(&td, nullptr, &target_);
    if (SUCCEEDED(rc)) {
        if (testFaults_.initialize == GpuCompositorInitializeFault::TargetRtv)
            rc = E_FAIL;
        else
            rc = device.device()->CreateRenderTargetView(target_, nullptr, &targetRtv_);
    }
    if (SUCCEEDED(rc)) {
        if (testFaults_.initialize == GpuCompositorInitializeFault::TargetSrv)
            rc = E_FAIL;
        else
            rc = device.device()->CreateShaderResourceView(target_, nullptr, &targetSrv_);
    }
    if (FAILED(rc)) {
        err = hrText("offscreen render targetの生成", rc);
        rollbackInitialize();
        return false;
    }
    ready_ = true;
    return true;
}

bool GpuCompositor::initializeExternal(SharedD3D11Device& device, ReadbackCounters& readbacks,
                                       std::string& err, GpuCompletionBackend backend) {
    if (ready_ || !device.valid()) {
        err = "GpuCompositorのexternal初期化引数が不正です";
        return false;
    }
    shared_ = &device;
    if (!converter_.initialize(device, &readbacks, err)) {
        rollbackInitialize();
        return false;
    }
    if (testFaults_.initialize == GpuCompositorInitializeFault::Completion ||
        !completion_.initialize(device, err, backend)) {
        if (err.empty())
            err = "test fault: completion initialize";
        rollbackInitialize();
        return false;
    }
    ready_ = true;
    return true;
}

bool GpuCompositor::prepareComposition(const ComposedFrame& frame,
                                       const ExternalCompositionTarget& target,
                                       size_t expectedLayerCount, std::string& err) {
    if (!target.rtv || target.width <= 0 || target.height <= 0) {
        err = "composition targetが不正です";
        return false;
    }
    if (frame.layers.size() != expectedLayerCount) {
        err = "compositionのlayer数が診断契約と一致しません";
        return false;
    }
    for (const auto& layer : frame.layers) {
        const bool legacyGeometryInvalid =
            !layer.effectsEnabled && (layer.destination.x < 0 || layer.destination.y < 0 ||
                                      layer.destination.x + layer.destination.width > 1.0f ||
                                      layer.destination.y + layer.destination.height > 1.0f);
        if (!layer.frame.valid() || layer.destination.width <= 0 || layer.destination.height <= 0 ||
            legacyGeometryInvalid || layer.sourceUv.width <= 0 || layer.sourceUv.height <= 0 ||
            layer.sourceUv.x < 0 || layer.sourceUv.y < 0 ||
            layer.sourceUv.x + layer.sourceUv.width > 1.0f ||
            layer.sourceUv.y + layer.sourceUv.height > 1.0f || layer.opacity < 0.0f ||
            layer.opacity > 1.0f || !std::isfinite(layer.rotationDegrees)) {
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
        // SRV 生成を issue 前に終える。ここで失敗した場合 GPU command は 0。
        if (!converter_.prepareLayer(layer.frame, err))
            return false;
    }
    return true;
}

bool GpuCompositor::compose(const ComposedFrame& frame, std::string& err) {
    if (!targetRtv_) {
        ++counters_.compositionRequestedCount;
        err = "external-only GpuCompositorではoffscreen composeを使用できません";
        return false;
    }
    return composeToTarget(frame, {targetRtv_, width_, height_}, err);
}

bool GpuCompositor::composeToTarget(const ComposedFrame& frame,
                                    const ExternalCompositionTarget& target, std::string& err) {
    return composeProductToTarget(frame, target, 2, "compositionのlayer数が診断契約と一致しません",
                                  err);
}

bool GpuCompositor::composeSingleLayerToTarget(const ComposedFrame& frame,
                                               const ExternalCompositionTarget& target,
                                               std::string& err) {
    return composeProductToTarget(frame, target, 1,
                                  "single-layer product compositionはexactly 1 layer必須です", err);
}

bool GpuCompositor::composeLayersToTarget(const ComposedFrame& frame,
                                          const ExternalCompositionTarget& target,
                                          size_t expectedLayerCount, std::string& err) {
    if (expectedLayerCount == 0) {
        err = "product compositionは1 layer以上必須です";
        ++counters_.compositionRequestedCount;
        return false;
    }
    return composeProductToTarget(frame, target, expectedLayerCount,
                                  "product compositionのlayer数が要求と一致しません", err);
}

bool GpuCompositor::composeProductToTarget(const ComposedFrame& frame,
                                           const ExternalCompositionTarget& target,
                                           size_t expectedLayerCount, const char* layerCountError,
                                           std::string& err) {
    ++counters_.compositionRequestedCount;
    if (!ready_) {
        err = "GpuCompositorが初期化されていません";
        return false;
    }
    if (fatal_) {
        ++counters_.composeAfterFatalRejectedCount;
        err = "GpuCompositorはfatal状態です: " + fatalReason_;
        return false;
    }
    if (frame.layers.size() != expectedLayerCount) {
        err = layerCountError;
        return false;
    }
    if (!prepareComposition(frame, target, expectedLayerCount, err))
        return false;
    // owned offscreen wrapperだけがclearする。external targetはQRhi passがclear owner。
    const bool clearTarget = target.rtv == targetRtv_;
    return issueComposition(frame, target, clearTarget, nullptr, err);
}

bool GpuCompositor::composeDiagnosticToTarget(const ComposedFrame& frame,
                                              const ExternalCompositionTarget& target,
                                              GpuCompositorStageTiming& timing, std::string& err) {
    ++counters_.compositionRequestedCount;
    if (!ready_ || fatal_ || (frame.layers.size() != 1 && frame.layers.size() != 2)) {
        err = !ready_  ? "GpuCompositorが初期化されていません"
              : fatal_ ? "GpuCompositorはfatal状態です: " + fatalReason_
                       : "P2-D3診断compositionは1または2 layer必須です";
        return false;
    }
    const long long prepareBegin = qpcTicks();
    const bool prepared = prepareComposition(frame, target, frame.layers.size(), err);
    timing.prepareUs = qpcUsBetween(prepareBegin, qpcTicks());
    if (!prepared)
        return false;
    const bool clearTarget = target.rtv == targetRtv_;
    return issueComposition(frame, target, clearTarget, &timing, err);
}

bool GpuCompositor::issueComposition(const ComposedFrame& frame,
                                     const ExternalCompositionTarget& target, bool clearTarget,
                                     GpuCompositorStageTiming* timing, std::string& err) {
    const long long issueBegin = timing ? qpcTicks() : 0;
    if (clearTarget) {
        std::lock_guard<D3D11Lock> guard(shared_->lock());
        const float clear[4] = {0, 0, 0, 1};
        shared_->context()->ClearRenderTargetView(target.rtv, clear);
        ++counters_.clearCount;
    }
    int layerIndex = 0;
    for (const auto& layer : frame.layers) {
        const FitRect destinationBox{
            static_cast<int>(std::lround(layer.destination.x * static_cast<float>(target.width))),
            static_cast<int>(std::lround(layer.destination.y * static_cast<float>(target.height))),
            static_cast<int>(
                std::lround(layer.destination.width * static_cast<float>(target.width))),
            static_cast<int>(
                std::lround(layer.destination.height * static_cast<float>(target.height)))};
        const float uv[4] = {layer.sourceUv.x, layer.sourceUv.y, layer.sourceUv.width,
                             layer.sourceUv.height};
        // Project の destination は「配置可能な枠」であり、素材をそこまで変形する
        // 指示ではない。crop 後の素材比率を保って枠内へ収める。
        const int croppedWidth =
            std::max(1, static_cast<int>(std::lround(static_cast<float>(layer.frame.width) *
                                                     layer.sourceUv.width)));
        const int croppedHeight =
            std::max(1, static_cast<int>(std::lround(static_cast<float>(layer.frame.height) *
                                                     layer.sourceUv.height)));
        FitRect destination =
            aspectFit(croppedWidth, croppedHeight, destinationBox.width, destinationBox.height);
        destination.x += destinationBox.x;
        destination.y += destinationBox.y;
        const bool injectedFailure = testFaults_.failBeforeLayerDraw == layerIndex;
        if (injectedFailure)
            err = "test fault: issue開始後のlayer描画失敗";
        const bool drawn =
            !injectedFailure &&
            (layer.effectsEnabled
                 ? converter_.drawEffectLayer(layer.frame, target.rtv, target.width, target.height,
                                              destination, uv, layer.opacity, layer.rotationDegrees,
                                              true, err)
                 : converter_.drawLayer(layer.frame, target.rtv, destination, uv, layer.opacity,
                                        true, err));
        if (injectedFailure || !drawn) {
            ++counters_.partialGpuIssueFailureCount;
            const SubmissionResult partial = completion_.signalSubmission();
            if (partial.tracked()) {
                ++counters_.gpuSubmissionCount;
                converter_.stampSubmissionSerial(partial.serial);
                retirement_.retire(partial.serial, aggregateLifetime(frame));
            } else {
                ++counters_.untrackedSubmissionCount;
                retirement_.retire(kNeverCompletingSerial, aggregateLifetime(frame));
            }
            enterFatal("issue開始後のlayer描画に失敗しました: " + err);
            return false;
        }
        ++counters_.layerDrawCount;
        ++layerIndex;
    }
    const SubmissionResult submission = completion_.signalSubmission();
    if (!submission.tracked()) {
        ++counters_.untrackedSubmissionCount;
        retirement_.retire(kNeverCompletingSerial, aggregateLifetime(frame));
        err = "composition submissionをGPU完了trackerで追跡できません";
        enterFatal(err);
        return false;
    }
    ++counters_.gpuSubmissionCount;
    ++counters_.compositionDrawnCount;
    converter_.stampSubmissionSerial(submission.serial);
    retirement_.retire(submission.serial, aggregateLifetime(frame));
    counters_.retirementDepthPeak = retirement_.depthPeak();
    if (timing)
        timing->issueUs = qpcUsBetween(issueBegin, qpcTicks());
    const long long pollBegin = timing ? qpcTicks() : 0;
    if (!poll(err))
        return false;
    if (timing)
        timing->completionPollUs = qpcUsBetween(pollBegin, qpcTicks());
    return true;
}

bool GpuCompositor::poll(std::string& err) {
    if (testFaults_.failCompletionPoll) {
        ++counters_.completionPollFailureCount;
        err = "test fault: GPU completion poll failure";
        enterFatal(err);
        return false;
    }
    const CompletionPollResult result = completion_.polledCompleted();
    if (result.status != CompletionPollStatus::Ok) {
        ++counters_.completionPollFailureCount;
        err = completion_.fatalReason().empty() ? "GPU completion pollに失敗しました"
                                                : completion_.fatalReason();
        enterFatal(err);
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
    finishShutdown();
    return true;
}

bool GpuCompositor::beginShutdown(int timeoutMs, std::string& err) {
    if (shutdownStarted_ || timeoutMs < 0) {
        err = "GPU compositor shutdownの開始状態が不正です";
        return false;
    }
    shutdownStarted_ = true;
    shutdownFailed_ = false;
    if (!ready_)
        return true;
    completion_.flushForShutdown();
    const long long timeoutTicks = static_cast<long long>(
        (static_cast<long double>(timeoutMs) * qpcFrequencyTicks()) / 1000.0L);
    shutdownDeadlineQpc_ = qpcTicks() + timeoutTicks;
    return true;
}

GpuCompositorShutdownStatus GpuCompositor::pollShutdown(std::string& err) {
    if (!shutdownStarted_) {
        err = "GPU compositor shutdownが開始されていません";
        return GpuCompositorShutdownStatus::Failed;
    }
    if (shutdownFailed_)
        return GpuCompositorShutdownStatus::Failed;
    if (!ready_)
        return GpuCompositorShutdownStatus::Complete;
    // shutdownではcompose/poll用のtest faultを再注入せず、実GPU completionだけを
    // 非blockingに確認する。fatal発生後も既投入resourceは安全にretireさせる。
    if (testFaults_.failShutdownCompletionPoll)
        return failShutdownCompletionPoll("test fault: GPU shutdown completion poll failure", err);
    const CompletionPollResult result = completion_.polledCompleted();
    if (result.status != CompletionPollStatus::Ok)
        return failShutdownCompletionPoll(completion_.fatalReason().empty()
                                              ? "GPU completion shutdown pollに失敗しました"
                                              : completion_.fatalReason(),
                                          err);
    retirement_.poll(result.completed);
    counters_.retirementDepthAfterDrain = retirement_.depthCurrent();
    counters_.retirementDepthPeak = retirement_.depthPeak();
    counters_.payloadsReleasedBeforeCompletion = retirement_.payloadsReleasedBeforeCompletion();
    if (counters_.retirementDepthAfterDrain == 0) {
        finishShutdown();
        return GpuCompositorShutdownStatus::Complete;
    }
    if (qpcTicks() < shutdownDeadlineQpc_)
        return GpuCompositorShutdownStatus::Pending;
    ++counters_.retirementTimeoutCount;
    shutdownFailed_ = true;
    err = "GPU retirementを有限時間でdrainできませんでした";
    return GpuCompositorShutdownStatus::Failed;
}

GpuCompositorShutdownStatus GpuCompositor::failShutdownCompletionPoll(const std::string& reason,
                                                                      std::string& err) {
    ++counters_.completionPollFailureCount;
    err = reason;
    shutdownFailed_ = true;
    return GpuCompositorShutdownStatus::Failed;
}

void GpuCompositor::finishShutdown() {
    converter_.release();
    completion_.release();
    releaseTarget();
    ready_ = false;
    shared_ = nullptr;
    width_ = height_ = 0;
    shutdownStarted_ = false;
    shutdownFailed_ = false;
    shutdownDeadlineQpc_ = 0;
}

bool GpuCompositor::readOutputProbe(int x, int y, int width, int height,
                                    std::vector<unsigned char>& rgba, std::string& err) {
    return converter_.readOutputProbe(target_, x, y, width, height, rgba, err);
}

bool GpuCompositor::readExternalOutputProbe(ID3D11Texture2D* texture, int x, int y, int width,
                                            int height, std::vector<unsigned char>& rgba,
                                            std::string& err) {
    return converter_.readOutputProbe(texture, x, y, width, height, rgba, err);
}

bool GpuCompositor::readSourceProbe(const DecodedGpuFrame& frame, float u, float v,
                                    std::vector<unsigned char>& rgba, std::string& err) {
    return converter_.readSourceProbe(frame, u, v, rgba, err);
}

bool GpuCompositor::readSourceMarker(const DecodedGpuFrame& frame, int width, int height,
                                     std::vector<unsigned char>& rgba, std::string& err) {
    return converter_.readMarkerBand(frame, width, height, rgba, err);
}

} // namespace mvm::gpu

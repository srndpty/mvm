#include "app/preview/compositor_rhi_item.h"

#include "media/gpu_preview/exact_frame_pairer.h"
#include "media/gpu_preview/output_scheduler.h"
#include "media/gpu_preview/qpc_clock.h"

#include <algorithm>
#include <cmath>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <vector>

namespace mvm::app {
namespace {

class CompositorRhiRenderer final : public QQuickRhiItemRenderer {
public:
    CompositorRhiRenderer(std::shared_ptr<CompositorSpikeState> state,
                          gpu::GpuCompletionBackend backend)
        : state_(std::move(state)), backend_(backend) {}

    ~CompositorRhiRenderer() override { releaseRtv(); }

protected:
    void initialize(QRhiCommandBuffer*) override {
        QRhi* r = rhi();
        if (!r || r->backend() != QRhi::D3D11) {
            fail("QRhi backendがD3D11ではありません");
            return;
        }
        const auto* h = static_cast<const QRhiD3D11NativeHandles*>(r->nativeHandles());
        if (!h || !h->dev || !h->context) {
            fail("QRhiのnative D3D11 device/contextを取得できません");
            return;
        }
        if (nativeDevice_ == h->dev && nativeContext_ == h->context)
            return;
        if (nativeDevice_) {
            state_->deviceReady.store(false, std::memory_order_release);
            state_->ledger.abort();
            fail("QRhiのD3D11 deviceが変更されました。P2-C2では回復しません");
            return;
        }
        std::string err;
        if (!state_->device.adopt(static_cast<ID3D11Device*>(h->dev),
                                  static_cast<ID3D11DeviceContext*>(h->context), err) ||
            !state_->compositor.initializeExternal(state_->device, state_->readbacks, err,
                                                   backend_)) {
            fail(err);
            return;
        }
        nativeDevice_ = h->dev;
        nativeContext_ = h->context;
        state_->nativeDevicePointer.store(reinterpret_cast<unsigned long long>(h->dev),
                                          std::memory_order_relaxed);
        state_->qtAdapter = state_->device.adapter();
        state_->deviceReady.store(true, std::memory_order_release);
    }

    void synchronize(QQuickRhiItem*) override {}

    void render(QRhiCommandBuffer* cb) override {
        state_->presentCallbackCount.fetch_add(1, std::memory_order_relaxed);
        // P1と同じくrender threadから次のframeを要求する。GUI timerだけでは
        // scene graph requestがcoalesceされ、60Hz output deadlineを取りこぼす。
        update();
        if (state_->teardownRequested.load(std::memory_order_acquire)) {
            teardown();
            return;
        }
        if (!state_->deviceReady.load(std::memory_order_acquire) ||
            state_->fatal.load(std::memory_order_acquire))
            return;
        if (state_->testDeviceChange.exchange(false)) {
            fail("test fault: QRhi D3D11 device change");
            return;
        }

        std::shared_ptr<gpu::SourceDecodeWorker> a;
        std::shared_ptr<gpu::SourceDecodeWorker> b;
        {
            std::lock_guard<std::mutex> lock(state_->workerMutex);
            a = state_->workerA;
            b = state_->workerB;
        }
        long long output = state_->requestedOutput.exchange(-1);
        if (output < 0 && state_->playbackSchedulerEnabled.load(std::memory_order_acquire)) {
            const long long now = gpu::qpcTicks();
            if (!schedulerStarted_) {
                scheduler_.start(now, static_cast<long long>(gpu::qpcFrequency()));
                schedulerStarted_ = true;
            }
            const gpu::OutputScheduleDecision decision = scheduler_.takeDue(now);
            if (decision.due) {
                output = decision.output.outputFrameNumber;
                state_->scheduledOutputCount.fetch_add(decision.skippedDeadlineCount + 1,
                                                       std::memory_order_relaxed);
                state_->droppedOutputCount.fetch_add(decision.skippedDeadlineCount,
                                                     std::memory_order_relaxed);
                state_->schedulerDeadlineDropCount.fetch_add(decision.skippedDeadlineCount,
                                                             std::memory_order_relaxed);
            }
        }
        if (!a || !b || output < 0) {
            state_->repeatedPresentCount.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        gpu::ExactFramePairer pairer(a->buffer(), b->buffer(), state_->coordinator);
        gpu::ComposedFrame frame;
        const gpu::PairResult paired = pairer.tryPair(output, frame);
        if (paired != gpu::PairResult::Paired) {
            state_->droppedOutputCount.fetch_add(1, std::memory_order_relaxed);
            state_->missingPairDropCount.fetch_add(1, std::memory_order_relaxed);
            if (paired == gpu::PairResult::MissingA || paired == gpu::PairResult::MissingBoth)
                state_->missingSourceADropCount.fetch_add(1, std::memory_order_relaxed);
            if (paired == gpu::PairResult::MissingB || paired == gpu::PairResult::MissingBoth)
                state_->missingSourceBDropCount.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (state_->coordinator.validateForDisplay(frame) != gpu::CompositionResult::Accepted) {
            state_->droppedOutputCount.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::string err;
        if (!ensureRtv(colorTexture(), err)) {
            state_->renderFailureCount.fetch_add(1, std::memory_order_relaxed);
            fail(err);
            return;
        }

        // clear ownerはQRhi passのみ。GpuCompositor external modeはclearしない。
        cb->beginPass(renderTarget(), Qt::black, {1.0f, 0}, nullptr,
                      QRhiCommandBuffer::ExternalContent);
        state_->logicalClearCount.fetch_add(1, std::memory_order_relaxed);
        cb->beginExternal();
        const QSize size = colorTexture()->pixelSize();
        const bool ok =
            state_->compositor.composeToTarget(frame, {rtv_, size.width(), size.height()}, err);
        cb->endExternal();
        cb->endPass();
        if (!ok) {
            state_->renderFailureCount.fetch_add(1, std::memory_order_relaxed);
            state_->droppedOutputCount.fetch_add(1, std::memory_order_relaxed);
            if (state_->compositor.fatal())
                fail(state_->compositor.fatalReason());
            return;
        }
        if (!state_->actualTargetProbeDone.exchange(true))
            runActualTargetProbe(frame, size);
        a->buffer().noteDisplayed(output);
        b->buffer().noteDisplayed(output);
        state_->ledger.record(frame, gpu::qpcTicks());
        state_->displayedCompositionCount.fetch_add(1, std::memory_order_relaxed);
    }

private:
    void runActualTargetProbe(const gpu::ComposedFrame& frame, const QSize& size) {
        // performance区間外の最初のactual targetだけを、sourceの同じsampling結果と比較する。
        const int points[4][2] = {
            {size.width() / 4, size.height() / 4},
            {size.width() * 3 / 4, size.height() * 3 / 4},
            {size.width() / 2, size.height() / 2},
            {std::max(0, size.width() / 2 - 1), std::max(0, size.height() / 2 - 1)}};
        for (const auto& point : points) {
            std::vector<unsigned char> actual;
            std::vector<unsigned char> sourceA;
            std::vector<unsigned char> sourceB;
            std::string err;
            const float uA =
                (static_cast<float>(point[0]) + 0.5f) / static_cast<float>(size.width());
            const float vA =
                (static_cast<float>(point[1]) + 0.5f) / static_cast<float>(size.height());
            const bool overlap = point[0] >= size.width() / 2 && point[1] >= size.height() / 2;
            bool ok =
                state_->compositor.readExternalOutputProbe(rtvTexture_, point[0], point[1], 1, 1,
                                                           actual, err) &&
                state_->compositor.readSourceProbe(frame.layers[0].frame, uA, vA, sourceA, err);
            if (ok && overlap) {
                const float uB = (static_cast<float>(point[0] - size.width() / 2) + 0.5f) /
                                 static_cast<float>(size.width() / 2);
                const float vB = (static_cast<float>(point[1] - size.height() / 2) + 0.5f) /
                                 static_cast<float>(size.height() / 2);
                ok =
                    state_->compositor.readSourceProbe(frame.layers[1].frame, uB, vB, sourceB, err);
            }
            if (!ok || actual.size() != 4 || sourceA.size() != 4 ||
                (overlap && sourceB.size() != 4)) {
                state_->actualTargetProbeMismatch.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            for (size_t channel = 0; channel < 3; ++channel) {
                int expected = sourceA[channel];
                if (overlap) {
                    const float opacity = frame.layers[1].opacity;
                    expected = static_cast<int>(
                        std::lround(static_cast<float>(sourceB[channel]) * opacity +
                                    static_cast<float>(sourceA[channel]) * (1.0f - opacity)));
                }
                if (std::abs(static_cast<int>(actual[channel]) - expected) > 3)
                    state_->actualTargetProbeMismatch.fetch_add(1, std::memory_order_relaxed);
            }
            if (actual[3] != 255)
                state_->actualTargetProbeMismatch.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void fail(const std::string& reason) {
        {
            std::lock_guard<std::mutex> lock(state_->errorMutex);
            if (state_->fatalReason.empty())
                state_->fatalReason = reason;
        }
        state_->fatal.store(true, std::memory_order_release);
        state_->deviceReady.store(false, std::memory_order_release);
        state_->ledger.abort();
    }

    bool ensureRtv(QRhiTexture* texture, std::string& err) {
        if (!texture) {
            err = "QQuickRhiItem color textureがありません";
            return false;
        }
        auto* native = reinterpret_cast<ID3D11Texture2D*>(texture->nativeTexture().object);
        if (!native) {
            err = "QQuickRhiItem color textureのnative handleがありません";
            return false;
        }
        if (native == rtvTexture_ && rtv_)
            return true;
        releaseRtv();
        const HRESULT hr = state_->device.device()->CreateRenderTargetView(native, nullptr, &rtv_);
        if (FAILED(hr)) {
            err = "QQuickRhiItem actual targetのRTV生成に失敗しました";
            return false;
        }
        rtvTexture_ = native;
        return true;
    }

    void releaseRtv() {
        if (rtv_)
            rtv_->Release();
        rtv_ = nullptr;
        rtvTexture_ = nullptr;
    }

    void teardown() {
        if (state_->teardownComplete.load(std::memory_order_acquire))
            return;
        std::shared_ptr<gpu::SourceDecodeWorker> a;
        std::shared_ptr<gpu::SourceDecodeWorker> b;
        {
            std::lock_guard<std::mutex> lock(state_->workerMutex);
            a = state_->workerA;
            b = state_->workerB;
        }
        if ((a && !a->joined()) || (b && !b->joined())) {
            state_->lifecycleOrderViolationCount.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::string err;
        if (!state_->compositor.shutdown(10000, err))
            fail(err);
        releaseRtv();
        state_->device.release();
        nativeDevice_ = nativeContext_ = nullptr;
        state_->deviceReady.store(false, std::memory_order_release);
        state_->teardownComplete.store(true, std::memory_order_release);
    }

    std::shared_ptr<CompositorSpikeState> state_;
    gpu::GpuCompletionBackend backend_;
    void* nativeDevice_ = nullptr;
    void* nativeContext_ = nullptr;
    ID3D11Texture2D* rtvTexture_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    gpu::OutputScheduler60Hz scheduler_;
    bool schedulerStarted_ = false;
};

} // namespace

CompositorRhiItem::CompositorRhiItem(QQuickItem* parent)
    : QQuickRhiItem(parent), state_(std::make_shared<CompositorSpikeState>()) {
    setMirrorVertically(true);
}

void CompositorRhiItem::requestTeardown() {
    state_->teardownRequested.store(true, std::memory_order_release);
    update();
}

QQuickRhiItemRenderer* CompositorRhiItem::createRenderer() {
    return new CompositorRhiRenderer(state_, preferredCompletion_);
}

} // namespace mvm::app

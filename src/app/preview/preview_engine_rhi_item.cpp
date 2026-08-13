#include "app/preview/preview_engine_rhi_item.h"

#include "preview_engine/preview_engine_internal.h"

#include <d3d11.h>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <utility>

#include <QColor>

namespace mvm::app {
namespace {

class PreviewEngineRhiRenderer final : public QQuickRhiItemRenderer {
public:
    explicit PreviewEngineRhiRenderer(std::shared_ptr<preview::PreviewEngine> engine)
        : engine_(std::move(engine)) {}

    ~PreviewEngineRhiRenderer() override {
        releaseRtv();
        if (engine_)
            preview::internal::PreviewRenderPort::completeRendererDetach(*engine_);
    }

protected:
    void initialize(QRhiCommandBuffer*) override { attachEngine(); }

    void synchronize(QQuickRhiItem* item) override {
        std::shared_ptr<preview::PreviewEngine> requested =
            static_cast<PreviewEngineRhiItem*>(item)->engine();
        if (requested == engine_ && !switchPending_)
            return;
        if (engine_) {
            const preview::PreviewEngineState state = engine_->status().state;
            if (state != preview::PreviewEngineState::Shutdown &&
                state != preview::PreviewEngineState::Error) {
                pendingEngine_ = std::move(requested);
                switchPending_ = true;
                attached_ = attached_ ||
                            preview::internal::PreviewRenderPort::nativeRuntimeAttached(*engine_);
                if (state != preview::PreviewEngineState::ShuttingDown)
                    preview::internal::PreviewRenderPort::reportEngineReplacement(*engine_);
                return;
            }
        }
        switchEngine(std::move(requested));
    }

    void render(QRhiCommandBuffer* commandBuffer) override {
        if (!engine_)
            return;

        const preview::PreviewEngineState state = engine_->status().state;
        if (state == preview::PreviewEngineState::Shutdown ||
            state == preview::PreviewEngineState::Error)
            return;

        if (state == preview::PreviewEngineState::ShuttingDown) {
            releaseRtv();
            if (!attached_) {
                // native runtimeを所有していない場合も論理teardownを明示的に完了する。
                const auto completed =
                    preview::internal::PreviewRenderPort::completeTeardown(*engine_);
                if (completed && switchPending_)
                    switchEngine(std::move(pendingEngine_));
                return;
            }
            // 新規submission停止後、borrowed target参照をdevice releaseより先に外す。
            const auto completed =
                preview::internal::PreviewRenderPort::completeRuntimeTeardown(*engine_);
            if (completed && completed.value()) {
                attached_ = false;
                if (switchPending_)
                    switchEngine(std::move(pendingEngine_));
                return;
            }
            // GPU retirement pollがpendingの間だけ次回frameを予約する。
            update();
            return;
        }
        // Playing/ReadyPausedでは描画とdevice lost監視を継続する。
        update();
        if (!attached_)
            return;
        if (detectDeviceLost())
            return;
        if (state != preview::PreviewEngineState::Playing)
            return;
        if (!ensureRtv(colorTexture()))
            return;

        commandBuffer->beginPass(renderTarget(), Qt::black, {1.0F, 0}, nullptr,
                                 QRhiCommandBuffer::ExternalContent);
        commandBuffer->beginExternal();
        const QSize size = colorTexture()->pixelSize();
        preview::internal::PreviewRenderPort::renderFrame(*engine_, rtv_, size.width(),
                                                          size.height());
        commandBuffer->endExternal();
        commandBuffer->endPass();
    }

private:
    void switchEngine(std::shared_ptr<preview::PreviewEngine> engine) {
        releaseRtv();
        engine_ = std::move(engine);
        pendingEngine_.reset();
        switchPending_ = false;
        attached_ = false;
        attachEngine();
        if (engine_ && engine_->status().state != preview::PreviewEngineState::Shutdown &&
            engine_->status().state != preview::PreviewEngineState::Error)
            update();
    }

    bool detectDeviceLost() {
        QRhi* renderHardware = rhi();
        const auto* handles =
            renderHardware
                ? static_cast<const QRhiD3D11NativeHandles*>(renderHardware->nativeHandles())
                : nullptr;
        if (!handles || !handles->dev) {
            preview::internal::PreviewRenderPort::reportMissingNativeD3D11Handles(*engine_);
            return true;
        }
        const HRESULT removed = static_cast<ID3D11Device*>(handles->dev)->GetDeviceRemovedReason();
        if (SUCCEEDED(removed))
            return false;
        preview::internal::PreviewRenderPort::reportDeviceLost(*engine_, removed);
        return true;
    }

    void attachEngine() {
        if (!engine_)
            return;
        QRhi* renderHardware = rhi();
        if (!renderHardware || renderHardware->backend() != QRhi::D3D11) {
            preview::internal::PreviewRenderPort::reportUnsupportedRenderBackend(*engine_);
            return;
        }
        const auto* handles =
            static_cast<const QRhiD3D11NativeHandles*>(renderHardware->nativeHandles());
        if (!handles || !handles->dev || !handles->context) {
            preview::internal::PreviewRenderPort::reportMissingNativeD3D11Handles(*engine_);
            return;
        }
        if (attached_) {
            preview::internal::PreviewRenderPort::validateNativeD3D11Device(*engine_, handles->dev,
                                                                            handles->context);
            return;
        }
        auto attached = preview::internal::PreviewRenderPort::acquireNativeD3D11Device(
            *engine_, handles->dev, handles->context);
        attached_ = static_cast<bool>(attached) ||
                    preview::internal::PreviewRenderPort::nativeRuntimeAttached(*engine_);
    }

    bool ensureRtv(QRhiTexture* texture) {
        if (!texture)
            return false;
        auto* native = reinterpret_cast<ID3D11Texture2D*>(texture->nativeTexture().object);
        if (!native)
            return false;
        if (native == rtvTexture_ && rtv_)
            return true;
        releaseRtv();
        QRhi* renderHardware = rhi();
        const auto* handles =
            renderHardware
                ? static_cast<const QRhiD3D11NativeHandles*>(renderHardware->nativeHandles())
                : nullptr;
        if (!handles || !handles->dev)
            return false;
        auto* device = static_cast<ID3D11Device*>(handles->dev);
        const HRESULT created = device->CreateRenderTargetView(native, nullptr, &rtv_);
        if (FAILED(created)) {
            preview::internal::PreviewRenderPort::reportRenderTargetFailure(*engine_, created);
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

    std::shared_ptr<preview::PreviewEngine> engine_;
    std::shared_ptr<preview::PreviewEngine> pendingEngine_;
    bool switchPending_ = false;
    bool attached_ = false;
    ID3D11Texture2D* rtvTexture_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
};

} // namespace

PreviewEngineRhiItem::PreviewEngineRhiItem(QQuickItem* parent) : QQuickRhiItem(parent) {
    setMirrorVertically(true);
}

void PreviewEngineRhiItem::setEngine(std::shared_ptr<preview::PreviewEngine> engine) {
    engine_ = std::move(engine);
    update();
}

QQuickRhiItemRenderer* PreviewEngineRhiItem::createRenderer() {
    return new PreviewEngineRhiRenderer(engine_);
}

} // namespace mvm::app

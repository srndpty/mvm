#include "app/preview/preview_engine_rhi_item.h"

#include "preview_engine/preview_engine_internal.h"

#include <d3d11.h>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <QColor>

namespace mvm::app {
namespace {

class PreviewEngineRhiRenderer final : public QQuickRhiItemRenderer {
public:
    explicit PreviewEngineRhiRenderer(preview::PreviewEngine* engine) : engine_(engine) {}

    ~PreviewEngineRhiRenderer() override { releaseRtv(); }

protected:
    void initialize(QRhiCommandBuffer*) override {
        if (!engine_)
            return;
        QRhi* renderHardware = rhi();
        if (!renderHardware || renderHardware->backend() != QRhi::D3D11)
            return;
        const auto* handles =
            static_cast<const QRhiD3D11NativeHandles*>(renderHardware->nativeHandles());
        if (!handles || !handles->dev || !handles->context)
            return;
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

    void synchronize(QQuickRhiItem* item) override {
        engine_ = static_cast<PreviewEngineRhiItem*>(item)->engine();
    }

    void render(QRhiCommandBuffer* commandBuffer) override {
        update();
        if (!engine_)
            return;

        const preview::PreviewEngineState state = engine_->status().state;
        if (state == preview::PreviewEngineState::ShuttingDown) {
            releaseRtv();
            if (!attached_) {
                // native runtimeを所有していない場合も論理teardownを明示的に完了する。
                preview::internal::PreviewRenderPort::completeTeardown(*engine_);
                return;
            }
            // 新規submission停止後、borrowed target参照をdevice releaseより先に外す。
            const auto completed =
                preview::internal::PreviewRenderPort::completeRuntimeTeardown(*engine_);
            if (completed && completed.value())
                attached_ = false;
            return;
        }
        if (!attached_)
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

    preview::PreviewEngine* engine_ = nullptr;
    bool attached_ = false;
    ID3D11Texture2D* rtvTexture_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
};

} // namespace

PreviewEngineRhiItem::PreviewEngineRhiItem(QQuickItem* parent) : QQuickRhiItem(parent) {
    setMirrorVertically(true);
}

void PreviewEngineRhiItem::setEngine(preview::PreviewEngine* engine) {
    engine_ = engine;
    update();
}

void PreviewEngineRhiItem::requestRenderUpdate() {
    update();
}

QQuickRhiItemRenderer* PreviewEngineRhiItem::createRenderer() {
    return new PreviewEngineRhiRenderer(engine_);
}

} // namespace mvm::app

#include "app/preview/preview_rhi_item.h"

#include "core/mvm_marker.h"
#include "media/gpu_preview/qpc_clock.h"

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <QQuickWindow>

namespace mvm::app {
namespace {

// marker 帯の大きさ。docs/research/test-media-format.md の仕様そのもの。
// 1080p 全画素の 3.7%、4K の 0.9% しか読まない。
constexpr int kMarkerBandWidth = mvm::marker::kCellSize * mvm::marker::kCellCount; // 1216
constexpr int kMarkerBandHeight = mvm::marker::kCellSize;                          // 64

// device lost を毎フレーム問い合わせると測定へ影響するので間引く。
constexpr long long kDeviceLostCheckInterval = 120;

class PreviewRhiRenderer : public QQuickRhiItemRenderer {
public:
    explicit PreviewRhiRenderer(std::shared_ptr<gpu::PreviewState> state)
        : state_(std::move(state)) {}

    ~PreviewRhiRenderer() override { releaseRtv(); }

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void synchronize(QQuickRhiItem* item) override;
    void render(QRhiCommandBuffer* cb) override;

private:
    void releaseRtv() {
        if (rtv_) {
            rtv_->Release();
            rtv_ = nullptr;
        }
        rtvTexture_ = nullptr;
    }

    bool ensureRtv(QRhiTexture* tex, std::string& err);
    void runMarkerProbe(const gpu::DecodedGpuFrame& frame);

    std::shared_ptr<gpu::PreviewState> state_;
    bool adopted_ = false;

    ID3D11Texture2D* rtvTexture_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;

    // synchronize() で GUI thread から受け取る (両スレッドが止まっている時点)。
    bool linearFilter_ = true;
    float clearColor_[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool clearRequested_ = false;

    long long lastPresentTicks_ = 0;
};

void PreviewRhiRenderer::initialize(QRhiCommandBuffer* /*cb*/) {
    QRhi* r = rhi();
    if (!r) {
        state_->initFailed.store(true);
        return;
    }

    {
        std::lock_guard<std::mutex> g(state_->infoMutex);
        state_->rhiBackend = r->backendName();
    }

    if (r->backend() != QRhi::D3D11) {
        // D3D12 / Vulkan / OpenGL へ黙って乗らない。
        // P1 が検証しているのは D3D11 の経路そのものである。
        std::lock_guard<std::mutex> g(state_->infoMutex);
        state_->initError =
            std::string("QRhi の backend が D3D11 ではありません: ") + r->backendName();
        state_->initFailed.store(true);
        return;
    }

    // color texture が作り直されるたびに initialize() が呼ばれる。
    // device の採用は 1 度だけでよい。
    if (adopted_)
        return;

    const auto* h = static_cast<const QRhiD3D11NativeHandles*>(r->nativeHandles());
    if (!h || !h->dev || !h->context) {
        std::lock_guard<std::mutex> g(state_->infoMutex);
        state_->initError = "QRhiD3D11NativeHandles から device / context を取得できません";
        state_->initFailed.store(true);
        return;
    }

    auto* dev = static_cast<ID3D11Device*>(h->dev);
    auto* ctx = static_cast<ID3D11DeviceContext*>(h->context);

    std::string err;
    if (!state_->device.adopt(dev, ctx, err)) {
        std::lock_guard<std::mutex> g(state_->infoMutex);
        state_->initError = "D3D11 device を共有できません: " + err;
        state_->initFailed.store(true);
        return;
    }
    if (!state_->converter.initialize(state_->device, &state_->counters, err)) {
        std::lock_guard<std::mutex> g(state_->infoMutex);
        state_->initError = "NV12 変換パスを初期化できません: " + err;
        state_->initFailed.store(true);
        return;
    }
    state_->queue.setExpectedDevice(state_->device.device());

    {
        std::lock_guard<std::mutex> g(state_->infoMutex);
        // QRhi が申告する値と、device から DXGI 経由で取った値を **両方** 記録する。
        // 一致しなければ、どちらかの取得経路が誤っている。
        state_->qtReportedLuidLow = h->adapterLuidLow;
        state_->qtReportedLuidHigh = h->adapterLuidHigh;
        state_->qtFeatureLevel = h->featureLevel;
        state_->qtDevicePointer = reinterpret_cast<unsigned long long>(dev);
        state_->qtContextPointer = reinterpret_cast<unsigned long long>(ctx);
    }

    adopted_ = true;
    state_->deviceReady.store(true, std::memory_order_release);
}

void PreviewRhiRenderer::synchronize(QQuickRhiItem* item) {
    auto* self = static_cast<PreviewRhiItem*>(item);
    linearFilter_ = self->linearFilter();
    const QColor c = self->backgroundColor();
    clearColor_[0] = static_cast<float>(c.redF());
    clearColor_[1] = static_cast<float>(c.greenF());
    clearColor_[2] = static_cast<float>(c.blueF());
    clearColor_[3] = 1.0f;
    clearRequested_ = state_->clearRequested.exchange(false);
}

bool PreviewRhiRenderer::ensureRtv(QRhiTexture* tex, std::string& err) {
    if (!tex) {
        err = "color texture がありません";
        return false;
    }
    const QRhiTexture::NativeTexture nt = tex->nativeTexture();
    auto* native = reinterpret_cast<ID3D11Texture2D*>(nt.object);
    if (!native) {
        err = "color texture の native handle を取得できません";
        return false;
    }
    if (native == rtvTexture_ && rtv_)
        return true;

    releaseRtv();
    const HRESULT hr = state_->device.device()->CreateRenderTargetView(native, nullptr, &rtv_);
    if (FAILED(hr)) {
        err = "render target view を作成できません";
        return false;
    }
    rtvTexture_ = native;
    return true;
}

void PreviewRhiRenderer::runMarkerProbe(const gpu::DecodedGpuFrame& frame) {
    auto& slot = state_->markerProbe;
    {
        std::lock_guard<std::mutex> g(slot.mutex);
        if (!slot.requested)
            return;
        // 依頼されたフレームが実際に描かれるまで読まない。
        // 「別のフレームを読んで一致した」を起こさないための条件である。
        if (slot.expectedFrame >= 0 && frame.frameNumber != slot.expectedFrame)
            return;
    }

    std::vector<unsigned char> rgba;
    std::string err;
    const bool ok =
        state_->converter.readMarkerBand(frame, kMarkerBandWidth, kMarkerBandHeight, rgba, err);

    // 読み取りロジックは Phase 0 と同じ実装を使う (src/core/mvm_marker.h)。
    // 別実装にすると「表示は壊れているが marker は合う」状態を作れてしまう。
    mvm::marker::MarkerRead read;
    if (ok) {
        read = mvm::marker::readMarkerWithCellSize(rgba.data(), kMarkerBandWidth, kMarkerBandHeight,
                                                   mvm::marker::kCellSize);
    }

    std::lock_guard<std::mutex> g(slot.mutex);
    slot.requested = false;
    slot.done = true;
    slot.displayedFrame = frame.frameNumber;
    slot.markerValue = read.value;
    slot.syncOk = read.syncOk;
    slot.error = ok ? std::string() : err;
}

void PreviewRhiRenderer::render(QRhiCommandBuffer* cb) {
    state_->presentCount.fetch_add(1, std::memory_order_relaxed);

    // 連続描画を続ける。update() を呼ばないと次の render() が来ない。
    update();

    if (!state_->deviceReady.load(std::memory_order_acquire))
        return;

    if ((state_->presentCount.load(std::memory_order_relaxed) % kDeviceLostCheckInterval) == 0) {
        long reason = 0;
        if (state_->device.deviceLost(reason))
            state_->deviceLostCount.fetch_add(1, std::memory_order_relaxed);
    }

    std::string err;
    if (!ensureRtv(colorTexture(), err)) {
        state_->renderErrorCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    gpu::DecodedGpuFrame frame;
    const bool haveNew = state_->queue.takeForDisplay(frame);

    if (!haveNew && !clearRequested_) {
        // 新しい frame が無い。前の絵をそのまま残す。
        // **これを「表示した」に数えない。** Phase 0 で
        // 「配信数を fps と呼んで 60fps に見えた」事故があった。
        state_->repeatedPresents.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // QRhi の pass を開き、その中で生の D3D11 を発行する。
    // ExternalContent + beginExternal() が、QRhi へ
    // 「状態追跡を捨てて再構築せよ」と伝える契約である。
    const QColor bg = QColor::fromRgbF(clearColor_[0], clearColor_[1], clearColor_[2], 1.0f);
    cb->beginPass(renderTarget(), bg, {1.0f, 0}, nullptr, QRhiCommandBuffer::ExternalContent);

    if (clearRequested_ && !haveNew) {
        // beginPass の clear だけで背景になる。
        state_->displayedFrameNumber.store(-1, std::memory_order_relaxed);
        cb->endPass();
        return;
    }

    cb->beginExternal();

    const QSize sz = colorTexture()->pixelSize();
    const bool ok = state_->converter.drawToRenderTarget(frame, rtv_, sz.width(), sz.height(),
                                                         linearFilter_, clearColor_, err);

    if (ok)
        runMarkerProbe(frame);

    cb->endExternal();
    cb->endPass();

    if (!ok) {
        state_->renderErrorCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // **描画してから** displayed とする。submit した番号ではない。
    state_->queue.noteDisplayed(frame);
    state_->displayedFrameNumber.store(frame.frameNumber, std::memory_order_relaxed);
    state_->uniqueDisplayed.fetch_add(1, std::memory_order_relaxed);

    const long long now = gpu::qpcTicks();
    if (lastPresentTicks_ != 0)
        state_->pushInterval(gpu::qpcMsBetween(lastPresentTicks_, now));
    lastPresentTicks_ = now;
}

} // namespace

// --------------------------------------------------------------------------

PreviewRhiItem::PreviewRhiItem(QQuickItem* parent)
    : QQuickRhiItem(parent), state_(std::make_shared<gpu::PreviewState>()) {
    setSampleCount(1);
    setColorBufferFormat(TextureFormat::RGBA8);
    setAutoRenderTarget(true);
}

PreviewRhiItem::~PreviewRhiItem() {
    // decode 側は先に止まっている前提。ここでは表示待ちだけを捨てる。
    state_->queue.stop();
}

QQuickRhiItemRenderer* PreviewRhiItem::createRenderer() {
    return new PreviewRhiRenderer(state_);
}

bool PreviewRhiItem::deviceReady() const {
    return state_->deviceReady.load(std::memory_order_acquire);
}

QString PreviewRhiItem::initError() const {
    std::lock_guard<std::mutex> g(state_->infoMutex);
    return QString::fromStdString(state_->initError);
}

qlonglong PreviewRhiItem::displayedFrame() const {
    return state_->displayedFrameNumber.load(std::memory_order_relaxed);
}

void PreviewRhiItem::setLinearFilter(bool on) {
    if (linearFilter_ == on)
        return;
    linearFilter_ = on;
    state_->linearFilter.store(on, std::memory_order_relaxed);
    Q_EMIT linearFilterChanged();
    update();
}

void PreviewRhiItem::setBackgroundColor(const QColor& c) {
    if (background_ == c)
        return;
    background_ = c;
    Q_EMIT backgroundColorChanged();
    update();
}

void PreviewRhiItem::clearSurface() {
    state_->queue.clear();
    state_->clearRequested.store(true, std::memory_order_relaxed);
    update();
}

void PreviewRhiItem::refreshStatus() {
    Q_EMIT statusChanged();
}

} // namespace mvm::app

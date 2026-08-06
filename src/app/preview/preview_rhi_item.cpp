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

    ~PreviewRhiRenderer() override { teardownDeviceResources(); }

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
    void runColorPatchProbe(const gpu::DecodedGpuFrame& frame);
    void noteDisplayCompletion(const gpu::DecodedGpuFrame& frame, long long qpc);
    bool adoptDevice(void* dev, void* ctx, int featureLevel, unsigned int luidLow, int luidHigh);
    void teardownDeviceResources();

    std::shared_ptr<gpu::PreviewState> state_;
    // **adopted_ だけで「もう初期化済み」と判断しない (§8)。**
    // initialize() は color texture の再生成のたびに呼ばれ、
    // そのとき device が別物へ差し替わっていることがありうる。
    // 実際の device / context ポインタを毎回照合する。
    void* adoptedDevice_ = nullptr;
    void* adoptedContext_ = nullptr;

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

    // **initialize() は複数回呼ばれる。** color texture の再生成 (resize)、
    // window の作り直し、scene graph の invalidate で来る。
    // そのたびに native device / context を照合する (§8)。
    const auto* h = static_cast<const QRhiD3D11NativeHandles*>(r->nativeHandles());
    if (!h || !h->dev || !h->context) {
        std::lock_guard<std::mutex> g(state_->infoMutex);
        state_->initError = "QRhiD3D11NativeHandles から device / context を取得できません";
        state_->initFailed.store(true);
        return;
    }

    if (adoptedDevice_ == h->dev && adoptedContext_ == h->context) {
        // 同一 device。既存 resource をそのまま使ってよい。
        return;
    }

    if (adoptedDevice_ != nullptr) {
        // device が変わった。**黙って古い device を使い続けない。**
        state_->deviceChangeCount.fetch_add(1, std::memory_order_relaxed);
        // P1.1 の方針: 完全再初期化を試みる。
        // decoder は GUI thread が所有しているのでここでは止められない。
        // したがって deviceReady を落として fail-closed にし、
        // GUI 側が decoder を止めてから再 open する契機にする。
        state_->deviceReady.store(false, std::memory_order_release);
        state_->queue.stop();
        teardownDeviceResources();
    }

    if (!adoptDevice(h->dev, h->context, h->featureLevel, h->adapterLuidLow, h->adapterLuidHigh)) {
        if (adoptedDevice_ != nullptr)
            state_->deviceChangeFailClosedCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (state_->deviceChangeCount.load(std::memory_order_relaxed) > 0)
        state_->deviceChangeHandledCount.fetch_add(1, std::memory_order_relaxed);
}

void PreviewRhiRenderer::teardownDeviceResources() {
    // GPU がまだ読んでいるものを即 Release しない。有限 timeout で drain する。
    // timeout したら fail-closed で数える (retirement_timeout_count)。
    if (state_->completion.ready()) {
        state_->retirement.drain([this] { return state_->completion.polledCompleted(); }, 2000);
    }
    state_->retirement.releaseWithoutCompletion();
    releaseRtv();
    state_->converter.release();
    state_->completion.release();
    state_->device.release();
    adoptedDevice_ = nullptr;
    adoptedContext_ = nullptr;
}

bool PreviewRhiRenderer::adoptDevice(void* dev, void* ctx, int featureLevel, unsigned int luidLow,
                                     int luidHigh) {
    auto* d3dDev = static_cast<ID3D11Device*>(dev);
    auto* d3dCtx = static_cast<ID3D11DeviceContext*>(ctx);

    std::string err;
    if (!state_->device.adopt(d3dDev, d3dCtx, err)) {
        std::lock_guard<std::mutex> g(state_->infoMutex);
        state_->initError = "D3D11 device を共有できません: " + err;
        state_->initFailed.store(true);
        return false;
    }
    if (!state_->completion.initialize(state_->device, err)) {
        // GPU 完了を確認する手段が無いなら、frame lifetime を保証できない。
        // 「たぶん大丈夫」で続けない (fail-closed)。
        std::lock_guard<std::mutex> g(state_->infoMutex);
        state_->initError = "GPU 完了追跡を初期化できません: " + err;
        state_->initFailed.store(true);
        return false;
    }
    if (!state_->converter.initialize(state_->device, &state_->counters, err)) {
        std::lock_guard<std::mutex> g(state_->infoMutex);
        state_->initError = "NV12 変換パスを初期化できません: " + err;
        state_->initFailed.store(true);
        return false;
    }
    state_->queue.setExpectedDevice(state_->device.device());

    {
        std::lock_guard<std::mutex> g(state_->infoMutex);
        // QRhi が申告する値と、device から DXGI 経由で取った値を **両方** 記録する。
        // 一致しなければ、どちらかの取得経路が誤っている。
        state_->qtReportedLuidLow = luidLow;
        state_->qtReportedLuidHigh = luidHigh;
        state_->qtFeatureLevel = featureLevel;
        state_->qtDevicePointer = reinterpret_cast<unsigned long long>(d3dDev);
        state_->qtContextPointer = reinterpret_cast<unsigned long long>(d3dCtx);
        state_->gpuCompletionBackend = gpu::toString(state_->completion.backend());
    }

    adoptedDevice_ = dev;
    adoptedContext_ = ctx;
    state_->deviceReady.store(true, std::memory_order_release);
    return true;
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

    // **毎 render で完了済み serial を poll して解放する (§1)。**
    // blocking wait はしない。Flush も呼ばない。
    // GPU が遅れれば retirement queue が深くなるだけで、正しさは崩れない。
    state_->retirement.poll(state_->completion.polledCompleted());

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

    if (ok) {
        runMarkerProbe(frame);
        runColorPatchProbe(frame);
    }

    // draw を発行したので submission serial を得る。**draw の直後に 1 回だけ。**
    // ここで得た serial が完了するまで、この frame の lifetime token と
    // 使用した SRV を解放しない。
    const unsigned long long serial = state_->completion.signalSubmission();
    state_->converter.stampSubmissionSerial(serial);

    cb->endExternal();
    cb->endPass();

    if (!ok) {
        state_->renderErrorCount.fetch_add(1, std::memory_order_relaxed);
        // 描けなかった frame も GPU が触った可能性があるので retire する。
        state_->retirement.retire(serial, frame.lifetime);
        return;
    }

    // **描画してから** displayed とする。submit した番号ではない。
    state_->queue.noteDisplayed(frame);
    state_->displayedFrameNumber.store(frame.frameNumber, std::memory_order_relaxed);
    state_->uniqueDisplayed.fetch_add(1, std::memory_order_relaxed);

    const long long now = gpu::qpcTicks();
    noteDisplayCompletion(frame, now);

    // retainDepth ではなく **GPU 完了**を根拠に保持する (§1)。
    state_->retirement.retire(serial, frame.lifetime);

    if (lastPresentTicks_ != 0)
        state_->pushInterval(gpu::qpcMsBetween(lastPresentTicks_, now));
    lastPresentTicks_ = now;
}

void PreviewRhiRenderer::noteDisplayCompletion(const gpu::DecodedGpuFrame& frame, long long qpc) {
    auto& slot = state_->displayCompletion;
    std::lock_guard<std::mutex> g(slot.mutex);
    if (!slot.waiting)
        return;
    // 4 つすべて一致したときだけ completion とする。
    // 1 つでも緩めると、古い表示を別 request の成功として使ってしまう。
    if (frame.generation != slot.generation || frame.frameNumber != slot.requestedFrame)
        return;

    slot.last.valid = true;
    slot.last.requestId = slot.requestId;
    slot.last.generation = slot.generation;
    slot.last.requestedFrame = slot.requestedFrame;
    slot.last.displayedFrame = frame.frameNumber;
    slot.last.displayedQpc = qpc;
    slot.waiting = false;
}

void PreviewRhiRenderer::runColorPatchProbe(const gpu::DecodedGpuFrame& frame) {
    auto& slot = state_->colorPatch;
    int w = 0, h = 0;
    {
        std::lock_guard<std::mutex> g(slot.mutex);
        if (!slot.requested)
            return;
        if (slot.expectedFrame >= 0 && frame.frameNumber != slot.expectedFrame)
            return;
        w = slot.patchWidth;
        h = slot.patchHeight;
    }

    std::vector<unsigned char> rgba;
    std::string err;
    const bool ok = state_->converter.readColorPatches(frame, w, h, rgba, err);

    std::lock_guard<std::mutex> g(slot.mutex);
    slot.requested = false;
    slot.done = true;
    slot.rgba = std::move(rgba);
    // **どの行列 / レンジが選ばれたか**を必ず一緒に出す。
    // 期待 RGB と合わないとき、metadata が違うのか変換が違うのかを分けるため。
    slot.colorSpace = frame.colorSpace;
    slot.colorRange = frame.colorRange;
    slot.colorSpaceInferred = frame.colorSpaceInferred;
    slot.colorRangeInferred = frame.colorRangeInferred;
    slot.error = ok ? std::string() : err;
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

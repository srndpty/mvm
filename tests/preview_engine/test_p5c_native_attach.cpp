#include "preview_engine/preview_engine_internal.h"

#include <atomic>
#include <chrono>
#include <d3d11.h>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace mvm::preview;
using namespace mvm::preview::internal;

namespace {

template<typename T>
struct ComReleaser {
    void operator()(T* value) const {
        if (value)
            value->Release();
    }
};

template<typename T>
using ComPointer = std::unique_ptr<T, ComReleaser<T>>;

class ImmediateDispatcher final : public PreviewEventDispatcher {
public:
    bool post(std::function<void()> task) override {
        task();
        return true;
    }
};

class RecordingSink final : public PreviewEventSink {
public:
    void stateChanged(PreviewEngineState state) override { states.push_back(state); }

    void positionChanged(PreviewPosition) override {}

    void framePresented(PresentedFrameInfo) override {}

    void errorOccurred(PreviewError error) override { errors.push_back(std::move(error)); }

    void deviceChanged(PreviewDeviceInfo) override { ++deviceChanges; }

    std::vector<PreviewEngineState> states;
    std::vector<PreviewError> errors;
    int deviceChanges = 0;
};

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "検査失敗: " << message << '\n';
        throw std::runtime_error(message);
    }
}

template<typename T>
void require(const Result<T>& result, const char* message) {
    require(result.hasValue(), message);
}

void requireFailure(const Result<void>& result, PreviewErrorCategory category,
                    const char* message) {
    require(!result && result.error().category == category, message);
}

bool createDevice(ComPointer<ID3D11Device>& device, ComPointer<ID3D11DeviceContext>& context) {
    ID3D11Device* rawDevice = nullptr;
    ID3D11DeviceContext* rawContext = nullptr;
    D3D_FEATURE_LEVEL actual{};
    const HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                             D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                             D3D11_SDK_VERSION, &rawDevice, &actual, &rawContext);
    device.reset(rawDevice);
    context.reset(rawContext);
    return SUCCEEDED(result) && device && context;
}

class TestMotion final : public PreviewMotion {
public:
    PreviewMotionValue evaluate(std::int64_t frame) const override {
        lastFrame = frame;
        ++calls;
        PreviewMotionValue value;
        value.destination = {frame == 0 ? 0.0F : 0.5F, 0, 0.5F, 1};
        value.opacity = frame == 0 ? 1.0F : 0.5F;
        if (frame == 16)
            value.opacity = std::numeric_limits<float>::quiet_NaN();
        return value;
    }

    mutable std::int64_t lastFrame = -1;
    mutable int calls = 0;
};

void testMotionRender(ID3D11Device* device, ID3D11DeviceContext* context) {
    D3D11_TEXTURE2D_DESC descriptor{};
    descriptor.Width = 64;
    descriptor.Height = 32;
    descriptor.MipLevels = descriptor.ArraySize = 1;
    descriptor.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    descriptor.SampleDesc.Count = 1;
    descriptor.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* rawTarget = nullptr;
    require(SUCCEEDED(device->CreateTexture2D(&descriptor, nullptr, &rawTarget)),
            "モーション検査の描画先を作れません");
    ComPointer<ID3D11Texture2D> target(rawTarget);
    ID3D11RenderTargetView* rawView = nullptr;
    require(SUCCEEDED(device->CreateRenderTargetView(target.get(), nullptr, &rawView)),
            "モーション検査の描画先viewを作れません");
    ComPointer<ID3D11RenderTargetView> view(rawView);
    descriptor.BindFlags = 0;
    descriptor.Usage = D3D11_USAGE_STAGING;
    descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* rawStaging = nullptr;
    require(SUCCEEDED(device->CreateTexture2D(&descriptor, nullptr, &rawStaging)),
            "モーション検査の読み戻し先を作れません");
    ComPointer<ID3D11Texture2D> staging(rawStaging);

    PreviewEngine engine;
    const auto releaseEngine = [](PreviewEngine* value) {
        if (value->status().state == PreviewEngineState::Shutdown ||
            value->status().state == PreviewEngineState::Error)
            return;
        value->requestShutdown();
        for (int attempt = 0; attempt < 8; ++attempt) {
            const auto completed = PreviewRenderPort::completeRuntimeTeardown(*value);
            if (completed && completed.value())
                break;
        }
    };
    std::unique_ptr<PreviewEngine, decltype(releaseEngine)> cleanup(&engine, releaseEngine);
    require(engine.initialize({{{60, 1}}}, std::make_shared<ImmediateDispatcher>()),
            "モーション検査のengineを初期化できません");
    require(PreviewRenderPort::bindRenderThread(engine), "モーション描画threadを登録できません");
    require(PreviewRenderPort::attachNativeD3D11Device(engine, device, context),
            "モーション検査のdeviceを接続できません");
    auto image = std::make_shared<PreviewStillImage>();
    image->width = 64;
    image->height = 32;
    image->rgba.assign(64 * 32 * 4, 255);
    auto motion = std::make_shared<TestMotion>();
    auto composition = std::make_shared<CompositionSnapshot>();
    PreviewCompositionLayer layer;
    layer.stillImage = image;
    layer.motion = motion;
    composition->layers.push_back(layer);
    require(engine.submitComposition(composition), "モーション構成を受理できません");
    for (const int frame : {0, 15}) {
        require(engine.seek({frame}), "モーションの指定frameへseekできません");
        const float background[4] = {0, 0, 0, 1};
        context->ClearRenderTargetView(view.get(), background);
        bool presented = false;
        for (int attempt = 0; attempt < 8 && !presented; ++attempt) {
            const auto rendered = PreviewRenderPort::renderFrame(engine, view.get(), 64, 32);
            require(rendered, "モーションの指定frameを描画できません");
            presented = rendered.value().presented;
        }
        require(presented && motion->lastFrame == frame && motion->calls > 0,
                "構成を再送せずに描画frameのモーションを評価できません");
        context->CopyResource(staging.get(), target.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(SUCCEEDED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)),
                "モーション描画結果を読み戻せません");
        const auto* pixels = static_cast<const unsigned char*>(mapped.pData) + 16 * mapped.RowPitch;
        const int left = pixels[16 * 4], right = pixels[48 * 4];
        context->Unmap(staging.get(), 0);
        std::cout << "モーション frame " << frame << " 左 " << left << " 右 " << right << std::endl;
        require(frame == 0 ? left > 240 && right < 10 : left < 10 && right >= 120 && right <= 136,
                "描画frameの移動と不透明度が画素へ反映されません");
    }
    require(engine.seek({16}), "不正モーションのframeへseekできません");
    const auto invalid = PreviewRenderPort::renderFrame(engine, view.get(), 64, 32);
    require(!invalid &&
                invalid.error().detail.find("モーションの評価値が不正") != std::string::npos,
            "不正な不透明度を描画して成功扱いにしました");
    require(engine.requestShutdown(), "モーション検査を終了できません");
    bool complete = false;
    for (int attempt = 0; attempt < 8 && !complete; ++attempt) {
        const auto teardown = PreviewRenderPort::completeRuntimeTeardown(engine);
        require(teardown, "モーション検査の解放に失敗しました");
        complete = teardown.value();
    }
    require(complete, "モーション検査の解放が完了しません");
}

// 静止画の矩形 (16, 8)-(48, 24) を frame ごとに塗り替える。frame 0 は赤、1 は緑、20 以降は
// 元の静止画 (白) に戻す。fillPatch を呼んだ回数を数える (state が変わった時だけ呼ぶこと)。
class TestStillAnimation final : public PreviewStillAnimation {
public:
    PreviewPixelRect patchRect() const override { return {16, 8, 32, 16}; }

    std::int64_t stateAt(std::int64_t frame) const override { return frame >= 20 ? -1 : frame % 2; }

    void fillPatch(std::int64_t state, std::uint8_t* out) const override {
        ++fills;
        for (int index = 0; index < 32 * 16; ++index) {
            out[index * 4 + 0] = state == 0 ? 255 : 0;
            out[index * 4 + 1] = state == 0 ? 0 : 255;
            out[index * 4 + 2] = 0;
            out[index * 4 + 3] = 255;
        }
    }

    mutable int fills = 0;
};

void testStillAnimationRender(ID3D11Device* device, ID3D11DeviceContext* context) {
    D3D11_TEXTURE2D_DESC descriptor{};
    descriptor.Width = 64;
    descriptor.Height = 32;
    descriptor.MipLevels = descriptor.ArraySize = 1;
    descriptor.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    descriptor.SampleDesc.Count = 1;
    descriptor.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* rawTarget = nullptr;
    require(SUCCEEDED(device->CreateTexture2D(&descriptor, nullptr, &rawTarget)),
            "静止画animation検査の描画先を作れません");
    ComPointer<ID3D11Texture2D> target(rawTarget);
    ID3D11RenderTargetView* rawView = nullptr;
    require(SUCCEEDED(device->CreateRenderTargetView(target.get(), nullptr, &rawView)),
            "静止画animation検査の描画先viewを作れません");
    ComPointer<ID3D11RenderTargetView> view(rawView);
    descriptor.BindFlags = 0;
    descriptor.Usage = D3D11_USAGE_STAGING;
    descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* rawStaging = nullptr;
    require(SUCCEEDED(device->CreateTexture2D(&descriptor, nullptr, &rawStaging)),
            "静止画animation検査の読み戻し先を作れません");
    ComPointer<ID3D11Texture2D> staging(rawStaging);

    PreviewEngine engine;
    const auto releaseEngine = [](PreviewEngine* value) {
        if (value->status().state == PreviewEngineState::Shutdown ||
            value->status().state == PreviewEngineState::Error)
            return;
        value->requestShutdown();
        for (int attempt = 0; attempt < 8; ++attempt) {
            const auto completed = PreviewRenderPort::completeRuntimeTeardown(*value);
            if (completed && completed.value())
                break;
        }
    };
    std::unique_ptr<PreviewEngine, decltype(releaseEngine)> cleanup(&engine, releaseEngine);
    require(engine.initialize({{{60, 1}}}, std::make_shared<ImmediateDispatcher>()),
            "静止画animation検査のengineを初期化できません");
    require(PreviewRenderPort::bindRenderThread(engine),
            "静止画animationの描画threadを登録できません");
    require(PreviewRenderPort::attachNativeD3D11Device(engine, device, context),
            "静止画animation検査のdeviceを接続できません");
    auto image = std::make_shared<PreviewStillImage>();
    image->width = 64;
    image->height = 32;
    image->rgba.assign(64 * 32 * 4, 255);
    auto animation = std::make_shared<TestStillAnimation>();
    auto composition = std::make_shared<CompositionSnapshot>();
    PreviewCompositionLayer layer;
    layer.stillImage = image;
    layer.stillAnimation = animation;
    composition->layers.push_back(layer);
    require(engine.submitComposition(composition), "静止画animationの構成を受理できません");

    struct Expected {
        int frame;
        int fillsAfter;
        unsigned char insideRed;
        unsigned char insideGreen;
    };

    // 構成を再送せずに frame だけを変える。同じ state の frame (2) では塗り直さない。
    for (const Expected expected :
         {Expected{0, 1, 255, 0}, Expected{1, 2, 0, 255}, Expected{2, 3, 255, 0},
          Expected{2, 3, 255, 0}, Expected{25, 3, 255, 255}, Expected{3, 4, 0, 255}}) {
        require(engine.seek({expected.frame}), "静止画animationの指定frameへseekできません");
        const float background[4] = {0, 0, 0, 1};
        context->ClearRenderTargetView(view.get(), background);
        bool presented = false;
        for (int attempt = 0; attempt < 8 && !presented; ++attempt) {
            const auto rendered = PreviewRenderPort::renderFrame(engine, view.get(), 64, 32);
            require(rendered, "静止画animationの指定frameを描画できません");
            presented = rendered.value().presented;
        }
        require(presented, "静止画animationの指定frameを提示できません");
        context->CopyResource(staging.get(), target.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(SUCCEEDED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)),
                "静止画animationの描画結果を読み戻せません");
        const auto* row = static_cast<const unsigned char*>(mapped.pData) + 16 * mapped.RowPitch;
        const unsigned char insideRed = row[32 * 4], insideGreen = row[32 * 4 + 1];
        const unsigned char outsideRed = row[4 * 4], outsideGreen = row[4 * 4 + 1];
        context->Unmap(staging.get(), 0);
        std::cout << "静止画animation frame " << expected.frame << " 内側 (" << int(insideRed)
                  << ", " << int(insideGreen) << ") 外側 (" << int(outsideRed) << ", "
                  << int(outsideGreen) << ") fillPatch " << animation->fills << std::endl;
        require(insideRed == expected.insideRed && insideGreen == expected.insideGreen,
                "矩形の中が frame の state の画素になりません");
        require(outsideRed == 255 && outsideGreen == 255,
                "矩形の外が静止画の画素のままではありません");
        require(animation->fills == expected.fillsAfter,
                "state が変わった frame でだけ fillPatch を呼ぶ契約に反しました");
    }
    require(engine.requestShutdown(), "静止画animation検査を終了できません");
    bool complete = false;
    for (int attempt = 0; attempt < 8 && !complete; ++attempt) {
        const auto teardown = PreviewRenderPort::completeRuntimeTeardown(engine);
        require(teardown, "静止画animation検査の解放に失敗しました");
        complete = teardown.value();
    }
    require(complete, "静止画animation検査の解放が完了しません");
}

} // namespace

int main() {
    try {
        ComPointer<ID3D11Device> deviceA;
        ComPointer<ID3D11DeviceContext> contextA;
        ComPointer<ID3D11Device> deviceB;
        ComPointer<ID3D11DeviceContext> contextB;
        require(createDevice(deviceA, contextA), "D3D11 device Aを作成できません");
        require(createDevice(deviceB, contextB), "D3D11 device Bを作成できません");
        testMotionRender(deviceA.get(), contextA.get());
        testStillAnimationRender(deviceA.get(), contextA.get());

        PreviewEngine failed;
        auto failedSink = std::make_shared<RecordingSink>();
        require(failed.initialize({{{60, 1}}}, std::make_shared<ImmediateDispatcher>()),
                "failure engine initializeに失敗しました");
        require(failed.attachEventSink(failedSink), "failure sink attachに失敗しました");
        require(PreviewRenderPort::bindRenderThread(failed),
                "failure render thread bindに失敗しました");
        const Result<void> mismatch =
            PreviewRenderPort::attachNativeD3D11Device(failed, deviceA.get(), contextB.get());
        requireFailure(mismatch, PreviewErrorCategory::DeviceFailure,
                       "device/context不一致のattachを受理しました");
        require(mismatch.error().severity == PreviewErrorSeverity::FatalToSession,
                "attach failureをFatalToSessionとして返していません");
        require(failed.status().state == PreviewEngineState::ShuttingDown,
                "attach failureがShuttingDownへ遷移していません");
        require(failed.status().lastError == mismatch.error(),
                "engineに正確なattach root failureが残っていません");
        require(failedSink->errors == std::vector{mismatch.error()},
                "attach error eventが重複または欠落しています");
        require(failedSink->states == std::vector{PreviewEngineState::ShuttingDown},
                "attach failureのShuttingDown eventが重複または欠落しています");
        require(failedSink->deviceChanges == 0,
                "失敗したnative deviceをdevice eventで公開しました");

        const std::size_t errorCount = failedSink->errors.size();
        const std::size_t stateCount = failedSink->states.size();
        requireFailure(
            PreviewRenderPort::attachNativeD3D11Device(failed, deviceA.get(), contextA.get()),
            PreviewErrorCategory::InvalidState, "fatal attach後のsilent retryを受理しました");
        require(failedSink->errors.size() == errorCount && failedSink->states.size() == stateCount,
                "retry rejectでerror/state eventを重複発行しました");
        require(PreviewRenderPort::completeTeardown(failed),
                "runtime未attachのfatal teardownを完了できません");
        require(failed.status().state == PreviewEngineState::Error,
                "attach failureがErrorまでteardownされていません");
        require(failed.status().lastError == mismatch.error(),
                "terminal Errorでattach root failureを失いました");
        require(failedSink->states ==
                    std::vector{PreviewEngineState::ShuttingDown, PreviewEngineState::Error},
                "attach failureのterminal state event順序が違います");
        require(failedSink->errors.size() == 1,
                "terminal completionでattach error eventを重複発行しました");
        require(failed.requestShutdown(), "terminal Errorのshutdownがidempotentではありません");

        PreviewEngine nullFailed;
        require(nullFailed.initialize({{{60, 1}}}, std::make_shared<ImmediateDispatcher>()),
                "null failure engine initializeに失敗しました");
        require(PreviewRenderPort::bindRenderThread(nullFailed),
                "null failure render thread bindに失敗しました");
        requireFailure(PreviewRenderPort::attachNativeD3D11Device(nullFailed, nullptr, nullptr),
                       PreviewErrorCategory::DeviceFailure, "null attachを受理しました");
        require(nullFailed.status().state == PreviewEngineState::ShuttingDown,
                "null attach failureをShuttingDownにしませんでした");
        require(PreviewRenderPort::completeTeardown(nullFailed),
                "null attach failureのteardownを完了できません");
        require(nullFailed.status().state == PreviewEngineState::Error,
                "null attach failureをterminal Errorにしませんでした");

        PreviewEngine unsupportedBackend;
        auto unsupportedSink = std::make_shared<RecordingSink>();
        require(unsupportedBackend.initialize({{{60, 1}}}, std::make_shared<ImmediateDispatcher>()),
                "unsupported backend engine initializeに失敗しました");
        require(unsupportedBackend.attachEventSink(unsupportedSink),
                "unsupported backend sink attachに失敗しました");
        require(PreviewRenderPort::reportUnsupportedRenderBackend(unsupportedBackend),
                "非D3D11 backendをsession fatalへ昇格できませんでした");
        require(
            unsupportedBackend.status().state == PreviewEngineState::ShuttingDown &&
                unsupportedSink->errors.size() == 1 &&
                unsupportedSink->errors.front().category == PreviewErrorCategory::DeviceFailure &&
                unsupportedSink->errors.front().severity == PreviewErrorSeverity::FatalToSession,
            "非D3D11 backendのfail-closed状態が不正です");
        require(PreviewRenderPort::completeTeardown(unsupportedBackend) &&
                    unsupportedBackend.status().state == PreviewEngineState::Error,
                "非D3D11 backendをlogical teardownできませんでした");

        PreviewEngine missingHandles;
        auto missingHandlesSink = std::make_shared<RecordingSink>();
        require(missingHandles.initialize({{{60, 1}}}, std::make_shared<ImmediateDispatcher>()),
                "missing native handles engine initializeに失敗しました");
        require(missingHandles.attachEventSink(missingHandlesSink),
                "missing native handles sink attachに失敗しました");
        require(PreviewRenderPort::reportMissingNativeD3D11Handles(missingHandles),
                "native handles欠如をsession fatalへ昇格できませんでした");
        require(missingHandles.status().state == PreviewEngineState::ShuttingDown &&
                    missingHandlesSink->errors.size() == 1 &&
                    missingHandlesSink->errors.front().category ==
                        PreviewErrorCategory::DeviceFailure &&
                    missingHandlesSink->errors.front().severity ==
                        PreviewErrorSeverity::FatalToSession &&
                    missingHandlesSink->errors.front().detail.find("device/context") !=
                        std::string::npos,
                "native handles欠如のfail-closed状態が不正です");
        require(PreviewRenderPort::completeTeardown(missingHandles) &&
                    missingHandles.status().state == PreviewEngineState::Error,
                "native handles欠如をlogical teardownできませんでした");

        PreviewEngine changed;
        auto changedSink = std::make_shared<RecordingSink>();
        require(changed.initialize({{{60, 1}}}, std::make_shared<ImmediateDispatcher>()),
                "device change engine initializeに失敗しました");
        require(changed.attachEventSink(changedSink), "device change sink attachに失敗しました");
        require(PreviewRenderPort::bindRenderThread(changed),
                "device change render thread bindに失敗しました");
        require(PreviewRenderPort::attachNativeD3D11Device(changed, deviceA.get(), contextA.get()),
                "device change testの初回attachに失敗しました");
        require(PreviewRenderPort::acquireNativeD3D11Device(changed, deviceA.get(), contextA.get()),
                "renderer再生成時の同一native runtime引き継ぎを拒否しました");
        const Result<void> changedIdentity =
            PreviewRenderPort::acquireNativeD3D11Device(changed, deviceB.get(), contextB.get());
        requireFailure(changedIdentity, PreviewErrorCategory::DeviceFailure,
                       "native device差し替えを受理しました");
        require(changedIdentity.error().severity == PreviewErrorSeverity::FatalToSession,
                "native device差し替えをsession fatalにしませんでした");
        require(changed.status().state == PreviewEngineState::ShuttingDown,
                "native device差し替えがteardownを開始しませんでした");
        require(changed.status().lastError == changedIdentity.error(),
                "native device差し替えのroot errorを保持していません");
        require(changedSink->errors == std::vector{changedIdentity.error()},
                "native device差し替えのerror eventが重複または欠落しています");
        bool changedComplete = false;
        for (int attempt = 0; attempt < 8 && !changedComplete; ++attempt) {
            const Result<bool> teardown = PreviewRenderPort::completeRuntimeTeardown(changed);
            require(teardown, "device change runtime teardownに失敗しました");
            changedComplete = teardown.value();
        }
        require(changedComplete && changed.status().state == PreviewEngineState::Error,
                "native device差し替えをterminal Errorにできませんでした");

        PreviewEngine rtvFailed;
        auto rtvSink = std::make_shared<RecordingSink>();
        require(rtvFailed.initialize({{{60, 1}}}, std::make_shared<ImmediateDispatcher>()),
                "RTV failure engine initializeに失敗しました");
        require(rtvFailed.attachEventSink(rtvSink), "RTV failure sink attachに失敗しました");
        require(
            PreviewRenderPort::acquireNativeD3D11Device(rtvFailed, deviceA.get(), contextA.get()),
            "RTV failure native attachに失敗しました");
        require(PreviewRenderPort::reportRenderTargetFailure(rtvFailed, E_INVALIDARG),
                "RTV生成失敗をengineへ記録できませんでした");
        require(rtvFailed.status().state == PreviewEngineState::ShuttingDown,
                "RTV生成失敗がteardownを開始しませんでした");
        require(rtvSink->errors.size() == 1 &&
                    rtvSink->errors.front().category == PreviewErrorCategory::DeviceFailure &&
                    rtvSink->errors.front().severity == PreviewErrorSeverity::FatalToSession &&
                    rtvSink->errors.front().detail.find("HRESULT=0x80070057") != std::string::npos,
                "RTV生成失敗のHRESULT付きsession fatalが不正です");
        bool rtvComplete = false;
        for (int attempt = 0; attempt < 8 && !rtvComplete; ++attempt) {
            const Result<bool> teardown = PreviewRenderPort::completeRuntimeTeardown(rtvFailed);
            require(teardown, "RTV failure runtime teardownに失敗しました");
            rtvComplete = teardown.value();
        }
        require(rtvComplete && rtvFailed.status().state == PreviewEngineState::Error,
                "RTV生成失敗をterminal Errorにできませんでした");

        PreviewEngine pausedDeviceLost;
        auto pausedDeviceLostSink = std::make_shared<RecordingSink>();
        require(pausedDeviceLost.initialize({{{60, 1}}}, std::make_shared<ImmediateDispatcher>()),
                "paused device lost test initializeに失敗しました");
        require(pausedDeviceLost.attachEventSink(pausedDeviceLostSink),
                "paused device lost sink attachに失敗しました");
        require(PreviewRenderPort::acquireNativeD3D11Device(pausedDeviceLost, deviceA.get(),
                                                            contextA.get()),
                "paused device lost native attachに失敗しました");
        require(pausedDeviceLost.status().state == PreviewEngineState::ReadyPaused,
                "device lost注入前のstateがReadyPausedではありません");
        require(PreviewRenderPort::reportDeviceLost(pausedDeviceLost, DXGI_ERROR_DEVICE_REMOVED),
                "pause中のdevice lostをengineへ記録できませんでした");
        require(pausedDeviceLost.status().state == PreviewEngineState::ShuttingDown &&
                    pausedDeviceLostSink->errors.size() == 1 &&
                    pausedDeviceLostSink->errors.front().category ==
                        PreviewErrorCategory::DeviceFailure &&
                    pausedDeviceLostSink->errors.front().severity ==
                        PreviewErrorSeverity::FatalToSession &&
                    pausedDeviceLostSink->errors.front().detail.find("GetDeviceRemovedReason") !=
                        std::string::npos,
                "pause中のdevice lostがHRESULT付きsession fatalになっていません");
        require(PreviewRenderPort::runtimeDiagnostics(pausedDeviceLost).deviceLostCount == 1,
                "実device lostをactive runtime診断へ1件記録していません");
        bool pausedDeviceLostComplete = false;
        for (int attempt = 0; attempt < 8 && !pausedDeviceLostComplete; ++attempt) {
            const Result<bool> teardown =
                PreviewRenderPort::completeRuntimeTeardown(pausedDeviceLost);
            require(teardown, "paused device lost runtime teardownに失敗しました");
            pausedDeviceLostComplete = teardown.value();
        }
        require(pausedDeviceLostComplete &&
                    pausedDeviceLost.status().state == PreviewEngineState::Error,
                "pause中のdevice lostをterminal Errorにできませんでした");
        require(PreviewRenderPort::runtimeDiagnostics(pausedDeviceLost).deviceLostCount == 1,
                "実device lostがterminal diagnosticsから失われました");

        PreviewEngine replaced;
        auto replacedSink = std::make_shared<RecordingSink>();
        require(replaced.initialize({{{60, 1}}}, std::make_shared<ImmediateDispatcher>()),
                "engine replacement test initializeに失敗しました");
        require(replaced.attachEventSink(replacedSink),
                "engine replacement sink attachに失敗しました");
        require(
            PreviewRenderPort::acquireNativeD3D11Device(replaced, deviceA.get(), contextA.get()),
            "engine replacement native attachに失敗しました");
        require(PreviewRenderPort::reportEngineReplacement(replaced),
                "engine差し替えをsession fatalへ昇格できませんでした");
        require(replaced.status().state == PreviewEngineState::ShuttingDown &&
                    replacedSink->errors.size() == 1 &&
                    replacedSink->errors.front().detail.find("engine差し替え") != std::string::npos,
                "engine差し替えで旧runtimeのteardownが開始されませんでした");
        bool replacedComplete = false;
        for (int attempt = 0; attempt < 8 && !replacedComplete; ++attempt) {
            const Result<bool> teardown = PreviewRenderPort::completeRuntimeTeardown(replaced);
            require(teardown, "engine replacement runtime teardownに失敗しました");
            replacedComplete = teardown.value();
        }
        require(replacedComplete && replaced.status().state == PreviewEngineState::Error,
                "engine差し替え後に旧runtimeをterminal Errorまでteardownできませんでした");

        PreviewEngine temporaryDetach;
        auto temporaryDetachSink = std::make_shared<RecordingSink>();
        require(temporaryDetach.initialize({{{60, 1}}}, std::make_shared<ImmediateDispatcher>()),
                "temporary renderer detach test initializeに失敗しました");
        require(temporaryDetach.attachEventSink(temporaryDetachSink),
                "temporary renderer detach sink attachに失敗しました");
        require(PreviewRenderPort::acquireNativeD3D11Device(temporaryDetach, deviceA.get(),
                                                            contextA.get()),
                "temporary renderer detach native attachに失敗しました");
        require(PreviewRenderPort::completeRendererDetach(temporaryDetach),
                "active rendererの一時detachに失敗しました");
        require(temporaryDetach.status().state == PreviewEngineState::ReadyPaused &&
                    temporaryDetachSink->errors.empty() &&
                    PreviewRenderPort::nativeRuntimeAttached(temporaryDetach),
                "一時的なrenderer破棄でactive sessionを終了しました");
        require(PreviewRenderPort::acquireNativeD3D11Device(temporaryDetach, deviceA.get(),
                                                            contextA.get()),
                "後継rendererへnative runtimeを引き継げませんでした");
        require(temporaryDetach.requestShutdown(),
                "temporary renderer detach test shutdown requestに失敗しました");
        require(PreviewRenderPort::completeRendererDetach(temporaryDetach),
                "temporary renderer detach test cleanupに失敗しました");
        require(temporaryDetach.status().state == PreviewEngineState::Shutdown,
                "temporary renderer detach testを安全に終了できませんでした");

        PreviewEngine detachedShutdown;
        require(detachedShutdown.initialize({{{60, 1}}}, std::make_shared<ImmediateDispatcher>()),
                "detached shutdown test initializeに失敗しました");
        require(PreviewRenderPort::acquireNativeD3D11Device(detachedShutdown, deviceA.get(),
                                                            contextA.get()),
                "detached shutdown native attachに失敗しました");
        require(PreviewRenderPort::completeRendererDetach(detachedShutdown),
                "shutdown前のrenderer detachに失敗しました");
        require(detachedShutdown.requestShutdown(),
                "renderer消失後のshutdown requestに失敗しました");
        for (int attempt = 0;
             attempt < 200 && detachedShutdown.status().state != PreviewEngineState::Shutdown;
             ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(detachedShutdown.status().state == PreviewEngineState::Shutdown,
                "renderer消失後のstandby authorityがteardownを完了しませんでした");

        for (int iteration = 0; iteration < 16; ++iteration) {
            PreviewEngine racedDetach;
            require(racedDetach.initialize({{{60, 1}}}, std::make_shared<ImmediateDispatcher>()),
                    "detach/shutdown race test initializeに失敗しました");
            std::atomic_bool attached = false;
            std::atomic_bool start = false;
            bool attachSucceeded = false;
            bool detachSucceeded = false;
            std::thread renderer([&] {
                attachSucceeded = PreviewRenderPort::acquireNativeD3D11Device(
                                      racedDetach, deviceA.get(), contextA.get())
                                      .hasValue();
                attached.store(true, std::memory_order_release);
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                detachSucceeded = PreviewRenderPort::completeRendererDetach(racedDetach).hasValue();
            });
            while (!attached.load(std::memory_order_acquire))
                std::this_thread::yield();
            require(attachSucceeded, "race test native attachに失敗しました");
            start.store(true, std::memory_order_release);
            require(racedDetach.requestShutdown(),
                    "detachと競合したshutdown requestに失敗しました");
            renderer.join();
            require(detachSucceeded, "shutdownと競合したrenderer detachに失敗しました");
            for (int attempt = 0;
                 attempt < 200 && racedDetach.status().state != PreviewEngineState::Shutdown;
                 ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(racedDetach.status().state == PreviewEngineState::Shutdown,
                    "detach/shutdown競合後にteardown authorityを失いました");
        }

        PreviewEngine requestedDetach;
        auto requestedDetachSink = std::make_shared<RecordingSink>();
        require(requestedDetach.initialize({{{60, 1}}}, std::make_shared<ImmediateDispatcher>()),
                "requested detach test initializeに失敗しました");
        require(requestedDetach.attachEventSink(requestedDetachSink),
                "requested detach sink attachに失敗しました");
        require(PreviewRenderPort::acquireNativeD3D11Device(requestedDetach, deviceA.get(),
                                                            contextA.get()),
                "requested detach native attachに失敗しました");
        require(requestedDetach.requestShutdown(),
                "renderer detach前のshutdown requestに失敗しました");
        require(PreviewRenderPort::completeRendererDetach(requestedDetach),
                "shutdown requested rendererの最終detach handshakeに失敗しました");
        require(requestedDetach.status().state == PreviewEngineState::Shutdown &&
                    requestedDetachSink->errors.empty(),
                "正常shutdown済みrenderer detachをErrorへ誤変換しました");

        PreviewEngine engine;
        require(engine.initialize({{{60, 1}}}, std::make_shared<ImmediateDispatcher>()),
                "success engine initializeに失敗しました");
        require(PreviewRenderPort::bindRenderThread(engine),
                "success render thread bindに失敗しました");
        require(PreviewRenderPort::attachNativeD3D11Device(engine, deviceA.get(), contextA.get()),
                "compatible native attachに失敗しました");
        require(engine.status().state == PreviewEngineState::ReadyPaused,
                "successful real attachだけがReadyPausedになっていません");
        requireFailure(
            PreviewRenderPort::attachNativeD3D11Device(engine, deviceA.get(), contextA.get()),
            PreviewErrorCategory::InvalidState, "duplicate attachを受理しました");
        requireFailure(
            PreviewRenderPort::attachNativeD3D11Device(engine, deviceB.get(), contextB.get()),
            PreviewErrorCategory::InvalidState, "incompatible reattachを受理しました");

        require(engine.requestShutdown(), "shutdown requestに失敗しました");
        bool complete = false;
        for (int attempt = 0; attempt < 8 && !complete; ++attempt) {
            const Result<bool> teardown = PreviewRenderPort::completeRuntimeTeardown(engine);
            require(teardown, "runtime teardownに失敗しました");
            complete = teardown.value();
        }
        require(complete && engine.status().state == PreviewEngineState::Shutdown,
                "native runtimeを安全にShutdownできません");
        std::cout << "PASS: P5-C native attach negative contract\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}

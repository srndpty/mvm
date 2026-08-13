#include "preview_engine/preview_engine_internal.h"

#include <d3d11.h>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
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
    if (!condition)
        throw std::runtime_error(message);
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

} // namespace

int main() {
    try {
        ComPointer<ID3D11Device> deviceA;
        ComPointer<ID3D11DeviceContext> contextA;
        ComPointer<ID3D11Device> deviceB;
        ComPointer<ID3D11DeviceContext> contextB;
        require(createDevice(deviceA, contextA), "D3D11 device Aを作成できません");
        require(createDevice(deviceB, contextB), "D3D11 device Bを作成できません");

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

        PreviewEngine changed;
        auto changedSink = std::make_shared<RecordingSink>();
        require(changed.initialize({{{60, 1}}}, std::make_shared<ImmediateDispatcher>()),
                "device change engine initializeに失敗しました");
        require(changed.attachEventSink(changedSink), "device change sink attachに失敗しました");
        require(PreviewRenderPort::bindRenderThread(changed),
                "device change render thread bindに失敗しました");
        require(PreviewRenderPort::attachNativeD3D11Device(changed, deviceA.get(), contextA.get()),
                "device change testの初回attachに失敗しました");
        require(
            PreviewRenderPort::validateNativeD3D11Device(changed, deviceA.get(), contextA.get()),
            "同一native identityを拒否しました");
        const Result<void> changedIdentity =
            PreviewRenderPort::validateNativeD3D11Device(changed, deviceB.get(), contextB.get());
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

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

        PreviewEngine finalDetach;
        auto finalDetachSink = std::make_shared<RecordingSink>();
        require(finalDetach.initialize({{{60, 1}}}, std::make_shared<ImmediateDispatcher>()),
                "final renderer detach test initializeに失敗しました");
        require(finalDetach.attachEventSink(finalDetachSink),
                "final renderer detach sink attachに失敗しました");
        require(
            PreviewRenderPort::acquireNativeD3D11Device(finalDetach, deviceA.get(), contextA.get()),
            "final renderer detach native attachに失敗しました");
        require(PreviewRenderPort::completeRendererDetach(finalDetach),
                "active rendererの最終detach handshakeに失敗しました");
        require(finalDetach.status().state == PreviewEngineState::Error &&
                    finalDetachSink->errors.size() == 1 &&
                    finalDetachSink->errors.front().detail.find("renderer破棄") !=
                        std::string::npos,
                "active renderer最終detachをfail-closed terminalにできませんでした");

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

#include "preview_engine/preview_engine_internal.h"

#include <d3d11.h>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

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

        PreviewEngine engine;
        require(engine.initialize({{{60, 1}}}, std::make_shared<ImmediateDispatcher>()),
                "engine initializeに失敗しました");
        require(PreviewRenderPort::bindRenderThread(engine), "render thread bindに失敗しました");
        requireFailure(PreviewRenderPort::attachNativeD3D11Device(engine, nullptr, nullptr),
                       PreviewErrorCategory::DeviceFailure, "null attachを受理しました");
        requireFailure(
            PreviewRenderPort::attachNativeD3D11Device(engine, deviceA.get(), contextB.get()),
            PreviewErrorCategory::DeviceFailure, "device/context不一致のattachを受理しました");
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

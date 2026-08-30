#include "app/preview/presentation_eligibility_preflight.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <windows.h>

namespace mvm::app {
namespace {

std::string narrow(const wchar_t* text) {
    if (text == nullptr || *text == L'\0')
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string result(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}

void captureAdapter(ID3D11Device* device, PresentationEligibilityPreflight& result) {
    if (device == nullptr)
        return;
    IDXGIDevice* dxgiDevice = nullptr;
    if (FAILED(
            device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) ||
        dxgiDevice == nullptr)
        return;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter != nullptr) {
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            result.adapter_available = true;
            result.adapter_luid_low = desc.AdapterLuid.LowPart;
            result.adapter_luid_high = desc.AdapterLuid.HighPart;
            result.adapter_description = narrow(desc.Description);
        }
        // ALLOW_TEARING は factory capability であり、この Present が tearing した
        // ことを意味しない。
        IDXGIFactory5* factory5 = nullptr;
        if (SUCCEEDED(
                adapter->GetParent(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&factory5))) &&
            factory5 != nullptr) {
            BOOL allowTearing = FALSE;
            if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                        &allowTearing, sizeof(allowTearing)))) {
                result.tearing_support_available = true;
                result.tearing_supported = allowTearing != FALSE;
            }
            factory5->Release();
        }
        adapter->Release();
    }
    dxgiDevice->Release();
}

void captureOutput(IDXGISwapChain1* swapChain, PresentationEligibilityPreflight& result) {
    IDXGIOutput* output = nullptr;
    if (FAILED(swapChain->GetContainingOutput(&output)) || output == nullptr)
        return;
    DXGI_OUTPUT_DESC desc{};
    if (SUCCEEDED(output->GetDesc(&desc))) {
        result.output_available = true;
        result.monitor_handle = reinterpret_cast<std::uint64_t>(desc.Monitor);
        result.output_device_name = narrow(desc.DeviceName);
        result.desktop_left = desc.DesktopCoordinates.left;
        result.desktop_top = desc.DesktopCoordinates.top;
        result.desktop_right = desc.DesktopCoordinates.right;
        result.desktop_bottom = desc.DesktopCoordinates.bottom;
        result.attached_to_desktop = desc.AttachedToDesktop != FALSE;
    }
    // hardware composition capability。actual MPO 使用の証拠ではない。
    IDXGIOutput6* output6 = nullptr;
    if (SUCCEEDED(
            output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&output6))) &&
        output6 != nullptr) {
        UINT flags = 0;
        if (SUCCEEDED(output6->CheckHardwareCompositionSupport(&flags))) {
            result.hardware_composition_support_available = true;
            result.hardware_composition_support_flags = flags;
        }
        output6->Release();
    }
    output->Release();
}

void captureWindow(HWND window, PresentationEligibilityPreflight& result) {
    if (window == nullptr || IsWindow(window) == FALSE)
        return;
    result.window_available = true;
    result.window_handle = reinterpret_cast<std::uint64_t>(window);
    result.window_style = static_cast<std::uint32_t>(GetWindowLongPtrW(window, GWL_STYLE));
    result.window_ex_style = static_cast<std::uint32_t>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        result.cloaked_available = true;
        result.cloaked = cloaked;
    }
    RECT windowRect{};
    if (GetWindowRect(window, &windowRect)) {
        result.window_left = windowRect.left;
        result.window_top = windowRect.top;
        result.window_right = windowRect.right;
        result.window_bottom = windowRect.bottom;
    }
    RECT clientRect{};
    if (GetClientRect(window, &clientRect)) {
        result.client_width = clientRect.right - clientRect.left;
        result.client_height = clientRect.bottom - clientRect.top;
    }
}

} // namespace

PresentationEligibilityPreflight
capturePresentationEligibilityPreflight(std::uint64_t swapchainIdentity, void* nativeDevice,
                                        void* windowHandle) {
    PresentationEligibilityPreflight result;
    result.swapchain_identity = swapchainIdentity;
    captureWindow(static_cast<HWND>(windowHandle), result);
    captureAdapter(static_cast<ID3D11Device*>(nativeDevice), result);
    if (swapchainIdentity == 0) {
        result.error = "native present hookがswapchain identityを記録していません";
        return result;
    }
    auto* unknown = reinterpret_cast<IUnknown*>(static_cast<std::uintptr_t>(swapchainIdentity));
    IDXGISwapChain1* swapChain1 = nullptr;
    if (FAILED(unknown->QueryInterface(__uuidof(IDXGISwapChain1),
                                       reinterpret_cast<void**>(&swapChain1))) ||
        swapChain1 == nullptr) {
        result.error = "swapchain identityからIDXGISwapChain1を取得できません";
        return result;
    }
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (SUCCEEDED(swapChain1->GetDesc1(&desc))) {
        result.swapchain_desc_available = true;
        result.width = desc.Width;
        result.height = desc.Height;
        result.format = static_cast<std::uint32_t>(desc.Format);
        result.stereo = desc.Stereo != FALSE;
        result.sample_count = desc.SampleDesc.Count;
        result.sample_quality = desc.SampleDesc.Quality;
        result.buffer_usage = desc.BufferUsage;
        result.buffer_count = desc.BufferCount;
        result.scaling = static_cast<std::uint32_t>(desc.Scaling);
        result.swap_effect = static_cast<std::uint32_t>(desc.SwapEffect);
        result.alpha_mode = static_cast<std::uint32_t>(desc.AlphaMode);
        result.flags = desc.Flags;
        result.frame_latency_waitable_object =
            (desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0;
    } else {
        result.error = "GetDesc1に失敗しました";
    }
    captureOutput(swapChain1, result);
    IDXGISwapChain2* swapChain2 = nullptr;
    if (SUCCEEDED(swapChain1->QueryInterface(__uuidof(IDXGISwapChain2),
                                             reinterpret_cast<void**>(&swapChain2))) &&
        swapChain2 != nullptr) {
        UINT latency = 0;
        if (SUCCEEDED(swapChain2->GetMaximumFrameLatency(&latency))) {
            result.maximum_frame_latency_available = true;
            result.maximum_frame_latency = latency;
        }
        swapChain2->Release();
    }
    swapChain1->Release();
    result.captured = result.swapchain_desc_available;
    return result;
}

} // namespace mvm::app

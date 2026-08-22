#include "media/gpu_preview/window_output_vblank_observer.h"

#include "media/gpu_preview/qpc_clock.h"

#include <dxgi1_6.h>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

namespace mvm::gpu {
namespace {

using Microsoft::WRL::ComPtr;

std::string narrow(const wchar_t* value) {
    if (!value)
        return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
        return {};
    std::string result(static_cast<std::size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), length, nullptr, nullptr);
    return result;
}

// windowが載っているmonitorのGDI device名に一致するdisplay pathから、exactな
// refresh rationalを取る。DWMのrateRefreshは使わない。
bool queryOutputRefreshRational(const std::wstring& gdiDeviceName, long long& numerator,
                                long long& denominator) {
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                           modes.data(), nullptr) != ERROR_SUCCESS)
        return false;
    paths.resize(pathCount);
    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = path.sourceInfo.adapterId;
        sourceName.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS)
            continue;
        if (gdiDeviceName != sourceName.viewGdiDeviceName)
            continue;
        if (path.targetInfo.refreshRate.Numerator == 0 ||
            path.targetInfo.refreshRate.Denominator == 0)
            return false;
        numerator = path.targetInfo.refreshRate.Numerator;
        denominator = path.targetInfo.refreshRate.Denominator;
        return true;
    }
    return false;
}

ComPtr<IDXGIOutput> findOutputForMonitor(HMONITOR monitor, WindowOutputIdentity& identity,
                                         std::string& error) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        error = "CreateDXGIFactory1に失敗しました";
        return nullptr;
    }
    for (UINT adapterIndex = 0;; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 adapterDesc{};
        if (FAILED(adapter->GetDesc1(&adapterDesc)))
            continue;
        for (UINT outputIndex = 0;; ++outputIndex) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_OUTPUT_DESC outputDesc{};
            if (FAILED(output->GetDesc(&outputDesc)))
                continue;
            if (outputDesc.Monitor != monitor)
                continue;
            identity.monitorHandle = reinterpret_cast<std::uint64_t>(monitor);
            identity.outputIndex = outputIndex;
            identity.adapterLuidLow = adapterDesc.AdapterLuid.LowPart;
            identity.adapterLuidHigh = adapterDesc.AdapterLuid.HighPart;
            identity.outputDeviceName = narrow(outputDesc.DeviceName);
            identity.desktopLeft = outputDesc.DesktopCoordinates.left;
            identity.desktopTop = outputDesc.DesktopCoordinates.top;
            identity.desktopRight = outputDesc.DesktopCoordinates.right;
            identity.desktopBottom = outputDesc.DesktopCoordinates.bottom;
            return output;
        }
    }
    error = "windowのHMONITORに一致するDXGI outputがありません";
    return nullptr;
}

} // namespace

WindowOutputResolveResult resolveWindowOutput(void* windowHandle) {
    WindowOutputResolveResult result;
    const auto hwnd = static_cast<HWND>(windowHandle);
    if (!hwnd || !IsWindow(hwnd)) {
        result.error = "window handleが不正です";
        return result;
    }
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    if (!monitor) {
        result.error = "windowがどのmonitorにも載っていません";
        return result;
    }
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        result.error = "GetMonitorInfoWに失敗しました";
        return result;
    }
    const auto output = findOutputForMonitor(monitor, result.identity, result.error);
    if (!output)
        return result;
    result.identity.gdiDeviceName = narrow(monitorInfo.szDevice);
    if (!queryOutputRefreshRational(monitorInfo.szDevice, result.identity.refreshNumerator,
                                    result.identity.refreshDenominator)) {
        result.error = "window outputのQueryDisplayConfig refresh rationalを取得できません";
        return result;
    }
    result.identity.available = true;
    result.ok = true;
    return result;
}

WindowOutputVBlankObserver::~WindowOutputVBlankObserver() {
    stop();
}

bool WindowOutputVBlankObserver::start(void* windowHandle, std::string& error) {
    if (running_.load(std::memory_order_acquire)) {
        error = "VBlank observerは既に動作しています";
        return false;
    }
    const auto hwnd = static_cast<HWND>(windowHandle);
    if (!hwnd || !IsWindow(hwnd)) {
        error = "window handleが不正です";
        return false;
    }
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    if (!monitor) {
        error = "windowがどのmonitorにも載っていません";
        return false;
    }
    auto resolved = resolveWindowOutput(windowHandle);
    if (!resolved.ok) {
        error = resolved.error;
        return false;
    }
    WindowOutputIdentity discard;
    std::string findError;
    ComPtr<IDXGIOutput> output = findOutputForMonitor(monitor, discard, findError);
    if (!output) {
        error = findError;
        return false;
    }
    identity_ = resolved.identity;
    ring_.reset();
    waitFailures_.store(0, std::memory_order_relaxed);
    priorityState_.store(0, std::memory_order_relaxed);
    stopRequested_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    output->AddRef();
    thread_ = std::thread(&WindowOutputVBlankObserver::run, this, output.Get());
    // normal priorityではVBlankを取りこぼすため、authorityとして成立しない。
    // 昇格に失敗したらfallbackせずAUTHORITY_UNAVAILABLEとして失敗させる。
    for (int attempt = 0; attempt < 500; ++attempt) {
        const int state = priorityState_.load(std::memory_order_acquire);
        if (state == 1)
            return true;
        if (state == 2) {
            stop();
            error = "VBlank observer threadをTIME_CRITICALへ昇格できません "
                    "(AUTHORITY_UNAVAILABLE)";
            return false;
        }
        Sleep(1);
    }
    stop();
    error = "VBlank observer threadの優先度設定を確認できません (AUTHORITY_UNAVAILABLE)";
    return false;
}

void WindowOutputVBlankObserver::stop() {
    stopRequested_.store(true, std::memory_order_release);
    if (thread_.joinable())
        thread_.join();
    running_.store(false, std::memory_order_release);
}

void WindowOutputVBlankObserver::run(void* outputHandle) {
    ComPtr<IDXGIOutput> output;
    output.Attach(static_cast<IDXGIOutput*>(outputHandle));
    // wakeが1 refresh遅れるとWaitForVBlankは次のVBlankまで待つため、observerは
    // VBlankを1本取りこぼす。ordinalは自前counterなのでそれを検出できない。
    // 優先度を上げて取りこぼし自体を起こさせない。失敗は黙って許容しない。
    const bool promoted = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != 0;
    priorityState_.store(promoted ? 1 : 2, std::memory_order_release);
    if (!promoted)
        return;
    long long ordinal = 0;
    while (!stopRequested_.load(std::memory_order_acquire)) {
        if (FAILED(output->WaitForVBlank())) {
            waitFailures_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        ring_.capture({ordinal, qpcTicks()});
        ++ordinal;
    }
}

} // namespace mvm::gpu

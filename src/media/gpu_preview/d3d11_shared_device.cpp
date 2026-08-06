#include "media/gpu_preview/d3d11_shared_device.h"

#include <cstdio>
#include <dxgi.h>

namespace mvm::gpu {
namespace {

std::string wideToUtf8(const wchar_t* w) {
    if (!w)
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string hresultText(const char* what, HRESULT hr) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "%s に失敗しました (HRESULT=0x%08lX)", what,
                  static_cast<unsigned long>(hr));
    return buf;
}

} // namespace

bool queryAdapterInfo(ID3D11Device* device, AdapterInfo& out, std::string& err) {
    out = AdapterInfo{};
    if (!device) {
        err = "device が null です";
        return false;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    HRESULT hr =
        device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(hr) || !dxgiDevice) {
        err = hresultText("IDXGIDevice の取得", hr);
        return false;
    }

    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(hr) || !adapter) {
        err = hresultText("IDXGIAdapter の取得", hr);
        return false;
    }

    DXGI_ADAPTER_DESC desc{};
    hr = adapter->GetDesc(&desc);
    adapter->Release();
    if (FAILED(hr)) {
        err = hresultText("DXGI_ADAPTER_DESC の取得", hr);
        return false;
    }

    out.valid = true;
    out.luidLow = desc.AdapterLuid.LowPart;
    out.luidHigh = desc.AdapterLuid.HighPart;
    out.vendorId = desc.VendorId;
    out.deviceId = desc.DeviceId;
    out.description = wideToUtf8(desc.Description);
    out.featureLevel = static_cast<int>(device->GetFeatureLevel());
    return true;
}

SharedD3D11Device::~SharedD3D11Device() {
    release();
}

bool SharedD3D11Device::adopt(ID3D11Device* device, ID3D11DeviceContext* context,
                              std::string& err) {
    release();

    if (!device || !context) {
        err = "Qt から D3D11 device / context を取得できませんでした";
        return false;
    }

    // ID3D11DeviceContext は thread-safe ではない。decode thread と
    // render thread が同じ immediate context を共有する以上、
    // driver 側の直列化を必ず有効にする。
    ID3D10Multithread* mt = nullptr;
    HRESULT hr =
        context->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&mt));
    if (FAILED(hr) || !mt) {
        err = hresultText("ID3D10Multithread の取得", hr);
        return false;
    }
    const BOOL prev = mt->SetMultithreadProtected(TRUE);
    const BOOL now = mt->GetMultithreadProtected();
    mt->Release();
    (void)prev;

    if (!now) {
        // ここで成功を返すと、稀にしか壊れない共有状態を作ってしまう。
        err = "ID3D10Multithread::SetMultithreadProtected が有効になりませんでした";
        return false;
    }
    multithreadProtected_ = true;

    AdapterInfo info;
    if (!queryAdapterInfo(device, info, err))
        return false;

    device_ = device;
    context_ = context;
    device_->AddRef();
    context_->AddRef();
    adapter_ = info;
    return true;
}

void SharedD3D11Device::release() {
    if (context_) {
        context_->Release();
        context_ = nullptr;
    }
    if (device_) {
        device_->Release();
        device_ = nullptr;
    }
    adapter_ = AdapterInfo{};
    multithreadProtected_ = false;
}

bool SharedD3D11Device::deviceLost(long& reasonOut) const {
    reasonOut = 0;
    if (!device_)
        return false;
    const HRESULT hr = device_->GetDeviceRemovedReason();
    reasonOut = static_cast<long>(hr);
    return hr != S_OK;
}

} // namespace mvm::gpu

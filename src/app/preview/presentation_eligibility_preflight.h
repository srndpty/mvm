#ifndef MVM_APP_PREVIEW_PRESENTATION_ELIGIBILITY_PREFLIGHT_H
#define MVM_APP_PREVIEW_PRESENTATION_ELIGIBILITY_PREFLIGHT_H

#include <cstdint>
#include <string>

namespace mvm::app {

// F3-C3-A3-T2-D1-B0 diagnostic-only preflight。
//
// これは presentation path の authority ではない。actual presentation path は
// PresentMon/ETW 側の PresentMode / DisplayedQPC provenance でのみ判定する。
// ここに載るのは eligibility を説明しうる static configuration と capability
// だけであり、capability が真でもその Present が independent flip / MPO された
// ことを意味しない。
//
// 値は Qt の設定や環境変数から再構成せず、実際に作成済みの
// IDXGISwapChain / IDXGIOutput / IDXGIFactory / Win32 window から取得する。
struct PresentationEligibilityPreflight {
    bool captured = false;
    std::string error;

    // 実 swapchain object の identity と DXGI_SWAP_CHAIN_DESC1 実値。
    std::uint64_t swapchain_identity = 0;
    bool swapchain_desc_available = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = 0;
    bool stereo = false;
    std::uint32_t sample_count = 0;
    std::uint32_t sample_quality = 0;
    std::uint32_t buffer_usage = 0;
    std::uint32_t buffer_count = 0;
    std::uint32_t scaling = 0;
    std::uint32_t swap_effect = 0;
    std::uint32_t alpha_mode = 0;
    std::uint32_t flags = 0;

    bool frame_latency_waitable_object = false;
    bool maximum_frame_latency_available = false;
    std::uint32_t maximum_frame_latency = 0;

    // adapter / output identity。
    bool adapter_available = false;
    std::uint32_t adapter_luid_low = 0;
    std::int32_t adapter_luid_high = 0;
    std::string adapter_description;

    bool output_available = false;
    std::uint64_t monitor_handle = 0;
    std::string output_device_name;
    std::int32_t desktop_left = 0;
    std::int32_t desktop_top = 0;
    std::int32_t desktop_right = 0;
    std::int32_t desktop_bottom = 0;
    bool attached_to_desktop = false;

    // capability。actual use ではない。
    bool tearing_support_available = false;
    bool tearing_supported = false;
    bool hardware_composition_support_available = false;
    std::uint32_t hardware_composition_support_flags = 0;

    // window state。
    bool window_available = false;
    std::uint64_t window_handle = 0;
    std::uint32_t window_style = 0;
    std::uint32_t window_ex_style = 0;
    bool cloaked_available = false;
    std::uint32_t cloaked = 0;
    std::int32_t window_left = 0;
    std::int32_t window_top = 0;
    std::int32_t window_right = 0;
    std::int32_t window_bottom = 0;
    std::int32_t client_width = 0;
    std::int32_t client_height = 0;
};

// swapchainIdentity は patched Qt が記録した実 IDXGISwapChain ポインタである。
// 同一プロセス・swapchain を所有する render thread から呼ぶこと。
PresentationEligibilityPreflight capturePresentationEligibilityPreflight(
    std::uint64_t swapchainIdentity, void* nativeDevice, void* windowHandle);

} // namespace mvm::app
#endif

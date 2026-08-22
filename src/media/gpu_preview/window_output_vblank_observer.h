#ifndef MVM_GPU_PREVIEW_WINDOW_OUTPUT_VBLANK_OBSERVER_H
#define MVM_GPU_PREVIEW_WINDOW_OUTPUT_VBLANK_OBSERVER_H

#include "media/gpu_preview/window_output_vblank_authority.h"

#include <atomic>
#include <string>
#include <thread>

namespace mvm::gpu {

struct WindowOutputResolveResult {
    bool ok = false;
    WindowOutputIdentity identity;
    std::string error;
};

// windowのHMONITORに対応するDXGI outputと、その同じoutputの
// QueryDisplayConfig exact refresh rationalを解決する。
// DwmGetCompositionTimingInfoは参照しない。
WindowOutputResolveResult resolveWindowOutput(void* windowHandle);

// 解決したoutput専用threadでIDXGIOutput::WaitForVBlankを回し、physical VBlank
// sequenceだけを記録する。formal opportunity ordinalのauthorityはこれに限る。
class WindowOutputVBlankObserver {
public:
    ~WindowOutputVBlankObserver();

    bool start(void* windowHandle, std::string& error);
    void stop();

    const WindowOutputIdentity& identity() const { return identity_; }

    const VBlankRing& ring() const { return ring_; }

    long long waitFailureCount() const { return waitFailures_.load(std::memory_order_acquire); }

    bool running() const { return running_.load(std::memory_order_acquire); }

    // TIME_CRITICALを取れなかった場合、normal priorityへ黙ってfallbackしない。
    bool timeCriticalPriority() const {
        return priorityState_.load(std::memory_order_acquire) == 1;
    }

private:
    void run(void* output);

    WindowOutputIdentity identity_{};
    VBlankRing ring_;
    std::thread thread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::atomic<long long> waitFailures_{0};
    std::atomic<int> priorityState_{0};
};

} // namespace mvm::gpu

#endif

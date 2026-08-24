#ifndef MVM_GPU_PREVIEW_WINDOW_OUTPUT_VBLANK_OBSERVER_H
#define MVM_GPU_PREVIEW_WINDOW_OUTPUT_VBLANK_OBSERVER_H

#include "media/gpu_preview/window_output_vblank_authority.h"

#include <atomic>
#include <string>
#include <thread>

namespace mvm::gpu {

// P2-D5-2-W2-A.1。measurement 窓が開く前に physical VBlank を1本観測したことの
// 証拠。これが無いと domain の下側 bracket (predecessor) が
// 「observer の最初の wake が render callback より先に返ったか」という race に
// なる。
struct VBlankPrerollResult {
    bool completed = false;
    // acquisition liveness timeout。performance threshold ではない。
    bool timedOut = false;
    VBlankObservation sample{};
    long long waitElapsedQpc = 0;
};

// P2-D5-2-W2-C0.1。frozen measurement end の上側を閉じる physical VBlank。
// sample 数の増加ではなく sample.qpc >= frozenMeasurementEndQpc を証拠にする。
struct VBlankSuccessorResult {
    bool completed = false;
    // acquisition liveness timeout。performance threshold ではない。
    bool timedOut = false;
    long long frozenMeasurementEndQpc = 0;
    VBlankObservation sample{};
    long long waitElapsedQpc = 0;
};

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

    // baselineSerial より後に新しく publish された sample を1件だけ bounded に
    // 待つ。ring が空でないことではなく publish serial の前進で判定するので、
    // start/stop 再利用時に stale sample を受理しない。
    // timeoutMs は acquisition liveness timeout であり performance threshold では
    // ない。timeout した場合は false を返す。呼び出し側は measurement を開始して
    // はならない。
    bool prerollNewSample(unsigned long long baselineSerial, long long timeoutMs,
                          VBlankPrerollResult& result);

    // frozenMeasurementEndQpc 以上の最初の published sample を bounded に待つ。
    // measurement end 自体は変更しない。timeout または observer failure は false。
    bool waitForSuccessor(long long frozenMeasurementEndQpc, long long timeoutMs,
                          VBlankSuccessorResult& result);

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

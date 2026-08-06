/*
 * mvm Phase 1 / P1 - GUI / decode / render の 3 スレッドが共有する状態
 *
 * Qt には依存しない。Qt 側 (PreviewRhiItem / PreviewRhiRenderer) は
 * この構造体への shared_ptr だけを持つ。
 *
 * どのスレッドが何を触ってよいかを、フィールドごとに明記する。
 * ここが曖昧なままだと、再現しない不具合として後で出てくる。
 */

#ifndef MVM_GPU_PREVIEW_PREVIEW_STATE_H
#define MVM_GPU_PREVIEW_PREVIEW_STATE_H

#include "media/gpu_preview/d3d11_shared_device.h"
#include "media/gpu_preview/frame_queue.h"
#include "media/gpu_preview/nv12_converter.h"
#include "media/gpu_preview/readback_counter.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace mvm::gpu {

// marker 検証の依頼と結果。render thread が実行し、GUI thread が読む。
struct MarkerProbeSlot {
    std::mutex mutex;
    bool requested = false; // GUI -> render
    bool done = false;      // render -> GUI
    long long expectedFrame = -1;
    long long displayedFrame = -1;
    long long markerValue = -1;
    bool syncOk = false;
    std::string error;
};

struct PreviewState {
    // --- render thread が所有する -------------------------------------------
    SharedD3D11Device device;
    Nv12Converter converter;
    ReadbackCounters counters;

    // --- 3 スレッド共有 (内部で lock 済み) ----------------------------------
    PreviewFrameQueue queue{3, 3};
    MarkerProbeSlot markerProbe;

    // --- render -> GUI (atomic) ---------------------------------------------
    std::atomic<bool> deviceReady{false};
    std::atomic<bool> initFailed{false};
    std::atomic<long long> presentCount{0};     // render() が呼ばれた回数
    std::atomic<long long> uniqueDisplayed{0};  // 新しい frame を描いた回数
    std::atomic<long long> repeatedPresents{0}; // 新しい frame が無く前の絵のままだった回数
    std::atomic<long long> deviceLostCount{0};
    std::atomic<long long> renderErrorCount{0};
    std::atomic<long long> displayedFrameNumber{-1};

    // --- GUI -> render (atomic) ---------------------------------------------
    std::atomic<bool> linearFilter{true};
    std::atomic<bool> clearRequested{false};
    // 計測区間だけ frame interval を記録する。
    std::atomic<bool> collectIntervals{false};

    // --- 初期化結果 (render thread が書き、deviceReady 後に GUI が読む) ------
    std::mutex infoMutex;
    std::string initError;
    std::string rhiBackend;
    // QRhi が申告する adapter LUID (native handles 由来)。
    unsigned int qtReportedLuidLow = 0;
    int qtReportedLuidHigh = 0;
    int qtFeatureLevel = 0;
    unsigned long long qtDevicePointer = 0;
    unsigned long long qtContextPointer = 0;

    // --- frame interval (render thread が push、GUI が最後に読む) -----------
    std::mutex intervalMutex;
    std::vector<double> frameIntervalsMs;

    void pushInterval(double ms) {
        if (!collectIntervals.load(std::memory_order_relaxed))
            return;
        std::lock_guard<std::mutex> g(intervalMutex);
        frameIntervalsMs.push_back(ms);
    }
};

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_PREVIEW_STATE_H

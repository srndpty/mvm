#ifndef MVM_APP_PREVIEW_COMPOSITOR_RHI_ITEM_H
#define MVM_APP_PREVIEW_COMPOSITOR_RHI_ITEM_H

#include "media/gpu_preview/composition_display_ledger.h"
#include "media/gpu_preview/compositor_coordinator.h"
#include "media/gpu_preview/gpu_compositor.h"
#include "media/gpu_preview/source_decode_worker.h"

#include <atomic>
#include <memory>
#include <mutex>

#include <QQuickRhiItem>

namespace mvm::app {

struct CompositorSpikeState {
    gpu::SharedD3D11Device device;
    gpu::ReadbackCounters readbacks;
    gpu::GpuCompositor compositor;
    gpu::CompositorCoordinator coordinator;
    gpu::CompositionDisplayLedger ledger{1024};

    mutable std::mutex workerMutex;
    std::shared_ptr<gpu::SourceDecodeWorker> workerA;
    std::shared_ptr<gpu::SourceDecodeWorker> workerB;

    std::atomic<bool> deviceReady{false};
    std::atomic<bool> fatal{false};
    std::atomic<bool> teardownRequested{false};
    std::atomic<bool> teardownComplete{false};
    std::atomic<bool> playbackSchedulerEnabled{false};
    std::atomic<bool> testDeviceChange{false};
    std::atomic<long long> requestedOutput{-1};
    std::atomic<long long> scheduledOutputCount{0};
    std::atomic<long long> droppedOutputCount{0};
    std::atomic<long long> schedulerDeadlineDropCount{0};
    std::atomic<long long> missingPairDropCount{0};
    std::atomic<long long> missingSourceADropCount{0};
    std::atomic<long long> missingSourceBDropCount{0};
    std::atomic<long long> displayedCompositionCount{0};
    std::atomic<long long> presentCallbackCount{0};
    std::atomic<long long> repeatedPresentCount{0};
    std::atomic<long long> logicalClearCount{0};
    std::atomic<long long> renderFailureCount{0};
    std::atomic<long long> deviceLostCount{0};
    std::atomic<long long> lifecycleOrderViolationCount{0};
    std::atomic<long long> actualTargetProbeMismatch{0};
    std::atomic<bool> actualTargetProbeDone{false};
    std::atomic<unsigned long long> nativeDevicePointer{0};
    gpu::AdapterInfo qtAdapter;
    std::mutex errorMutex;
    std::string fatalReason;
};

class CompositorRhiItem : public QQuickRhiItem {
    Q_OBJECT
public:
    explicit CompositorRhiItem(QQuickItem* parent = nullptr);

    std::shared_ptr<CompositorSpikeState> state() const { return state_; }

    void setPreferredCompletionBackend(gpu::GpuCompletionBackend backend) {
        preferredCompletion_ = backend;
    }

    void requestTeardown();

protected:
    QQuickRhiItemRenderer* createRenderer() override;

private:
    std::shared_ptr<CompositorSpikeState> state_;
    gpu::GpuCompletionBackend preferredCompletion_ = gpu::GpuCompletionBackend::Fence;
};

} // namespace mvm::app
#endif

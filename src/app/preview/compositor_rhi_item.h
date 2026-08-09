#ifndef MVM_APP_PREVIEW_COMPOSITOR_RHI_ITEM_H
#define MVM_APP_PREVIEW_COMPOSITOR_RHI_ITEM_H

#include "media/audio_preview/audio_clock.h"
#include "media/gpu_preview/composition_display_ledger.h"
#include "media/gpu_preview/compositor_coordinator.h"
#include "media/gpu_preview/gpu_compositor.h"
#include "media/gpu_preview/source_decode_worker.h"

#include <atomic>
#include <memory>
#include <mutex>

#include <QQuickRhiItem>

namespace mvm::app {

enum class CompositorDiagnosticCase { None = 0, SingleDecode, PairOnly, FixedTextures, FullPath };

struct CompositorDiagnosticRenderSample {
    double schedulerToPairUs = 0.0;
    double pairUs = 0.0;
    double compositionPrepareUs = 0.0;
    double compositionIssueUs = 0.0;
    double completionPollUs = 0.0;
    double qtExternalUs = 0.0;
    double renderCallbackTotalUs = 0.0;
    size_t bufferDepthA = 0;
    size_t bufferDepthB = 0;
};

struct CompositorMeasurementCounters {
    long long qpc = 0;
    long long compositionRequested = 0;
    long long compositionDrawn = 0;
    long long gpuSubmission = 0;
    long long layerDraw = 0;
    long long logicalClear = 0;
    long long scheduled = 0;
    long long displayed = 0;
    long long dropped = 0;
    long long missingPair = 0;
    long long sourceAEof = 0;
    long long sourceBEof = 0;
    long long dropSchedulerDeadline = 0;
    long long dropMissingSourceA = 0;
    long long dropMissingSourceB = 0;
    long long dropMissingBoth = 0;
    long long dropStaleGeneration = 0;
    long long dropFutureGeneration = 0;
    long long dropStaleCompositionEpoch = 0;
    long long dropRenderFailure = 0;
    long long presentCallback = 0;
    long long repeatedPresent = 0;
    long long partialGpuIssueFailure = 0;
    long long completionPollFailure = 0;
    long long untrackedSubmission = 0;
};

struct CompositorMarkerProbe {
    std::mutex mutex;
    bool requested = false;
    bool done = false;
    long long expectedFrame = -1;
    gpu::DecodedGpuFrame frameA;
    gpu::DecodedGpuFrame frameB;
    long long markerA = -1;
    long long markerB = -1;
    bool syncA = false;
    bool syncB = false;
    std::string error;
};

struct P3MeasurementDisplayRecord {
    long long outputFrameNumber = -1;
    long long displayRecordQpc = 0;
    bool applicationAvProjectionValid = false;
    double applicationAvDeltaMs = 0.0;
};

struct ActualTargetPixelSize {
    int width = 0;
    int height = 0;
};

struct CompositorSpikeState {
    gpu::SharedD3D11Device device;
    gpu::ReadbackCounters readbacks;
    gpu::GpuCompositor compositor;
    gpu::CompositorCoordinator coordinator;
    // formal playback の 3600 display identity を欠落なく保持する。
    gpu::CompositionDisplayLedger ledger{4096};

    mutable std::mutex workerMutex;
    std::shared_ptr<gpu::SourceDecodeWorker> workerA;
    std::shared_ptr<gpu::SourceDecodeWorker> workerB;

    std::atomic<bool> deviceReady{false};
    std::atomic<bool> fatal{false};
    std::atomic<bool> teardownRequested{false};
    std::atomic<bool> teardownComplete{false};
    std::atomic<bool> playbackSchedulerEnabled{false};
    // P3-B 専用。P2 の QPC scheduler とは同時に有効にしない。
    std::shared_ptr<audio::AudioMasterClock> audioMasterClock;
    std::atomic<bool> audioMasterSchedulerEnabled{false};
    std::atomic<unsigned long long> audioMasterGeneration{0};
    std::atomic<long long> audioMasterVideoFrameCount{0};
    std::atomic<long long> audioMasterLastRequested{-1};
    std::atomic<long long> audioMasterLastDisplayed{-1};
    std::atomic<long long> audioClockVideoStaleDiscardA{0};
    std::atomic<long long> audioClockVideoStaleDiscardB{0};
    std::atomic<long long> audioClockVideoCatchupSkipCount{0};
    std::atomic<long long> videoPairWaitCount{0};
    std::atomic<long long> videoTargetSupersededCount{0};
    std::atomic<long long> videoAheadViolationCount{0};
    std::atomic<long long> videoClockRegressionCount{0};
    std::atomic<long long> videoQpcMasterFallbackCount{0};
    std::atomic<bool> audioMasterMarkerProbePending{false};
    std::mutex applicationAvDeltaMutex;
    std::vector<double> applicationAvDeltaMs;
    std::atomic<bool> p3MeasurementActive{false};
    std::atomic<long long> p3MeasurementEndSampleExclusive{0};
    std::mutex p3MeasurementDisplayMutex;
    std::vector<P3MeasurementDisplayRecord> p3MeasurementDisplays;
    std::atomic<bool> testDeviceChange{false};
    std::atomic<long long> requestedOutput{-1};
    std::atomic<long long> scheduledOutputCount{0};
    std::atomic<long long> droppedOutputCount{0};
    std::atomic<long long> schedulerDeadlineDropCount{0};
    std::atomic<long long> missingPairDropCount{0};
    std::atomic<long long> sourceAEofCount{0};
    std::atomic<long long> sourceBEofCount{0};
    std::atomic<long long> missingSourceADropCount{0};
    std::atomic<long long> missingSourceBDropCount{0};
    std::atomic<long long> missingBothDropCount{0};
    std::atomic<long long> staleGenerationDropCount{0};
    std::atomic<long long> futureGenerationDropCount{0};
    std::atomic<long long> staleCompositionEpochDropCount{0};
    std::atomic<long long> displayedCompositionCount{0};
    std::atomic<long long> presentCallbackCount{0};
    std::atomic<long long> repeatedPresentCount{0};
    std::atomic<long long> logicalClearCount{0};
    std::atomic<long long> renderFailureCount{0};
    std::atomic<long long> deviceLostCount{0};
    std::atomic<long long> lifecycleOrderViolationCount{0};
    std::atomic<long long> actualTargetProbeMismatch{0};
    std::atomic<long long> actualTargetProbeChecked{0};
    std::atomic<bool> actualTargetProbeStarted{false};
    std::atomic<bool> actualTargetProbeDone{false};
    std::atomic<int> actualOutputWidth{0};
    std::atomic<int> actualOutputHeight{0};
    std::atomic<unsigned long long> actualOutputSizePacked{0};

    void publishActualOutputSize(int width, int height) {
        actualOutputWidth.store(width, std::memory_order_relaxed);
        actualOutputHeight.store(height, std::memory_order_relaxed);
        const auto packed =
            (static_cast<unsigned long long>(static_cast<unsigned int>(width)) << 32U) |
            static_cast<unsigned int>(height);
        actualOutputSizePacked.store(packed, std::memory_order_release);
    }

    ActualTargetPixelSize actualOutputSizeSnapshot() const {
        const auto packed = actualOutputSizePacked.load(std::memory_order_acquire);
        return {static_cast<int>(static_cast<unsigned int>(packed >> 32U)),
                static_cast<int>(static_cast<unsigned int>(packed))};
    }

    std::string actualGpuCompletionBackend;
    std::atomic<CompositorDiagnosticCase> diagnosticCase{CompositorDiagnosticCase::None};
    std::atomic<bool> diagnosticTimingEnabled{false};
    std::mutex diagnosticTimingMutex;
    std::vector<CompositorDiagnosticRenderSample> diagnosticRenderSamples;
    std::vector<double> diagnosticRenderCallbackIntervalUs;
    gpu::D3D11LockTimingSnapshot diagnosticLockTiming;
    gpu::ComposedFrame diagnosticFixedFrame;
    CompositorMarkerProbe markerProbe;
    std::atomic<long long> markerAChecked{0};
    std::atomic<long long> markerBChecked{0};
    std::atomic<long long> markerAMismatch{0};
    std::atomic<long long> markerBMismatch{0};

    // render threadが境界snapshotを作る。これによりwarmup中のGPU commandを
    // measurementへ混ぜず、全counterを同じ区間で差分化する。
    std::atomic<bool> measurementResetRequested{false};
    std::atomic<bool> measurementResetCaptured{false};
    std::atomic<bool> measurementStartRequested{false};
    std::atomic<bool> measurementStartCaptured{false};
    std::atomic<bool> measurementStopRequested{false};
    std::atomic<bool> measurementStopCaptured{false};
    std::atomic<long long> measurementDurationQpc{0};
    std::atomic<long long> measurementEndQpc{0};
    std::atomic<bool> measurementIntervalActive{false};
    std::atomic<long long> measurementFirstOutputFrame{-1};
    std::mutex measurementMutex;
    CompositorMeasurementCounters measurementStart;
    CompositorMeasurementCounters measurementStop;
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

#ifndef MVM_APPS_COMPOSITOR_SPIKE_CONTROLLER_H
#define MVM_APPS_COMPOSITOR_SPIKE_CONTROLLER_H

#include "app/preview/compositor_rhi_item.h"
#include "core/mvm_parallel_dispatch.h"
#include "core/presentation_opportunity_mapper.h"
#include "media/gpu_preview/window_output_vblank_observer.h"

#include <vector>

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

namespace mvm::app {

struct DwmPresentationTimingSnapshot {
    bool available = false;
    unsigned int refreshNumerator = 0;
    unsigned int refreshDenominator = 0;
    unsigned int composeNumerator = 0;
    unsigned int composeDenominator = 0;
    bool displayConfigAvailable = false;
    bool displayConfigSingleActiveFallback = false;
    unsigned int displayConfigActivePathCount = 0;
    unsigned int displayRefreshNumerator = 0;
    unsigned int displayRefreshDenominator = 0;
    long long qpcVBlank = 0;
    long long qpcRefreshPeriod = 0;
    unsigned long long refreshCount = 0;
    unsigned long long frameDisplayedCount = 0;
};

enum class CompositorMode { Playback, Seek, Layout };
enum class NativePresentHookMode { Disabled, OffControl, OnDiagnostic };

struct CompositorSpikeConfig {
    QString sourceA;
    QString sourceB;
    QString metricsPath;
    int warmupSeconds = 5;
    int measureSeconds = 10;
    unsigned int seed = 20260808;
    int seekCount = 64;
    int displayTimeoutMs = 2000;
    CompositorMode mode = CompositorMode::Playback;
    gpu::GpuCompletionBackend completion = gpu::GpuCompletionBackend::Fence;
    QString testFault;
    bool formalPreflight = false;
    bool diagnosticTiming = false;
    bool schedulerPhaseRing = false;
    bool presentationOpportunityRing = false;
    bool formalSchedulerInvocationLedger = false;
    bool vblankObserver = false;
    bool incrementalMapperShadow = false;
    NativePresentHookMode nativePresentHook = NativePresentHookMode::Disabled;
    bool diagnosticTargetPixelToggle = false;
    CompositorDiagnosticCase diagnosticCase = CompositorDiagnosticCase::None;
};

class CompositorSpikeController final : public QObject {
    Q_OBJECT
public:
    explicit CompositorSpikeController(CompositorSpikeConfig config, QObject* parent = nullptr);
    void attach(CompositorRhiItem* item);

    int exitCode() const { return exitCode_; }

Q_SIGNALS:
    void finished();

private:
    struct SeekConcurrencySample {
        long long requestStartQpc = 0;
        long long aRequestQpc = 0;
        long long bRequestQpc = 0;
        long long dispatchCompleteQpc = 0;
        long long aBeginQpc = 0;
        long long aReadyQpc = 0;
        long long bBeginQpc = 0;
        long long bReadyQpc = 0;
        unsigned long long aRequestId = 0;
        unsigned long long bRequestId = 0;
        gpu::SeekRequestResult aRequestResult = gpu::SeekRequestResult::RejectedInvalid;
        gpu::SeekRequestResult bRequestResult = gpu::SeekRequestResult::RejectedInvalid;
        bool dispatchOrderValid = false;
    };
    enum class Phase {
        WaitDevice,
        MarkerStart,
        MarkerWait,
        OutputPreflightWait,
        Warmup,
        MeasurementResetStart,
        MeasurementResetWait,
        MeasurementPrimeStart,
        MeasurementPrimeWait,
        CaptureEnvelopeStartWait,
        MeasureStartWait,
        Measure,
        MeasureStopWait,
        CaptureEnvelopeStopWait,
        FatalMeasureStopWait,
        SeekStart,
        SeekDecodeWait,
        SeekDisplayWait,
        LayoutStart,
        LayoutWait,
        ShutdownWait,
        Done
    };
    static const char* phaseName(Phase phase);
    void reportDiagnosticPhase();
    void tick();
    bool startWorkers();
    void startMarkerProbe();
    void pollMarkerProbe();
    bool resetAfterMarkerPreflight();
    bool resetPlaybackForMeasurement();
    void requestMeasurementStart();
    void armMeasurementAfterCaptureEnvelopeOpen();
    bool startVBlankObserverWithPreroll();
    bool closeVBlankMappingSupportAfterTeardown();
    void startSeek();
    void pollSeekDecode();
    void pollSeekDisplay();
    void startLayoutChange();
    void pollLayoutChange();
    void beginShutdown(const QString& reason, bool failure);
    void performShutdown();
    bool writeMetrics();
    bool pollIncrementalMapperShadow(bool finalizing);

    struct IncrementalMapperTransition {
        const char* eventType = "NONE";
        long long qpc = 0;
        long long vblankOrdinal = -1;
        long long swapOrdinal = -1;
        long long sourceFrame = -1;
        core::MappingSolutionClass solutionClass = core::MappingSolutionClass::NoSolution;
        bool hasClosedRecords = false;
        std::size_t closedRecordCount = 0;
        std::size_t commitWatermark = 0;
        core::IncrementalMappingError error = core::IncrementalMappingError::None;
    };

    CompositorSpikeConfig config_;
    CompositorRhiItem* item_ = nullptr;
    std::shared_ptr<CompositorSpikeState> state_;
    std::shared_ptr<gpu::SourceDecodeWorker> workerA_;
    std::shared_ptr<gpu::SourceDecodeWorker> workerB_;
    QTimer timer_;
    QElapsedTimer phaseTimer_;
    Phase phase_ = Phase::WaitDevice;
    Phase lastReportedDiagnosticPhase_ = Phase::Done;
    int exitCode_ = 0;
    CompositorMeasurementCounters measurementStart_;
    CompositorMeasurementCounters measurementStop_;
    gpu::SourceDecoderSnapshot measurementStartA_;
    gpu::SourceDecoderSnapshot measurementStartB_;
    gpu::SourceDecoderSnapshot measurementStopA_;
    gpu::SourceDecoderSnapshot measurementStopB_;
    double measureElapsedSeconds_ = 0;
    bool measurementStartCaptured_ = false;
    bool measurementStopCaptured_ = false;
    bool measurementAvailable_ = false;
    long long sourceAFrameCount_ = -1;
    long long sourceBFrameCount_ = -1;
    long long requiredMeasurementFrameCount_ = 0;
    bool sourceCoverageOk_ = false;
    bool measurementPrerollOk_ = false;
    long long measurementPrerollDepthA_ = -1;
    long long measurementPrerollDepthB_ = -1;
    long long measurementPrerollFrontA_ = -1;
    long long measurementPrerollFrontB_ = -1;
    std::vector<long long> seekTargets_;
    const std::vector<long long> markerTargets_{0, 1, 137, 299, 600, 1799, 3599};
    size_t markerIndex_ = 0;
    size_t seekIndex_ = 0;
    unsigned long long waitBaseline_ = 0;
    gpu::CompositionDisplayExpectation waitExpectation_;
    QElapsedTimer waitTimer_;
    long long seekRequestStartQpc_ = 0;
    long long seekDecodeReadyQpc_ = 0;
    core::ParallelDispatchOrder seekDispatchOrder_;
    gpu::SeekTicket seekTicketA_;
    gpu::SeekTicket seekTicketB_;
    gpu::SeekCompletion seekCompletionA_;
    gpu::SeekCompletion seekCompletionB_;
    bool seekAReady_ = false;
    bool seekBReady_ = false;
    std::vector<double> seekAMs_;
    std::vector<double> seekBMs_;
    std::vector<double> seekDecodeReadyMs_;
    std::vector<double> seekDecodeReadyToPairMs_;
    std::vector<double> seekPairToSubmissionMs_;
    std::vector<double> seekSubmissionToDisplayMs_;
    std::vector<double> seekDecodeReadyToDisplayMs_;
    std::vector<double> seekDisplayedMs_;
    std::vector<SeekConcurrencySample> seekConcurrencySamples_;
    int seekMismatch_ = 0;
    int seekTimeout_ = 0;
    int seekStaleCompletion_ = 0;
    size_t layoutIndex_ = 0;
    int layoutMismatch_ = 0;
    bool seekLockTimingActive_ = false;
    QString shutdownReason_;
    QString startupError_;
    DwmPresentationTimingSnapshot dwmTimingStart_;
    DwmPresentationTimingSnapshot dwmTimingStop_;
    // F3-B0 shadow: physical VBlank observerはformal authorityへは接続せず、
    // 対応証明のためのrawとしてだけ採取する。
    gpu::WindowOutputVBlankObserver vblankObserver_;
    gpu::WindowOutputIdentity vblankIdentityStart_{};
    gpu::WindowOutputIdentity vblankIdentityEnd_{};
    bool vblankObserverStarted_ = false;
    bool vblankObserverRunning_ = false;
    // P2-D5-2-W2-A.1 lower boundary preroll。shadow-only provenance。
    gpu::VBlankPrerollResult vblankPreroll_{};
    // P2-D5-2-W2-C0.1 upper boundary closure。measurement endはこの待機で変更しない。
    gpu::VBlankSuccessorResult vblankSuccessor_{};
    // W2-C1.1。candidate capture全体を包含するmapping supportの上側witness。
    gpu::VBlankSuccessorResult vblankMappingSupportPostroll_{};
    long long vblankMappingSupportPostrollBoundaryQpc_ = 0;
    bool vblankMappingSupportTeardownCompleted_ = false;
    long long frozenMeasurementEndQpc_ = 0;
    bool captureEnvelopeCloseFailure_ = false;
    QString captureEnvelopeCloseReason_;
    std::string vblankObserverError_;
    core::IncrementalOpportunityMapper incrementalMapperShadow_{1};
    std::size_t incrementalVblankRead_ = 0;
    std::size_t incrementalSwapRead_ = 0;
    gpu::VBlankObservation incrementalLastBeforeStart_{};
    gpu::VBlankObservation incrementalDomainBoundary_{};
    bool incrementalMapperOriginSelected_ = false;
    bool incrementalMapperDomainClosed_ = false;
    bool incrementalMapperFinalized_ = false;
    bool incrementalMapperPass_ = false;
    std::string incrementalMapperError_;
    std::vector<IncrementalMapperTransition> incrementalMapperTransitions_;
    std::vector<long long> incrementalCommitQpc_;
    long long formalAuthorityLastPollQpc_ = 0;
};

} // namespace mvm::app
#endif

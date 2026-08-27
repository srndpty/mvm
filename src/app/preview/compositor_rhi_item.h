#ifndef MVM_APP_PREVIEW_COMPOSITOR_RHI_ITEM_H
#define MVM_APP_PREVIEW_COMPOSITOR_RHI_ITEM_H

#include "app/preview/native_present_hook.h"
#include "app/preview/presentation_eligibility_preflight.h"
#include "media/audio_preview/audio_clock.h"
#include "media/audio_preview/runtime_attribution.h"
#include "media/gpu_preview/composition_display_ledger.h"
#include "media/gpu_preview/compositor_coordinator.h"
#include "media/gpu_preview/gpu_compositor.h"
#include "media/gpu_preview/phase4_composition_driver.h"
#include "media/gpu_preview/presentation_opportunity_attribution.h"
#include "media/gpu_preview/presentation_opportunity_scheduler.h"
#include "media/gpu_preview/scheduler_phase_attribution.h"
#include "media/gpu_preview/source_decode_worker.h"
#include "media/gpu_preview/transition_probe.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

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

enum class NativePresentIntentScope {
    ForeignPreMeasurement = 0,
    CurrentMeasurement = 1,
};

enum class MeasurementBoundaryRelation {
    Unresolved = 0,
    PreMeasurementArm,
    ArmedPreMeasurement,
    WithinCurrentMeasurement,
    PostMeasurement,
};

enum class RenderTeardownDiagnosticStage {
    NotRequested = 0,
    Requested,
    RenderCallbackObserved,
    WorkerJoinPending,
    ProbeDrain,
    CompositorDrain,
    Failed,
    Complete,
};

enum class TerminalRenderExitDiagnosticStage {
    NotObserved = 0,
    FinishMeasurementEntered,
    FinishMeasurementReturned,
    PresentationCaptureDestructorComplete,
    NativeTokenDestructorEntered,
    NativeTokenDestructorComplete,
    RenderCallbackExited,
};

struct NativePresentIntentScopeRecord {
    std::uint64_t tokenSerial = 0;
    std::uint64_t intentOrdinal = 0;
    NativePresentIntentScope scope = NativePresentIntentScope::ForeignPreMeasurement;
    long long decisionQpc = 0;
    bool decisionQpcExact = false;
    bool requiredCurrentMembership = false;
    bool requiredCurrentMembershipExact = false;
    MeasurementBoundaryRelation measurementBoundaryRelation =
        MeasurementBoundaryRelation::Unresolved;
    bool producerSemanticsExact = false;
    bool duplicateCallback = false;
    bool repeat = false;
    bool pastSourceDomain = false;
    long long targetFrame = -1;
    long long lastFinalizedOpportunityOrdinal = -1;
    long long renderBeginQpc = 0;
    gpu::FormalIntentTransportDisposition transportDisposition =
        gpu::FormalIntentTransportDisposition::Transport;
};

inline const char* nativePresentIntentScopeName(NativePresentIntentScope scope) {
    switch (scope) {
    case NativePresentIntentScope::ForeignPreMeasurement:
        return "FOREIGN_PRE_MEASUREMENT";
    case NativePresentIntentScope::CurrentMeasurement:
        return "CURRENT_MEASUREMENT";
    }
    return "UNKNOWN";
}

inline const char* measurementBoundaryRelationName(MeasurementBoundaryRelation relation) {
    switch (relation) {
    case MeasurementBoundaryRelation::PreMeasurementArm:
        return "PRE_MEASUREMENT_ARM";
    case MeasurementBoundaryRelation::ArmedPreMeasurement:
        return "ARMED_PRE_MEASUREMENT";
    case MeasurementBoundaryRelation::WithinCurrentMeasurement:
        return "WITHIN_CURRENT_MEASUREMENT";
    case MeasurementBoundaryRelation::PostMeasurement:
        return "POST_MEASUREMENT";
    case MeasurementBoundaryRelation::Unresolved:
        break;
    }
    return "UNRESOLVED_WITHOUT_EXACT_BOUNDARY_QPC";
}

// P3 integrated seek timeout時にrender threadの到達stageを凍結する診断値。
// 判定やscheduler動作には使わない。
struct P3SeekRenderDiagnostics {
    std::atomic<bool> active{false};
    std::atomic<long long> expectedFrame{-1};
    std::atomic<long long> renderCallbackCount{0};
    std::atomic<long long> lastRenderCallbackQpc{0};
    std::atomic<long long> schedulerLastDisplayed{-1};
    std::atomic<long long> schedulerLastRequested{-1};
    std::atomic<long long> schedulerTargetFrame{-1};
    std::atomic<int> schedulerLastAction{-1};
    std::atomic<long long> schedulerSkippedFrames{0};
    std::atomic<int> schedulerFirstAction{-1};
    std::atomic<long long> schedulerFirstTargetFrame{-1};
    std::atomic<long long> schedulerFirstSkippedFrames{0};
    std::atomic<long long> pairAttemptCount{0};
    std::atomic<int> lastPairResult{-1};
    std::atomic<long long> exactPairFormedQpc{0};
    std::atomic<long long> gpuComposeSubmittedQpc{0};
    std::atomic<long long> gpuCompletionObservedQpc{0};
    std::atomic<long long> displayLedgerAppendQpc{0};

    void reset(long long frame) {
        active.store(false, std::memory_order_release);
        expectedFrame.store(frame, std::memory_order_relaxed);
        renderCallbackCount.store(0, std::memory_order_relaxed);
        lastRenderCallbackQpc.store(0, std::memory_order_relaxed);
        schedulerLastDisplayed.store(-1, std::memory_order_relaxed);
        schedulerLastRequested.store(-1, std::memory_order_relaxed);
        schedulerTargetFrame.store(-1, std::memory_order_relaxed);
        schedulerLastAction.store(-1, std::memory_order_relaxed);
        schedulerSkippedFrames.store(0, std::memory_order_relaxed);
        schedulerFirstAction.store(-1, std::memory_order_relaxed);
        schedulerFirstTargetFrame.store(-1, std::memory_order_relaxed);
        schedulerFirstSkippedFrames.store(0, std::memory_order_relaxed);
        pairAttemptCount.store(0, std::memory_order_relaxed);
        lastPairResult.store(-1, std::memory_order_relaxed);
        exactPairFormedQpc.store(0, std::memory_order_relaxed);
        gpuComposeSubmittedQpc.store(0, std::memory_order_relaxed);
        gpuCompletionObservedQpc.store(0, std::memory_order_relaxed);
        displayLedgerAppendQpc.store(0, std::memory_order_relaxed);
        active.store(true, std::memory_order_release);
    }
};

struct CompositorSpikeState {
    gpu::SharedD3D11Device device;
    gpu::ReadbackCounters readbacks;
    gpu::GpuCompositor compositor;
    gpu::CompositorCoordinator coordinator;
    gpu::CompositionDisplayLedger ledger{gpu::kCompositionDisplayLedgerCapacity};

    mutable std::mutex workerMutex;
    std::shared_ptr<gpu::SourceDecodeWorker> workerA;
    std::shared_ptr<gpu::SourceDecodeWorker> workerB;

    std::atomic<bool> deviceReady{false};
    std::atomic<bool> fatal{false};
    std::atomic<bool> teardownRequested{false};
    std::atomic<bool> teardownComplete{false};
    std::atomic<RenderTeardownDiagnosticStage> teardownDiagnosticStage{
        RenderTeardownDiagnosticStage::NotRequested};
    std::atomic<bool> renderCallbackActive{false};
    std::atomic<bool> terminalRenderExitTracking{false};
    std::atomic<TerminalRenderExitDiagnosticStage> terminalRenderExitDiagnosticStage{
        TerminalRenderExitDiagnosticStage::NotObserved};
    std::atomic<bool> playbackSchedulerEnabled{false};
    // P3-B 専用。P2 の QPC scheduler とは同時に有効にしない。
    std::shared_ptr<audio::AudioMasterClock> audioMasterClock;
    std::atomic<bool> audioMasterSchedulerEnabled{false};
    std::atomic<unsigned long long> audioMasterGeneration{0};
    std::atomic<long long> audioMasterVideoFrameCount{0};
    std::atomic<long long> audioMasterLastRequested{-1};
    std::atomic<long long> audioMasterLastDisplayed{-1};
    // integrated seekの最初のexact frame。generation publish後、表示完了時だけclearする。
    std::atomic<long long> audioMasterPendingSeekFrame{-1};
    std::atomic<long long> audioClockVideoStaleDiscardA{0};
    std::atomic<long long> audioClockVideoStaleDiscardB{0};
    std::atomic<long long> audioClockVideoCatchupSkipCount{0};
    std::atomic<long long> videoPairWaitCount{0};
    std::atomic<long long> videoTargetSupersededCount{0};
    std::atomic<long long> videoAheadViolationCount{0};
    std::atomic<long long> videoClockRegressionCount{0};
    std::atomic<long long> videoQpcMasterFallbackCount{0};
    std::atomic<bool> audioMasterMarkerProbePending{false};
    P3SeekRenderDiagnostics p3SeekDiagnostics;
    // P5-E4 ATTR-Q1。最初のfailureだけをlock-free publishし、判定には使わない。
    audio::RuntimeAttributionState runtimeAttribution;

    // Phase 4 / B。driver は measurement 開始前に GUI thread が publish し、
    // 以後は変更しない。phase4Enabled の release/acquire が publish を見せる。
    std::shared_ptr<gpu::Phase4CompositionDriver> phase4Driver;
    std::atomic<bool> phase4Enabled{false};
    std::atomic<long long> phase4AdoptionFailureCount{0};
    // controllerがcanonical scheduleからworkload開始前に一度だけ設定する。
    gpu::TransitionProbeSelector transitionProbeSelector;
    gpu::AsyncTransitionProbeReadback transitionProbeReadback;
    std::atomic<bool> transitionProbeReady{false};
    std::atomic<long long> transitionProbeIssueFailureCount{0};
    std::mutex transitionProbeResultMutex;
    std::vector<gpu::TransitionProbeResult> transitionProbeResults;

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
    // P2-Q3診断専用。render threadは固定arrayへ単一writerで記録するだけで、
    // measurement停止後にcontrollerがsnapshot/JSON化する。
    std::atomic<bool> schedulerPhaseRingEnabled{false};
    gpu::SchedulerPhaseRing schedulerPhaseRing;
    std::atomic<bool> presentationOpportunityEnabled{false};
    std::atomic<bool> presentationCaptureActive{false};
    gpu::PresentationOpportunityRing presentationOpportunityRing;
    std::shared_ptr<NativePresentHook> nativePresentHook;
    std::atomic<bool> nativePresentHookEnabled{false};
    std::atomic<bool> nativePresentCaptureActive{false};
    // W2-C0.1 formal acquisition専用。B1/B2のmeasurement投影とは別に、
    // preroll前からphysical successor確認後までnative captureを保持する。
    std::atomic<bool> nativePresentCaptureEnvelopeEnabled{false};
    std::atomic<bool> nativePresentEnvelopeStartRequested{false};
    std::atomic<bool> nativePresentEnvelopeStarted{false};
    std::atomic<bool> nativePresentEnvelopeStopRequested{false};
    std::atomic<bool> nativePresentEnvelopeStopped{false};
    std::atomic<long long> nativePresentEnvelopeBeginQpc{0};
    std::atomic<long long> nativePresentEnvelopeCloseQpc{0};
    std::atomic<long long> measurementArmQpc{0};
    // W2-C0.1.1 shadow-only。scheduler decisionの生成時点でscopeを固定し、
    // QPC、source frame、Layer2 cohortからは復元しない。
    std::mutex nativePresentIntentScopeMutex;
    std::vector<NativePresentIntentScopeRecord> nativePresentIntentScopeLedger;
    // F3-C3-A3-T2診断専用。compositor最終出力の2x2 pixelだけを毎frame変える。
    std::atomic<bool> diagnosticTargetPixelToggle{false};
    // F3-C3-A3-T2-D1-B0 diagnostic-only。presentation pathのauthorityではない。
    std::atomic<bool> eligibilityPreflightRequested{false};
    std::atomic<bool> eligibilityPreflightCaptured{false};
    std::atomic<unsigned long long> eligibilityPreflightWindow{0};
    std::mutex eligibilityPreflightMutex;
    PresentationEligibilityPreflight eligibilityPreflight;
    std::atomic<long long> nativePresentTokenSetFailureCount{0};
    std::atomic<long long> latestCompletedRenderOrdinal{-1};
    std::atomic<long long> latestSubmittedRenderOrdinal{-1};
    std::atomic<long long> latestSubmittedOutputFrame{-1};
    std::atomic<long long> presentationSwapOrdinal{0};
    std::atomic<long long> measurementStartQpc{0};
    // P2-D5-2 formal Playback専用。scheduler本体とledgerはrender/swap callback間で
    // 同じlockにより直列化し、共有OutputScheduler60Hzから完全に分離する。
    std::atomic<bool> formalOpportunitySchedulerEnabled{false};
    std::atomic<bool> formalSchedulerInvocationLedgerEnabled{false};
    std::atomic<bool> formalOpportunityCaptureActive{false};
    // W2-C0.1 lower envelope専用。同じscheduler実装をmeasurement前だけ別runで使い、
    // B1 scope開始時にclose/restartする。source selection/counterへは接続しない。
    std::atomic<bool> formalOpportunityEnvelopePrerollActive{false};
    std::atomic<bool> formalOpportunityEnvelopePrerollStarted{false};
    std::atomic<bool> formalOpportunityEnvelopePrerollCompleted{false};
    std::atomic<bool> formalOpportunityIgnoreNextSwap{false};
    std::atomic<bool> formalOpportunityDomainReached{false};
    std::atomic<long long> formalOpportunityPresentedFrame{-1};
    std::atomic<long long> formalOpportunitySwapOrdinal{0};
    std::atomic<long long> formalOpportunityTrueDropCount{0};
    std::atomic<long long> formalDuplicateTransportSuppressedCount{0};
    std::atomic<long long> formalOutsideRequiredTransportSuppressedCount{0};
    std::atomic<long long> diagnosticSyntheticDeadlineDropCount{0};
    std::atomic<long long> formalRefreshNumerator{0};
    std::atomic<long long> formalRefreshDenominator{0};
    std::atomic<long long> formalRequiredFrameCount{0};
    std::mutex formalOpportunityMutex;
    gpu::PresentationOpportunityScheduler formalOpportunityScheduler;
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
    void recordFrameSwapped();

protected:
    QQuickRhiItemRenderer* createRenderer() override;

private:
    std::shared_ptr<CompositorSpikeState> state_;
    gpu::GpuCompletionBackend preferredCompletion_ = gpu::GpuCompletionBackend::Fence;
};

} // namespace mvm::app
#endif

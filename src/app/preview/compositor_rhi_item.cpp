#include "app/preview/compositor_rhi_item.h"

#include "core/mvm_marker.h"
#include "media/audio_preview/audio_video_scheduler.h"
#include "media/gpu_preview/exact_frame_pairer.h"
#include "media/gpu_preview/output_scheduler.h"
#include "media/gpu_preview/qpc_clock.h"
#include "media/gpu_preview/visible_uv.h"

#include <algorithm>
#include <cmath>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <tuple>
#include <vector>

namespace mvm::app {
namespace {

constexpr int kMarkerBandWidth = mvm::marker::kCellSize * mvm::marker::kCellCount;
constexpr int kMarkerBandHeight = mvm::marker::kCellSize;

void captureClockRegression(const std::shared_ptr<CompositorSpikeState>& state,
                            audio::ClockRegressionSite site, long long occurrenceQpc,
                            long long previousFrame, long long candidateFrame,
                            long long rawAudioMasterSamplePosition, long long schedulerTargetFrame,
                            long long currentDisplayedFrame, std::uint64_t sourceGeneration) {
    state->runtimeAttribution.firstClockRegression.capture(
        {site, occurrenceQpc, state->runtimeAttribution.context.snapshot(), previousFrame,
         candidateFrame, rawAudioMasterSamplePosition, schedulerTargetFrame, currentDisplayedFrame,
         sourceGeneration});
}

class CompositorRhiRenderer final : public QQuickRhiItemRenderer {
public:
    CompositorRhiRenderer(std::shared_ptr<CompositorSpikeState> state,
                          gpu::GpuCompletionBackend backend)
        : state_(std::move(state)), backend_(backend) {}

    ~CompositorRhiRenderer() override { releaseRtv(); }

protected:
    void initialize(QRhiCommandBuffer*) override {
        QRhi* r = rhi();
        if (!r || r->backend() != QRhi::D3D11) {
            fail("QRhi backendがD3D11ではありません");
            return;
        }
        const auto* h = static_cast<const QRhiD3D11NativeHandles*>(r->nativeHandles());
        if (!h || !h->dev || !h->context) {
            fail("QRhiのnative D3D11 device/contextを取得できません");
            return;
        }
        if (nativeDevice_ == h->dev && nativeContext_ == h->context)
            return;
        if (nativeDevice_) {
            state_->deviceReady.store(false, std::memory_order_release);
            state_->ledger.abort();
            fail("QRhiのD3D11 deviceが変更されました。P2-C2では回復しません");
            return;
        }
        std::string err;
        if (!state_->device.adopt(static_cast<ID3D11Device*>(h->dev),
                                  static_cast<ID3D11DeviceContext*>(h->context), err) ||
            !state_->compositor.initializeExternal(state_->device, state_->readbacks, err,
                                                   backend_) ||
            !state_->transitionProbeReadback.initialize(state_->device, err)) {
            fail(err);
            return;
        }
        nativeDevice_ = h->dev;
        nativeContext_ = h->context;
        state_->actualGpuCompletionBackend = gpu::toString(state_->compositor.completionBackend());
        state_->transitionProbeReady.store(true, std::memory_order_release);
        state_->nativeDevicePointer.store(reinterpret_cast<unsigned long long>(h->dev),
                                          std::memory_order_relaxed);
        state_->qtAdapter = state_->device.adapter();
        state_->deviceReady.store(true, std::memory_order_release);
    }

    void synchronize(QQuickRhiItem*) override {}

    void render(QRhiCommandBuffer* cb) override {
        gpu::D3D11LockRoleScope lockRole(gpu::D3D11LockRole::Render);
        const long long callbackBegin = gpu::qpcTicks();
        if (state_->p3SeekDiagnostics.active.load(std::memory_order_acquire)) {
            state_->p3SeekDiagnostics.renderCallbackCount.fetch_add(1, std::memory_order_relaxed);
            state_->p3SeekDiagnostics.lastRenderCallbackQpc.store(callbackBegin,
                                                                  std::memory_order_relaxed);
        }
        if (state_->teardownRequested.load(std::memory_order_acquire)) {
            if (teardown())
                update();
            return;
        }
        // P1と同じくrender threadから次のframeを要求する。GUI timerだけでは
        // scene graph requestがcoalesceされ、60Hz output deadlineを取りこぼす。
        update();
        if (!state_->deviceReady.load(std::memory_order_acquire) ||
            state_->fatal.load(std::memory_order_acquire))
            return;
        // pipelineをopenする前のP3-C-2 preflightでもactual targetを検査できるよう、
        // frame pairの有無に依存せずQRhi textureの実pixel sizeを公開する。
        if (auto* texture = colorTexture()) {
            const QSize targetSize = texture->pixelSize();
            state_->publishActualOutputSize(targetSize.width(), targetSize.height());
        }
        if (captureMeasurementBoundary(callbackBegin))
            return;
        state_->presentCallbackCount.fetch_add(1, std::memory_order_relaxed);
        if (state_->diagnosticTimingEnabled.load(std::memory_order_acquire)) {
            if (previousDiagnosticCallbackQpc_ != 0) {
                std::lock_guard<std::mutex> lock(state_->diagnosticTimingMutex);
                state_->diagnosticRenderCallbackIntervalUs.push_back(
                    gpu::qpcUsBetween(previousDiagnosticCallbackQpc_, callbackBegin));
            }
            previousDiagnosticCallbackQpc_ = callbackBegin;
        }
        processMarkerProbe();
        if (state_->testDeviceChange.exchange(false)) {
            fail("test fault: QRhi D3D11 device change");
            return;
        }

        std::shared_ptr<gpu::SourceDecodeWorker> a;
        std::shared_ptr<gpu::SourceDecodeWorker> b;
        {
            std::lock_guard<std::mutex> lock(state_->workerMutex);
            a = state_->workerA;
            b = state_->workerB;
        }
        const CompositorDiagnosticCase diagnosticCase =
            state_->diagnosticCase.load(std::memory_order_acquire);
        long long schedulerDeadlineQpc = callbackBegin;
        long long schedulerNowQpc = callbackBegin;
        gpu::OutputScheduleState schedulerStateBefore;
        gpu::OutputScheduleDecision schedulerDecision;
        bool schedulerDecisionObserved = false;
        const bool schedulerPhaseRingEnabled = schedulerPhaseRingEnabled_;
        long long output = state_->requestedOutput.exchange(-1);
        const bool audioMasterEnabled =
            state_->audioMasterSchedulerEnabled.load(std::memory_order_acquire);
        if (output < 0 && audioMasterEnabled) {
            const auto master = state_->audioMasterClock;
            const auto clockSnapshot = master ? master->snapshot() : audio::AudioClockSnapshot{};
            const long long projectionQpc = gpu::qpcTicks();
            audio::Qpc100ns now100ns;
            const bool converted =
                audio::qpcTicksTo100ns({static_cast<unsigned long long>(projectionQpc)},
                                       gpu::qpcFrequencyTicks(), now100ns);
            const audio::SourceGeneration expected{
                state_->audioMasterGeneration.load(std::memory_order_acquire)};
            const auto projection =
                master && converted ? audio::projectAtQpc100ns(clockSnapshot, now100ns, expected)
                                    : audio::AudioClockProjection{};
            if (!projection.valid) {
                if (state_->audioMasterSchedulerEnabled.load(std::memory_order_acquire)) {
                    const long long lastDisplayed =
                        state_->audioMasterLastDisplayed.load(std::memory_order_acquire);
                    const long long lastRequested =
                        state_->audioMasterLastRequested.load(std::memory_order_acquire);
                    captureClockRegression(
                        state_, audio::ClockRegressionSite::SchedulerProjectionInvalid,
                        projectionQpc, lastDisplayed, -1, clockSnapshot.mediaSamplePosition,
                        lastRequested, lastDisplayed, expected.value);
                    state_->videoClockRegressionCount.fetch_add(1, std::memory_order_relaxed);
                }
                return;
            }
            const long long formalEnd =
                state_->p3MeasurementEndSampleExclusive.load(std::memory_order_acquire);
            if (state_->p3MeasurementActive.load(std::memory_order_acquire) &&
                audio::formalVideoTargetForSample(projection.mediaSample, formalEnd)
                    .measurementEnded)
                return;
            const long long lastDisplayed =
                state_->audioMasterLastDisplayed.load(std::memory_order_acquire);
            const long long lastRequested =
                state_->audioMasterLastRequested.load(std::memory_order_acquire);
            const long long pendingSeekFrame =
                state_->audioMasterPendingSeekFrame.load(std::memory_order_acquire);
            const auto decision = audio::scheduleVideoForAudio(
                projection.mediaSample, lastDisplayed, lastRequested,
                state_->audioMasterVideoFrameCount.load(std::memory_order_acquire),
                pendingSeekFrame);
            if (state_->p3SeekDiagnostics.active.load(std::memory_order_acquire)) {
                auto& diagnostic = state_->p3SeekDiagnostics;
                diagnostic.schedulerLastDisplayed.store(lastDisplayed, std::memory_order_relaxed);
                diagnostic.schedulerLastRequested.store(lastRequested, std::memory_order_relaxed);
                diagnostic.schedulerTargetFrame.store(decision.targetFrame,
                                                      std::memory_order_relaxed);
                diagnostic.schedulerLastAction.store(static_cast<int>(decision.action),
                                                     std::memory_order_relaxed);
                diagnostic.schedulerSkippedFrames.store(decision.skippedFrames,
                                                        std::memory_order_relaxed);
                int unset = -1;
                if (diagnostic.schedulerFirstAction.compare_exchange_strong(
                        unset, static_cast<int>(decision.action), std::memory_order_relaxed)) {
                    diagnostic.schedulerFirstTargetFrame.store(decision.targetFrame,
                                                               std::memory_order_relaxed);
                    diagnostic.schedulerFirstSkippedFrames.store(decision.skippedFrames,
                                                                 std::memory_order_relaxed);
                }
            }
            if (decision.action == audio::AudioVideoScheduleAction::ClockRegression ||
                decision.action == audio::AudioVideoScheduleAction::Invalid) {
                captureClockRegression(state_, audio::ClockRegressionSite::SchedulerDecision,
                                       projectionQpc, lastDisplayed, decision.targetFrame,
                                       projection.mediaSample, decision.targetFrame, lastDisplayed,
                                       expected.value);
                state_->videoClockRegressionCount.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (decision.action == audio::AudioVideoScheduleAction::End)
                return;
            if (decision.action == audio::AudioVideoScheduleAction::Hold) {
                if (decision.targetFrame <= lastDisplayed) {
                    state_->repeatedPresentCount.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                output = decision.targetFrame;
            } else {
                output = decision.targetFrame;
                if (lastRequested > lastDisplayed && lastRequested != output)
                    state_->videoTargetSupersededCount.fetch_add(1, std::memory_order_relaxed);
                if (lastRequested != output) {
                    state_->audioMasterLastRequested.store(output, std::memory_order_release);
                    state_->audioClockVideoCatchupSkipCount.fetch_add(decision.skippedFrames,
                                                                      std::memory_order_relaxed);
                    state_->scheduledOutputCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
        } else if (output < 0 && state_->playbackSchedulerEnabled.load(std::memory_order_acquire)) {
            const long long now = gpu::qpcTicks();
            if (!schedulerStarted_) {
                scheduler_.start(now, static_cast<long long>(gpu::qpcFrequency()));
                schedulerStarted_ = true;
            }
            const long long measurementEnd =
                state_->measurementEndQpc.load(std::memory_order_acquire);
            if (schedulerPhaseRingEnabled) {
                schedulerNowQpc = now;
                schedulerStateBefore = scheduler_.state();
                schedulerDecisionObserved = true;
            }
            schedulerDecision = state_->measurementIntervalActive.load(std::memory_order_acquire)
                                    ? scheduler_.takeDueBefore(now, measurementEnd)
                                    : scheduler_.takeDue(now);
            if (schedulerDecision.due) {
                output = schedulerDecision.output.outputFrameNumber;
                schedulerDeadlineQpc = schedulerDecision.output.deadlineQpc;
                state_->scheduledOutputCount.fetch_add(schedulerDecision.skippedDeadlineCount + 1,
                                                       std::memory_order_relaxed);
                noteDrop(gpu::OutputDropReason::SchedulerDeadline,
                         schedulerDecision.skippedDeadlineCount);
            }
        }
        const bool needsB = diagnosticCase != CompositorDiagnosticCase::SingleDecode;
        const bool repeatedThisCallback = !a || (needsB && !b) || output < 0;
        if (schedulerDecisionObserved && schedulerPhaseRingEnabled &&
            state_->measurementIntervalActive.load(std::memory_order_relaxed)) {
            state_->schedulerPhaseRing.capture(
                {callbackBegin, previousSchedulerPhaseCallbackQpc_, schedulerNowQpc,
                 schedulerStateBefore.nextFrame, schedulerStateBefore.nextDeadlineQpc,
                 schedulerStateBefore.nextNextDeadlineQpc,
                 schedulerNowQpc - schedulerStateBefore.nextDeadlineQpc, schedulerDecision.due,
                 schedulerDecision.skippedDeadlineCount,
                 schedulerDecision.due ? schedulerDecision.output.outputFrameNumber : -1,
                 repeatedThisCallback});
            previousSchedulerPhaseCallbackQpc_ = callbackBegin;
        }
        if (repeatedThisCallback) {
            state_->repeatedPresentCount.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Phase 4 / B: exact pair より**前**に schedule を resolve し、state と
        // layout を atomic snapshot として adopt する。compose 後や display 後に
        // adopt すると、既に表示した frame へ後付けの state が付く。
        if (state_->phase4Enabled.load(std::memory_order_acquire)) {
            const auto driven = state_->phase4Driver->onTargetFrame(state_->coordinator, output);
            if (driven == gpu::Phase4DriveResult::Rejected ||
                driven == gpu::Phase4DriveResult::Unresolved) {
                state_->phase4AdoptionFailureCount.fetch_add(1, std::memory_order_relaxed);
                fail("Phase 4 composition snapshot を target frame へ適用できません");
                return;
            }
        }

        gpu::ComposedFrame frame;
        const long long pairBegin = gpu::qpcTicks();
        if (diagnosticCase == CompositorDiagnosticCase::FixedTextures) {
            frame = state_->diagnosticFixedFrame;
            frame.outputFrameNumber = output;
        } else if (diagnosticCase == CompositorDiagnosticCase::SingleDecode) {
            a->buffer().discardBefore(output);
            gpu::DecodedGpuFrame sourceFrame;
            if (!a->buffer().takeExact(output, sourceFrame)) {
                state_->missingPairDropCount.fetch_add(1, std::memory_order_relaxed);
                if (state_->measurementIntervalActive.load(std::memory_order_acquire) && a->eof())
                    state_->sourceAEofCount.fetch_add(1, std::memory_order_relaxed);
                noteDrop(gpu::OutputDropReason::MissingSourceA);
                return;
            }
            frame.outputFrameNumber = output;
            frame.layers.push_back({std::move(sourceFrame), {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0f, 0});
        } else {
            if (audioMasterEnabled) {
                state_->audioClockVideoStaleDiscardA.fetch_add(
                    static_cast<long long>(a->buffer().discardBefore(output)),
                    std::memory_order_relaxed);
                state_->audioClockVideoStaleDiscardB.fetch_add(
                    static_cast<long long>(b->buffer().discardBefore(output)),
                    std::memory_order_relaxed);
            }
            gpu::ExactFramePairer pairer(a->buffer(), b->buffer(), state_->coordinator);
            if (state_->p3SeekDiagnostics.active.load(std::memory_order_acquire))
                state_->p3SeekDiagnostics.pairAttemptCount.fetch_add(1, std::memory_order_relaxed);
            const gpu::PairResult paired = pairer.tryPair(output, frame);
            if (state_->p3SeekDiagnostics.active.load(std::memory_order_acquire))
                state_->p3SeekDiagnostics.lastPairResult.store(static_cast<int>(paired),
                                                               std::memory_order_relaxed);
            if (paired != gpu::PairResult::Paired) {
                if (audioMasterEnabled) {
                    state_->videoPairWaitCount.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                state_->missingPairDropCount.fetch_add(1, std::memory_order_relaxed);
                if (state_->measurementIntervalActive.load(std::memory_order_acquire)) {
                    if (a->eof())
                        state_->sourceAEofCount.fetch_add(1, std::memory_order_relaxed);
                    if (b->eof())
                        state_->sourceBEofCount.fetch_add(1, std::memory_order_relaxed);
                }
                noteDrop(gpu::OutputScheduler60Hz::classifyDeadline(
                    paired, gpu::CompositionResult::Accepted));
                return;
            }
            const auto validation = state_->coordinator.validateForDisplay(frame);
            if (validation != gpu::CompositionResult::Accepted) {
                noteDrop(gpu::OutputScheduler60Hz::classifyDeadline(gpu::PairResult::Paired,
                                                                    validation));
                return;
            }
        }
        std::string err;
        if (state_->phase4Enabled.load(std::memory_order_acquire) &&
            !normalizePhase4VisibleUv(frame, err)) {
            state_->renderFailureCount.fetch_add(1, std::memory_order_relaxed);
            noteDrop(gpu::OutputDropReason::RenderFailure);
            fail(err);
            return;
        }
        const long long pairReadyQpc = gpu::qpcTicks();
        if (state_->p3SeekDiagnostics.active.load(std::memory_order_acquire) &&
            output == state_->p3SeekDiagnostics.expectedFrame.load(std::memory_order_relaxed))
            state_->p3SeekDiagnostics.exactPairFormedQpc.store(pairReadyQpc,
                                                               std::memory_order_relaxed);
        if (audioMasterEnabled &&
            state_->audioMasterMarkerProbePending.exchange(false, std::memory_order_acq_rel)) {
            std::lock_guard<std::mutex> lock(state_->markerProbe.mutex);
            state_->markerProbe.expectedFrame = output;
            state_->markerProbe.frameA = frame.layers[0].frame;
            state_->markerProbe.frameB = frame.layers[1].frame;
            state_->markerProbe.requested = true;
            state_->markerProbe.done = false;
        }
        if (!ensureRtv(colorTexture(), err)) {
            state_->renderFailureCount.fetch_add(1, std::memory_order_relaxed);
            noteDrop(gpu::OutputDropReason::RenderFailure);
            fail(err);
            return;
        }

        // clear ownerはQRhi passのみ。GpuCompositor external modeはclearしない。
        cb->beginPass(renderTarget(), Qt::black, {1.0f, 0}, nullptr,
                      QRhiCommandBuffer::ExternalContent);
        state_->logicalClearCount.fetch_add(1, std::memory_order_relaxed);
        const long long externalBegin = gpu::qpcTicks();
        cb->beginExternal();
        const QSize size = colorTexture()->pixelSize();
        state_->publishActualOutputSize(size.width(), size.height());
        gpu::GpuCompositorStageTiming compositorTiming;
        const bool pairOnly = diagnosticCase == CompositorDiagnosticCase::PairOnly;
        const bool diagnostic = diagnosticCase != CompositorDiagnosticCase::None;
        bool ok = pairOnly || (diagnostic ? state_->compositor.composeDiagnosticToTarget(
                                                frame, {rtv_, size.width(), size.height()},
                                                compositorTiming, err)
                                          : state_->compositor.composeToTarget(
                                                frame, {rtv_, size.width(), size.height()}, err));
        if (ok && !diagnostic && state_->p3MeasurementActive.load(std::memory_order_acquire) &&
            state_->phase4Enabled.load(std::memory_order_acquire))
            ok = issueTransitionProbes(frame, size, err);
        cb->endExternal();
        const long long externalEnd = gpu::qpcTicks();
        cb->endPass();
        if (!ok) {
            state_->renderFailureCount.fetch_add(1, std::memory_order_relaxed);
            noteDrop(gpu::OutputDropReason::RenderFailure);
            if (state_->compositor.fatal())
                fail(state_->compositor.fatalReason());
            return;
        }
        const long long submissionQpc = gpu::qpcTicks();
        if (state_->p3SeekDiagnostics.active.load(std::memory_order_acquire) &&
            output == state_->p3SeekDiagnostics.expectedFrame.load(std::memory_order_relaxed))
            state_->p3SeekDiagnostics.gpuComposeSubmittedQpc.store(submissionQpc,
                                                                   std::memory_order_relaxed);
        if (!diagnostic &&
            !state_->actualTargetProbeStarted.exchange(true, std::memory_order_acq_rel)) {
            runActualTargetProbe(frame, size);
            // 4点すべてのreadback・比較とmismatch確定後にだけ完了を公開する。
            state_->actualTargetProbeDone.store(true, std::memory_order_release);
        }
        if (diagnosticCase != CompositorDiagnosticCase::FixedTextures)
            a->buffer().noteDisplayed(output);
        if (b && diagnosticCase != CompositorDiagnosticCase::FixedTextures)
            b->buffer().noteDisplayed(output);
        const auto displayClockSnapshot = audioMasterEnabled && state_->audioMasterClock
                                              ? state_->audioMasterClock->snapshot()
                                              : audio::AudioClockSnapshot{};
        const long long displayedQpc = gpu::qpcTicks();
        bool displayProjectionValid = false;
        double displayDeltaMs = 0.0;
        if (audioMasterEnabled) {
            const long long previousDisplayed =
                state_->audioMasterLastDisplayed.load(std::memory_order_acquire);
            state_->audioMasterLastDisplayed.store(output, std::memory_order_release);
            audio::Qpc100ns displayed100ns;
            const auto master = state_->audioMasterClock;
            const audio::SourceGeneration expected{
                state_->audioMasterGeneration.load(std::memory_order_acquire)};
            const bool converted =
                audio::qpcTicksTo100ns({static_cast<unsigned long long>(displayedQpc)},
                                       gpu::qpcFrequencyTicks(), displayed100ns);
            const auto projection =
                master && converted
                    ? audio::projectAtQpc100ns(displayClockSnapshot, displayed100ns, expected)
                    : audio::AudioClockProjection{};
            if (!projection.valid) {
                if (state_->audioMasterSchedulerEnabled.load(std::memory_order_acquire)) {
                    captureClockRegression(
                        state_, audio::ClockRegressionSite::DisplayProjectionInvalid, displayedQpc,
                        previousDisplayed, output, displayClockSnapshot.mediaSamplePosition,
                        state_->audioMasterLastRequested.load(std::memory_order_acquire), output,
                        expected.value);
                    state_->videoClockRegressionCount.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                const long long videoSample = output * audio::kSamplesPerVideoFrame;
                const double deltaMs = static_cast<double>(videoSample - projection.mediaSample) *
                                       1000.0 / audio::kInternalSampleRate;
                displayProjectionValid = true;
                displayDeltaMs = deltaMs;
                {
                    std::lock_guard<std::mutex> lock(state_->applicationAvDeltaMutex);
                    state_->applicationAvDeltaMs.push_back(deltaMs);
                }
                if (audio::isVideoAheadViolation(output, projection.mediaSample))
                    state_->videoAheadViolationCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
        state_->ledger.record(frame, displayedQpc, pairReadyQpc, submissionQpc,
                              displayProjectionValid, displayDeltaMs);
        long long pendingSeekFrame = output;
        state_->audioMasterPendingSeekFrame.compare_exchange_strong(pendingSeekFrame, -1,
                                                                    std::memory_order_acq_rel);
        if (state_->p3SeekDiagnostics.active.load(std::memory_order_acquire) &&
            output == state_->p3SeekDiagnostics.expectedFrame.load(std::memory_order_relaxed))
            state_->p3SeekDiagnostics.displayLedgerAppendQpc.store(gpu::qpcTicks(),
                                                                   std::memory_order_relaxed);
        if (state_->p3MeasurementActive.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(state_->p3MeasurementDisplayMutex);
            state_->p3MeasurementDisplays.push_back(
                {output, displayedQpc, displayProjectionValid, displayDeltaMs});
        }
        state_->displayedCompositionCount.fetch_add(1, std::memory_order_relaxed);
        if (state_->measurementIntervalActive.load(std::memory_order_acquire)) {
            long long unset = -1;
            state_->measurementFirstOutputFrame.compare_exchange_strong(unset, output,
                                                                        std::memory_order_relaxed);
        }
        if (diagnostic && state_->diagnosticTimingEnabled.load(std::memory_order_acquire)) {
            CompositorDiagnosticRenderSample sample;
            sample.schedulerToPairUs = gpu::qpcUsBetween(schedulerDeadlineQpc, pairReadyQpc);
            sample.pairUs = gpu::qpcUsBetween(pairBegin, pairReadyQpc);
            sample.compositionPrepareUs = compositorTiming.prepareUs;
            sample.compositionIssueUs = compositorTiming.issueUs;
            sample.completionPollUs = compositorTiming.completionPollUs;
            sample.qtExternalUs = gpu::qpcUsBetween(externalBegin, externalEnd);
            sample.renderCallbackTotalUs = gpu::qpcUsBetween(callbackBegin, displayedQpc);
            sample.bufferDepthA = a->buffer().depth();
            sample.bufferDepthB = b ? b->buffer().depth() : 0;
            std::lock_guard<std::mutex> lock(state_->diagnosticTimingMutex);
            state_->diagnosticRenderSamples.push_back(sample);
        }
    }

private:
    bool normalizePhase4VisibleUv(gpu::ComposedFrame& frame, std::string& err) const {
        // D3D11VAのallocationはlogical heightより大きいことがある（実fixtureは
        // 1920x1080に対して1920x1088）。catalogのfull visible UVをphysical
        // textureのvisible extentへ変換する。Phase 4 branch限定で、P3 pathや
        // destination/state/epoch/source identityは変更しない。
        for (auto& layer : frame.layers) {
            D3D11_TEXTURE2D_DESC desc{};
            layer.frame.texture->GetDesc(&desc);
            const auto normalized = gpu::normalizeVisibleUv(
                layer.sourceUv, layer.frame.width, layer.frame.height, static_cast<int>(desc.Width),
                static_cast<int>(desc.Height));
            if (!normalized) {
                err = "decode textureのphysical extentがlogical visible extentより小さいです";
                return false;
            }
            layer.sourceUv = *normalized;
        }
        return true;
    }

    bool issueTransitionProbes(const gpu::ComposedFrame& frame, const QSize& size,
                               std::string& err) {
        const auto boundary = state_->transitionProbeSelector.select(frame.outputFrameNumber);
        if (!boundary)
            return true;
        if (size.width() != 1920 || size.height() != 1080 || !rtvTexture_) {
            err = "transition probe actual targetが1920x1080ではありません";
            state_->transitionProbeIssueFailureCount.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::vector<gpu::SourceFrameIdentity> sources;
        sources.reserve(frame.layers.size());
        for (const auto& layer : frame.layers)
            sources.push_back(gpu::identityOf(layer.frame));
        for (const auto [point, x, y] : {std::tuple{gpu::TransitionProbePoint::TL, 480, 270},
                                         std::tuple{gpu::TransitionProbePoint::BR, 1440, 810}}) {
            gpu::TransitionProbeRequest request{*boundary,
                                                frame.outputFrameNumber,
                                                frame.compositionState,
                                                frame.compositionEpoch,
                                                point,
                                                x,
                                                y,
                                                sources};
            unsigned long long ticket = 0;
            if (!state_->transitionProbeReadback.issue(rtvTexture_, request, ticket, err)) {
                state_->transitionProbeIssueFailureCount.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        return true;
    }

    CompositorMeasurementCounters measurementCounters() const {
        const auto& c = state_->compositor.counters();
        return {gpu::qpcTicks(),
                c.compositionRequestedCount,
                c.compositionDrawnCount,
                c.gpuSubmissionCount,
                c.layerDrawCount,
                state_->logicalClearCount.load(std::memory_order_relaxed),
                state_->scheduledOutputCount.load(std::memory_order_relaxed),
                state_->displayedCompositionCount.load(std::memory_order_relaxed),
                state_->droppedOutputCount.load(std::memory_order_relaxed),
                state_->missingPairDropCount.load(std::memory_order_relaxed),
                state_->sourceAEofCount.load(std::memory_order_relaxed),
                state_->sourceBEofCount.load(std::memory_order_relaxed),
                state_->schedulerDeadlineDropCount.load(std::memory_order_relaxed),
                state_->missingSourceADropCount.load(std::memory_order_relaxed),
                state_->missingSourceBDropCount.load(std::memory_order_relaxed),
                state_->missingBothDropCount.load(std::memory_order_relaxed),
                state_->staleGenerationDropCount.load(std::memory_order_relaxed),
                state_->futureGenerationDropCount.load(std::memory_order_relaxed),
                state_->staleCompositionEpochDropCount.load(std::memory_order_relaxed),
                state_->renderFailureCount.load(std::memory_order_relaxed),
                state_->presentCallbackCount.load(std::memory_order_relaxed),
                state_->repeatedPresentCount.load(std::memory_order_relaxed),
                c.partialGpuIssueFailureCount,
                c.completionPollFailureCount,
                c.untrackedSubmissionCount};
    }

    bool captureMeasurementBoundary(long long callbackBegin) {
        if (state_->measurementResetRequested.exchange(false, std::memory_order_acq_rel)) {
            state_->playbackSchedulerEnabled.store(false, std::memory_order_release);
            state_->requestedOutput.store(-1, std::memory_order_release);
            state_->measurementIntervalActive.store(false, std::memory_order_release);
            state_->measurementResetCaptured.store(true, std::memory_order_release);
            return true;
        }
        const bool intervalEnded =
            state_->measurementIntervalActive.load(std::memory_order_acquire) &&
            callbackBegin >= state_->measurementEndQpc.load(std::memory_order_acquire);
        const bool stopRequested =
            state_->measurementStopRequested.exchange(false, std::memory_order_acq_rel);
        if (intervalEnded || stopRequested) {
            if (state_->measurementIntervalActive.exchange(false, std::memory_order_acq_rel)) {
                const long long closed = scheduler_.closeBefore(
                    state_->measurementEndQpc.load(std::memory_order_acquire));
                state_->scheduledOutputCount.fetch_add(closed, std::memory_order_relaxed);
                noteDrop(gpu::OutputDropReason::SchedulerDeadline, closed);
            }
            state_->playbackSchedulerEnabled.store(false, std::memory_order_release);
            state_->diagnosticTimingEnabled.store(false, std::memory_order_release);
            state_->diagnosticLockTiming = state_->device.lock().endDiagnostics();
            {
                std::lock_guard<std::mutex> lock(state_->measurementMutex);
                state_->measurementStop = measurementCounters();
                state_->measurementStop.qpc = callbackBegin;
            }
            state_->measurementStopCaptured.store(true, std::memory_order_release);
            return true;
        }
        if (state_->measurementStartRequested.exchange(false, std::memory_order_acq_rel)) {
            const long long duration =
                state_->measurementDurationQpc.load(std::memory_order_acquire);
            scheduler_.start(callbackBegin, static_cast<long long>(gpu::qpcFrequency()));
            schedulerStarted_ = true;
            previousSchedulerPhaseCallbackQpc_ = 0;
            schedulerPhaseRingEnabled_ =
                state_->schedulerPhaseRingEnabled.load(std::memory_order_acquire);
            if (schedulerPhaseRingEnabled_)
                state_->schedulerPhaseRing.reset();
            state_->measurementEndQpc.store(callbackBegin + duration, std::memory_order_release);
            state_->measurementIntervalActive.store(true, std::memory_order_release);
            if (state_->diagnosticCase.load(std::memory_order_acquire) !=
                CompositorDiagnosticCase::None) {
                {
                    std::lock_guard<std::mutex> lock(state_->diagnosticTimingMutex);
                    state_->diagnosticRenderSamples.clear();
                    state_->diagnosticRenderCallbackIntervalUs.clear();
                }
                previousDiagnosticCallbackQpc_ = callbackBegin;
                state_->device.lock().beginDiagnostics();
                state_->diagnosticTimingEnabled.store(true, std::memory_order_release);
            }
            {
                std::lock_guard<std::mutex> lock(state_->measurementMutex);
                state_->measurementStart = measurementCounters();
                state_->measurementStart.qpc = callbackBegin;
            }
            state_->playbackSchedulerEnabled.store(true, std::memory_order_release);
            state_->measurementStartCaptured.store(true, std::memory_order_release);
        }
        return false;
    }

    void noteDrop(gpu::OutputDropReason reason, long long count = 1) {
        if (count <= 0)
            return;
        state_->droppedOutputCount.fetch_add(count, std::memory_order_relaxed);
        switch (reason) {
        case gpu::OutputDropReason::MissingSourceA:
            state_->missingSourceADropCount.fetch_add(count, std::memory_order_relaxed);
            break;
        case gpu::OutputDropReason::MissingSourceB:
            state_->missingSourceBDropCount.fetch_add(count, std::memory_order_relaxed);
            break;
        case gpu::OutputDropReason::MissingBoth:
            state_->missingBothDropCount.fetch_add(count, std::memory_order_relaxed);
            break;
        case gpu::OutputDropReason::StaleGeneration:
            state_->staleGenerationDropCount.fetch_add(count, std::memory_order_relaxed);
            break;
        case gpu::OutputDropReason::FutureGeneration:
            state_->futureGenerationDropCount.fetch_add(count, std::memory_order_relaxed);
            break;
        case gpu::OutputDropReason::StaleCompositionEpoch:
            state_->staleCompositionEpochDropCount.fetch_add(count, std::memory_order_relaxed);
            break;
        case gpu::OutputDropReason::RenderFailure:
            break; // renderFailureCountがこのreasonのcounterを兼ねる。
        case gpu::OutputDropReason::SchedulerDeadline:
            state_->schedulerDeadlineDropCount.fetch_add(count, std::memory_order_relaxed);
            break;
        case gpu::OutputDropReason::None:
            break;
        }
    }

    void processMarkerProbe() {
        gpu::DecodedGpuFrame frameA;
        gpu::DecodedGpuFrame frameB;
        long long expected = -1;
        {
            std::lock_guard<std::mutex> lock(state_->markerProbe.mutex);
            if (!state_->markerProbe.requested)
                return;
            state_->markerProbe.requested = false;
            frameA = state_->markerProbe.frameA;
            frameB = state_->markerProbe.frameB;
            expected = state_->markerProbe.expectedFrame;
        }
        std::vector<unsigned char> rgbaA;
        std::vector<unsigned char> rgbaB;
        std::string errA;
        std::string errB;
        const bool okA = state_->compositor.readSourceMarker(frameA, kMarkerBandWidth,
                                                             kMarkerBandHeight, rgbaA, errA);
        const auto markerA =
            okA ? mvm::marker::readMarkerAuto(rgbaA.data(), kMarkerBandWidth, kMarkerBandHeight)
                : mvm::marker::MarkerRead{};
        const bool okB = state_->compositor.readSourceMarker(frameB, kMarkerBandWidth,
                                                             kMarkerBandHeight, rgbaB, errB);
        const auto markerB =
            okB ? mvm::marker::readMarkerAuto(rgbaB.data(), kMarkerBandWidth, kMarkerBandHeight)
                : mvm::marker::MarkerRead{};
        state_->markerAChecked.fetch_add(1, std::memory_order_relaxed);
        state_->markerBChecked.fetch_add(1, std::memory_order_relaxed);
        if (!okA || !markerA.syncOk || markerA.value != expected)
            state_->markerAMismatch.fetch_add(1, std::memory_order_relaxed);
        if (!okB || !markerB.syncOk || markerB.value != expected)
            state_->markerBMismatch.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(state_->markerProbe.mutex);
        state_->markerProbe.markerA = markerA.value;
        state_->markerProbe.markerB = markerB.value;
        state_->markerProbe.syncA = markerA.syncOk;
        state_->markerProbe.syncB = markerB.syncOk;
        state_->markerProbe.error = !errA.empty() ? errA : errB;
        state_->markerProbe.done = true;
    }

    void runActualTargetProbe(const gpu::ComposedFrame& frame, const QSize& size) {
        // performance区間外の最初のactual targetだけを、sourceの同じsampling結果と比較する。
        const int points[4][2] = {
            {size.width() / 4, size.height() / 4},
            {size.width() * 3 / 4, size.height() * 3 / 4},
            {size.width() / 2, size.height() / 2},
            {std::max(0, size.width() / 2 - 1), std::max(0, size.height() / 2 - 1)}};
        for (const auto& point : points) {
            state_->actualTargetProbeChecked.fetch_add(1, std::memory_order_relaxed);
            std::vector<unsigned char> actual;
            std::vector<unsigned char> sourceA;
            std::vector<unsigned char> sourceB;
            std::string err;
            const float uA =
                (static_cast<float>(point[0]) + 0.5f) / static_cast<float>(size.width());
            const float vA =
                (static_cast<float>(point[1]) + 0.5f) / static_cast<float>(size.height());
            const bool overlap = point[0] >= size.width() / 2 && point[1] >= size.height() / 2;
            bool ok =
                state_->compositor.readExternalOutputProbe(rtvTexture_, point[0], point[1], 1, 1,
                                                           actual, err) &&
                state_->compositor.readSourceProbe(frame.layers[0].frame, uA, vA, sourceA, err);
            if (ok && overlap) {
                const float uB = (static_cast<float>(point[0] - size.width() / 2) + 0.5f) /
                                 static_cast<float>(size.width() / 2);
                const float vB = (static_cast<float>(point[1] - size.height() / 2) + 0.5f) /
                                 static_cast<float>(size.height() / 2);
                ok =
                    state_->compositor.readSourceProbe(frame.layers[1].frame, uB, vB, sourceB, err);
            }
            if (!ok || actual.size() != 4 || sourceA.size() != 4 ||
                (overlap && sourceB.size() != 4)) {
                state_->actualTargetProbeMismatch.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            for (size_t channel = 0; channel < 3; ++channel) {
                int expected = sourceA[channel];
                if (overlap) {
                    const float opacity = frame.layers[1].opacity;
                    expected = static_cast<int>(
                        std::lround(static_cast<float>(sourceB[channel]) * opacity +
                                    static_cast<float>(sourceA[channel]) * (1.0f - opacity)));
                }
                if (std::abs(static_cast<int>(actual[channel]) - expected) > 3)
                    state_->actualTargetProbeMismatch.fetch_add(1, std::memory_order_relaxed);
            }
            if (actual[3] != 255)
                state_->actualTargetProbeMismatch.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void fail(const std::string& reason) {
        {
            std::lock_guard<std::mutex> lock(state_->errorMutex);
            if (state_->fatalReason.empty())
                state_->fatalReason = reason;
        }
        state_->fatal.store(true, std::memory_order_release);
        state_->deviceReady.store(false, std::memory_order_release);
        state_->ledger.abort();
    }

    bool ensureRtv(QRhiTexture* texture, std::string& err) {
        if (!texture) {
            err = "QQuickRhiItem color textureがありません";
            return false;
        }
        auto* native = reinterpret_cast<ID3D11Texture2D*>(texture->nativeTexture().object);
        if (!native) {
            err = "QQuickRhiItem color textureのnative handleがありません";
            return false;
        }
        if (native == rtvTexture_ && rtv_)
            return true;
        releaseRtv();
        const HRESULT hr = state_->device.device()->CreateRenderTargetView(native, nullptr, &rtv_);
        if (FAILED(hr)) {
            err = "QQuickRhiItem actual targetのRTV生成に失敗しました";
            return false;
        }
        rtvTexture_ = native;
        return true;
    }

    void releaseRtv() {
        if (rtv_)
            rtv_->Release();
        rtv_ = nullptr;
        rtvTexture_ = nullptr;
    }

    enum class TeardownStage { NotStarted, ProbeDrain, CompositorDrain, Failed, Complete };

    // true は次のrender callbackでpollを継続することを表す。ここでは待たない。
    bool teardown() {
        if (state_->teardownComplete.load(std::memory_order_acquire))
            return false;
        std::string err;
        if (teardownStage_ == TeardownStage::NotStarted) {
            std::shared_ptr<gpu::SourceDecodeWorker> a;
            std::shared_ptr<gpu::SourceDecodeWorker> b;
            {
                std::lock_guard<std::mutex> lock(state_->workerMutex);
                a = state_->workerA;
                b = state_->workerB;
            }
            if ((a && !a->joined()) || (b && !b->joined())) {
                state_->lifecycleOrderViolationCount.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            if (state_->transitionProbeReady.load(std::memory_order_acquire)) {
                if (!state_->transitionProbeReadback.beginDrain(5000, err)) {
                    fail(err);
                    teardownStage_ = TeardownStage::Failed;
                    return false;
                }
                teardownStage_ = TeardownStage::ProbeDrain;
            } else {
                if (!state_->compositor.beginShutdown(10000, err)) {
                    fail(err);
                    teardownStage_ = TeardownStage::Failed;
                    return false;
                }
                teardownStage_ = TeardownStage::CompositorDrain;
            }
        }

        if (teardownStage_ == TeardownStage::ProbeDrain) {
            std::vector<gpu::TransitionProbeResult> drained;
            const auto status = state_->transitionProbeReadback.pollDrain(drained, err);
            if (!drained.empty()) {
                std::lock_guard<std::mutex> lock(state_->transitionProbeResultMutex);
                state_->transitionProbeResults.insert(state_->transitionProbeResults.end(),
                                                      drained.begin(), drained.end());
            }
            if (status == gpu::TransitionProbeDrainStatus::Pending)
                return true;
            if (status == gpu::TransitionProbeDrainStatus::Failed) {
                fail(err);
                teardownStage_ = TeardownStage::Failed;
                return false;
            }
            state_->transitionProbeReady.store(false, std::memory_order_release);
            state_->transitionProbeReadback.release();
            if (!state_->compositor.beginShutdown(10000, err)) {
                fail(err);
                teardownStage_ = TeardownStage::Failed;
                return false;
            }
            teardownStage_ = TeardownStage::CompositorDrain;
        }

        if (teardownStage_ == TeardownStage::CompositorDrain) {
            const auto status = state_->compositor.pollShutdown(err);
            if (status == gpu::GpuCompositorShutdownStatus::Pending)
                return true;
            if (status == gpu::GpuCompositorShutdownStatus::Failed) {
                fail(err);
                teardownStage_ = TeardownStage::Failed;
                return false;
            }
        }
        releaseRtv();
        state_->device.release();
        nativeDevice_ = nativeContext_ = nullptr;
        state_->deviceReady.store(false, std::memory_order_release);
        state_->teardownComplete.store(true, std::memory_order_release);
        teardownStage_ = TeardownStage::Complete;
        return false;
    }

    std::shared_ptr<CompositorSpikeState> state_;
    gpu::GpuCompletionBackend backend_;
    void* nativeDevice_ = nullptr;
    void* nativeContext_ = nullptr;
    ID3D11Texture2D* rtvTexture_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    gpu::OutputScheduler60Hz scheduler_;
    bool schedulerStarted_ = false;
    long long previousDiagnosticCallbackQpc_ = 0;
    long long previousSchedulerPhaseCallbackQpc_ = 0;
    bool schedulerPhaseRingEnabled_ = false;
    TeardownStage teardownStage_ = TeardownStage::NotStarted;
};

} // namespace

CompositorRhiItem::CompositorRhiItem(QQuickItem* parent)
    : QQuickRhiItem(parent), state_(std::make_shared<CompositorSpikeState>()) {
    setMirrorVertically(true);
}

void CompositorRhiItem::requestTeardown() {
    state_->teardownRequested.store(true, std::memory_order_release);
    update();
}

QQuickRhiItemRenderer* CompositorRhiItem::createRenderer() {
    return new CompositorRhiRenderer(state_, preferredCompletion_);
}

} // namespace mvm::app

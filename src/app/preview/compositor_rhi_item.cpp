#include "app/preview/compositor_rhi_item.h"

#include "core/mvm_marker.h"
#include "media/audio_preview/audio_video_scheduler.h"
#include "media/gpu_preview/exact_frame_pairer.h"
#include "media/gpu_preview/output_scheduler.h"
#include "media/gpu_preview/qpc_clock.h"
#include "media/gpu_preview/visible_uv.h"

#include <algorithm>
#include <cmath>
#include <d3d11_1.h>
#include <dwmapi.h>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#include <tuple>
#include <vector>

#include <QQuickWindow>
#include <QScopeGuard>

namespace mvm::app {
namespace {

// B3-I5B。preroll drain handshakeのfail-close timeout。canonical measurement
// windowの外であり、超過はPROTOCOL_FATALとして扱う。
constexpr long long kPrerollTransitionTimeoutSeconds = 2;
constexpr int kMarkerBandWidth = mvm::marker::kCellSize * mvm::marker::kCellCount;
constexpr int kMarkerBandHeight = mvm::marker::kCellSize;

gpu::PresentationAuthoritySample
capturePresentationAuthority(const std::shared_ptr<CompositorSpikeState>& state) {
    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof(timing);
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &timing)))
        return {};
    return {true, static_cast<std::uint64_t>(timing.cRefresh),
            static_cast<long long>(timing.qpcVBlank),
            state->formalRefreshNumerator.load(std::memory_order_relaxed),
            state->formalRefreshDenominator.load(std::memory_order_relaxed)};
}

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

class PresentationRenderCapture {
public:
    PresentationRenderCapture(CompositorSpikeState* state, bool enabled, long long callbackBeginQpc,
                              long long renderOrdinal)
        : state_(state), enabled_(enabled), callbackBeginQpc_(callbackBeginQpc),
          renderOrdinal_(renderOrdinal) {}

    ~PresentationRenderCapture() {
        if (!enabled_)
            return;
        state_->presentationOpportunityRing.captureRender(
            {callbackBeginQpc_, gpu::qpcTicks(), renderOrdinal_, selectedOutputFrame_,
             submittedOutputFrame_, skippedDeadlineCount_, repeated_});
        if (submittedOutputFrame_ >= 0) {
            state_->latestSubmittedOutputFrame.store(submittedOutputFrame_,
                                                     std::memory_order_relaxed);
            state_->latestSubmittedRenderOrdinal.store(renderOrdinal_, std::memory_order_release);
        }
        state_->latestCompletedRenderOrdinal.store(renderOrdinal_, std::memory_order_release);
        if (state_->terminalRenderExitTracking.load(std::memory_order_acquire))
            state_->terminalRenderExitDiagnosticStage.store(
                TerminalRenderExitDiagnosticStage::PresentationCaptureDestructorComplete,
                std::memory_order_release);
    }

    void setDecision(long long selectedOutputFrame, long long skippedDeadlineCount, bool repeated) {
        if (!enabled_)
            return;
        selectedOutputFrame_ = selectedOutputFrame;
        skippedDeadlineCount_ = skippedDeadlineCount;
        repeated_ = repeated;
    }

    void markSubmitted(long long outputFrame) {
        if (!enabled_)
            return;
        submittedOutputFrame_ = outputFrame;
        repeated_ = false;
    }

private:
    CompositorSpikeState* state_ = nullptr;
    bool enabled_ = false;
    long long callbackBeginQpc_ = 0;
    long long renderOrdinal_ = -1;
    long long selectedOutputFrame_ = -1;
    long long submittedOutputFrame_ = -1;
    long long skippedDeadlineCount_ = 0;
    bool repeated_ = true;
};

class NativePresentTokenCapture {
public:
    NativePresentTokenCapture(const std::shared_ptr<CompositorSpikeState>& state,
                              const MvmNativePresentCompositionToken& lastToken,
                              std::uint64_t tokenSerial, std::uint64_t propagationSerial)
        : state_(state), token_(lastToken), propagationSerial_(propagationSerial),
          active_(state->nativePresentCaptureActive.load(std::memory_order_acquire)) {
        // non-formal callbackへ直前のformal identityを持ち越さない。
        token_.intentOrdinal = 0;
        token_.intentOrdinalValid = 0;
        if (active_ && token_.outputFrameNumber >= 0) {
            token_.tokenSerial = tokenSerial;
            token_.propagationSerial = propagationSerial_;
            valid_ = true;
        }
    }

    ~NativePresentTokenCapture() {
        if (!active_)
            return;
        const bool terminalExitTracking =
            state_->terminalRenderExitTracking.load(std::memory_order_acquire);
        if (terminalExitTracking)
            state_->terminalRenderExitDiagnosticStage.store(
                TerminalRenderExitDiagnosticStage::NativeTokenDestructorEntered,
                std::memory_order_release);
        const auto hook = state_->nativePresentHook;
        const bool succeeded = valid_ && hook && hook->setCompositionToken(token_);
        {
            std::lock_guard<std::mutex> lock(state_->compositionTokenAttributionMutex);
            state_->latestCompositionTokenPublication = {
                true, succeeded, static_cast<std::uint32_t>(GetCurrentThreadId()), gpu::qpcTicks(),
                token_};
        }
        if (!succeeded)
            state_->nativePresentTokenSetFailureCount.fetch_add(1, std::memory_order_relaxed);
        if (terminalExitTracking)
            state_->terminalRenderExitDiagnosticStage.store(
                TerminalRenderExitDiagnosticStage::NativeTokenDestructorComplete,
                std::memory_order_release);
    }

    bool setFormalIntentOrdinal(long long intentOrdinal) {
        if (intentOrdinal < 0)
            return false;
        token_.intentOrdinal = static_cast<std::uint64_t>(intentOrdinal);
        token_.intentOrdinalValid = 1;
        return true;
    }

    std::uint64_t tokenSerial() const { return token_.tokenSerial; }

    bool setFrame(const gpu::ComposedFrame& frame, std::uint64_t tokenSerial) {
        valid_ = makeNativePresentCompositionToken(frame, tokenSerial, token_.intentOrdinal,
                                                   token_.intentOrdinalValid != 0, token_);
        token_.propagationSerial = propagationSerial_;
        return valid_;
    }

    const MvmNativePresentCompositionToken& token() const { return token_; }

private:
    std::shared_ptr<CompositorSpikeState> state_;
    MvmNativePresentCompositionToken token_{};
    std::uint64_t propagationSerial_ = 0;
    bool active_ = false;
    bool valid_ = false;
};

class CompositorRhiRenderer final : public QQuickRhiItemRenderer {
public:
    CompositorRhiRenderer(std::shared_ptr<CompositorSpikeState> state,
                          gpu::GpuCompletionBackend backend)
        : state_(std::move(state)), backend_(backend) {}

    ~CompositorRhiRenderer() override {
        releaseRtv();
        releaseDiagnosticContext();
    }

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
        // diagnosticTargetPixelToggleはattach()がGUI threadで後から立てるため、ここで
        // flagを見るとinitialize()がattach()より先に走ったrunだけnativeContext1_がnullの
        // まま残り、render()のmarker発行がnull derefになる。capabilityの取得はflagと無関係
        // に常に行い、使用可否はissueTargetPixelToggle側でfail-closeする。
        const HRESULT contextResult = static_cast<ID3D11DeviceContext*>(h->context)
                                          ->QueryInterface(IID_PPV_ARGS(&nativeContext1_));
        if (FAILED(contextResult))
            nativeContext1_ = nullptr;
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
        state_->renderCallbackActive.store(true, std::memory_order_release);
        [[maybe_unused]] const auto renderCallbackExit = qScopeGuard([state = state_] {
            if (state->terminalRenderExitTracking.load(std::memory_order_acquire))
                state->terminalRenderExitDiagnosticStage.store(
                    TerminalRenderExitDiagnosticStage::RenderCallbackExited,
                    std::memory_order_release);
            state->renderCallbackActive.store(false, std::memory_order_release);
        });
        if (state_->p3SeekDiagnostics.active.load(std::memory_order_acquire)) {
            state_->p3SeekDiagnostics.renderCallbackCount.fetch_add(1, std::memory_order_relaxed);
            state_->p3SeekDiagnostics.lastRenderCallbackQpc.store(callbackBegin,
                                                                  std::memory_order_relaxed);
        }
        if (state_->teardownRequested.load(std::memory_order_acquire)) {
            auto requestedStage = RenderTeardownDiagnosticStage::Requested;
            state_->teardownDiagnosticStage.compare_exchange_strong(
                requestedStage, RenderTeardownDiagnosticStage::RenderCallbackObserved,
                std::memory_order_acq_rel);
            if (teardown())
                update();
            return;
        }
        // measurement stop後はGUI threadのenvelope stop updateが進行中frameへ
        // coalesceされ得る。render thread自身でstop request処理とteardown要求到着まで
        // callbackを橋渡しする。このgateはscheduler invocationとnative Present token生成
        // より前なので、terminal後の証跡を増やさない。
        if (state_->nativePresentCaptureEnvelopeEnabled.load(std::memory_order_acquire) &&
            state_->measurementStopCaptured.load(std::memory_order_acquire)) {
            captureMeasurementBoundary(callbackBegin);
            update();
            return;
        }
        const auto nativeHook = state_->nativePresentHook;
        const std::uint64_t propagationSerial =
            nativeHook ? nativeHook->recordDirtyPropagationStage(MVM_DIRTY_STAGE_COMPOSITOR_RENDER)
                       : 0;
        // P1と同じくrender threadから次のframeを要求する。GUI timerだけでは
        // scene graph requestがcoalesceされ、60Hz output deadlineを取りこぼす。
        // W2-C0.1 envelope drain中は新しいPresentを生まない。scope exit時点で
        // measurement stop済みかを判定するため、pastSourceDomain callback自身が
        // 次の無intent callbackを予約することもない。
        [[maybe_unused]] const auto nextFrameRequest = qScopeGuard([this] {
            if (!state_->nativePresentCaptureEnvelopeEnabled.load(std::memory_order_acquire) ||
                !state_->measurementStopCaptured.load(std::memory_order_acquire) ||
                !state_->nativePresentEnvelopeStopped.load(std::memory_order_acquire))
                update();
        });
        // fatal後もmeasurement stop要求だけはrender threadで採取する。
        // 先にreturnすると停止snapshotが未採取のままshutdownへ進んでしまう。
        if (captureMeasurementBoundary(callbackBegin))
            return;
        if (!state_->deviceReady.load(std::memory_order_acquire) ||
            state_->fatal.load(std::memory_order_acquire))
            return;
        const std::uint64_t nativeTokenSerial = ++nativePresentTokenSerial_;
        NativePresentTokenCapture nativePresentToken(state_, lastNativePresentToken_,
                                                     nativeTokenSerial, propagationSerial);
        // pipelineをopenする前のP3-C-2 preflightでもactual targetを検査できるよう、
        // frame pairの有無に依存せずQRhi textureの実pixel sizeを公開する。
        if (auto* texture = colorTexture()) {
            const QSize targetSize = texture->pixelSize();
            state_->publishActualOutputSize(targetSize.width(), targetSize.height());
        }
        state_->presentCallbackCount.fetch_add(1, std::memory_order_relaxed);
        const bool presentationCaptureActive =
            presentationOpportunityEnabled_ &&
            state_->presentationCaptureActive.load(std::memory_order_relaxed);
        const long long presentationRenderOrdinal =
            presentationCaptureActive ? presentationRenderOrdinal_++ : -1;
        PresentationRenderCapture presentationCapture(state_.get(), presentationCaptureActive,
                                                      callbackBegin, presentationRenderOrdinal);
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
        const bool formalOpportunityActive =
            state_->formalOpportunityCaptureActive.load(std::memory_order_acquire);
        // B3-I5B。admission closeは新規FOREIGN reservationだけを禁止する。
        // 既存active FOREIGN transactionのrender/Present/receipt/commitは止めない。
        bool foreignAdmissionOpen = true;
        if (formalOpportunityActive &&
            state_->formalOpportunityEnvelopePrerollActive.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(state_->formalOpportunityMutex);
            const auto transition = state_->formalPrerollTransition.snapshot();
            foreignAdmissionOpen = !transition.started || transition.foreignAdmissionOpen;
        }
        bool formalOpportunityRepeat = false;
        bool formalDecisionObserved = false;
        gpu::PresentationOpportunityDecision formalDecision;
        if (output < 0 && formalOpportunityActive && foreignAdmissionOpen) {
            gpu::PresentationOpportunityError formalError = gpu::PresentationOpportunityError::None;
            {
                std::lock_guard<std::mutex> lock(state_->formalOpportunityMutex);
                formalDecision = state_->formalOpportunityScheduler.selectForRender(
                    callbackBegin, capturePresentationAuthority(state_),
                    formalOpportunityRenderOrdinal_);
                formalError = state_->formalOpportunityScheduler.error();
            }
            if (!formalDecision.valid) {
                fail(std::string("P2-D5-2 opportunity select失敗: ") +
                     gpu::presentationOpportunityErrorName(formalError));
                return;
            }
            // scheduler decisionがintent identityの唯一のproducerである。duplicate callbackと
            // current required set外decisionはvalid decisionのまま、formal
            // transportだけを抑止する。
            const bool foreignPreMeasurement =
                state_->formalOpportunityEnvelopePrerollActive.load(std::memory_order_acquire);
            MeasurementBoundaryRelation boundaryRelation = MeasurementBoundaryRelation::Unresolved;
            if (foreignPreMeasurement) {
                boundaryRelation = MeasurementBoundaryRelation::PreMeasurementArm;
            } else {
                const long long armQpc = state_->measurementArmQpc.load(std::memory_order_acquire);
                const long long startQpc =
                    state_->measurementStartQpc.load(std::memory_order_acquire);
                const long long endQpc = state_->measurementEndQpc.load(std::memory_order_acquire);
                if (armQpc > 0 && armQpc <= startQpc && startQpc < endQpc) {
                    boundaryRelation = callbackBegin < armQpc
                                           ? MeasurementBoundaryRelation::PreMeasurementArm
                                       : callbackBegin < startQpc
                                           ? MeasurementBoundaryRelation::ArmedPreMeasurement
                                       : callbackBegin < endQpc
                                           ? MeasurementBoundaryRelation::WithinCurrentMeasurement
                                           : MeasurementBoundaryRelation::PostMeasurement;
                }
            }
            const auto transportDisposition =
                gpu::formalIntentTransportDisposition(foreignPreMeasurement, formalDecision);
            {
                std::lock_guard<std::mutex> lock(state_->formalOpportunityMutex);
                if (!state_->formalOpportunityScheduler.noteInvocationTransportDisposition(
                        formalDecision.invocationSerial, transportDisposition)) {
                    fail("W4-C2 scheduler invocationとtransport dispositionを結合できません");
                    return;
                }
            }
            const auto recordSuppressedNonFormal = [&] {
                if (nativePresentToken.tokenSerial() == 0) {
                    fail("P2-D5-2 suppressed transportに有効なtoken serialがありません");
                    return false;
                }
                std::lock_guard<std::mutex> lock(state_->nativePresentIntentScopeMutex);
                state_->nativePresentIntentScopeLedger.push_back(
                    {nativePresentToken.tokenSerial(), formalDecision.reservationId,
                     static_cast<std::uint64_t>(formalDecision.opportunityOrdinal),
                     foreignPreMeasurement ? NativePresentIntentScope::ForeignPreMeasurement
                                           : NativePresentIntentScope::CurrentMeasurement,
                     callbackBegin, true, formalDecision.requiredIntentMembership,
                     formalDecision.requiredIntentMembershipExact, boundaryRelation, true,
                     formalDecision.duplicateCallback, formalDecision.repeat,
                     formalDecision.pastSourceDomain, formalDecision.targetFrame,
                     formalDecision.lastFinalizedOpportunityOrdinal, formalDecision.renderBeginQpc,
                     transportDisposition});
                return true;
            };
            if (transportDisposition ==
                gpu::FormalIntentTransportDisposition::InvalidMembershipProvenance) {
                fail("P2-D5-2 formal intent membership provenanceが不正です");
                return;
            }
            if (transportDisposition ==
                gpu::FormalIntentTransportDisposition::SuppressDuplicateCallback) {
                if (!recordSuppressedNonFormal())
                    return;
                state_->formalDuplicateTransportSuppressedCount.fetch_add(
                    1, std::memory_order_relaxed);
                return;
            }
            if (transportDisposition ==
                gpu::FormalIntentTransportDisposition::SuppressOutsideRequiredSet) {
                if (!recordSuppressedNonFormal())
                    return;
                state_->formalOutsideRequiredTransportSuppressedCount.fetch_add(
                    1, std::memory_order_relaxed);
                return;
            }
            if (!nativePresentToken.setFormalIntentOrdinal(formalDecision.opportunityOrdinal)) {
                fail("P2-D5-2 formal intent ordinalをcomposition tokenへ設定できません");
                return;
            }
            if (nativePresentToken.tokenSerial() == 0) {
                fail("P2-D5-2 intent scopeに有効なtoken serialがありません");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(state_->formalOpportunityMutex);
                if (!state_->formalQualifiedCommitJoin.reserve(
                        {formalDecision.reservationId, formalDecision.opportunityOrdinal,
                         nativePresentToken.tokenSerial()})) {
                    fail(std::string("P2-D5-2 B3-I0 reservation join失敗: ") +
                         gpu::qualifiedCommitErrorName(state_->formalQualifiedCommitJoin.error()));
                    return;
                }
                if (foreignPreMeasurement &&
                    !state_->formalPrerollTransition.noteForeignReservationAdmitted(callbackBegin)) {
                    fail(std::string("P2-D5-2 B3-I5B FOREIGN admissionを記録できません: ") +
                         gpu::prerollTransitionErrorName(state_->formalPrerollTransition.error()));
                    return;
                }
            }
            if (!foreignPreMeasurement && !state_->boundaryFirstReservationRecorded.exchange(
                                              true, std::memory_order_acq_rel)) {
                BoundarySwapAttributionEvent reservationEvent;
                reservationEvent.kind = BoundarySwapEventKind::FirstReservation;
                reservationEvent.qpc = gpu::qpcTicks();
                reservationEvent.threadId = static_cast<std::uint32_t>(GetCurrentThreadId());
                reservationEvent.phase = "CURRENT_MEASUREMENT_RENDER";
                reservationEvent.activeReservation = true;
                reservationEvent.reservation = {formalDecision.reservationId,
                                                formalDecision.opportunityOrdinal,
                                                nativePresentToken.tokenSerial()};
                reservationEvent.transitionStepSerial =
                    state_->prerollTransitionStepSerial.load(std::memory_order_seq_cst);
                {
                    // diagnostic-only。orderingはevent serialとQPCで確定しており、
                    // このsnapshotをhandshake authorityには使わない。
                    std::lock_guard<std::mutex> lock(state_->formalOpportunityMutex);
                    const auto transition = state_->formalPrerollTransition.snapshot();
                    reservationEvent.transitionState = transition.state;
                    reservationEvent.transitionError = transition.error;
                    reservationEvent.transitionQuiescent = transition.verdict.quiescent;
                }
                state_->recordBoundarySwapAttribution(reservationEvent);
            }
            {
                // scheduler decisionがordinalとscopeの唯一のproducerである。
                // reservationとnative recordのexact join keyはABI v5 token serial。
                std::lock_guard<std::mutex> lock(state_->nativePresentIntentScopeMutex);
                state_->nativePresentIntentScopeLedger.push_back(
                    {nativePresentToken.tokenSerial(), formalDecision.reservationId,
                     static_cast<std::uint64_t>(formalDecision.opportunityOrdinal),
                     foreignPreMeasurement ? NativePresentIntentScope::ForeignPreMeasurement
                                           : NativePresentIntentScope::CurrentMeasurement,
                     callbackBegin, true,
                     !foreignPreMeasurement && formalDecision.requiredIntentMembership,
                     formalDecision.requiredIntentMembershipExact, boundaryRelation, true,
                     formalDecision.duplicateCallback, formalDecision.repeat,
                     formalDecision.pastSourceDomain, formalDecision.targetFrame,
                     formalDecision.lastFinalizedOpportunityOrdinal,
                     formalDecision.renderBeginQpc});
            }
            if (foreignPreMeasurement) {
                const long long repeatedFrame = lastNativePresentToken_.outputFrameNumber;
                std::string completionError;
                if (repeatedFrame < 0 ||
                    !markFormalRenderComplete(formalDecision, nativePresentToken.tokenSerial(),
                                              gpu::qpcTicks(), repeatedFrame, completionError)) {
                    fail("P2-D5-2 lower envelope render完了記録失敗: " + completionError);
                    return;
                }
                state_->formalOpportunityPresentedFrame.store(repeatedFrame,
                                                              std::memory_order_release);
                ++formalOpportunityRenderOrdinal_;
                // lower envelopeではintentだけを生成し、source selection、formal
                // counter、measurement schedulerへは接続しない。
                return;
            }
            formalDecisionObserved = true;
            if (!formalDecision.duplicateCallback)
                ++formalOpportunityRenderOrdinal_;

            // D5-1 synthetic deadline schedulerはshadow diagnosticとしてだけ進める。
            // このdecisionをframe selectionやformal drop判定へ使ってはいけない。
            const long long measurementEnd =
                state_->measurementEndQpc.load(std::memory_order_acquire);
            const auto shadow = scheduler_.takeDueBefore(callbackBegin, measurementEnd);
            if (shadow.due)
                state_->diagnosticSyntheticDeadlineDropCount.fetch_add(shadow.skippedDeadlineCount,
                                                                       std::memory_order_relaxed);

            output = formalDecision.targetFrame;
            formalOpportunityRepeat = formalDecision.repeat || formalDecision.duplicateCallback;
            // scheduled/dropped accountingはrender時点では確定しない。同一
            // opportunity内のswapがsupersedeされ得るため、closeでfinalize済み
            // ledgerからまとめて数える。
        } else if (output < 0 && audioMasterEnabled) {
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
        const bool repeatedThisCallback =
            formalOpportunityRepeat || !a || (needsB && !b) || output < 0;
        presentationCapture.setDecision(output, schedulerDecision.skippedDeadlineCount,
                                        repeatedThisCallback);
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
            if (state_->diagnosticTargetPixelToggle.load(std::memory_order_acquire)) {
                std::string markerError;
                if (!ensureRtv(colorTexture(), markerError)) {
                    fail(markerError);
                    return;
                }
                cb->beginExternal();
                const bool markerOk = issueTargetPixelToggle(nativeTokenSerial, nativeHook,
                                                             propagationSerial, markerError);
                cb->endExternal();
                if (!markerOk) {
                    fail(markerError);
                    return;
                }
            }
            if (formalDecisionObserved && !formalDecision.duplicateCallback) {
                const long long presented =
                    state_->formalOpportunityPresentedFrame.load(std::memory_order_acquire);
                std::string completionError;
                if (!markFormalRenderComplete(formalDecision, nativePresentToken.tokenSerial(),
                                              gpu::qpcTicks(), presented, completionError))
                    fail("P2-D5-2 render完了記録失敗: " + completionError);
            }
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
        bool markerFailed = false;
        if (ok && state_->diagnosticTargetPixelToggle.load(std::memory_order_acquire)) {
            ok = issueTargetPixelToggle(nativeTokenSerial, nativeHook, propagationSerial, err);
            markerFailed = !ok;
        }
        if (ok && !diagnostic && state_->p3MeasurementActive.load(std::memory_order_acquire) &&
            state_->phase4Enabled.load(std::memory_order_acquire))
            ok = issueTransitionProbes(frame, size, err);
        cb->endExternal();
        const long long externalEnd = gpu::qpcTicks();
        cb->endPass();
        if (!ok) {
            state_->renderFailureCount.fetch_add(1, std::memory_order_relaxed);
            noteDrop(gpu::OutputDropReason::RenderFailure);
            if (markerFailed)
                fail(err);
            else if (state_->compositor.fatal())
                fail(state_->compositor.fatalReason());
            return;
        }
        const long long submissionQpc = gpu::qpcTicks();
        if (!nativePresentToken.setFrame(frame, nativeTokenSerial)) {
            if (state_->nativePresentCaptureActive.load(std::memory_order_acquire)) {
                fail("native Present composition tokenを構築できません");
                return;
            }
        } else {
            // capture開始前にも最後の実compositionを保持し、lower envelopeの
            // scheduler-produced intentを架空frameへ結び付けない。
            lastNativePresentToken_ = nativePresentToken.token();
        }
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
        presentationCapture.markSubmitted(output);
        if (formalDecisionObserved) {
            state_->formalOpportunityPresentedFrame.store(output, std::memory_order_release);
            std::string completionError;
            if (!markFormalRenderComplete(formalDecision, nativePresentToken.tokenSerial(),
                                          displayedQpc, output, completionError)) {
                fail("P2-D5-2 render完了記録失敗: " + completionError);
                return;
            }
        }
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
    bool markFormalRenderComplete(const gpu::PresentationOpportunityDecision& decision,
                                  std::uint64_t tokenSerial, long long renderEndQpc,
                                  long long renderedFrame, std::string& error) {
        std::lock_guard<std::mutex> lock(state_->formalOpportunityMutex);
        if (!state_->formalOpportunityScheduler.markRenderComplete(renderEndQpc, renderedFrame,
                                                                   decision.renderOrdinal)) {
            error =
                gpu::presentationOpportunityErrorName(state_->formalOpportunityScheduler.error());
            return false;
        }
        if (!state_->formalQualifiedCommitJoin.markRenderComplete(
                decision.reservationId, decision.opportunityOrdinal, tokenSerial)) {
            error = gpu::qualifiedCommitErrorName(state_->formalQualifiedCommitJoin.error());
            return false;
        }
        return true;
    }

    bool issueTargetPixelToggle(std::uint64_t tokenSerial,
                                const std::shared_ptr<NativePresentHook>& nativeHook,
                                std::uint64_t propagationSerial, std::string& err) {
        if (!nativeContext1_) {
            err = "TARGET_RHIITEM_PIXEL_TOGGLEにD3D11.1 contextが必要です";
            return false;
        }
        const float marker = (tokenSerial & 1U) != 0 ? 1.0f : 0.0f;
        const float color[4]{marker, 1.0f - marker, marker, 1.0f};
        const D3D11_RECT rect{0, 0, 2, 2};
        nativeContext1_->ClearView(rtv_, color, &rect, 1);
        if (nativeHook)
            nativeHook->recordDirtyPropagationStage(MVM_DIRTY_STAGE_TARGET_PIXEL_TOGGLE,
                                                    propagationSerial);
        return true;
    }

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

    bool startFormalOpportunityScheduler(bool envelopePreroll = false) {
        const long long repeatedFrame = lastNativePresentToken_.outputFrameNumber;
        if (envelopePreroll && repeatedFrame < 0)
            return false;
        const gpu::PresentationOpportunityConfig config{
            envelopePreroll ? repeatedFrame + 1
                            : state_->formalRequiredFrameCount.load(std::memory_order_acquire),
            envelopePreroll ? repeatedFrame : 0,
            envelopePreroll ? 1 : 60,
            1,
            state_->formalRefreshNumerator.load(std::memory_order_relaxed),
            state_->formalRefreshDenominator.load(std::memory_order_relaxed),
            static_cast<long long>(gpu::qpcFrequency()),
            state_->formalSchedulerInvocationLedgerEnabled.load(std::memory_order_acquire)};
        std::lock_guard<std::mutex> lock(state_->formalOpportunityMutex);
        return state_->formalOpportunityScheduler.start(config);
    }

    // terminal decisionの事実をそのまま運ぶ。QPCやledger末尾から再構築しない。
    static StopWitnessTerminalFacts
    terminalStopFacts(const gpu::PresentationOpportunityDecision& decision) {
        StopWitnessTerminalFacts facts;
        facts.schedulerInvocationSerial = decision.invocationSerial;
        facts.intentOrdinal = decision.opportunityOrdinal;
        facts.targetFrame = decision.targetFrame;
        facts.pastSourceDomain = decision.pastSourceDomain;
        facts.requiredIntentMembership = decision.requiredIntentMembership;
        facts.formalOpportunityDomainReachedPublished = true;
        return facts;
    }

    void finishMeasurement(long long callbackBegin, StopArbitration cause,
                           const StopClaimResult& claim,
                           const StopWitnessTerminalFacts& terminal = {},
                           bool claimFromPublicationRecord = false, bool claimRecorded = true) {
        // W4-C3 stop witness v3。pre snapshotはfinishMeasurement entry時点。
        CompositorStopWitness witness;
        witness.cause = cause;
        witness.renderCallbackBeginQpc = callbackBegin;
        witness.terminal = terminal;
        witness.arbitrationClaimed = cause;
        witness.arbitrationPrevious = claim.previous;
        witness.arbitrationClaimSucceeded = claim.succeeded;
        witness.claimFromPublicationRecord = claimFromPublicationRecord;
        witness.claimRecorded = claimRecorded;
        witness.measurementStartState =
            state_->measurementStartArbitrationState.load(std::memory_order_seq_cst);
        witness.resetCountDuringMeasurement =
            state_->stopArbitrationResetDuringMeasurementCount.load(std::memory_order_seq_cst);
        witness.measurementStartExplicitStopPublishSerial =
            state_->measurementStartExplicitStopPublishSerial.load(std::memory_order_seq_cst);
        witness.measurementStartFatalPublishSerial =
            state_->measurementStartFatalPublishSerial.load(std::memory_order_seq_cst);
        witness.preExplicitStopPublishSerial =
            state_->explicitStopPublishSerial.load(std::memory_order_seq_cst);
        witness.preFatalPublishSerial = state_->fatalPublishSerial.load(std::memory_order_seq_cst);
        witness.preCaptureGateOpen =
            state_->formalOpportunityCaptureActive.load(std::memory_order_acquire);
        witness.preExplicitStopRequested =
            state_->measurementStopRequested.load(std::memory_order_acquire);
        witness.prePlannedWindowEndReached =
            state_->measurementIntervalActive.load(std::memory_order_acquire) &&
            callbackBegin >= state_->measurementEndQpc.load(std::memory_order_acquire);
        witness.preFatalLatched = state_->fatal.load(std::memory_order_acquire);
        witness.finishMeasurementEntered = true;
        const bool retainNativeEnvelope =
            state_->nativePresentCaptureEnvelopeEnabled.load(std::memory_order_acquire) &&
            !state_->fatal.load(std::memory_order_acquire);
        if (!retainNativeEnvelope &&
            state_->nativePresentCaptureActive.exchange(false, std::memory_order_acq_rel)) {
            std::string hookError;
            if (!state_->nativePresentHook || !state_->nativePresentHook->endCapture(hookError) ||
                !state_->nativePresentHook->authorityValid() ||
                state_->nativePresentTokenSetFailureCount.load(std::memory_order_relaxed) != 0) {
                fail(hookError.empty() ? "native Present authorityが不成立です" : hookError);
            }
        }
        if (state_->measurementIntervalActive.exchange(false, std::memory_order_acq_rel)) {
            // capture gate exchangeの直前にalternative publication serialを撮る。
            witness.gateCloseSnapshotCaptured = true;
            witness.gateCloseExplicitStopPublishSerial =
                state_->explicitStopPublishSerial.load(std::memory_order_seq_cst);
            witness.gateCloseFatalPublishSerial =
                state_->fatalPublishSerial.load(std::memory_order_seq_cst);
            // capture gate exchangeの実returnだけをactionとして記録する。
            const bool captureGateWasOpen =
                state_->formalOpportunityCaptureActive.exchange(false, std::memory_order_acq_rel);
            witness.captureGateExchangeClosed = captureGateWasOpen;
            if (captureGateWasOpen) {
                bool closed = false;
                gpu::PresentationOpportunitySnapshot snapshot;
                {
                    std::lock_guard<std::mutex> lock(state_->formalOpportunityMutex);
                    closed =
                        cause == StopArbitration::PlannedWindowEnd
                            ? state_->formalOpportunityScheduler.closePlannedWindow()
                            : state_->formalOpportunityScheduler.closeWithoutNormalCompletion();
                    snapshot = state_->formalOpportunityScheduler.snapshot();
                }
                if (!closed) {
                    fail(std::string("P2-D5-2 opportunity close失敗: ") +
                         gpu::presentationOpportunityErrorName(snapshot.error));
                } else {
                    state_->formalOpportunityTrueDropCount.fetch_add(snapshot.trueDrop,
                                                                     std::memory_order_relaxed);
                    state_->droppedOutputCount.fetch_add(snapshot.trueDrop,
                                                         std::memory_order_relaxed);
                    state_->scheduledOutputCount.fetch_add(
                        snapshot.displayedUnique + snapshot.trueDrop, std::memory_order_relaxed);
                }
                const long long shadowClosed = scheduler_.closeBefore(
                    state_->measurementEndQpc.load(std::memory_order_acquire));
                state_->diagnosticSyntheticDeadlineDropCount.fetch_add(shadowClosed,
                                                                       std::memory_order_relaxed);
            } else {
                const long long closed = scheduler_.closeBefore(
                    state_->measurementEndQpc.load(std::memory_order_acquire));
                state_->scheduledOutputCount.fetch_add(closed, std::memory_order_relaxed);
                noteDrop(gpu::OutputDropReason::SchedulerDeadline, closed);
            }
        }
        state_->presentationCaptureActive.store(false, std::memory_order_release);
        state_->playbackSchedulerEnabled.store(false, std::memory_order_release);
        state_->diagnosticTimingEnabled.store(false, std::memory_order_release);
        state_->diagnosticLockTiming = state_->device.lock().endDiagnostics();
        {
            std::lock_guard<std::mutex> lock(state_->measurementMutex);
            state_->measurementStop = measurementCounters();
            state_->measurementStop.qpc = callbackBegin;
        }
        state_->measurementStopCaptured.store(true, std::memory_order_release);
        witness.measurementStopPublished = true;
        witness.postCaptureGateOpen =
            state_->formalOpportunityCaptureActive.load(std::memory_order_acquire);
        witness.measurementStopQpc = callbackBegin;
        // winner witnessはwrite-once。後着のstopは上書きせずduplicateへ落とす。
        // capturedは「完全に書き終えたwitnessが存在する」publication contractなので、
        // payloadを保存し終えてから最後にpublishする。
        {
            std::lock_guard<std::mutex> lock(state_->stopWitnessMutex);
            if (!state_->stopWitnessCaptured.load(std::memory_order_acquire)) {
                state_->stopWitness = witness;
                state_->stopWitnessCaptured.store(true, std::memory_order_release);
            } else {
                state_->stopWitnessDuplicateCount.fetch_add(1, std::memory_order_seq_cst);
            }
        }
    }

    enum class PrerollTransitionProgress { Waiting = 0, Ready, Fatal };

    struct PrerollIdentityClosure {
        long long closedCount = 0;
        bool partitionExact = true;
    };

    // issued prefixの1:1 exact join。QPC近接、latest Present、callback index、
    // serial推定を使わず、token serialのexact一致だけでrecordを対応付ける。
    static PrerollIdentityClosure
    computePrerollIdentityClosure(const std::vector<NativePresentIntentScopeRecord>& ledger,
                                  const std::vector<MvmNativePresentRecord>& records) {
        PrerollIdentityClosure closure;
        long long ledgerMatchTotal = 0;
        for (const auto& scopeRecord : ledger) {
            if (scopeRecord.scope != NativePresentIntentScope::ForeignPreMeasurement) {
                // preroll drain中にCURRENT scope recordが存在してはならない。
                closure.partitionExact = false;
                continue;
            }
            long long matchCount = 0;
            const MvmNativePresentRecord* matched = nullptr;
            for (const auto& record : records) {
                if (record.tokenPresent == 0 || record.token.tokenSerial != scopeRecord.tokenSerial)
                    continue;
                ++matchCount;
                matched = &record;
            }
            ledgerMatchTotal += matchCount;
            if (scopeRecord.transportDisposition !=
                gpu::FormalIntentTransportDisposition::Transport) {
                // suppressed recordはissued transactionではない。Present 0件か、
                // reservationを持たない1件のどちらかで終端する。
                if (matchCount > 1)
                    closure.partitionExact = false;
                continue;
            }
            if (matchCount != 1 || matched == nullptr || scopeRecord.reservationId == 0 ||
                matched->presentSerial == 0 || matched->swapchainIdentity == 0 ||
                matched->hresult < 0 || matched->intentOrdinalValid == 0 ||
                matched->intentOrdinal != scopeRecord.intentOrdinal) {
                closure.partitionExact = false;
                continue;
            }
            ++closure.closedCount;
        }
        long long tokenBearingRecordCount = 0;
        for (const auto& record : records)
            if (record.tokenPresent != 0)
                ++tokenBearingRecordCount;
        // ledgerへ対応しないtoken付きPresentが残っていればpartitionはexactではない。
        if (tokenBearingRecordCount != ledgerMatchTotal)
            closure.partitionExact = false;
        return closure;
    }

    void recordPrerollTransitionEvent(BoundarySwapEventKind kind, const char* phase) {
        BoundarySwapAttributionEvent event;
        event.kind = kind;
        event.qpc = gpu::qpcTicks();
        event.threadId = static_cast<std::uint32_t>(GetCurrentThreadId());
        event.phase = phase;
        event.transitionStepSerial =
            state_->prerollTransitionStepSerial.fetch_add(1, std::memory_order_seq_cst) + 1;
        {
            std::lock_guard<std::mutex> lock(state_->formalOpportunityMutex);
            const auto transition = state_->formalPrerollTransition.snapshot();
            event.transitionState = transition.state;
            event.transitionError = transition.error;
            event.transitionQuiescent = transition.verdict.quiescent;
        }
        state_->recordBoundarySwapAttribution(event);
    }

    bool requestPrerollAdmissionClose(long long callbackBegin) {
        {
            std::lock_guard<std::mutex> lock(state_->formalOpportunityMutex);
            auto& handshake = state_->formalPrerollTransition;
            if (!handshake.requestAdmissionClose(callbackBegin) ||
                !handshake.beginDrain(callbackBegin)) {
                fail(std::string("P2-D5-2 B3-I5B admission closeに失敗しました: ") +
                     gpu::prerollTransitionErrorName(handshake.error()));
                return false;
            }
        }
        recordPrerollTransitionEvent(BoundarySwapEventKind::PrerollAdmissionClosed,
                                     "RENDER_PREROLL_ADMISSION_CLOSE");
        return true;
    }

    // 1 render callback内でqueue / join / scheduler / Qt one-shot / scope ledger /
    // transport counterを1つのlogical snapshotとして評価する。
    PrerollTransitionProgress advancePrerollTransition(long long callbackBegin) {
        const auto hook = state_->nativePresentHook;
        if (!hook) {
            fail("P2-D5-2 B3-I5B quiescence checkにnative Present hookが必要です");
            return PrerollTransitionProgress::Fatal;
        }
        MvmNativePresentOneShotSnapshot oneShot{};
        std::string snapshotError;
        if (!hook->readOneShotSnapshot(hook->captureEpoch(), oneShot, snapshotError)) {
            fail("P2-D5-2 B3-I5B one-shot snapshotを取得できません: " + snapshotError);
            return PrerollTransitionProgress::Fatal;
        }
        const auto hookSnapshot = hook->snapshot();
        std::vector<NativePresentIntentScopeRecord> scopeLedger;
        {
            std::lock_guard<std::mutex> lock(state_->nativePresentIntentScopeMutex);
            scopeLedger = state_->nativePresentIntentScopeLedger;
        }
        const auto closure = computePrerollIdentityClosure(scopeLedger, hookSnapshot.records);
        const long long transportFailureCounterTotal =
            static_cast<long long>(hookSnapshot.overflowCount) +
            static_cast<long long>(hookSnapshot.missingTokenCount) +
            static_cast<long long>(hookSnapshot.duplicateTokenCount) +
            static_cast<long long>(hookSnapshot.staleTokenCount) +
            static_cast<long long>(hookSnapshot.failedPresentCount) +
            static_cast<long long>(hookSnapshot.missingFrameSwappedReceiptCount) +
            static_cast<long long>(hookSnapshot.duplicateFrameSwappedReceiptCount) +
            static_cast<long long>(hookSnapshot.staleFrameSwappedReceiptCount) +
            static_cast<long long>(hookSnapshot.threadMismatchCount) +
            state_->nativePresentTokenSetFailureCount.load(std::memory_order_relaxed);

        bool waiting = false;
        std::string failure;
        bool closedForeignScheduler = false;
        {
            std::lock_guard<std::mutex> lock(state_->formalOpportunityMutex);
            auto& handshake = state_->formalPrerollTransition;
            auto& scheduler = state_->formalOpportunityScheduler;
            if (!handshake.snapshot().foreignSchedulerClosed) {
                const bool inflight =
                    scheduler.hasPendingRender() ||
                    state_->formalQualifiedCommitJoin.hasActiveReservation() ||
                    oneShot.pendingTokenValid != 0 || oneShot.pendingReceiptValid != 0 ||
                    handshake.snapshot().activeForeignTransactionCount != 0;
                if (!inflight) {
                    // scheduler closeはactive transaction drainとpending opportunity
                    // finalizeの後だけ行う。
                    if (!scheduler.finalizePendingOpportunityExact() ||
                        !handshake.notePendingOpportunityFinalized()) {
                        failure =
                            std::string("P2-D5-2 B3-I5B pending opportunity finalize失敗: ") +
                            gpu::prerollTransitionErrorName(handshake.error());
                    } else if (!scheduler.closeWithoutNormalCompletion() ||
                               !handshake.noteForeignSchedulerClosed()) {
                        failure = std::string("P2-D5-2 B3-I5B preroll scheduler close失敗: ") +
                                  gpu::prerollTransitionErrorName(handshake.error());
                    } else {
                        state_->formalOpportunityEnvelopePrerollActive.store(
                            false, std::memory_order_release);
                        state_->formalOpportunityEnvelopePrerollCompleted.store(
                            true, std::memory_order_release);
                        state_->formalOpportunityCaptureActive.store(false,
                                                                     std::memory_order_release);
                        closedForeignScheduler = true;
                    }
                }
            }
            if (failure.empty()) {
                const auto schedulerSnapshot = scheduler.snapshot();
                const auto& queue = schedulerSnapshot.requiredIntentQueue;
                gpu::PrerollQuiescenceObservation observation;
                observation.captureEpoch = oneShot.captureEpoch;
                observation.observerThreadId = static_cast<std::uint32_t>(GetCurrentThreadId());
                observation.prerollAdmissionClosed = !handshake.foreignAdmissionOpen();
                observation.schedulerPendingRender = scheduler.hasPendingRender();
                observation.schedulerPendingQualifiedEvidence =
                    scheduler.hasPendingQualifiedEvidence();
                observation.schedulerPendingOpportunity = scheduler.hasPendingOpportunity();
                observation.schedulerPendingOpportunityExactlyFinalized =
                    scheduler.pendingOpportunityExactlyFinalized();
                observation.queueActiveReservationCount = queue.activeReservationCount;
                observation.joinActiveReservation =
                    state_->formalQualifiedCommitJoin.hasActiveReservation();
                observation.qtPendingCompositionToken = oneShot.pendingTokenValid != 0;
                observation.qtPendingFrameSwappedReceipt = oneShot.pendingReceiptValid != 0;
                observation.issuedCount = queue.issuedCount;
                observation.renderedCount = queue.renderedCount;
                observation.qualifiedCommitCount = queue.qualifiedCommitCount;
                observation.dequeuedCount = queue.dequeuedCount;
                observation.queueConservationValid = queue.conservationValid;
                observation.issuedPrefixExactIdentityClosedCount = closure.closedCount;
                observation.prerollScopeLedgerTerminalPartitionExact = closure.partitionExact;
                observation.transportFailureCounterTotal = transportFailureCounterTotal;
                const auto verdict = handshake.evaluateQuiescence(observation, callbackBegin);
                if (handshake.error() != gpu::PrerollTransitionError::None) {
                    failure = std::string("P2-D5-2 B3-I5B quiescence handshake失敗: ") +
                              gpu::prerollTransitionErrorName(handshake.error());
                } else if (!verdict.quiescent) {
                    waiting = true;
                } else if (!handshake.ackQuiescence(gpu::qpcTicks())) {
                    failure = std::string("P2-D5-2 B3-I5B quiescence ack拒否: ") +
                              gpu::prerollTransitionErrorName(handshake.error());
                }
            }
        }
        if (!failure.empty()) {
            // timeoutを含め、handshake failureはPROTOCOL_FATALである。
            // performance dropやrequired setの縮小へは決して変換しない。
            fail(failure);
            return PrerollTransitionProgress::Fatal;
        }
        if (closedForeignScheduler)
            recordPrerollTransitionEvent(BoundarySwapEventKind::PrerollDrainObserved,
                                         "RENDER_PREROLL_SCHEDULER_CLOSED");
        if (waiting) {
            recordPrerollTransitionEvent(BoundarySwapEventKind::PrerollDrainObserved,
                                         "RENDER_PREROLL_DRAIN");
            return PrerollTransitionProgress::Waiting;
        }
        recordPrerollTransitionEvent(BoundarySwapEventKind::PrerollQuiescenceAck,
                                     "RENDER_PREROLL_QUIESCENCE_ACK");
        return PrerollTransitionProgress::Ready;
    }

    bool captureMeasurementBoundary(long long callbackBegin) {
        if (state_->nativePresentEnvelopeStartRequested.exchange(false,
                                                                 std::memory_order_acq_rel)) {
            std::string hookError;
            state_->nativePresentTokenSetFailureCount.store(0, std::memory_order_relaxed);
            state_->boundarySwapEventSerial.store(0, std::memory_order_relaxed);
            state_->prerollTransitionStepSerial.store(0, std::memory_order_relaxed);
            state_->boundaryFirstReservationRecorded.store(false, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(state_->boundarySwapAttributionMutex);
                state_->boundarySwapAttributionEvents.clear();
            }
            {
                std::lock_guard<std::mutex> lock(state_->compositionTokenAttributionMutex);
                state_->latestCompositionTokenPublication = {};
                state_->compositionTokenJoinFailure = {};
            }
            if (!state_->nativePresentHook || !state_->nativePresentHook->beginCapture(hookError)) {
                fail(hookError.empty() ? "native Present capture envelopeを開始できません"
                                       : hookError);
                return true;
            }
            {
                std::lock_guard<std::mutex> lock(state_->formalOpportunityMutex);
                if (!state_->formalQualifiedCommitJoin.startEnvelope()) {
                    fail("P2-D5-2 B3-I0 qualified commit envelopeを開始できません");
                    return true;
                }
            }
            state_->nativePresentEnvelopeBeginQpc.store(gpu::qpcTicks(), std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(state_->nativePresentIntentScopeMutex);
                state_->nativePresentIntentScopeLedger.clear();
            }
            state_->nativePresentCaptureActive.store(true, std::memory_order_release);
            if (state_->formalOpportunitySchedulerEnabled.load(std::memory_order_acquire)) {
                if (!startFormalOpportunityScheduler(true)) {
                    fail("P2-D5-2 lower envelope intent producerを開始できません");
                    return true;
                }
                formalOpportunityRenderOrdinal_ = 0;
                state_->formalOpportunityEnvelopePrerollCompleted.store(false,
                                                                        std::memory_order_release);
                state_->formalOpportunityEnvelopePrerollStarted.store(true,
                                                                      std::memory_order_release);
                state_->formalOpportunityEnvelopePrerollActive.store(true,
                                                                     std::memory_order_release);
                state_->formalOpportunityCaptureActive.store(true, std::memory_order_release);
                {
                    // B3-I5B。transitionのauthorityはこのstate machineだけである。
                    // capture epochとrender threadをここでfreezeする。
                    std::lock_guard<std::mutex> lock(state_->formalOpportunityMutex);
                    const std::uint64_t captureEpoch =
                        state_->nativePresentHook ? state_->nativePresentHook->captureEpoch() : 0;
                    if (!state_->formalPrerollTransition.begin(
                            captureEpoch, static_cast<std::uint32_t>(GetCurrentThreadId()),
                            gpu::qpcTicks(),
                            static_cast<long long>(gpu::qpcFrequency()) *
                                kPrerollTransitionTimeoutSeconds)) {
                        fail(std::string("P2-D5-2 B3-I5B preroll transitionを開始できません: ") +
                             gpu::prerollTransitionErrorName(
                                 state_->formalPrerollTransition.error()));
                        return true;
                    }
                }
            }
            state_->nativePresentEnvelopeStarted.store(true, std::memory_order_release);
        }
        if (state_->nativePresentEnvelopeStopRequested.exchange(false, std::memory_order_acq_rel)) {
            if (state_->nativePresentCaptureActive.exchange(false, std::memory_order_acq_rel)) {
                std::string hookError;
                if (!state_->nativePresentHook ||
                    !state_->nativePresentHook->endCapture(hookError) ||
                    !state_->nativePresentHook->captureEnvelopeTransportValid()) {
                    fail(hookError.empty() ? "native Present capture envelope authorityが不成立です"
                                           : hookError);
                }
            }
            state_->nativePresentEnvelopeCloseQpc.store(gpu::qpcTicks(), std::memory_order_release);
            state_->nativePresentEnvelopeStopped.store(true, std::memory_order_release);
            // GUI controllerがworker停止とteardown要求をpublishするまで、Presentを伴わない
            // 次callbackをrender threadから確実に予約する。
            update();
            return true;
        }
        if (state_->measurementResetRequested.exchange(false, std::memory_order_acq_rel)) {
            state_->playbackSchedulerEnabled.store(false, std::memory_order_release);
            state_->requestedOutput.store(-1, std::memory_order_release);
            state_->measurementIntervalActive.store(false, std::memory_order_release);
            state_->presentationCaptureActive.store(false, std::memory_order_release);
            state_->measurementResetCaptured.store(true, std::memory_order_release);
            return true;
        }
        const bool intervalEnded =
            state_->measurementIntervalActive.load(std::memory_order_acquire) &&
            callbackBegin >= state_->measurementEndQpc.load(std::memory_order_acquire);
        // flagとそのpublication recordを同じlock下で受け取る。
        const StopRequestConsumption stopConsumption = consumeStopRequest(*state_);
        const bool stopRequested = stopConsumption.requested;
        if (intervalEnded || stopRequested) {
            // W4-C3 amend 4。planned window endはここが publication site。
            // explicit stopはcontroller siteでclaim済みなので再claimしない。
            // explicit stopはpublication siteで確定したclaim recordをexactに引き継ぐ。
            StopArbitration cause = StopArbitration::PlannedWindowEnd;
            StopClaimResult claim;
            bool claimFromPublicationRecord = false;
            bool claimRecorded = false;
            if (intervalEnded) {
                claim = claimStopCause(*state_, StopArbitration::PlannedWindowEnd);
                claimRecorded = true;
            } else {
                cause = StopArbitration::ExplicitStop;
                const StopPublicationRecord published = stopConsumption.record;
                if (published.valid) {
                    cause = published.claimed;
                    claim.previous = published.previous;
                    claim.succeeded = published.succeeded;
                    claim.publishSerial = published.publishSerial;
                    claimFromPublicationRecord = true;
                    claimRecorded = true;
                }
            }
            finishMeasurement(callbackBegin, cause, claim, {}, claimFromPublicationRecord,
                              claimRecorded);
            return true;
        }
        if (state_->measurementStartRequested.exchange(false, std::memory_order_acq_rel)) {
            BoundarySwapAttributionEvent measurementStartEvent;
            measurementStartEvent.kind = BoundarySwapEventKind::MeasurementStartConsumed;
            measurementStartEvent.qpc = callbackBegin;
            measurementStartEvent.threadId = static_cast<std::uint32_t>(GetCurrentThreadId());
            measurementStartEvent.phase = "RENDER_MEASUREMENT_START";
            state_->recordBoundarySwapAttribution(measurementStartEvent);
            measurementStartPending_ = true;
            // admission closeはscheduler closeではない。新規FOREIGN reservationだけを止め、
            // 既に発行済みのFOREIGN transactionはそのままdrainさせる。
            if (state_->formalOpportunitySchedulerEnabled.load(std::memory_order_acquire) &&
                !requestPrerollAdmissionClose(callbackBegin))
                return true;
        }
        if (measurementStartPending_) {
            if (state_->formalOpportunitySchedulerEnabled.load(std::memory_order_acquire)) {
                const auto progress = advancePrerollTransition(callbackBegin);
                if (progress == PrerollTransitionProgress::Fatal)
                    return true;
                if (progress == PrerollTransitionProgress::Waiting) {
                    // handshake waitはcanonical measurement windowの外である。
                    // 未成立の間はcurrent intentを発行せず、次callbackを予約して待つ。
                    update();
                    return true;
                }
            }
            measurementStartPending_ = false;
            const long long duration =
                state_->measurementDurationQpc.load(std::memory_order_acquire);
            if (state_->formalOpportunitySchedulerEnabled.load(std::memory_order_acquire)) {
                // preroll producerのcloseはdrain stepで完了している。quiescence ack後に
                // current required queueをinitializeするだけで、issuanceはまだ開かない。
                {
                    std::lock_guard<std::mutex> lock(state_->formalOpportunityMutex);
                    if (!state_->formalPrerollTransition.startCurrentRequiredQueue()) {
                        fail(std::string("P2-D5-2 B3-I5B current queue start拒否: ") +
                             gpu::prerollTransitionErrorName(
                                 state_->formalPrerollTransition.error()));
                        return true;
                    }
                }
                if (!startFormalOpportunityScheduler()) {
                    fail("P2-D5-2 opportunity schedulerを開始できません");
                    return true;
                }
                recordPrerollTransitionEvent(BoundarySwapEventKind::RequiredQueueStarted,
                                             "RENDER_CURRENT_QUEUE_START");
                state_->formalOpportunityPresentedFrame.store(-1, std::memory_order_relaxed);
                state_->formalOpportunitySwapOrdinal.store(0, std::memory_order_relaxed);
                state_->formalOpportunityTrueDropCount.store(0, std::memory_order_relaxed);
                state_->formalDuplicateTransportSuppressedCount.store(0, std::memory_order_relaxed);
                state_->formalOutsideRequiredTransportSuppressedCount.store(
                    0, std::memory_order_relaxed);
                state_->diagnosticSyntheticDeadlineDropCount.store(0, std::memory_order_relaxed);
                state_->formalOpportunityDomainReached.store(false, std::memory_order_relaxed);
                formalOpportunityRenderOrdinal_ = 0;
            }
            // B3-I5B amendment 2。canonical measurement start authorityは
            // quiescence ack -> current required queue start -> current formal scheduler準備
            // がすべて成功した後、armMeasurement()直前のこの1点でsampleする。
            // current queue initializationはissuance closedのままwindow外で完了している。
            const long long measurementArmQpc = gpu::qpcTicks();
            scheduler_.start(measurementArmQpc, static_cast<long long>(gpu::qpcFrequency()));
            schedulerStarted_ = true;
            // W4-C3。measurement-start authorityが成立したこの一点でalternative
            // publication stateをsnapshotする。controllerのreset直後では撮らない。
            state_->measurementStartArbitrationState.store(
                state_->stopArbitration.load(std::memory_order_seq_cst), std::memory_order_seq_cst);
            state_->measurementStartExplicitStopPublishSerial.store(
                state_->explicitStopPublishSerial.load(std::memory_order_seq_cst),
                std::memory_order_seq_cst);
            state_->measurementStartFatalPublishSerial.store(
                state_->fatalPublishSerial.load(std::memory_order_seq_cst),
                std::memory_order_seq_cst);
            state_->stopWitnessCaptured.store(false, std::memory_order_seq_cst);
            previousSchedulerPhaseCallbackQpc_ = 0;
            schedulerPhaseRingEnabled_ =
                state_->schedulerPhaseRingEnabled.load(std::memory_order_acquire);
            if (schedulerPhaseRingEnabled_)
                state_->schedulerPhaseRing.reset();
            presentationOpportunityEnabled_ =
                state_->presentationOpportunityEnabled.load(std::memory_order_acquire);
            presentationRenderOrdinal_ = 0;
            if (presentationOpportunityEnabled_) {
                state_->presentationOpportunityRing.reset();
                state_->latestCompletedRenderOrdinal.store(-1, std::memory_order_relaxed);
                state_->latestSubmittedRenderOrdinal.store(-1, std::memory_order_relaxed);
                state_->latestSubmittedOutputFrame.store(-1, std::memory_order_relaxed);
                state_->presentationSwapOrdinal.store(0, std::memory_order_relaxed);
                state_->measurementStartQpc.store(measurementArmQpc, std::memory_order_release);
                state_->presentationCaptureActive.store(true, std::memory_order_release);
            }
            state_->measurementEndQpc.store(measurementArmQpc + duration,
                                            std::memory_order_release);
            if (state_->formalOpportunitySchedulerEnabled.load(std::memory_order_acquire)) {
                {
                    // canonical start/end authorityをfreezeしてからissuance gateを開く。
                    std::lock_guard<std::mutex> lock(state_->formalOpportunityMutex);
                    if (!state_->formalPrerollTransition.armMeasurement(
                            measurementArmQpc, measurementArmQpc + duration)) {
                        fail(std::string("P2-D5-2 B3-I5B measurement arm拒否: ") +
                             gpu::prerollTransitionErrorName(
                                 state_->formalPrerollTransition.error()));
                        return true;
                    }
                }
                recordPrerollTransitionEvent(BoundarySwapEventKind::MeasurementArmed,
                                             "RENDER_MEASUREMENT_ARM");
            }
            if (state_->nativePresentHookEnabled.load(std::memory_order_acquire)) {
                if (state_->nativePresentCaptureEnvelopeEnabled.load(std::memory_order_acquire)) {
                    if (!state_->nativePresentCaptureActive.load(std::memory_order_acquire)) {
                        fail("native Present capture envelopeがmeasurement arm前に開いていません");
                        return true;
                    }
                } else {
                    std::string hookError;
                    state_->nativePresentTokenSetFailureCount.store(0, std::memory_order_relaxed);
                    if (!state_->nativePresentHook ||
                        !state_->nativePresentHook->beginCapture(hookError)) {
                        fail(hookError.empty() ? "native Present hookを開始できません" : hookError);
                        return true;
                    }
                    state_->nativePresentCaptureActive.store(true, std::memory_order_release);
                }
            }
            state_->measurementIntervalActive.store(true, std::memory_order_release);
            if (state_->diagnosticCase.load(std::memory_order_acquire) !=
                CompositorDiagnosticCase::None) {
                {
                    std::lock_guard<std::mutex> lock(state_->diagnosticTimingMutex);
                    state_->diagnosticRenderSamples.clear();
                    state_->diagnosticRenderCallbackIntervalUs.clear();
                }
                previousDiagnosticCallbackQpc_ = measurementArmQpc;
                state_->device.lock().beginDiagnostics();
                state_->diagnosticTimingEnabled.store(true, std::memory_order_release);
            }
            {
                std::lock_guard<std::mutex> lock(state_->measurementMutex);
                state_->measurementStart = measurementCounters();
                state_->measurementStart.qpc = measurementArmQpc;
            }
            if (state_->formalOpportunitySchedulerEnabled.load(std::memory_order_acquire)) {
                {
                    std::lock_guard<std::mutex> lock(state_->formalOpportunityMutex);
                    if (!state_->formalPrerollTransition.openCurrentIssuanceGate()) {
                        fail(std::string("P2-D5-2 B3-I5B issuance gate拒否: ") +
                             gpu::prerollTransitionErrorName(
                                 state_->formalPrerollTransition.error()));
                        return true;
                    }
                }
                state_->formalOpportunityCaptureActive.store(true, std::memory_order_release);
                recordPrerollTransitionEvent(BoundarySwapEventKind::CurrentIssuanceOpened,
                                             "RENDER_CURRENT_ISSUANCE_OPEN");
            }
            state_->playbackSchedulerEnabled.store(true, std::memory_order_release);
            state_->measurementStartCaptured.store(true, std::memory_order_release);
        }
        return false;
    }

    MvmNativePresentCompositionToken lastNativePresentToken_{};
    std::uint64_t nativePresentTokenSerial_ = 0;

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
        // W4-C3 amend 4。fatal latchも外部可視なside effectより前にclaimする。
        claimStopCause(*state_, StopArbitration::Fatal);
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

    void releaseDiagnosticContext() {
        if (nativeContext1_)
            nativeContext1_->Release();
        nativeContext1_ = nullptr;
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
                state_->teardownDiagnosticStage.store(
                    RenderTeardownDiagnosticStage::WorkerJoinPending, std::memory_order_release);
                state_->lifecycleOrderViolationCount.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            if (state_->transitionProbeReady.load(std::memory_order_acquire)) {
                if (!state_->transitionProbeReadback.beginDrain(5000, err)) {
                    fail(err);
                    teardownStage_ = TeardownStage::Failed;
                    state_->teardownDiagnosticStage.store(RenderTeardownDiagnosticStage::Failed,
                                                          std::memory_order_release);
                    return false;
                }
                teardownStage_ = TeardownStage::ProbeDrain;
                state_->teardownDiagnosticStage.store(RenderTeardownDiagnosticStage::ProbeDrain,
                                                      std::memory_order_release);
            } else {
                if (!state_->compositor.beginShutdown(10000, err)) {
                    fail(err);
                    teardownStage_ = TeardownStage::Failed;
                    state_->teardownDiagnosticStage.store(RenderTeardownDiagnosticStage::Failed,
                                                          std::memory_order_release);
                    return false;
                }
                teardownStage_ = TeardownStage::CompositorDrain;
                state_->teardownDiagnosticStage.store(
                    RenderTeardownDiagnosticStage::CompositorDrain, std::memory_order_release);
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
                state_->teardownDiagnosticStage.store(RenderTeardownDiagnosticStage::Failed,
                                                      std::memory_order_release);
                return false;
            }
            state_->transitionProbeReady.store(false, std::memory_order_release);
            state_->transitionProbeReadback.release();
            if (!state_->compositor.beginShutdown(10000, err)) {
                fail(err);
                teardownStage_ = TeardownStage::Failed;
                state_->teardownDiagnosticStage.store(RenderTeardownDiagnosticStage::Failed,
                                                      std::memory_order_release);
                return false;
            }
            teardownStage_ = TeardownStage::CompositorDrain;
            state_->teardownDiagnosticStage.store(RenderTeardownDiagnosticStage::CompositorDrain,
                                                  std::memory_order_release);
        }

        if (teardownStage_ == TeardownStage::CompositorDrain) {
            const auto status = state_->compositor.pollShutdown(err);
            if (status == gpu::GpuCompositorShutdownStatus::Pending)
                return true;
            if (status == gpu::GpuCompositorShutdownStatus::Failed) {
                fail(err);
                teardownStage_ = TeardownStage::Failed;
                state_->teardownDiagnosticStage.store(RenderTeardownDiagnosticStage::Failed,
                                                      std::memory_order_release);
                return false;
            }
        }
        releaseRtv();
        releaseDiagnosticContext();
        state_->device.release();
        nativeDevice_ = nativeContext_ = nullptr;
        state_->deviceReady.store(false, std::memory_order_release);
        state_->teardownComplete.store(true, std::memory_order_release);
        teardownStage_ = TeardownStage::Complete;
        state_->teardownDiagnosticStage.store(RenderTeardownDiagnosticStage::Complete,
                                              std::memory_order_release);
        return false;
    }

    std::shared_ptr<CompositorSpikeState> state_;
    gpu::GpuCompletionBackend backend_;
    void* nativeDevice_ = nullptr;
    void* nativeContext_ = nullptr;
    ID3D11DeviceContext1* nativeContext1_ = nullptr;
    ID3D11Texture2D* rtvTexture_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    gpu::OutputScheduler60Hz scheduler_;
    bool schedulerStarted_ = false;
    long long previousDiagnosticCallbackQpc_ = 0;
    long long previousSchedulerPhaseCallbackQpc_ = 0;
    bool schedulerPhaseRingEnabled_ = false;
    bool presentationOpportunityEnabled_ = false;
    long long presentationRenderOrdinal_ = 0;
    long long formalOpportunityRenderOrdinal_ = 0;
    bool measurementStartPending_ = false;
    TeardownStage teardownStage_ = TeardownStage::NotStarted;
};

} // namespace

CompositorRhiItem::CompositorRhiItem(QQuickItem* parent)
    : QQuickRhiItem(parent), state_(std::make_shared<CompositorSpikeState>()) {
    setMirrorVertically(true);
}

void CompositorRhiItem::requestTeardown() {
    state_->teardownDiagnosticStage.store(RenderTeardownDiagnosticStage::Requested,
                                          std::memory_order_release);
    state_->teardownRequested.store(true, std::memory_order_release);
    update();
}

void CompositorRhiItem::recordFrameSwapped() {
    // F3-C3-A3-T2-D1-B0: diagnostic-only。presentation pathのauthorityではない。
    if (state_->eligibilityPreflightRequested.load(std::memory_order_acquire) &&
        !state_->eligibilityPreflightCaptured.load(std::memory_order_acquire)) {
        auto hook = state_->nativePresentHook;
        const std::uint64_t identity = hook ? hook->latestSwapchainIdentity() : 0;
        if (identity != 0) {
            auto preflight = capturePresentationEligibilityPreflight(
                identity,
                state_->nativeDevicePointer.load(std::memory_order_relaxed) != 0
                    ? reinterpret_cast<void*>(
                          state_->nativeDevicePointer.load(std::memory_order_relaxed))
                    : nullptr,
                reinterpret_cast<void*>(
                    state_->eligibilityPreflightWindow.load(std::memory_order_relaxed)));
            {
                std::lock_guard<std::mutex> lock(state_->eligibilityPreflightMutex);
                state_->eligibilityPreflight = preflight;
            }
            state_->eligibilityPreflightCaptured.store(true, std::memory_order_release);
        }
    }
    const auto failQualifiedJoin = [&](const std::string& reason) {
        claimStopCause(*state_, StopArbitration::Fatal);
        {
            std::lock_guard<std::mutex> lock(state_->errorMutex);
            state_->fatalReason = "P2-D5-2 B3-I0 exact qualified commit失敗: " + reason;
        }
        state_->fatal.store(true, std::memory_order_release);
    };

    MvmNativePresentFrameSwappedReceipt receipt;
    MvmNativePresentRecord nativeRecord;
    bool exactReceiptAvailable = false;
    const bool formalEnvelopeActive =
        state_->formalOpportunitySchedulerEnabled.load(std::memory_order_acquire) &&
        state_->nativePresentCaptureActive.load(std::memory_order_acquire);
    if (formalEnvelopeActive) {
        const auto hook = state_->nativePresentHook;
        if (!hook || !hook->takeFrameSwappedReceipt(receipt)) {
            failQualifiedJoin("frameSwapped one-shot receiptがありません");
            return;
        }
        if (!hook->recordForPresentSerial(receipt.presentSerial, nativeRecord)) {
            failQualifiedJoin("receipt present serialに一致するunique native recordがありません");
            return;
        }
        exactReceiptAvailable = true;
    }

    const auto captureJoinFailureAttribution =
        [&](gpu::QualifiedCommitRuntimeAttribution joinAttribution, long long frameSwappedQpc,
            gpu::PrerollTransitionState transitionState) {
            std::lock_guard<std::mutex> attributionLock(state_->compositionTokenAttributionMutex);
            if (state_->compositionTokenJoinFailure.captured)
                return;
            auto& failure = state_->compositionTokenJoinFailure;
            failure.captured = true;
            failure.join = joinAttribution;
            failure.nativeRecord = nativeRecord;
            failure.receipt = receipt;
            failure.latestPublication = state_->latestCompositionTokenPublication;
            failure.frameSwappedThreadId = static_cast<std::uint32_t>(GetCurrentThreadId());
            failure.frameSwappedQpc = frameSwappedQpc;
            failure.formalEnvelopeActive = formalEnvelopeActive;
            failure.transitionState = transitionState;
            failure.prerollActive =
                state_->formalOpportunityEnvelopePrerollActive.load(std::memory_order_acquire);
        };

    bool callbackScopeExact = false;
    NativePresentIntentScope callbackScope = NativePresentIntentScope::ForeignPreMeasurement;
    unsigned long long callbackScopeMatchCount = 0;
    if (exactReceiptAvailable && receipt.tokenPresent != 0) {
        std::lock_guard<std::mutex> scopeLock(state_->nativePresentIntentScopeMutex);
        for (const auto& scopeRecord : state_->nativePresentIntentScopeLedger) {
            if (scopeRecord.tokenSerial != receipt.tokenSerial)
                continue;
            ++callbackScopeMatchCount;
            callbackScope = scopeRecord.scope;
        }
        callbackScopeExact = callbackScopeMatchCount == 1;
    }
    // B3-I5B。positional ignore-next-swapは削除済みである。boundary ownershipは
    // quiescence handshakeとexact receipt identityだけが決める。
    const auto recordBoundaryCallback =
        [&](bool activeReservation, const gpu::QualifiedCommitReservation& reservation,
            const gpu::PrerollTransitionSnapshot& transition) {
        BoundarySwapAttributionEvent event;
        event.kind = BoundarySwapEventKind::FrameSwapped;
        event.qpc = gpu::qpcTicks();
        event.threadId = static_cast<std::uint32_t>(GetCurrentThreadId());
        event.phase = "FRAME_SWAPPED_CALLBACK";
        event.transitionStepSerial =
            state_->prerollTransitionStepSerial.load(std::memory_order_seq_cst);
        event.transitionState = transition.state;
        event.transitionError = transition.error;
        event.transitionQuiescent = transition.verdict.quiescent;
        event.receiptObserved = exactReceiptAvailable;
        event.receiptPresentSerial = receipt.presentSerial;
        event.receiptTokenSerial = receipt.tokenSerial;
        event.receiptIntentOrdinal = receipt.intentOrdinal;
        event.receiptIntentOrdinalValid = receipt.intentOrdinalValid != 0;
        event.receiptTokenPresent = receipt.tokenPresent != 0;
        event.receiptSwapchainIdentity = receipt.swapchainIdentity;
        event.receiptHresult = receipt.hresult;
        event.presentEnterQpc = nativeRecord.presentEnterQpc;
        event.presentReturnQpc = nativeRecord.presentReturnQpc;
        event.presentThreadId = nativeRecord.threadId;
        event.intentScopeExact = callbackScopeExact;
        event.intentScope = callbackScope;
        event.intentScopeMatchCount = callbackScopeMatchCount;
        event.activeReservation = activeReservation;
        event.reservation = reservation;
        state_->recordBoundarySwapAttribution(event);
    };
    gpu::PrerollTransitionState callbackTransitionState = gpu::PrerollTransitionState::Open;
    {
        std::lock_guard<std::mutex> lock(state_->formalOpportunityMutex);
        auto& handshake = state_->formalPrerollTransition;
        const auto transition = handshake.snapshot();
        callbackTransitionState = transition.state;
        const bool activeReservation = state_->formalQualifiedCommitJoin.hasActiveReservation();
        const auto reservation = state_->formalQualifiedCommitJoin.reservation();
        recordBoundaryCallback(activeReservation, reservation, transition);
        // exact boundary reservationはdrain中のactive FOREIGN transactionにだけ束縛する。
        // 完了済みFOREIGN PresentへもCURRENT Presentへもownerを後付けしない。
        const bool inDrain = transition.state == gpu::PrerollTransitionState::DrainRequested ||
                             transition.state == gpu::PrerollTransitionState::Draining;
        const bool postQuiescence = transition.state == gpu::PrerollTransitionState::Quiescent ||
                                    transition.state == gpu::PrerollTransitionState::CurrentReady ||
                                    transition.state ==
                                        gpu::PrerollTransitionState::MeasurementArmed ||
                                    transition.state == gpu::PrerollTransitionState::CurrentRunning;
        // quiescence ack後にFOREIGN callbackは残らない。残っていたなら
        // active reservationを破棄する前にPROTOCOL_FATALで停止する。
        if (postQuiescence && callbackScopeExact &&
            callbackScope == NativePresentIntentScope::ForeignPreMeasurement) {
            auto attribution = state_->formalQualifiedCommitJoin.runtimeAttribution();
            attribution.failurePhase = gpu::QualifiedCommitFailurePhase::PreJoinBoundarySwap;
            attribution.failurePredicate =
                gpu::QualifiedCommitFailurePredicate::BoundarySwapRequiresNoActiveReservation;
            captureJoinFailureAttribution(attribution, gpu::qpcTicks(), transition.state);
            failQualifiedJoin("quiescence ack後にFOREIGN boundary swapが到達しました");
            return;
        }
        if (inDrain && activeReservation && !transition.boundaryOwnerBound && callbackScopeExact &&
            callbackScope == NativePresentIntentScope::ForeignPreMeasurement &&
            !handshake.bindBoundaryOwner({reservation.reservationId, reservation.intentOrdinal,
                                          reservation.tokenSerial, true, true})) {
            failQualifiedJoin(std::string("P2-D5-2 B3-I5B boundary owner束縛失敗: ") +
                              gpu::prerollTransitionErrorName(handshake.error()));
            return;
        }
    }
    if (!state_->formalOpportunityDomainReached.load(std::memory_order_acquire) &&
        state_->formalOpportunityCaptureActive.load(std::memory_order_acquire)) {
        if (!exactReceiptAvailable) {
            failQualifiedJoin("formal frameSwappedにexact receiptがありません");
            return;
        }
        const long long swapQpc = gpu::qpcTicks();
        const long long swapOrdinal =
            state_->formalOpportunitySwapOrdinal.fetch_add(1, std::memory_order_relaxed);
        bool committed = false;
        gpu::PresentationOpportunityError error = gpu::PresentationOpportunityError::None;
        {
            std::lock_guard<std::mutex> lock(state_->formalOpportunityMutex);
            const auto reservation = state_->formalQualifiedCommitJoin.reservation();
            const gpu::QualifiedNativePresentEvidence nativeEvidence{
                true,
                nativeRecord.presentSerial,
                nativeRecord.swapchainIdentity,
                nativeRecord.hresult,
                nativeRecord.tokenPresent != 0,
                nativeRecord.token.tokenSerial,
                static_cast<long long>(nativeRecord.intentOrdinal),
                nativeRecord.intentOrdinalValid != 0};
            const gpu::QualifiedFrameSwappedEvidence swapEvidence{
                true,
                reservation.reservationId,
                static_cast<long long>(receipt.intentOrdinal),
                receipt.tokenSerial,
                receipt.presentSerial,
                receipt.swapchainIdentity,
                receipt.hresult};
            const bool nativeBound =
                state_->formalQualifiedCommitJoin.bindNativePresent(nativeEvidence);
            const auto qualified =
                nativeBound ? state_->formalQualifiedCommitJoin.commitFrameSwapped(swapEvidence)
                            : gpu::QualifiedCommitResult::Rejected;
            if (qualified == gpu::QualifiedCommitResult::QualifiedCommit) {
                committed = state_->formalOpportunityScheduler.commitQualifiedPresent(
                                reservation.reservationId, reservation.intentOrdinal) &&
                            state_->formalOpportunityScheduler.commitSwap(
                                swapQpc, capturePresentationAuthority(state_), swapOrdinal);
            }
            error = state_->formalOpportunityScheduler.error();
            if (committed && callbackScopeExact &&
                callbackScope == NativePresentIntentScope::ForeignPreMeasurement &&
                !state_->formalPrerollTransition.noteForeignTransactionTerminal(swapQpc)) {
                failQualifiedJoin(
                    std::string("P2-D5-2 B3-I5B FOREIGN transaction終端記録失敗: ") +
                    gpu::prerollTransitionErrorName(state_->formalPrerollTransition.error()));
                return;
            }
            if (qualified != gpu::QualifiedCommitResult::QualifiedCommit) {
                captureJoinFailureAttribution(
                    state_->formalQualifiedCommitJoin.runtimeAttribution(), swapQpc,
                    callbackTransitionState);
                failQualifiedJoin(
                    gpu::qualifiedCommitErrorName(state_->formalQualifiedCommitJoin.error()));
                return;
            }
        }
        if (!committed) {
            // W4-C3 amend 4。fail()を経由しないfatal latch siteも同じhelperを通す。
            claimStopCause(*state_, StopArbitration::Fatal);
            {
                std::lock_guard<std::mutex> lock(state_->errorMutex);
                state_->fatalReason = std::string("P2-D5-2 render↔swap authority失敗: ") +
                                      gpu::presentationOpportunityErrorName(error);
            }
            state_->fatal.store(true, std::memory_order_release);
        }
    }
    if (!state_->presentationCaptureActive.load(std::memory_order_acquire))
        return;
    const long long completed =
        state_->latestCompletedRenderOrdinal.load(std::memory_order_acquire);
    const long long submitted =
        state_->latestSubmittedRenderOrdinal.load(std::memory_order_acquire);
    const long long frame = state_->latestSubmittedOutputFrame.load(std::memory_order_relaxed);
    const long long ordinal =
        state_->presentationSwapOrdinal.fetch_add(1, std::memory_order_relaxed);
    state_->presentationOpportunityRing.captureSwap(
        {gpu::qpcTicks(), ordinal, completed, submitted, frame});
}

QQuickRhiItemRenderer* CompositorRhiItem::createRenderer() {
    return new CompositorRhiRenderer(state_, preferredCompletion_);
}

} // namespace mvm::app

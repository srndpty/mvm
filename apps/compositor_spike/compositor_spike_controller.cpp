#include "compositor_spike_controller.h"

#include "media/gpu_preview/measurement_preroll.h"
#include "media/gpu_preview/physical_vblank_domain.h"
#include "media/gpu_preview/qpc_clock.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <dwmapi.h>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>
#include <QSaveFile>

namespace mvm::app {
namespace {
const char* teardownDiagnosticStageName(RenderTeardownDiagnosticStage stage) {
    switch (stage) {
    case RenderTeardownDiagnosticStage::NotRequested:
        return "NOT_REQUESTED";
    case RenderTeardownDiagnosticStage::Requested:
        return "REQUESTED";
    case RenderTeardownDiagnosticStage::RenderCallbackObserved:
        return "RENDER_CALLBACK_OBSERVED";
    case RenderTeardownDiagnosticStage::WorkerJoinPending:
        return "WORKER_JOIN_PENDING";
    case RenderTeardownDiagnosticStage::ProbeDrain:
        return "PROBE_DRAIN";
    case RenderTeardownDiagnosticStage::CompositorDrain:
        return "COMPOSITOR_DRAIN";
    case RenderTeardownDiagnosticStage::Failed:
        return "FAILED";
    case RenderTeardownDiagnosticStage::Complete:
        return "COMPLETE";
    }
    return "UNKNOWN";
}

const char* terminalRenderExitDiagnosticStageName(TerminalRenderExitDiagnosticStage stage) {
    switch (stage) {
    case TerminalRenderExitDiagnosticStage::NotObserved:
        return "NOT_OBSERVED";
    case TerminalRenderExitDiagnosticStage::FinishMeasurementEntered:
        return "FINISH_MEASUREMENT_ENTERED";
    case TerminalRenderExitDiagnosticStage::FinishMeasurementReturned:
        return "FINISH_MEASUREMENT_RETURNED";
    case TerminalRenderExitDiagnosticStage::PresentationCaptureDestructorComplete:
        return "PRESENTATION_CAPTURE_DESTRUCTOR_COMPLETE";
    case TerminalRenderExitDiagnosticStage::NativeTokenDestructorEntered:
        return "NATIVE_TOKEN_DESTRUCTOR_ENTERED";
    case TerminalRenderExitDiagnosticStage::NativeTokenDestructorComplete:
        return "NATIVE_TOKEN_DESTRUCTOR_COMPLETE";
    case TerminalRenderExitDiagnosticStage::RenderCallbackExited:
        return "RENDER_CALLBACK_EXITED";
    }
    return "UNKNOWN";
}

DwmPresentationTimingSnapshot captureDwmTiming(QQuickWindow* window) {
    DwmPresentationTimingSnapshot result;
    if (!window)
        return result;
    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof(timing);
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    if (FAILED(DwmGetCompositionTimingInfo(hwnd, &timing))) {
        timing = {};
        timing.cbSize = sizeof(timing);
        if (FAILED(DwmGetCompositionTimingInfo(nullptr, &timing)))
            return result;
    }
    result.available = true;
    result.refreshNumerator = timing.rateRefresh.uiNumerator;
    result.refreshDenominator = timing.rateRefresh.uiDenominator;
    result.composeNumerator = timing.rateCompose.uiNumerator;
    result.composeDenominator = timing.rateCompose.uiDenominator;
    result.qpcVBlank = static_cast<long long>(timing.qpcVBlank);
    result.qpcRefreshPeriod = static_cast<long long>(timing.qpcRefreshPeriod);
    result.refreshCount = timing.cRefresh;
    result.frameDisplayedCount = timing.cFrameDisplayed;
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) ==
        ERROR_SUCCESS) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                               modes.data(), nullptr) == ERROR_SUCCESS) {
            paths.resize(pathCount);
            result.displayConfigActivePathCount = pathCount;
            MONITORINFOEXW monitorInfo{};
            monitorInfo.cbSize = sizeof(monitorInfo);
            const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            const QString monitorDevice = GetMonitorInfoW(monitor, &monitorInfo)
                                              ? QString::fromWCharArray(monitorInfo.szDevice)
                                              : QString{};
            for (const auto& path : paths) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
                sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                sourceName.header.size = sizeof(sourceName);
                sourceName.header.adapterId = path.sourceInfo.adapterId;
                sourceName.header.id = path.sourceInfo.id;
                const LONG sourceNameStatus = DisplayConfigGetDeviceInfo(&sourceName.header);
                const QString gdiName = QString::fromWCharArray(sourceName.viewGdiDeviceName);
                if (sourceNameStatus != ERROR_SUCCESS ||
                    gdiName.compare(monitorDevice, Qt::CaseInsensitive) != 0)
                    continue;
                result.displayConfigAvailable = path.targetInfo.refreshRate.Numerator > 0 &&
                                                path.targetInfo.refreshRate.Denominator > 0;
                result.displayRefreshNumerator = path.targetInfo.refreshRate.Numerator;
                result.displayRefreshDenominator = path.targetInfo.refreshRate.Denominator;
                break;
            }
            if (!result.displayConfigAvailable && paths.size() == 1) {
                const auto& refresh = paths.front().targetInfo.refreshRate;
                result.displayConfigAvailable = refresh.Numerator > 0 && refresh.Denominator > 0;
                result.displayConfigSingleActiveFallback = result.displayConfigAvailable;
                result.displayRefreshNumerator = refresh.Numerator;
                result.displayRefreshDenominator = refresh.Denominator;
            }
        }
    }
    return result;
}

bool validDwmAuthority(const DwmPresentationTimingSnapshot& value) {
    return value.available && value.displayConfigAvailable && value.displayRefreshNumerator > 0 &&
           value.displayRefreshDenominator > 0 && value.qpcVBlank > 0;
}

bool sameDwmAuthority(const DwmPresentationTimingSnapshot& start,
                      const DwmPresentationTimingSnapshot& current) {
    return validDwmAuthority(start) && validDwmAuthority(current) &&
           start.displayRefreshNumerator == current.displayRefreshNumerator &&
           start.displayRefreshDenominator == current.displayRefreshDenominator &&
           current.qpcVBlank >= start.qpcVBlank && current.refreshCount >= start.refreshCount;
}

QJsonObject dwmTimingJson(const DwmPresentationTimingSnapshot& value) {
    return {{"available", value.available},
            {"refresh_numerator", static_cast<qint64>(value.refreshNumerator)},
            {"refresh_denominator", static_cast<qint64>(value.refreshDenominator)},
            {"compose_numerator", static_cast<qint64>(value.composeNumerator)},
            {"compose_denominator", static_cast<qint64>(value.composeDenominator)},
            {"display_config_available", value.displayConfigAvailable},
            {"display_config_single_active_fallback", value.displayConfigSingleActiveFallback},
            {"display_config_active_path_count",
             static_cast<qint64>(value.displayConfigActivePathCount)},
            {"display_refresh_numerator", static_cast<qint64>(value.displayRefreshNumerator)},
            {"display_refresh_denominator", static_cast<qint64>(value.displayRefreshDenominator)},
            {"qpc_vblank", value.qpcVBlank},
            {"qpc_refresh_period", value.qpcRefreshPeriod},
            {"refresh_count", static_cast<qint64>(value.refreshCount)},
            {"frame_displayed_count", static_cast<qint64>(value.frameDisplayedCount)}};
}

QJsonObject windowOutputJson(const gpu::WindowOutputIdentity& value) {
    return {{"available", value.available},
            {"monitor_handle", QString::number(value.monitorHandle)},
            {"output_index", static_cast<qint64>(value.outputIndex)},
            {"adapter_luid_low", static_cast<qint64>(value.adapterLuidLow)},
            {"adapter_luid_high", static_cast<qint64>(value.adapterLuidHigh)},
            {"gdi_device_name", QString::fromStdString(value.gdiDeviceName)},
            {"output_device_name", QString::fromStdString(value.outputDeviceName)},
            {"refresh_numerator", value.refreshNumerator},
            {"refresh_denominator", value.refreshDenominator},
            {"desktop_left", value.desktopLeft},
            {"desktop_top", value.desktopTop},
            {"desktop_right", value.desktopRight},
            {"desktop_bottom", value.desktopBottom}};
}

QJsonObject presentationAuthorityJson(const gpu::PresentationAuthoritySample& value) {
    return {{"available", value.available},
            {"refresh_count", static_cast<qint64>(value.refreshCount)},
            {"qpc_vblank", value.qpcVBlank},
            {"refresh_numerator", value.refreshNumerator},
            {"refresh_denominator", value.refreshDenominator}};
}

QJsonObject
presentationInvocationStateJson(const gpu::PresentationSchedulerInvocationState& value) {
    return {{"started", value.started},
            {"closed", value.closed},
            {"anchored", value.anchored},
            {"origin_refresh_count", static_cast<qint64>(value.originRefreshCount)},
            {"last_finalized_opportunity_ordinal", value.lastFinalizedOpportunityOrdinal},
            {"pending_render", value.pendingRender},
            {"past_source_domain", value.pastSourceDomain}};
}

QJsonObject presentationFirstEventJson(const gpu::PresentationOpportunityFirstEvent& value) {
    return {{"captured", value.captured},
            {"classification", QString::fromLatin1(gpu::presentationOpportunityClassificationName(
                                   value.classification))},
            {"last_finalized_opportunity_ordinal", value.lastFinalizedOpportunityOrdinal},
            {"predicted_opportunity_ordinal", value.predictedOpportunityOrdinal},
            {"actual_opportunity_ordinal", value.actualOpportunityOrdinal},
            {"render_begin_qpc", value.renderBeginQpc},
            {"render_end_qpc", value.renderEndQpc},
            {"presentation_swap_qpc", value.swapQpc},
            {"pre_render_authority", presentationAuthorityJson(value.preRenderAuthority)},
            {"post_swap_authority", presentationAuthorityJson(value.postSwapAuthority)},
            {"predicted_source_frame", value.predictedSourceFrame},
            {"actual_target_source_frame", value.actualTargetFrame},
            {"rendered_source_frame", value.renderedSourceFrame},
            {"render_ordinal", value.renderOrdinal},
            {"swap_ordinal", value.swapOrdinal},
            {"authority_continuous", value.authorityContinuous}};
}

std::vector<gpu::LayerLayout> layoutFor(size_t index) {
    const bool topLeft = index == 1 || index == 2;
    const float opacity = index < 2 ? 0.75f : 0.5f;
    return {{gpu::SourceId{1}, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0f, 0},
            {gpu::SourceId{2},
             {topLeft ? 0.0f : 0.5f, topLeft ? 0.0f : 0.5f, 0.5f, 0.5f},
             {0, 0, 1, 1},
             opacity,
             1}};
}

QJsonArray doubles(const std::vector<double>& values) {
    QJsonArray out;
    for (double value : values)
        out.append(value);
    return out;
}

double nearestRank(const std::vector<double>& sorted, double percentile) {
    if (sorted.empty())
        return -1.0;
    const size_t index =
        static_cast<size_t>(std::ceil(static_cast<double>(sorted.size()) * percentile) - 1.0);
    return sorted[std::min(index, sorted.size() - 1)];
}

QJsonObject distribution(const std::vector<double>& values, const char* valuesField) {
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0.0;
    for (double value : values)
        sum += value;
    QJsonObject result{{"count", static_cast<qint64>(values.size())},
                       {"mean", values.empty() ? -1.0 : sum / static_cast<double>(values.size())},
                       {"p50", nearestRank(sorted, 0.50)},
                       {"p95", nearestRank(sorted, 0.95)},
                       {"p99", nearestRank(sorted, 0.99)},
                       {"max", sorted.empty() ? -1.0 : sorted.back()}};
    result.insert(valuesField, doubles(values));
    return result;
}

QString diagnosticCaseName(CompositorDiagnosticCase value) {
    switch (value) {
    case CompositorDiagnosticCase::SingleDecode:
        return QStringLiteral("a");
    case CompositorDiagnosticCase::PairOnly:
        return QStringLiteral("b");
    case CompositorDiagnosticCase::FixedTextures:
        return QStringLiteral("c");
    case CompositorDiagnosticCase::FullPath:
        return QStringLiteral("d");
    case CompositorDiagnosticCase::None:
        return QStringLiteral("none");
    }
    return QStringLiteral("none");
}

QString nativePresentHookModeName(NativePresentHookMode value) {
    switch (value) {
    case NativePresentHookMode::Disabled:
        return QStringLiteral("disabled");
    case NativePresentHookMode::OffControl:
        return QStringLiteral("off");
    case NativePresentHookMode::OnDiagnostic:
        return QStringLiteral("on");
    }
    return QStringLiteral("disabled");
}

QString seekDiagnosticText(const char* source, const gpu::SourceDecodeWorker& worker) {
    const auto value = worker.seekDiagnosticSnapshot();
    return QStringLiteral("source=%1 phase=%2 requestId=%3 target=%4 phaseEnterQpc=%5 "
                          "lastProgressQpc=%6 mailbox(stopped=%7 outstanding=%8 pending=%9 "
                          "ready=%10 ticket=%11/%12 completion=%13) rejects=%14 mismatch=%15 "
                          "stopSuperseded=%16")
        .arg(QString::fromLatin1(source))
        .arg(QString::fromLatin1(gpu::toString(value.phase)))
        .arg(value.requestId)
        .arg(value.targetFrame)
        .arg(value.phaseEnterQpc)
        .arg(value.lastProgressQpc)
        .arg(value.mailbox.stopped)
        .arg(value.mailbox.outstanding)
        .arg(value.mailbox.pending)
        .arg(value.mailbox.completionReady)
        .arg(value.mailbox.currentTicket.requestId)
        .arg(value.mailbox.currentTicket.targetFrame)
        .arg(value.mailbox.completionRequestId)
        .arg(value.completionPublishRejectCount)
        .arg(value.completionRequestMismatchCount)
        .arg(value.completionStoppedSupersededCount);
}

QString seekRequestResultName(gpu::SeekRequestResult result) {
    switch (result) {
    case gpu::SeekRequestResult::Accepted:
        return QStringLiteral("Accepted");
    case gpu::SeekRequestResult::RejectedBusy:
        return QStringLiteral("RejectedBusy");
    case gpu::SeekRequestResult::RejectedStopped:
        return QStringLiteral("RejectedStopped");
    case gpu::SeekRequestResult::RejectedInvalid:
        return QStringLiteral("RejectedInvalid");
    }
    return QStringLiteral("Unknown");
}

CompositorMeasurementCounters subtract(const CompositorMeasurementCounters& end,
                                       const CompositorMeasurementCounters& start) {
    CompositorMeasurementCounters out;
#define MVM_DELTA(field) out.field = end.field - start.field
    MVM_DELTA(qpc);
    MVM_DELTA(compositionRequested);
    MVM_DELTA(compositionDrawn);
    MVM_DELTA(gpuSubmission);
    MVM_DELTA(layerDraw);
    MVM_DELTA(logicalClear);
    MVM_DELTA(scheduled);
    MVM_DELTA(displayed);
    MVM_DELTA(dropped);
    MVM_DELTA(missingPair);
    MVM_DELTA(sourceAEof);
    MVM_DELTA(sourceBEof);
    MVM_DELTA(dropSchedulerDeadline);
    MVM_DELTA(dropMissingSourceA);
    MVM_DELTA(dropMissingSourceB);
    MVM_DELTA(dropMissingBoth);
    MVM_DELTA(dropStaleGeneration);
    MVM_DELTA(dropFutureGeneration);
    MVM_DELTA(dropStaleCompositionEpoch);
    MVM_DELTA(dropRenderFailure);
    MVM_DELTA(presentCallback);
    MVM_DELTA(repeatedPresent);
    MVM_DELTA(partialGpuIssueFailure);
    MVM_DELTA(completionPollFailure);
    MVM_DELTA(untrackedSubmission);
#undef MVM_DELTA
    return out;
}

bool nonnegative(const CompositorMeasurementCounters& value) {
#define MVM_NONNEGATIVE(field) (value.field >= 0)
    return MVM_NONNEGATIVE(qpc) && MVM_NONNEGATIVE(compositionRequested) &&
           MVM_NONNEGATIVE(compositionDrawn) && MVM_NONNEGATIVE(gpuSubmission) &&
           MVM_NONNEGATIVE(layerDraw) && MVM_NONNEGATIVE(logicalClear) &&
           MVM_NONNEGATIVE(scheduled) && MVM_NONNEGATIVE(displayed) && MVM_NONNEGATIVE(dropped) &&
           MVM_NONNEGATIVE(missingPair) && MVM_NONNEGATIVE(sourceAEof) &&
           MVM_NONNEGATIVE(sourceBEof) && MVM_NONNEGATIVE(dropSchedulerDeadline) &&
           MVM_NONNEGATIVE(dropMissingSourceA) && MVM_NONNEGATIVE(dropMissingSourceB) &&
           MVM_NONNEGATIVE(dropMissingBoth) && MVM_NONNEGATIVE(dropStaleGeneration) &&
           MVM_NONNEGATIVE(dropFutureGeneration) && MVM_NONNEGATIVE(dropStaleCompositionEpoch) &&
           MVM_NONNEGATIVE(dropRenderFailure) && MVM_NONNEGATIVE(presentCallback) &&
           MVM_NONNEGATIVE(repeatedPresent) && MVM_NONNEGATIVE(partialGpuIssueFailure) &&
           MVM_NONNEGATIVE(completionPollFailure) && MVM_NONNEGATIVE(untrackedSubmission);
#undef MVM_NONNEGATIVE
}
} // namespace

CompositorSpikeController::CompositorSpikeController(CompositorSpikeConfig config, QObject* parent)
    : QObject(parent), config_(std::move(config)) {
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(1);
    connect(&timer_, &QTimer::timeout, this, &CompositorSpikeController::tick);
}

void CompositorSpikeController::attach(CompositorRhiItem* item) {
    item_ = item;
    state_ = item->state();
    state_->diagnosticCase.store(config_.diagnosticCase, std::memory_order_release);
    state_->schedulerPhaseRingEnabled.store(config_.schedulerPhaseRing, std::memory_order_release);
    state_->presentationOpportunityEnabled.store(config_.presentationOpportunityRing,
                                                 std::memory_order_release);
    state_->diagnosticTargetPixelToggle.store(config_.diagnosticTargetPixelToggle,
                                              std::memory_order_release);
    // F3-C3-A3-T2-D1-B0 diagnostic-only preflight。production semanticsは変えない。
    state_->eligibilityPreflightRequested.store(true, std::memory_order_release);
    if (item->window() != nullptr) {
        state_->eligibilityPreflightWindow.store(
            static_cast<unsigned long long>(item->window()->winId()), std::memory_order_relaxed);
    }
    if (config_.nativePresentHook != NativePresentHookMode::Disabled) {
        auto nativeHook = std::make_shared<NativePresentHook>();
        std::string error;
        if (!nativeHook->load(error)) {
            startupError_ = QString::fromStdString(error);
        } else {
            state_->nativePresentHook = std::move(nativeHook);
            state_->nativePresentHookEnabled.store(config_.nativePresentHook ==
                                                       NativePresentHookMode::OnDiagnostic,
                                                   std::memory_order_release);
        }
    }
    state_->formalOpportunitySchedulerEnabled.store(
        config_.formalPreflight && config_.mode == CompositorMode::Playback &&
            config_.diagnosticCase == CompositorDiagnosticCase::None,
        std::memory_order_release);
    state_->formalSchedulerInvocationLedgerEnabled.store(config_.formalSchedulerInvocationLedger,
                                                         std::memory_order_release);
    state_->nativePresentCaptureEnvelopeEnabled.store(
        config_.formalPreflight && config_.mode == CompositorMode::Playback &&
            config_.diagnosticCase == CompositorDiagnosticCase::None && config_.vblankObserver &&
            config_.nativePresentHook == NativePresentHookMode::OnDiagnostic,
        std::memory_order_release);
    // W2-C1.1。Main.qmlはまだvisible:falseであり、target Presentは始まっていない。
    // この順序でphysical observer/prerollを成立させ、ETW session内のstartup
    // Presented candidateもlower supportから脱落させない。
    if (state_->nativePresentCaptureEnvelopeEnabled.load(std::memory_order_acquire) &&
        !startVBlankObserverWithPreroll()) {
        startupError_ = QStringLiteral("physical VBlank startup supportを開始できません: %1")
                            .arg(QString::fromStdString(vblankObserverError_));
    }
    phaseTimer_.start();
    timer_.start();
}

bool CompositorSpikeController::startWorkers() {
    const bool single = config_.diagnosticCase == CompositorDiagnosticCase::SingleDecode;
    const bool fixed = config_.diagnosticCase == CompositorDiagnosticCase::FixedTextures;
    workerA_ = std::make_shared<gpu::SourceDecodeWorker>(gpu::SourceId{1}, state_->device,
                                                         state_->readbacks, 16);
    if (!single)
        workerB_ = std::make_shared<gpu::SourceDecodeWorker>(gpu::SourceId{2}, state_->device,
                                                             state_->readbacks, 16);
    std::string err;
    if (!workerA_->start(config_.sourceA.toUtf8().constData(), err) ||
        (workerB_ && !workerB_->start(config_.sourceB.toUtf8().constData(), err))) {
        beginShutdown(QString::fromStdString(err), true);
        return false;
    }
    // open時点の設定値ではなく、actual decode textureが出た後のGetDevice結果を照合する。
    double initialAMs = 0;
    double initialBMs = 0;
    if (!workerA_->seekBlocking(0, initialAMs, err) ||
        (workerB_ && !workerB_->seekBlocking(0, initialBMs, err))) {
        beginShutdown(QString::fromStdString(err), true);
        return false;
    }
    const auto a = workerA_->snapshot();
    const auto b = workerB_ ? workerB_->snapshot() : gpu::SourceDecoderSnapshot{};
    sourceAFrameCount_ = a.info.frameCount;
    sourceBFrameCount_ = b.info.frameCount;
    requiredMeasurementFrameCount_ = static_cast<long long>(config_.measureSeconds) * 60;
    sourceCoverageOk_ = sourceAFrameCount_ >= requiredMeasurementFrameCount_ &&
                        (!workerB_ || sourceBFrameCount_ >= requiredMeasurementFrameCount_);
    if (config_.formalPreflight && config_.mode == CompositorMode::Playback &&
        config_.diagnosticCase == CompositorDiagnosticCase::None && !sourceCoverageOk_) {
        beginShutdown(QStringLiteral("Playback測定区間をsourceがcoverageしていません"), true);
        return false;
    }
    if (a.decodeDevicePointer != state_->nativeDevicePointer.load() ||
        (workerB_ && b.decodeDevicePointer != state_->nativeDevicePointer.load()) ||
        !a.adapter.sameAdapterAs(state_->qtAdapter) ||
        (workerB_ && !b.adapter.sameAdapterAs(state_->qtAdapter))) {
        beginShutdown(QStringLiteral("A/B decode texture deviceがQt deviceと一致しません"), true);
        return false;
    }
    if (!single &&
        state_->coordinator.configure(layoutFor(0), {{gpu::SourceId{1}, a.sourceGeneration},
                                                     {gpu::SourceId{2}, b.sourceGeneration}}) !=
            gpu::ConfigureResult::Configured) {
        beginShutdown(QStringLiteral("CompositorCoordinatorを初期化できません"), true);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state_->workerMutex);
        state_->workerA = workerA_;
        state_->workerB = workerB_;
    }
    if (fixed) {
        gpu::DecodedGpuFrame frameA;
        gpu::DecodedGpuFrame frameB;
        if (!workerA_->buffer().takeExact(0, frameA) || !workerB_->buffer().takeExact(0, frameB)) {
            beginShutdown(QStringLiteral("fixed texture診断用frameを取得できません"), true);
            return false;
        }
        state_->diagnosticFixedFrame = {
            0,
            state_->coordinator.compositionEpoch(),
            {{std::move(frameA), {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0f, 0},
             {std::move(frameB), {0.5f, 0.5f, 0.5f, 0.5f}, {0, 0, 1, 1}, 0.75f, 1}}};
    }
    if (config_.testFault == "device_change")
        state_->testDeviceChange.store(true);
    else if (config_.testFault == "completion_fatal")
        state_->compositor.setTestFaults({gpu::GpuCompositorInitializeFault::None, -1, true});
    if (config_.mode == CompositorMode::Seek) {
        const long long limit = std::min(a.info.frameCount, b.info.frameCount);
        if (limit <= 0) {
            beginShutdown(QStringLiteral("seek対象のframe countが不正です"), true);
            return false;
        }
        std::mt19937 rng(config_.seed);
        std::uniform_int_distribution<long long> dist(0, limit - 1);
        for (int i = 0; i < config_.seekCount; ++i)
            seekTargets_.push_back(dist(rng));
        if (config_.diagnosticTiming) {
            state_->device.lock().beginDiagnostics();
            seekLockTimingActive_ = true;
        }
    }
    if (config_.formalPreflight && config_.mode != CompositorMode::Layout) {
        phase_ = Phase::MarkerStart;
    } else if (config_.mode == CompositorMode::Seek) {
        phase_ = Phase::SeekStart;
    } else {
        if (!fixed)
            workerA_->play();
        if (workerB_ && !fixed)
            workerB_->play();
        phase_ = config_.mode == CompositorMode::Layout ? Phase::LayoutStart : Phase::Warmup;
        state_->playbackSchedulerEnabled.store(true, std::memory_order_release);
    }
    phaseTimer_.restart();
    return true;
}

void CompositorSpikeController::startMarkerProbe() {
    if (markerIndex_ >= markerTargets_.size()) {
        if (!resetAfterMarkerPreflight())
            return;
        return;
    }
    const long long target = markerTargets_[markerIndex_];
    double elapsed = 0;
    std::string err;
    if (!workerA_->seekBlocking(target, elapsed, err) ||
        !workerB_->seekBlocking(target, elapsed, err)) {
        beginShutdown(QString::fromStdString(err), true);
        return;
    }
    gpu::DecodedGpuFrame frameA;
    gpu::DecodedGpuFrame frameB;
    if (!workerA_->buffer().takeExact(target, frameA) ||
        !workerB_->buffer().takeExact(target, frameB)) {
        beginShutdown(QStringLiteral("marker preflightのexact frameを取得できません"), true);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state_->markerProbe.mutex);
        state_->markerProbe.done = false;
        state_->markerProbe.expectedFrame = target;
        state_->markerProbe.frameA = std::move(frameA);
        state_->markerProbe.frameB = std::move(frameB);
        state_->markerProbe.error.clear();
        state_->markerProbe.requested = true;
    }
    waitTimer_.restart();
    phase_ = Phase::MarkerWait;
    item_->update();
}

void CompositorSpikeController::pollMarkerProbe() {
    bool done = false;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(state_->markerProbe.mutex);
        done = state_->markerProbe.done;
        error = state_->markerProbe.error;
    }
    if (done) {
        if (!error.empty()) {
            beginShutdown(QString::fromStdString(error), true);
            return;
        }
        ++markerIndex_;
        phase_ = Phase::MarkerStart;
    } else if (waitTimer_.elapsed() >= config_.displayTimeoutMs) {
        beginShutdown(QStringLiteral("marker preflightがtimeoutしました"), true);
    } else {
        item_->update();
    }
}

bool CompositorSpikeController::resetAfterMarkerPreflight() {
    double elapsed = 0;
    std::string err;
    if (!workerA_->seekBlocking(0, elapsed, err) || !workerB_->seekBlocking(0, elapsed, err)) {
        beginShutdown(QString::fromStdString(err), true);
        return false;
    }
    const auto a = workerA_->snapshot();
    const auto b = workerB_->snapshot();
    state_->coordinator.setSourceGeneration({1}, a.sourceGeneration);
    state_->coordinator.setSourceGeneration({2}, b.sourceGeneration);
    if (config_.mode == CompositorMode::Seek) {
        // seek latency配列へactual target probeの4 readbackを混ぜない。
        state_->requestedOutput.store(0, std::memory_order_release);
        state_->scheduledOutputCount.fetch_add(1, std::memory_order_relaxed);
        phase_ = Phase::OutputPreflightWait;
        item_->update();
    } else {
        workerA_->play();
        workerB_->play();
        state_->playbackSchedulerEnabled.store(true, std::memory_order_release);
        phase_ = Phase::OutputPreflightWait;
    }
    phaseTimer_.restart();
    return true;
}

bool CompositorSpikeController::resetPlaybackForMeasurement() {
    state_->playbackSchedulerEnabled.store(false, std::memory_order_release);
    state_->requestedOutput.store(-1, std::memory_order_release);
    state_->measurementIntervalActive.store(false, std::memory_order_release);
    if (config_.diagnosticCase == CompositorDiagnosticCase::FixedTextures)
        return true;

    workerA_->pause();
    if (workerB_)
        workerB_->pause();
    double elapsed = 0.0;
    std::string err;
    if (!workerA_->seekBlocking(0, elapsed, err) ||
        (workerB_ && !workerB_->seekBlocking(0, elapsed, err))) {
        beginShutdown(QString::fromStdString(err), true);
        return false;
    }
    const auto a = workerA_->snapshot();
    const auto b = workerB_ ? workerB_->snapshot() : gpu::SourceDecoderSnapshot{};
    state_->coordinator.setSourceGeneration({1}, a.sourceGeneration);
    if (workerB_)
        state_->coordinator.setSourceGeneration({2}, b.sourceGeneration);

    gpu::SourceFrameIdentity frontA;
    gpu::SourceFrameIdentity frontB;
    const bool validA = workerA_->buffer().peekFrontIdentity(frontA) && frontA.frameNumber == 0 &&
                        frontA.sourceGeneration == a.sourceGeneration;
    const bool validB =
        !workerB_ || (workerB_->buffer().peekFrontIdentity(frontB) && frontB.frameNumber == 0 &&
                      frontB.sourceGeneration == b.sourceGeneration);
    if (!validA || !validB) {
        beginShutdown(QStringLiteral("測定開始reset後のsource buffer先頭がframe 0ではありません"),
                      true);
        return false;
    }
    return true;
}

// W2-A.1 acquisition liveness timeout。60Hzなら約30 VBlank分の余裕がある。
// performance thresholdではない。observer start timeoutと同じ桁に揃えている。
constexpr long long kVBlankPrerollTimeoutMs = 500;
constexpr long long kVBlankSuccessorLivenessMs = 500;

bool CompositorSpikeController::startVBlankObserverWithPreroll() {
    if (!config_.vblankObserver || vblankObserverStarted_)
        return true;
    // W4-C2はphysical mapping authorityを明示的に持たない。observerを開始して
    // 後から無視すると、停止join自体がC2 captureのlivenessを支配してしまう。
    if (config_.formalSchedulerInvocationLedger)
        return true;
    auto* window = item_ ? item_->window() : nullptr;
    void* vblankHwnd = window ? reinterpret_cast<void*>(window->winId()) : nullptr;
    const auto resolved = gpu::resolveWindowOutput(vblankHwnd);
    vblankIdentityStart_ = resolved.identity;
    if (!resolved.ok) {
        vblankObserverError_ = resolved.error;
        return false;
    }
    // ring reset後にpublishされた実sampleだけをlower witnessとして受理する。
    const unsigned long long prerollBaseline = vblankObserver_.ring().publishSerial();
    if (!vblankObserver_.start(vblankHwnd, vblankObserverError_)) {
        return false;
    }
    vblankObserverStarted_ = true;
    vblankObserverRunning_ = true;
    if (!vblankObserver_.prerollNewSample(prerollBaseline, kVBlankPrerollTimeoutMs,
                                          vblankPreroll_)) {
        vblankObserverError_ = vblankPreroll_.timedOut
                                   ? "capture開始前にphysical VBlankを観測できません "
                                     "(PHYSICAL_VBLANK_PREROLL_TIMEOUT)"
                                   : "physical VBlank observerがpreroll中に停止しました";
        return false;
    }
    return true;
}

bool CompositorSpikeController::closeVBlankMappingSupportAfterTeardown() {
    if (!vblankObserverRunning_)
        return true;
    const auto stopObserver = [this] {
        vblankObserver_.stop();
        vblankObserverRunning_ = false;
        vblankIdentityEnd_ = gpu::resolveWindowOutput(
                                 item_ && item_->window()
                                     ? reinterpret_cast<void*>(item_->window()->winId())
                                     : nullptr)
                                 .identity;
    };
    // W4-C2はscheduler/capture-gate因果診断であり、W2のphysical mapping supportを
    // authorityにしない。terminal後のDWM sleepをsuccessor failureへ変換しない。
    if (config_.formalSchedulerInvocationLedger) {
        stopObserver();
        return true;
    }
    bool supportClosed = true;
    if (state_->nativePresentCaptureEnvelopeEnabled.load(std::memory_order_acquire)) {
        // producer/render teardownが完了した時点をfreezeし、その後の実sampleを
        // upper witnessにする。frame数・QPC tolerance・queue depthは推測しない。
        vblankMappingSupportTeardownCompleted_ = true;
        vblankMappingSupportPostrollBoundaryQpc_ = gpu::qpcTicks();
        if (!vblankObserver_.waitForSuccessor(vblankMappingSupportPostrollBoundaryQpc_,
                                              kVBlankSuccessorLivenessMs,
                                              vblankMappingSupportPostroll_)) {
            vblankObserverError_ =
                vblankMappingSupportPostroll_.timedOut
                    ? "PHYSICAL_MAPPING_SUPPORT_POSTROLL_TIMEOUT"
                    : "physical VBlank observerがmapping postroll中に停止しました";
            supportClosed = false;
        }
    }
    stopObserver();
    return supportClosed;
}

void CompositorSpikeController::requestMeasurementStart() {
    measurementStartCaptured_ = false;
    measurementStopCaptured_ = false;
    measurementAvailable_ = false;
    vblankSuccessor_ = {};
    vblankMappingSupportPostroll_ = {};
    vblankMappingSupportPostrollBoundaryQpc_ = 0;
    vblankMappingSupportTeardownCompleted_ = false;
    frozenMeasurementEndQpc_ = 0;
    captureEnvelopeCloseFailure_ = false;
    captureEnvelopeCloseReason_.clear();
    if (config_.incrementalMapperShadow) {
        auto* window = item_ ? item_->window() : nullptr;
        const bool d3d11 =
            window && window->rendererInterface() &&
            window->rendererInterface()->graphicsApi() == QSGRendererInterface::Direct3D11;
        if (!window || QString::fromLatin1(qVersion()) != QStringLiteral("6.11.1") ||
            window->requestedFormat().swapInterval() != 1 ||
            qEnvironmentVariableIsSet("QSG_NO_VSYNC") || !d3d11) {
            beginShutdown(QStringLiteral("Qt D3D11 Present preconditionを固定できません"), true);
            return;
        }
        incrementalMapperShadow_ = core::IncrementalOpportunityMapper(1);
        incrementalVblankRead_ = 0;
        incrementalSwapRead_ = 0;
        incrementalLastBeforeStart_ = {};
        incrementalDomainBoundary_ = {};
        incrementalMapperOriginSelected_ = false;
        incrementalMapperDomainClosed_ = false;
        incrementalMapperFinalized_ = false;
        incrementalMapperPass_ = false;
        incrementalMapperError_.clear();
        incrementalMapperTransitions_.clear();
        incrementalMapperTransitions_.reserve(gpu::kVBlankRingCapacity +
                                              gpu::kPresentationSwapRingCapacity);
        incrementalCommitQpc_.clear();
        incrementalCommitQpc_.reserve(gpu::kPresentationSwapRingCapacity);
    }
    const bool formalOpportunity =
        state_->formalOpportunitySchedulerEnabled.load(std::memory_order_acquire);
    if (config_.presentationOpportunityRing || formalOpportunity)
        dwmTimingStart_ = captureDwmTiming(item_ ? item_->window() : nullptr);
    if (formalOpportunity) {
        if (!validDwmAuthority(dwmTimingStart_)) {
            beginShutdown(QStringLiteral("P2-D5-2 presentation authorityを開始できません"), true);
            return;
        }
        state_->formalRefreshNumerator.store(dwmTimingStart_.displayRefreshNumerator,
                                             std::memory_order_relaxed);
        state_->formalRefreshDenominator.store(dwmTimingStart_.displayRefreshDenominator,
                                               std::memory_order_relaxed);
        state_->formalRequiredFrameCount.store(requiredMeasurementFrameCount_,
                                               std::memory_order_release);
        formalAuthorityLastPollQpc_ = gpu::qpcTicks();
    }
    // W2-C1.1。candidate producerを開く前にphysical observerと実preroll
    // witnessを成立させる。これによりlower supportをQPC heuristicなしで閉じる。
    if (state_->nativePresentCaptureEnvelopeEnabled.load(std::memory_order_acquire)) {
        if (!startVBlankObserverWithPreroll()) {
            beginShutdown(QStringLiteral("physical VBlank mapping supportを開始できません"), true);
            return;
        }
        state_->nativePresentEnvelopeStarted.store(false, std::memory_order_release);
        state_->nativePresentEnvelopeStopped.store(false, std::memory_order_release);
        state_->nativePresentEnvelopeStopRequested.store(false, std::memory_order_release);
        state_->nativePresentEnvelopeStartRequested.store(true, std::memory_order_release);
        phase_ = Phase::CaptureEnvelopeStartWait;
        phaseTimer_.restart();
        item_->update();
        return;
    }
    armMeasurementAfterCaptureEnvelopeOpen();
}

void CompositorSpikeController::armMeasurementAfterCaptureEnvelopeOpen() {
    if (!startVBlankObserverWithPreroll()) {
        beginShutdown(QStringLiteral("physical VBlank lower boundary prerollに失敗しました"), true);
        return;
    }
    state_->measurementFirstOutputFrame.store(-1, std::memory_order_release);
    state_->measurementDurationQpc.store(static_cast<long long>(gpu::qpcFrequency()) *
                                             config_.measureSeconds,
                                         std::memory_order_release);
    state_->measurementArmQpc.store(gpu::qpcTicks(), std::memory_order_release);
    state_->measurementStartCaptured.store(false, std::memory_order_release);
    state_->measurementStopRequested.store(false, std::memory_order_release);
    state_->measurementStopCaptured.store(false, std::memory_order_release);
    // W4-C3 amend 4。stop arbitration lifecycleのreset siteはここ1箇所だけであり、
    // measurement startのpublicationより前でなければならない。render threadが
    // start requestをconsumeした後にresetが走るinterleavingを作らない。
    resetStopArbitrationForMeasurement(*state_);
    // start requestはこのepoch初期化がすべて済んだ後で最後にpublishする。
    state_->measurementStartRequested.store(true, std::memory_order_release);
    phase_ = Phase::MeasureStartWait;
    item_->update();
}

bool CompositorSpikeController::pollIncrementalMapperShadow(bool finalizing) {
    if (!config_.incrementalMapperShadow)
        return true;
    if (!state_ || incrementalMapperFinalized_ || !incrementalMapperError_.empty())
        return incrementalMapperPass_;

    const long long measurementStart = state_->measurementStartQpc.load(std::memory_order_acquire);
    const long long measurementEnd = state_->measurementEndQpc.load(std::memory_order_acquire);
    if (measurementStart <= 0 || measurementEnd <= measurementStart)
        return !finalizing;

    const auto appendTransition = [this](const char* eventType, long long qpc,
                                         long long vblankOrdinal, long long swapOrdinal,
                                         long long sourceFrame) {
        const auto& snapshot = incrementalMapperShadow_.snapshot();
        while (incrementalCommitQpc_.size() < snapshot.committedAssignment.size())
            incrementalCommitQpc_.push_back(qpc);
        incrementalMapperTransitions_.push_back(
            {eventType, qpc, vblankOrdinal, swapOrdinal, sourceFrame, snapshot.solutionClass,
             snapshot.hasClosedRecords, snapshot.closedRecordCount,
             snapshot.committedAssignment.size(), snapshot.error});
    };
    const auto selectOrigin = [this, &appendTransition]() {
        if (incrementalMapperOriginSelected_)
            return true;
        if (incrementalLastBeforeStart_.qpc <= 0) {
            incrementalMapperError_ = "BEFORE_FIRST_VBLANK";
            return false;
        }
        const bool accepted = incrementalMapperShadow_.observeVBlank(
            {incrementalLastBeforeStart_.ordinal, incrementalLastBeforeStart_.qpc});
        appendTransition("ORIGIN", incrementalLastBeforeStart_.qpc,
                         incrementalLastBeforeStart_.ordinal, -1, -1);
        incrementalMapperOriginSelected_ = accepted;
        if (!accepted)
            incrementalMapperError_ =
                core::incrementalMappingErrorName(incrementalMapperShadow_.snapshot().error);
        return accepted;
    };

    while (true) {
        gpu::VBlankObservation vblank;
        gpu::PresentationSwapRecord swap;
        const bool haveVblank = vblankObserver_.ring().read(incrementalVblankRead_, vblank);
        const bool haveSwap =
            state_->presentationOpportunityRing.readSwap(incrementalSwapRead_, swap);
        if (!haveVblank && !haveSwap)
            break;
        const bool takeVblank = haveVblank && (!haveSwap || vblank.qpc <= swap.swapQpc);
        bool accepted = true;
        if (takeVblank) {
            ++incrementalVblankRead_;
            if (vblank.qpc <= measurementStart && !incrementalMapperOriginSelected_) {
                incrementalLastBeforeStart_ = vblank;
                continue;
            }
            if (incrementalMapperDomainClosed_)
                continue;
            if (!selectOrigin())
                return false;
            accepted = incrementalMapperShadow_.observeVBlank({vblank.ordinal, vblank.qpc});
            if (vblank.qpc >= measurementEnd) {
                incrementalMapperDomainClosed_ = true;
                incrementalDomainBoundary_ = vblank;
            }
            appendTransition("VBLANK", vblank.qpc, vblank.ordinal, -1, -1);
        } else {
            ++incrementalSwapRead_;
            if (swap.swapQpc < measurementStart || swap.swapQpc >= measurementEnd) {
                incrementalMapperError_ = "SWAP_OUTSIDE_MEASUREMENT";
                return false;
            }
            if (!selectOrigin())
                return false;
            accepted = incrementalMapperShadow_.observeCallback(swap.swapQpc);
            appendTransition("CALLBACK", swap.swapQpc, -1, swap.swapOrdinal,
                             swap.presentedOutputFrame);
        }
        if (!accepted) {
            incrementalMapperError_ =
                core::incrementalMappingErrorName(incrementalMapperShadow_.snapshot().error);
            return false;
        }
    }

    if (!finalizing)
        return true;
    incrementalMapperFinalized_ = true;
    if (!incrementalMapperOriginSelected_ || !incrementalMapperDomainClosed_) {
        incrementalMapperError_ = "MEASUREMENT_DOMAIN_NOT_CLOSED";
        return false;
    }
    const bool resolved = incrementalMapperShadow_.finish();
    appendTransition("END", incrementalDomainBoundary_.qpc, incrementalDomainBoundary_.ordinal, -1,
                     -1);
    if (!resolved) {
        incrementalMapperError_ =
            core::incrementalMappingErrorName(incrementalMapperShadow_.snapshot().error);
        return false;
    }
    const auto& snapshot = incrementalMapperShadow_.snapshot();
    incrementalMapperPass_ = snapshot.solutionClass == core::MappingSolutionClass::Unique &&
                             snapshot.committedAssignment.size() == incrementalSwapRead_;
    if (!incrementalMapperPass_)
        incrementalMapperError_ = "UNRESOLVED_MAPPING";
    return incrementalMapperPass_;
}

const char* CompositorSpikeController::phaseName(Phase phase) {
    switch (phase) {
    case Phase::WaitDevice:
        return "WAIT_DEVICE";
    case Phase::MarkerStart:
        return "MARKER_START";
    case Phase::MarkerWait:
        return "MARKER_WAIT";
    case Phase::OutputPreflightWait:
        return "OUTPUT_PREFLIGHT_WAIT";
    case Phase::Warmup:
        return "WARMUP";
    case Phase::MeasurementResetStart:
        return "MEASUREMENT_RESET_START";
    case Phase::MeasurementResetWait:
        return "MEASUREMENT_RESET_WAIT";
    case Phase::MeasurementPrimeStart:
        return "MEASUREMENT_PRIME_START";
    case Phase::MeasurementPrimeWait:
        return "MEASUREMENT_PRIME_WAIT";
    case Phase::CaptureEnvelopeStartWait:
        return "CAPTURE_ENVELOPE_START_WAIT";
    case Phase::MeasureStartWait:
        return "MEASURE_START_WAIT";
    case Phase::Measure:
        return "MEASURE";
    case Phase::MeasureStopWait:
        return "MEASURE_STOP_WAIT";
    case Phase::CaptureEnvelopeStopWait:
        return "CAPTURE_ENVELOPE_STOP_WAIT";
    case Phase::FatalMeasureStopWait:
        return "FATAL_MEASURE_STOP_WAIT";
    case Phase::SeekStart:
        return "SEEK_START";
    case Phase::SeekDecodeWait:
        return "SEEK_DECODE_WAIT";
    case Phase::SeekDisplayWait:
        return "SEEK_DISPLAY_WAIT";
    case Phase::LayoutStart:
        return "LAYOUT_START";
    case Phase::LayoutWait:
        return "LAYOUT_WAIT";
    case Phase::ShutdownWait:
        return "SHUTDOWN_WAIT";
    case Phase::Done:
        return "DONE";
    }
    return "UNKNOWN";
}

void CompositorSpikeController::reportDiagnosticPhase() {
    if (!config_.formalSchedulerInvocationLedger || phase_ == lastReportedDiagnosticPhase_)
        return;
    std::fprintf(stderr, "W4-C2_DIAGNOSTIC_PHASE: %s\n", phaseName(phase_));
    std::fflush(stderr);
    lastReportedDiagnosticPhase_ = phase_;
}

void CompositorSpikeController::tick() {
    if (!item_ || phase_ == Phase::Done)
        return;
    reportDiagnosticPhase();
    if (!startupError_.isEmpty()) {
        const QString error = std::exchange(startupError_, {});
        beginShutdown(error, true);
        return;
    }
    if (state_->fatal.load() && phase_ != Phase::ShutdownWait &&
        phase_ != Phase::CaptureEnvelopeStopWait && phase_ != Phase::FatalMeasureStopWait) {
        QString reason;
        {
            std::lock_guard<std::mutex> lock(state_->errorMutex);
            reason = QString::fromStdString(state_->fatalReason);
        }
        beginShutdown(reason, true);
        return;
    }
    if (phase_ == Phase::WaitDevice) {
        item_->update();
        if (state_->deviceReady.load())
            startWorkers();
        else if (phaseTimer_.elapsed() > config_.displayTimeoutMs)
            beginShutdown(QStringLiteral("Qt D3D11 device初期化がtimeoutしました"), true);
        return;
    }
    if (phase_ == Phase::MarkerStart) {
        startMarkerProbe();
    } else if (phase_ == Phase::MarkerWait) {
        pollMarkerProbe();
    } else if (phase_ == Phase::OutputPreflightWait) {
        if (state_->actualTargetProbeDone.load(std::memory_order_acquire)) {
            phase_ = config_.mode == CompositorMode::Seek ? Phase::SeekStart : Phase::Warmup;
            phaseTimer_.restart();
        } else if (phaseTimer_.elapsed() >= config_.displayTimeoutMs) {
            beginShutdown(QStringLiteral("actual target preflightがtimeoutしました"), true);
        } else {
            item_->update();
        }
    } else if (phase_ == Phase::Warmup && phaseTimer_.elapsed() >= config_.warmupSeconds * 1000) {
        state_->playbackSchedulerEnabled.store(false, std::memory_order_release);
        phase_ = Phase::MeasurementResetStart;
    } else if (phase_ == Phase::MeasurementResetStart) {
        state_->measurementResetCaptured.store(false, std::memory_order_release);
        state_->measurementResetRequested.store(true, std::memory_order_release);
        phase_ = Phase::MeasurementResetWait;
        item_->update();
    } else if (phase_ == Phase::MeasurementResetWait) {
        if (!state_->measurementResetCaptured.load(std::memory_order_acquire)) {
            item_->update();
            return;
        }
        if (!resetPlaybackForMeasurement())
            return;
        if (config_.diagnosticCase == CompositorDiagnosticCase::FixedTextures) {
            requestMeasurementStart();
            return;
        }
        phase_ = Phase::MeasurementPrimeStart;
    } else if (phase_ == Phase::MeasurementPrimeStart) {
        if (state_->playbackSchedulerEnabled.load(std::memory_order_acquire)) {
            beginShutdown(QStringLiteral("pre-roll開始前にschedulerが有効です"), true);
            return;
        }
        workerA_->play();
        workerB_->play();
        phaseTimer_.restart();
        phase_ = Phase::MeasurementPrimeWait;
    } else if (phase_ == Phase::MeasurementPrimeWait) {
        const auto a = workerA_->snapshot();
        const auto b = workerB_->snapshot();
        gpu::SourceFrameIdentity frontA;
        gpu::SourceFrameIdentity frontB;
        const bool hasA = workerA_->buffer().peekFrontIdentity(frontA);
        const bool hasB = workerB_->buffer().peekFrontIdentity(frontB);
        const gpu::MeasurementPrerollSourceState stateA{workerA_->buffer().depth(), hasA,  frontA,
                                                        a.sourceGeneration,         a.eof, a.fatal};
        const gpu::MeasurementPrerollSourceState stateB{workerB_->buffer().depth(), hasB,  frontB,
                                                        b.sourceGeneration,         b.eof, b.fatal};
        const auto result = gpu::evaluateMeasurementPreroll(
            stateA, stateB, state_->playbackSchedulerEnabled.load(std::memory_order_acquire),
            static_cast<int>(phaseTimer_.elapsed()));
        if (result == gpu::MeasurementPrerollResult::Waiting)
            return;
        if (result != gpu::MeasurementPrerollResult::Ready) {
            beginShutdown(QStringLiteral("測定pre-roll契約が不成立です: result=%1 depthA=%2 "
                                         "depthB=%3 frontA=%4 frontB=%5")
                              .arg(static_cast<int>(result))
                              .arg(stateA.depth)
                              .arg(stateB.depth)
                              .arg(hasA ? frontA.frameNumber : -1)
                              .arg(hasB ? frontB.frameNumber : -1),
                          true);
            return;
        }
        measurementPrerollOk_ = true;
        measurementPrerollDepthA_ = static_cast<long long>(stateA.depth);
        measurementPrerollDepthB_ = static_cast<long long>(stateB.depth);
        measurementPrerollFrontA_ = frontA.frameNumber;
        measurementPrerollFrontB_ = frontB.frameNumber;
        requestMeasurementStart();
    } else if (phase_ == Phase::CaptureEnvelopeStartWait) {
        if (state_->nativePresentEnvelopeStarted.load(std::memory_order_acquire)) {
            armMeasurementAfterCaptureEnvelopeOpen();
        } else if (phaseTimer_.elapsed() >= config_.displayTimeoutMs) {
            beginShutdown(QStringLiteral("native Present capture envelope開始がtimeoutしました"),
                          true);
        } else {
            item_->update();
        }
    } else if (phase_ == Phase::MeasureStartWait) {
        if (state_->measurementStartCaptured.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(state_->measurementMutex);
            measurementStart_ = state_->measurementStart;
            measurementStartCaptured_ = true;
            measurementStartA_ = workerA_ ? workerA_->snapshot() : gpu::SourceDecoderSnapshot{};
            measurementStartB_ = workerB_ ? workerB_->snapshot() : gpu::SourceDecoderSnapshot{};
            phase_ = Phase::Measure;
        } else {
            item_->update();
        }
    } else if (phase_ == Phase::Measure) {
        pollIncrementalMapperShadow(false);
        const long long nowQpc = gpu::qpcTicks();
        const bool formalOpportunity =
            state_->formalOpportunitySchedulerEnabled.load(std::memory_order_acquire);
        const long long pollInterval = static_cast<long long>(gpu::qpcFrequency()) / 4;
        if (formalOpportunity && nowQpc - formalAuthorityLastPollQpc_ >= pollInterval) {
            const auto current = captureDwmTiming(item_ ? item_->window() : nullptr);
            formalAuthorityLastPollQpc_ = nowQpc;
            if (!sameDwmAuthority(dwmTimingStart_, current)) {
                beginShutdown(QStringLiteral("P2-D5-2 refresh/DWM authorityが測定中に変化しました"),
                              true);
                return;
            }
        }
        if (state_->formalOpportunityDomainReached.load(std::memory_order_acquire) ||
            gpu::qpcMsBetween(measurementStart_.qpc, nowQpc) >= config_.measureSeconds * 1000.0) {
            phase_ = Phase::MeasureStopWait;
            if (!state_->measurementStopCaptured.load(std::memory_order_acquire)) {
                // W4-C3 amend 4。stop side effectより前にownershipをclaimする。
                const StopClaimResult claim = claimStopCause(*state_, StopArbitration::ExplicitStop);
                explicitStopClaim_ = claim;
                state_->measurementStopRequested.store(true, std::memory_order_release);
                state_->measurementStopCaptured.store(false, std::memory_order_release);
                item_->update();
            }
        }
    } else if (phase_ == Phase::MeasureStopWait) {
        if (state_->measurementStopCaptured.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> lock(state_->measurementMutex);
                measurementStop_ = state_->measurementStop;
            }
            measurementStopCaptured_ = true;
            measurementAvailable_ =
                measurementStartCaptured_ && measurementStop_.qpc >= measurementStart_.qpc;
            measureElapsedSeconds_ =
                gpu::qpcMsBetween(measurementStart_.qpc, measurementStop_.qpc) / 1000.0;
            measurementStopA_ = workerA_ ? workerA_->snapshot() : gpu::SourceDecoderSnapshot{};
            measurementStopB_ = workerB_ ? workerB_->snapshot() : gpu::SourceDecoderSnapshot{};
            const bool formalOpportunity =
                state_->formalOpportunitySchedulerEnabled.load(std::memory_order_acquire);
            if (config_.presentationOpportunityRing || formalOpportunity)
                dwmTimingStop_ = captureDwmTiming(item_ ? item_->window() : nullptr);
            if (vblankObserverStarted_) {
                // W2-C0.1。planned measurement endをfreezeし、その値以上のQPCを
                // 持つphysical sampleを証拠として待つ。measurement end/counterは
                // このcapture closure待機で変更しない。
                frozenMeasurementEndQpc_ =
                    state_->measurementEndQpc.load(std::memory_order_acquire);
                if (!config_.formalSchedulerInvocationLedger) {
                    const long long now = gpu::qpcTicks();
                    const long long untilEnd = std::max(0LL, frozenMeasurementEndQpc_ - now);
                    const long long untilEndMs = static_cast<long long>(std::ceil(
                        static_cast<double>(untilEnd) * 1000.0 /
                        static_cast<double>(gpu::qpcFrequency())));
                    const long long timeoutMs = untilEndMs + kVBlankSuccessorLivenessMs;
                    const bool successorConfirmed = vblankObserver_.waitForSuccessor(
                        frozenMeasurementEndQpc_, timeoutMs, vblankSuccessor_);
                    if (!successorConfirmed) {
                        captureEnvelopeCloseFailure_ = true;
                        captureEnvelopeCloseReason_ =
                            vblankSuccessor_.timedOut
                                ? QStringLiteral("PHYSICAL_VBLANK_SUCCESSOR_TIMEOUT")
                                : QStringLiteral(
                                      "physical VBlank observerがsuccessor待機中に停止しました");
                    }
                }
            }
            if (state_->nativePresentCaptureEnvelopeEnabled.load(std::memory_order_acquire) &&
                state_->nativePresentCaptureActive.load(std::memory_order_acquire)) {
                state_->nativePresentEnvelopeStopRequested.store(true, std::memory_order_release);
                phase_ = Phase::CaptureEnvelopeStopWait;
                phaseTimer_.restart();
                item_->update();
                return;
            }
            if (vblankObserverStarted_) {
                vblankObserver_.stop();
                vblankObserverRunning_ = false;
                vblankIdentityEnd_ =
                    gpu::resolveWindowOutput(item_ && item_->window()
                                                 ? reinterpret_cast<void*>(item_->window()->winId())
                                                 : nullptr)
                        .identity;
            }
            const bool incrementalMapperResolved = pollIncrementalMapperShadow(true);
            const bool authorityStable =
                !formalOpportunity || sameDwmAuthority(dwmTimingStart_, dwmTimingStop_);
            const bool measurementSucceeded = authorityStable && incrementalMapperResolved;
            beginShutdown(
                measurementSucceeded ? QStringLiteral("playback measurement完了")
                : !authorityStable
                    ? QStringLiteral("P2-D5-2 refresh/DWM authorityが途中で変化しました")
                    : QStringLiteral("incremental presentation mapperを一意に解決できません"),
                !measurementSucceeded);
        } else {
            item_->update();
        }
    } else if (phase_ == Phase::CaptureEnvelopeStopWait) {
        if (state_->nativePresentEnvelopeStopped.load(std::memory_order_acquire)) {
            // stoppedはrender callback内部でpublishされる。callback退出前に次のupdateを
            // 要求すると進行中frameへcoalesceされ、teardown callbackが失われる。
            if (state_->renderCallbackActive.load(std::memory_order_acquire)) {
                if (phaseTimer_.elapsed() >= 15000) {
                    std::fprintf(stderr,
                                 "W4-C2_DIAGNOSTIC_EXIT6_ENVELOPE_CALLBACK_TIMEOUT: "
                                 "capture envelope停止callbackが15秒以内に退出しませんでした\n");
                    exitCode_ = 6;
                    phase_ = Phase::Done;
                    timer_.stop();
                    Q_EMIT finished();
                }
                return;
            }
            // W2-C1.1ではobserverをここで止めない。candidate capture closure後も
            // render teardownとpostroll実sampleまで採取を継続する。
            if (captureEnvelopeCloseFailure_) {
                beginShutdown(captureEnvelopeCloseReason_, true);
                return;
            }
            const bool formalOpportunity =
                state_->formalOpportunitySchedulerEnabled.load(std::memory_order_acquire);
            const bool incrementalMapperResolved = pollIncrementalMapperShadow(true);
            const bool authorityStable =
                !formalOpportunity || sameDwmAuthority(dwmTimingStart_, dwmTimingStop_);
            const long long envelopeCloseQpc =
                state_->nativePresentEnvelopeCloseQpc.load(std::memory_order_acquire);
            const bool envelopeClosedAfterSuccessor = config_.formalSchedulerInvocationLedger
                                                          ? envelopeCloseQpc >= measurementStop_.qpc
                                                          : vblankSuccessor_.completed &&
                                                                vblankSuccessor_.sample.qpc >=
                                                                    frozenMeasurementEndQpc_ &&
                                                                envelopeCloseQpc >=
                                                                    vblankSuccessor_.sample.qpc;
            const bool measurementSucceeded = authorityStable && incrementalMapperResolved &&
                                              envelopeClosedAfterSuccessor &&
                                              !state_->fatal.load(std::memory_order_acquire);
            QString nativeFailure;
            if (state_->fatal.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lock(state_->errorMutex);
                nativeFailure = QString::fromStdString(state_->fatalReason);
            }
            beginShutdown(
                measurementSucceeded ? QStringLiteral("playback measurement完了")
                : !authorityStable
                    ? QStringLiteral("P2-D5-2 refresh/DWM authorityが途中で変化しました")
                : !envelopeClosedAfterSuccessor
                    ? QStringLiteral("capture envelopeのupper closureが不成立です")
                : !nativeFailure.isEmpty()
                    ? nativeFailure
                    : QStringLiteral("incremental presentation mapperを一意に解決できません"),
                !measurementSucceeded);
        } else if (phaseTimer_.elapsed() >= 15000) {
            std::fprintf(stderr,
                         "W4-C2_DIAGNOSTIC_EXIT6_ENVELOPE_STOP_TIMEOUT: "
                         "native Present capture envelopeが15秒以内に停止しませんでした\n");
            exitCode_ = 6;
            phase_ = Phase::Done;
            timer_.stop();
            Q_EMIT finished();
        } else if (phaseTimer_.elapsed() >= config_.displayTimeoutMs) {
            captureEnvelopeCloseFailure_ = true;
            captureEnvelopeCloseReason_ =
                QStringLiteral("native Present capture envelope停止がtimeoutしました");
            item_->update();
        } else {
            item_->update();
        }
    } else if (phase_ == Phase::FatalMeasureStopWait) {
        if (state_->measurementStopCaptured.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> lock(state_->measurementMutex);
                if (state_->measurementStartCaptured.load(std::memory_order_acquire)) {
                    measurementStart_ = state_->measurementStart;
                    measurementStartCaptured_ = true;
                }
                measurementStop_ = state_->measurementStop;
            }
            measurementStopCaptured_ = true;
            measurementAvailable_ =
                measurementStartCaptured_ && measurementStop_.qpc >= measurementStart_.qpc;
            if (measurementAvailable_)
                measureElapsedSeconds_ =
                    gpu::qpcMsBetween(measurementStart_.qpc, measurementStop_.qpc) / 1000.0;
            measurementStopA_ = workerA_ ? workerA_->snapshot() : gpu::SourceDecoderSnapshot{};
            measurementStopB_ = workerB_ ? workerB_->snapshot() : gpu::SourceDecoderSnapshot{};
            dwmTimingStop_ = captureDwmTiming(item_ ? item_->window() : nullptr);
            if (state_->nativePresentCaptureActive.load(std::memory_order_acquire)) {
                captureEnvelopeCloseFailure_ = true;
                captureEnvelopeCloseReason_ = shutdownReason_;
                state_->nativePresentEnvelopeStopRequested.store(true, std::memory_order_release);
                phase_ = Phase::CaptureEnvelopeStopWait;
                phaseTimer_.restart();
                item_->update();
            } else {
                performShutdown();
            }
        } else if (phaseTimer_.elapsed() >= config_.displayTimeoutMs) {
            if (state_->nativePresentCaptureActive.load(std::memory_order_acquire)) {
                captureEnvelopeCloseFailure_ = true;
                captureEnvelopeCloseReason_ = shutdownReason_;
                state_->nativePresentEnvelopeStopRequested.store(true, std::memory_order_release);
                phase_ = Phase::CaptureEnvelopeStopWait;
                phaseTimer_.restart();
                item_->update();
            } else {
                performShutdown();
            }
        } else {
            item_->update();
        }
    } else if (phase_ == Phase::SeekStart) {
        startSeek();
    } else if (phase_ == Phase::SeekDecodeWait) {
        pollSeekDecode();
    } else if (phase_ == Phase::SeekDisplayWait) {
        pollSeekDisplay();
    } else if (phase_ == Phase::LayoutStart) {
        startLayoutChange();
    } else if (phase_ == Phase::LayoutWait) {
        pollLayoutChange();
    } else if (phase_ == Phase::ShutdownWait) {
        item_->update();
        if (state_->teardownComplete.load()) {
            if (!closeVBlankMappingSupportAfterTeardown()) {
                std::fprintf(stderr,
                             "W4-C2_DIAGNOSTIC_EXIT6_CLOSE_MAPPING_FAILURE: "
                             "VBlank mapping supportの終了に失敗しました\n");
                exitCode_ = 6;
            }
            if (!writeMetrics()) {
                std::fprintf(stderr,
                             "W4-C2_DIAGNOSTIC_EXIT6_METRICS_WRITE_FAILURE: "
                             "最終metricsの書込みに失敗しました\n");
                exitCode_ = 6;
            }
            phase_ = Phase::Done;
            timer_.stop();
            Q_EMIT finished();
        } else if (phaseTimer_.elapsed() > 15000) {
            const auto diagnosticStage =
                state_->teardownDiagnosticStage.load(std::memory_order_acquire);
            const auto terminalExitStage =
                state_->terminalRenderExitDiagnosticStage.load(std::memory_order_acquire);
            std::fprintf(stderr,
                         "W4-C2_DIAGNOSTIC_EXIT6_TEARDOWN_TIMEOUT: "
                         "render teardownが15秒以内に完了しませんでした stage=%s "
                         "worker_a_joined=%d worker_b_joined=%d render_callback_active=%d "
                         "terminal_exit_stage=%s\n",
                         teardownDiagnosticStageName(diagnosticStage),
                         workerA_ && workerA_->joined() ? 1 : 0,
                         workerB_ && workerB_->joined() ? 1 : 0,
                         state_->renderCallbackActive.load(std::memory_order_acquire) ? 1 : 0,
                         terminalRenderExitDiagnosticStageName(terminalExitStage));
            exitCode_ = 6;
            phase_ = Phase::Done;
            timer_.stop();
            Q_EMIT finished();
        }
    }
}

void CompositorSpikeController::startSeek() {
    if (seekIndex_ >= seekTargets_.size()) {
        beginShutdown(QStringLiteral("dual seek完了"),
                      seekMismatch_ != 0 || seekTimeout_ != 0 || seekStaleCompletion_ != 0);
        return;
    }
    const long long target = seekTargets_[seekIndex_];
    waitBaseline_ = state_->ledger.baseline();
    seekRequestStartQpc_ = gpu::qpcTicks();
    seekDispatchOrder_ = {};
    if (!seekDispatchOrder_.begin(seekRequestStartQpc_)) {
        ++seekMismatch_;
        beginShutdown(QStringLiteral("parallel dispatch開始順序が不正です"), true);
        return;
    }
    waitTimer_.restart();
    std::string err;
    seekAReady_ = false;
    seekBReady_ = false;
    const long long aRequestQpc = gpu::qpcTicks();
    const bool aOrderValid = seekDispatchOrder_.requestA(aRequestQpc);
    const auto aResult = workerA_->requestSeek(target, seekTicketA_, err);
    const long long bRequestQpc = gpu::qpcTicks();
    const bool bOrderValid = seekDispatchOrder_.requestB(bRequestQpc);
    const auto bResult = workerB_->requestSeek(target, seekTicketB_, err);
    const long long dispatchCompleteQpc = gpu::qpcTicks();
    const bool completeOrderValid = seekDispatchOrder_.dispatchComplete(dispatchCompleteQpc);
    seekConcurrencySamples_.push_back(
        {seekRequestStartQpc_, aRequestQpc, bRequestQpc, dispatchCompleteQpc, 0, 0, 0, 0,
         seekTicketA_.requestId, seekTicketB_.requestId, aResult, bResult,
         aOrderValid && bOrderValid && completeOrderValid && seekDispatchOrder_.valid()});
    if (aResult != gpu::SeekRequestResult::Accepted ||
        bResult != gpu::SeekRequestResult::Accepted || !seekDispatchOrder_.valid()) {
        ++seekMismatch_;
        beginShutdown(QString::fromStdString(err), true);
        return;
    }
    phase_ = Phase::SeekDecodeWait;
}

void CompositorSpikeController::pollSeekDecode() {
    const long long target = seekTargets_[seekIndex_];
    if (!seekDispatchOrder_.completionPoll()) {
        ++seekMismatch_;
        beginShutdown(QStringLiteral("B dispatch完了前にcompletion pollへ進みました"), true);
        return;
    }
    if (!seekAReady_) {
        const auto waited = workerA_->waitSeek(seekTicketA_, 0, seekCompletionA_);
        if (waited == gpu::SeekWaitResult::StaleTicket) {
            ++seekStaleCompletion_;
            beginShutdown(QStringLiteral("Source A seek completionのrequestIdが一致しません"),
                          true);
            return;
        }
        seekAReady_ = waited == gpu::SeekWaitResult::Ready;
    }
    if (!seekBReady_) {
        const auto waited = workerB_->waitSeek(seekTicketB_, 0, seekCompletionB_);
        if (waited == gpu::SeekWaitResult::StaleTicket) {
            ++seekStaleCompletion_;
            beginShutdown(QStringLiteral("Source B seek completionのrequestIdが一致しません"),
                          true);
            return;
        }
        seekBReady_ = waited == gpu::SeekWaitResult::Ready;
    }
    if (!seekAReady_ || !seekBReady_) {
        if (waitTimer_.elapsed() >= config_.displayTimeoutMs) {
            ++seekTimeout_;
            beginShutdown(QStringLiteral("dual seek decode completionがtimeoutしました: %1 | %2")
                              .arg(seekDiagnosticText("A", *workerA_))
                              .arg(seekDiagnosticText("B", *workerB_)),
                          true);
        }
        return;
    }
    const bool validA = seekCompletionA_.status == gpu::SeekCompletionStatus::Completed &&
                        seekCompletionA_.requestId == seekTicketA_.requestId &&
                        seekCompletionA_.targetFrame == target &&
                        seekCompletionA_.decodedFrameNumber == target;
    const bool validB = seekCompletionB_.status == gpu::SeekCompletionStatus::Completed &&
                        seekCompletionB_.requestId == seekTicketB_.requestId &&
                        seekCompletionB_.targetFrame == target &&
                        seekCompletionB_.decodedFrameNumber == target;
    if (!validA || !validB) {
        ++seekMismatch_;
        const std::string error =
            !seekCompletionA_.error.empty() ? seekCompletionA_.error : seekCompletionB_.error;
        beginShutdown(
            QString::fromStdString(error.empty() ? "dual exact seekが失敗しました" : error), true);
        return;
    }
    seekAMs_.push_back(
        gpu::qpcMsBetween(seekCompletionA_.requestQpc, seekCompletionA_.decodeReadyQpc));
    seekBMs_.push_back(
        gpu::qpcMsBetween(seekCompletionB_.requestQpc, seekCompletionB_.decodeReadyQpc));
    seekDecodeReadyQpc_ =
        std::max(seekCompletionA_.decodeReadyQpc, seekCompletionB_.decodeReadyQpc);
    seekDecodeReadyMs_.push_back(gpu::qpcMsBetween(seekRequestStartQpc_, seekDecodeReadyQpc_));
    auto& concurrencySample = seekConcurrencySamples_.back();
    concurrencySample.aBeginQpc = seekCompletionA_.beginQpc;
    concurrencySample.aReadyQpc = seekCompletionA_.decodeReadyQpc;
    concurrencySample.bBeginQpc = seekCompletionB_.beginQpc;
    concurrencySample.bReadyQpc = seekCompletionB_.decodeReadyQpc;

    // A/B両completionのidentity検証が終わるまでcomposition stateを変更しない。
    state_->requestedOutput.store(-1, std::memory_order_release);
    state_->coordinator.setSourceGeneration({1}, seekCompletionA_.sourceGeneration);
    state_->coordinator.setSourceGeneration({2}, seekCompletionB_.sourceGeneration);
    waitExpectation_ = {
        target,
        state_->coordinator.compositionEpoch(),
        {{{1}, seekCompletionA_.sourceGeneration, seekCompletionA_.resourceEpoch, target},
         {{2}, seekCompletionB_.sourceGeneration, seekCompletionB_.resourceEpoch, target}}};
    state_->requestedOutput.store(target);
    state_->scheduledOutputCount.fetch_add(1);
    item_->update();
    phase_ = Phase::SeekDisplayWait;
}

void CompositorSpikeController::pollSeekDisplay() {
    gpu::CompositionDisplayRecord found;
    if (state_->ledger.findAfter(waitBaseline_, waitExpectation_, found)) {
        if (found.pairReadyQpc <= 0 || found.submissionQpc < found.pairReadyQpc ||
            found.displayedQpc < found.submissionQpc) {
            ++seekMismatch_;
        } else {
            seekDecodeReadyToPairMs_.push_back(
                gpu::qpcMsBetween(seekDecodeReadyQpc_, found.pairReadyQpc));
            seekPairToSubmissionMs_.push_back(
                gpu::qpcMsBetween(found.pairReadyQpc, found.submissionQpc));
            seekSubmissionToDisplayMs_.push_back(
                gpu::qpcMsBetween(found.submissionQpc, found.displayedQpc));
            seekDecodeReadyToDisplayMs_.push_back(
                gpu::qpcMsBetween(seekDecodeReadyQpc_, found.displayedQpc));
            seekDisplayedMs_.push_back(gpu::qpcMsBetween(seekRequestStartQpc_, found.displayedQpc));
        }
        ++seekIndex_;
        phase_ = Phase::SeekStart;
    } else if (waitTimer_.elapsed() >= config_.displayTimeoutMs) {
        ++seekTimeout_;
        ++seekIndex_;
        phase_ = Phase::SeekStart;
    } else {
        item_->update();
    }
}

void CompositorSpikeController::startLayoutChange() {
    if (layoutIndex_ >= 4) {
        beginShutdown(QStringLiteral("layout epoch stress完了"), layoutMismatch_ != 0);
        return;
    }
    waitBaseline_ = state_->ledger.baseline();
    const auto result = state_->coordinator.updateLayout(layoutFor(layoutIndex_));
    if (layoutIndex_ == 0) {
        if (result != gpu::LayoutUpdateResult::NoOp)
            ++layoutMismatch_;
    } else if (result != gpu::LayoutUpdateResult::Updated) {
        ++layoutMismatch_;
    }
    waitExpectation_ = {};
    waitExpectation_.compositionEpoch = state_->coordinator.compositionEpoch();
    waitTimer_.restart();
    phase_ = Phase::LayoutWait;
}

void CompositorSpikeController::pollLayoutChange() {
    // frame番号は連続再生で変化するため、baseline後かつ要求epochのactual displayを待つ。
    gpu::CompositionDisplayRecord found;
    if (state_->ledger.findEpochAfter(waitBaseline_, waitExpectation_.compositionEpoch, found)) {
        ++layoutIndex_;
        phase_ = Phase::LayoutStart;
    } else if (waitTimer_.elapsed() >= config_.displayTimeoutMs) {
        ++layoutMismatch_;
        ++layoutIndex_;
        phase_ = Phase::LayoutStart;
    }
}

void CompositorSpikeController::beginShutdown(const QString& reason, bool failure) {
    if (phase_ == Phase::ShutdownWait || phase_ == Phase::FatalMeasureStopWait ||
        phase_ == Phase::Done)
        return;
    shutdownReason_ = reason;
    if (failure)
        exitCode_ = 3;
    if (config_.mode == CompositorMode::Playback && !measurementStopCaptured_ &&
        state_->measurementStopCaptured.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lock(state_->measurementMutex);
            if (state_->measurementStartCaptured.load(std::memory_order_acquire)) {
                measurementStart_ = state_->measurementStart;
                measurementStartCaptured_ = true;
            }
            measurementStop_ = state_->measurementStop;
        }
        measurementStopCaptured_ = true;
        measurementAvailable_ =
            measurementStartCaptured_ && measurementStop_.qpc >= measurementStart_.qpc;
        if (measurementAvailable_)
            measureElapsedSeconds_ =
                gpu::qpcMsBetween(measurementStart_.qpc, measurementStop_.qpc) / 1000.0;
    }
    if (config_.mode == CompositorMode::Playback &&
        !state_->measurementStopCaptured.load(std::memory_order_acquire) &&
        (state_->measurementStartCaptured.load(std::memory_order_acquire) ||
         state_->measurementIntervalActive.load(std::memory_order_acquire))) {
        // W4-C3 amend 4。fatal shutdown由来のstopもownershipをclaimしてから発行する。
        fatalStopClaim_ = claimStopCause(*state_, StopArbitration::Fatal);
        state_->measurementStopRequested.store(true, std::memory_order_release);
        phase_ = Phase::FatalMeasureStopWait;
        phaseTimer_.restart();
        item_->update();
        return;
    }
    if (state_->nativePresentCaptureActive.load(std::memory_order_acquire)) {
        captureEnvelopeCloseFailure_ = true;
        captureEnvelopeCloseReason_ = reason;
        state_->nativePresentEnvelopeStopRequested.store(true, std::memory_order_release);
        phase_ = Phase::CaptureEnvelopeStopWait;
        phaseTimer_.restart();
        item_->update();
        return;
    }
    performShutdown();
}

void CompositorSpikeController::performShutdown() {
    if (seekLockTimingActive_) {
        state_->diagnosticLockTiming = state_->device.lock().endDiagnostics();
        seekLockTimingActive_ = false;
    }
    if (workerA_)
        workerA_->stop();
    if (workerB_)
        workerB_->stop();
    state_->playbackSchedulerEnabled.store(false, std::memory_order_release);
    item_->requestTeardown();
    phase_ = Phase::ShutdownWait;
    phaseTimer_.restart();
}

bool CompositorSpikeController::writeMetrics() {
    const auto a = workerA_ ? workerA_->snapshot() : gpu::SourceDecoderSnapshot{};
    const auto b = workerB_ ? workerB_->snapshot() : gpu::SourceDecoderSnapshot{};
    const auto seekA =
        workerA_ ? workerA_->seekDiagnosticSnapshot() : gpu::SourceSeekDiagnosticSnapshot{};
    const auto seekB =
        workerB_ ? workerB_->seekDiagnosticSnapshot() : gpu::SourceSeekDiagnosticSnapshot{};
    const auto& c = state_->compositor.counters();
    CompositorMeasurementCounters measurement;
    if (config_.mode == CompositorMode::Playback) {
        if (measurementAvailable_) {
            measurement = subtract(measurementStop_, measurementStart_);
            measurementAvailable_ = nonnegative(measurement);
        }
    } else {
        measurement.compositionRequested = c.compositionRequestedCount;
        measurement.compositionDrawn = c.compositionDrawnCount;
        measurement.gpuSubmission = c.gpuSubmissionCount;
        measurement.layerDraw = c.layerDrawCount;
        measurement.logicalClear = state_->logicalClearCount.load();
        measurement.scheduled = state_->scheduledOutputCount.load();
        measurement.displayed = state_->displayedCompositionCount.load();
        measurement.dropped = state_->droppedOutputCount.load();
        measurement.missingPair = state_->missingPairDropCount.load();
        measurement.sourceAEof = state_->sourceAEofCount.load();
        measurement.sourceBEof = state_->sourceBEofCount.load();
        measurement.dropSchedulerDeadline = state_->schedulerDeadlineDropCount.load();
        measurement.dropMissingSourceA = state_->missingSourceADropCount.load();
        measurement.dropMissingSourceB = state_->missingSourceBDropCount.load();
        measurement.dropMissingBoth = state_->missingBothDropCount.load();
        measurement.dropStaleGeneration = state_->staleGenerationDropCount.load();
        measurement.dropFutureGeneration = state_->futureGenerationDropCount.load();
        measurement.dropStaleCompositionEpoch = state_->staleCompositionEpochDropCount.load();
        measurement.dropRenderFailure = state_->renderFailureCount.load();
        measurement.presentCallback = state_->presentCallbackCount.load();
        measurement.repeatedPresent = state_->repeatedPresentCount.load();
        measurement.partialGpuIssueFailure = c.partialGpuIssueFailureCount;
        measurement.completionPollFailure = c.completionPollFailureCount;
        measurement.untrackedSubmission = c.untrackedSubmissionCount;
    }
    std::vector<double> sorted = seekDisplayedMs_;
    std::sort(sorted.begin(), sorted.end());
    const double p95 =
        sorted.empty()
            ? -1.0
            : sorted[static_cast<size_t>(std::ceil(static_cast<double>(sorted.size()) * 0.95) - 1)];
    const double observedMax = sorted.empty() ? -1.0 : sorted.back();
    std::vector<double> schedulerToPairUs;
    std::vector<double> pairUs;
    std::vector<double> prepareUs;
    std::vector<double> issueUs;
    std::vector<double> pollUs;
    std::vector<double> qtExternalUs;
    std::vector<double> renderTotalUs;
    std::vector<double> bufferDepthA;
    std::vector<double> bufferDepthB;
    std::vector<double> callbackIntervalUs;
    {
        std::lock_guard<std::mutex> lock(state_->diagnosticTimingMutex);
        callbackIntervalUs = state_->diagnosticRenderCallbackIntervalUs;
        for (const auto& sample : state_->diagnosticRenderSamples) {
            schedulerToPairUs.push_back(sample.schedulerToPairUs);
            pairUs.push_back(sample.pairUs);
            prepareUs.push_back(sample.compositionPrepareUs);
            issueUs.push_back(sample.compositionIssueUs);
            pollUs.push_back(sample.completionPollUs);
            qtExternalUs.push_back(sample.qtExternalUs);
            renderTotalUs.push_back(sample.renderCallbackTotalUs);
            bufferDepthA.push_back(static_cast<double>(sample.bufferDepthA));
            bufferDepthB.push_back(static_cast<double>(sample.bufferDepthB));
        }
    }
    QJsonObject renderStages{
        {"scheduler_to_pair_us", distribution(schedulerToPairUs, "values_us")},
        {"pair_us", distribution(pairUs, "values_us")},
        {"composition_prepare_us", distribution(prepareUs, "values_us")},
        {"composition_issue_us", distribution(issueUs, "values_us")},
        {"completion_poll_us", distribution(pollUs, "values_us")},
        {"qt_external_us", distribution(qtExternalUs, "values_us")},
        {"render_callback_total_us", distribution(renderTotalUs, "values_us")},
        {"render_callback_interval_us", distribution(callbackIntervalUs, "values_us")},
        {"buffer_depth_a", distribution(bufferDepthA, "values")},
        {"buffer_depth_b", distribution(bufferDepthB, "values")}};
    QJsonObject lockTimings{
        {"render_d3d11_lock_wait_us",
         distribution(state_->diagnosticLockTiming.renderWaitUs, "values_us")},
        {"decoder_a_d3d11_lock_wait_us",
         distribution(state_->diagnosticLockTiming.decoderAWaitUs, "values_us")},
        {"decoder_b_d3d11_lock_wait_us",
         distribution(state_->diagnosticLockTiming.decoderBWaitUs, "values_us")}};
    QJsonArray concurrencySamples;
    int overlapCount = 0;
    int parallelDispatchValidCount = 0;
    for (const auto& sample : seekConcurrencySamples_) {
        const bool overlaps = std::max(sample.aBeginQpc, sample.bBeginQpc) <
                              std::min(sample.aReadyQpc, sample.bReadyQpc);
        const bool parallelDispatchValid =
            sample.dispatchOrderValid &&
            sample.aRequestResult == gpu::SeekRequestResult::Accepted &&
            sample.bRequestResult == gpu::SeekRequestResult::Accepted &&
            sample.aRequestQpc >= sample.requestStartQpc &&
            sample.bRequestQpc >= sample.requestStartQpc &&
            sample.dispatchCompleteQpc >= sample.aRequestQpc &&
            sample.dispatchCompleteQpc >= sample.bRequestQpc &&
            sample.dispatchCompleteQpc <= std::min(sample.aReadyQpc, sample.bReadyQpc);
        if (overlaps)
            ++overlapCount;
        if (parallelDispatchValid)
            ++parallelDispatchValidCount;
        concurrencySamples.append(QJsonObject{
            {"request_start_qpc", sample.requestStartQpc},
            {"a_request_qpc", sample.aRequestQpc},
            {"b_request_qpc", sample.bRequestQpc},
            {"dispatch_complete_qpc", sample.dispatchCompleteQpc},
            {"a_begin_qpc", sample.aBeginQpc},
            {"a_ready_qpc", sample.aReadyQpc},
            {"b_begin_qpc", sample.bBeginQpc},
            {"b_ready_qpc", sample.bReadyQpc},
            {"a_request_id", static_cast<qint64>(sample.aRequestId)},
            {"b_request_id", static_cast<qint64>(sample.bRequestId)},
            {"a_request_result", seekRequestResultName(sample.aRequestResult)},
            {"b_request_result", seekRequestResultName(sample.bRequestResult)},
            {"parallel_dispatch_valid", parallelDispatchValid},
            {"execution_overlap", overlaps},
            {"serial_equivalent_ms", gpu::qpcMsBetween(sample.aBeginQpc, sample.aReadyQpc) +
                                         gpu::qpcMsBetween(sample.bBeginQpc, sample.bReadyQpc)},
            {"actual_dual_ready_ms",
             gpu::qpcMsBetween(sample.requestStartQpc,
                               std::max(sample.aReadyQpc, sample.bReadyQpc))}});
    }
    QJsonObject seekStages{
        {"seek_a_ms", distribution(seekAMs_, "values_ms")},
        {"seek_b_ms", distribution(seekBMs_, "values_ms")},
        {"seek_a_request_to_ready_ms", distribution(seekAMs_, "values_ms")},
        {"seek_b_request_to_ready_ms", distribution(seekBMs_, "values_ms")},
        {"dual_decode_ready_ms", distribution(seekDecodeReadyMs_, "values_ms")},
        {"both_ready_to_pair_ms", distribution(seekDecodeReadyToPairMs_, "values_ms")},
        {"decode_ready_to_pair_ms", distribution(seekDecodeReadyToPairMs_, "values_ms")},
        {"pair_to_submission_ms", distribution(seekPairToSubmissionMs_, "values_ms")},
        {"submission_to_display_record_ms", distribution(seekSubmissionToDisplayMs_, "values_ms")},
        {"decode_ready_to_display_ms", distribution(seekDecodeReadyToDisplayMs_, "values_ms")},
        {"request_to_display_ms", distribution(seekDisplayedMs_, "values_ms")}};
    const auto schedulerPhaseRecords = config_.schedulerPhaseRing
                                           ? state_->schedulerPhaseRing.snapshot()
                                           : std::vector<gpu::SchedulerPhaseRecord>{};
    const long long qpcFrequency = static_cast<long long>(gpu::qpcFrequency());
    const auto schedulerPhaseSummary =
        gpu::classifySchedulerPhase(schedulerPhaseRecords, qpcFrequency);
    QJsonArray schedulerPhaseRecordJson;
    for (size_t index = 0; index < schedulerPhaseRecords.size(); ++index) {
        const auto& record = schedulerPhaseRecords[index];
        const auto classification = schedulerPhaseSummary.classifications[index];
        schedulerPhaseRecordJson.append(QJsonObject{
            {"index", static_cast<qint64>(index)},
            {"callback_qpc", record.callbackQpc},
            {"previous_callback_qpc", record.previousCallbackQpc},
            {"scheduler_now_qpc", record.schedulerNowQpc},
            {"scheduler_next_frame_before", record.nextFrameBefore},
            {"next_deadline_qpc", record.nextDeadlineQpc},
            {"next_next_deadline_qpc", record.nextNextDeadlineQpc},
            {"now_minus_next_deadline_qpc", record.nowMinusNextDeadlineQpc},
            {"decision_due", record.decisionDue},
            {"decision_skipped_deadline_count", record.decisionSkippedDeadlineCount},
            {"decision_output_frame", record.decisionOutputFrame},
            {"repeated_this_callback", record.repeatedThisCallback},
            {"skip_classification", classification == gpu::SchedulerPhaseClassification::None
                                        ? QString{}
                                        : QString::fromLatin1(gpu::toString(classification))}});
    }
    QJsonArray schedulerPhasePairJson;
    for (const auto& pair : schedulerPhaseSummary.phasePairs) {
        schedulerPhasePairJson.append(
            QJsonObject{{"previous_record_index", static_cast<qint64>(pair.previousRecordIndex)},
                        {"current_record_index", static_cast<qint64>(pair.currentRecordIndex)},
                        {"skipped_deadline_count", pair.skippedDeadlineCount},
                        {"previous_early_us", pair.previousEarlyUs},
                        {"current_late_us", pair.currentLateUs},
                        {"callback_interval_us", pair.callbackIntervalUs}});
    }
    const long long unobservedBoundaryDeadlineCount = std::max(
        0LL, measurement.dropSchedulerDeadline - schedulerPhaseSummary.classifiedDeadlineCount);
    QJsonObject schedulerPhaseAttribution{
        {"enabled", config_.schedulerPhaseRing},
        {"capacity", static_cast<qint64>(gpu::kSchedulerPhaseRingCapacity)},
        {"record_count", static_cast<qint64>(schedulerPhaseRecords.size())},
        {"overflow_count", state_->schedulerPhaseRing.overflowCount()},
        {"qpc_frequency", qpcFrequency},
        {"skip_event_count", schedulerPhaseSummary.skipEventCount},
        {"classified_deadline_count", schedulerPhaseSummary.classifiedDeadlineCount},
        {"phase_pair_deadline_count", schedulerPhaseSummary.phasePairDeadlineCount},
        {"long_callback_gap_deadline_count", schedulerPhaseSummary.longCallbackGapDeadlineCount},
        {"unpaired_skip_deadline_count", schedulerPhaseSummary.unpairedSkipDeadlineCount},
        {"unobserved_boundary_deadline_count", unobservedBoundaryDeadlineCount},
        {"phase_pairs", schedulerPhasePairJson},
        {"records", schedulerPhaseRecordJson}};
    gpu::PresentationOpportunitySnapshot formalOpportunitySnapshot;
    if (state_->formalOpportunitySchedulerEnabled.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(state_->formalOpportunityMutex);
        formalOpportunitySnapshot = state_->formalOpportunityScheduler.snapshot();
    }
    QJsonArray formalOpportunityLedger;
    for (const auto& record : formalOpportunitySnapshot.records) {
        formalOpportunityLedger.append(QJsonObject{
            {"last_finalized_opportunity_ordinal", record.lastFinalizedOpportunityOrdinal},
            {"predicted_opportunity_ordinal", record.predictedOpportunityOrdinal},
            {"actual_opportunity_ordinal", record.actualOpportunityOrdinal},
            {"render_begin_qpc", record.renderBeginQpc},
            {"render_end_qpc", record.renderEndQpc},
            {"presentation_swap_qpc", record.swapQpc},
            {"render_ordinal", record.renderOrdinal},
            {"swap_ordinal", record.swapOrdinal},
            {"refresh_numerator", record.refreshNumerator},
            {"refresh_denominator", record.refreshDenominator},
            {"pre_render_authority", presentationAuthorityJson(record.preRenderAuthority)},
            {"post_swap_authority", presentationAuthorityJson(record.postSwapAuthority)},
            {"authority_continuous", record.authorityContinuous},
            {"predicted_source_frame", record.predictedSourceFrame},
            {"expected_source_frame", record.expectedSourceFrame},
            {"presented_source_frame", record.presentedSourceFrame},
            {"repeat", record.repeat},
            {"true_drop_before_this_opportunity", record.trueDropBefore},
            {"lost_opportunity_count", record.lostOpportunityCount},
            {"superseded_candidate_count", record.supersededCandidateCount},
            {"forward_reconciliation", record.forwardReconciliation},
            {"classification", QString::fromLatin1(gpu::presentationOpportunityClassificationName(
                                   record.classification))}});
    }
    const bool formalRefreshStable =
        !state_->formalOpportunitySchedulerEnabled.load(std::memory_order_acquire) ||
        sameDwmAuthority(dwmTimingStart_, dwmTimingStop_);
    const auto presentationRenders = config_.presentationOpportunityRing
                                         ? state_->presentationOpportunityRing.renderSnapshot()
                                         : std::vector<gpu::PresentationRenderRecord>{};
    const auto presentationSwaps = config_.presentationOpportunityRing
                                       ? state_->presentationOpportunityRing.swapSnapshot()
                                       : std::vector<gpu::PresentationSwapRecord>{};
    QJsonArray presentationRenderJson;
    for (const auto& record : presentationRenders) {
        presentationRenderJson.append(
            QJsonObject{{"callback_begin_qpc", record.callbackBeginQpc},
                        {"render_end_qpc", record.renderEndQpc},
                        {"render_ordinal", record.renderOrdinal},
                        {"selected_output_frame", record.selectedOutputFrame},
                        {"submitted_output_frame", record.submittedOutputFrame},
                        {"scheduler_skipped_deadline_count", record.schedulerSkippedDeadlineCount},
                        {"repeated", record.repeated}});
    }
    QJsonArray presentationSwapJson;
    std::unordered_set<long long> uniquePresentedFrames;
    for (const auto& record : presentationSwaps) {
        presentationSwapJson.append(
            QJsonObject{{"swap_qpc", record.swapQpc},
                        {"swap_ordinal", record.swapOrdinal},
                        {"completed_render_ordinal", record.completedRenderOrdinal},
                        {"submitted_render_ordinal", record.submittedRenderOrdinal},
                        {"presented_output_frame", record.presentedOutputFrame}});
        if (record.presentedOutputFrame >= 0)
            uniquePresentedFrames.insert(record.presentedOutputFrame);
    }
    // F3-B0 shadow evidence: physical VBlank列とidentity。formal logicへは
    // 一切入力しない。
    const auto vblankSamples = config_.vblankObserver ? vblankObserver_.ring().snapshot()
                                                      : std::vector<gpu::VBlankObservation>{};
    QJsonArray vblankSampleJson;
    for (const auto& sample : vblankSamples)
        vblankSampleJson.append(QJsonObject{{"ordinal", sample.ordinal}, {"qpc", sample.qpc}});
    gpu::VBlankIntervalReport vblankIntervals;
    const bool vblankIntervalsOk =
        !vblankSamples.empty() &&
        gpu::vblankIntervalReport(
            vblankSamples.data(), vblankSamples.size(), vblankIdentityStart_.refreshNumerator,
            vblankIdentityStart_.refreshDenominator, qpcFrequency, vblankIntervals);
    const auto vblankSequence =
        gpu::vblankSequenceStatus(vblankSamples.data(), vblankSamples.size());
    const char* vblankSequenceName = gpu::vblankSequenceStatusName(vblankSequence);
    const QJsonObject physicalVBlank{
        {"enabled", config_.vblankObserver},
        {"observer_started", vblankObserverStarted_},
        {"observer_error", QString::fromStdString(vblankObserverError_)},
        {"time_critical_priority", vblankObserver_.timeCriticalPriority()},
        {"window_output_start", windowOutputJson(vblankIdentityStart_)},
        {"window_output_end", windowOutputJson(vblankIdentityEnd_)},
        {"window_output_stable", gpu::sameWindowOutput(vblankIdentityStart_, vblankIdentityEnd_)},
        {"sample_count", static_cast<qint64>(vblankSamples.size())},
        {"ring_overflow_count", vblankObserver_.ring().overflowCount()},
        {"wait_failure_count", vblankObserver_.waitFailureCount()},
        {"sequence_status", QString::fromLatin1(vblankSequenceName)},
        {"interval_report_ok", vblankIntervalsOk},
        {"interval_count", vblankIntervals.intervalCount},
        {"long_interval_count", vblankIntervals.longIntervalCount},
        {"short_interval_count", vblankIntervals.shortIntervalCount},
        {"max_interval_qpc", vblankIntervals.maxIntervalQpc},
        {"min_interval_qpc", vblankIntervals.minIntervalQpc},
        {"nominal_period_qpc", vblankIntervals.nominalPeriodQpc},
        {"cumulative_deviation_numerator", vblankIntervals.cumulativeDeviationNumerator},
        {"cumulative_tolerance_unit", vblankIntervals.cumulativeToleranceUnit},
        {"cumulative_consistent", vblankIntervals.cumulativeConsistent},
        {"samples", vblankSampleJson}};
    // P2-D5-2-W2-A shadow。measurement窓に対するphysical VBlank domainを
    // exactに構築する。legacy formal scheduler / counters / shutdown / threshold
    // へは一切入力しない。measurement窓のauthorityは既存formal measurement
    // lifecycleであり、collector側で独自のendを作らない。
    const long long shadowMeasurementStartQpc =
        state_->measurementStartQpc.load(std::memory_order_acquire);
    const long long shadowMeasurementEndQpc =
        state_->measurementEndQpc.load(std::memory_order_acquire);
    gpu::PhysicalVBlankDomainInput domainInput;
    domainInput.samples = vblankSamples.data();
    domainInput.sampleCount = vblankSamples.size();
    domainInput.measurementStartQpc = shadowMeasurementStartQpc;
    domainInput.measurementEndQpc = shadowMeasurementEndQpc;
    domainInput.refreshNumerator = vblankIdentityStart_.refreshNumerator;
    domainInput.refreshDenominator = vblankIdentityStart_.refreshDenominator;
    domainInput.qpcFrequency = qpcFrequency;
    domainInput.ringOverflowCount = vblankObserver_.ring().overflowCount();
    domainInput.waitFailureCount = vblankObserver_.waitFailureCount();
    domainInput.observerStarted = vblankObserverStarted_;
    domainInput.timeCriticalPriority = vblankObserver_.timeCriticalPriority();
    domainInput.outputStable = gpu::sameWindowOutput(vblankIdentityStart_, vblankIdentityEnd_);
    domainInput.prerollCompleted = vblankPreroll_.completed;
    domainInput.prerollTimedOut = vblankPreroll_.timedOut;
    domainInput.prerollSample = vblankPreroll_.sample;
    domainInput.prerollWaitElapsedQpc = vblankPreroll_.waitElapsedQpc;
    // Layer 1A は shadow 出力のためだけに渡す。physical count との一致は
    // 要求せず、verdict にも接続しない。
    domainInput.requiredIntentCount = requiredMeasurementFrameCount_;
    gpu::PhysicalVBlankDomain vblankDomain;
    gpu::buildPhysicalVBlankDomain(domainInput, vblankDomain);
    const QJsonObject physicalVBlankDomainShadow{
        {"shadow_only", true},
        {"formal_counter_authority_changed", false},
        // performance semantics へは接続しない。required と physical の差を
        // drop と判定しない。
        {"performance_semantics_connected", false},
        {"measurement_window_authority", "formal measurement lifecycle"},
        {"physical_opportunity_authority", "window output physical VBlank observer"},
        {"domain_relation", "measurement_start_qpc <= vblank.qpc < measurement_end_qpc"},
        {"measurement_start_qpc", vblankDomain.measurementStartQpc},
        {"measurement_end_qpc_exclusive", vblankDomain.measurementEndQpc},
        // W2-A.1。下側boundaryがraceではなく構造的に保証されたことのprovenance。
        // 確認する不変量はordinalが0かどうかではなく
        // preroll sample.qpc < measurement_start_qpc である。
        {"prestart_vblank_preroll_completed", vblankDomain.prerollCompleted},
        {"prestart_vblank_preroll_timeout", vblankDomain.prerollTimedOut},
        {"prestart_vblank_sample_ordinal", vblankDomain.prerollSample.ordinal},
        {"prestart_vblank_sample_qpc", vblankDomain.prerollSample.qpc},
        {"prestart_wait_elapsed_qpc", vblankDomain.prerollWaitElapsedQpc},
        {"predecessor_valid", vblankDomain.predecessorValid},
        {"predecessor_ordinal", vblankDomain.predecessor.ordinal},
        {"predecessor_qpc", vblankDomain.predecessor.qpc},
        {"successor_valid", vblankDomain.successorValid},
        {"successor_ordinal", vblankDomain.successor.ordinal},
        {"successor_qpc", vblankDomain.successor.qpc},
        {"successor_wait_completed", vblankSuccessor_.completed},
        {"successor_wait_timeout", vblankSuccessor_.timedOut},
        {"successor_wait_frozen_measurement_end_qpc", vblankSuccessor_.frozenMeasurementEndQpc},
        {"successor_wait_sample_ordinal", vblankSuccessor_.sample.ordinal},
        {"successor_wait_sample_qpc", vblankSuccessor_.sample.qpc},
        {"successor_wait_elapsed_qpc", vblankSuccessor_.waitElapsedQpc},
        {"origin_ordinal", vblankDomain.originOrdinal},
        {"origin_qpc", vblankDomain.originQpc},
        {"last_ordinal", vblankDomain.lastOrdinal},
        {"last_qpc", vblankDomain.lastQpc},
        {"physical_opportunity_count", vblankDomain.physicalOpportunityCount},
        // shadow artifact は builder が判定した state だけを serialize する。
        // 外側で別に計算した値を混ぜると二重 producer になる。
        {"sequence_status",
         QString::fromLatin1(gpu::vblankSequenceStatusName(vblankDomain.sequenceStatus))},
        {"long_interval_count", vblankDomain.longIntervalCount},
        {"short_interval_count", vblankDomain.shortIntervalCount},
        {"ring_overflow_count", vblankDomain.ringOverflowCount},
        {"wait_failure_count", vblankDomain.waitFailureCount},
        {"cumulative_consistent", vblankDomain.cumulativeConsistent},
        {"output_stable", vblankDomain.outputStable},
        {"boundary_bracketed", vblankDomain.boundaryBracketed},
        {"shadow_authority_valid", vblankDomain.shadowAuthorityValid},
        {"shadow_authority_error", QString::fromLatin1(gpu::physicalVBlankDomainErrorName(
                                       vblankDomain.shadowAuthorityError))},
        // W1 で freeze した v2 canonical reason 語彙への射影。
        {"shadow_authority_canonical_reason",
         QString::fromLatin1(
             gpu::physicalVBlankDomainCanonicalReason(vblankDomain.shadowAuthorityError))},
        {"required_intent_count", vblankDomain.requiredIntentCount},
        {"intent_overhang_count", vblankDomain.intentOverhangCount},
        {"intent_surplus_count", vblankDomain.intentSurplusCount}};
    // W2-C1.1。measurement domainとは独立した、Presented candidate mapping用の
    // closed physical support。lower/upperはいずれも実VBlank sampleで固定する。
    const long long mappingCaptureBeginQpc =
        state_->nativePresentEnvelopeBeginQpc.load(std::memory_order_acquire);
    const long long mappingCaptureCloseQpc =
        state_->nativePresentEnvelopeCloseQpc.load(std::memory_order_acquire);
    const bool mappingSupportLowerClosed = vblankPreroll_.completed && !vblankPreroll_.timedOut &&
                                           mappingCaptureBeginQpc > 0 &&
                                           vblankPreroll_.sample.qpc < mappingCaptureBeginQpc;
    const bool mappingSupportUpperClosed =
        vblankMappingSupportTeardownCompleted_ && vblankMappingSupportPostroll_.completed &&
        !vblankMappingSupportPostroll_.timedOut && mappingCaptureCloseQpc > 0 &&
        mappingCaptureCloseQpc <= vblankMappingSupportPostrollBoundaryQpc_ &&
        vblankMappingSupportPostrollBoundaryQpc_ <= vblankMappingSupportPostroll_.sample.qpc;
    const bool mappingSupportAuthorityValid =
        state_->nativePresentCaptureEnvelopeEnabled.load(std::memory_order_acquire) &&
        mappingSupportLowerClosed && mappingSupportUpperClosed &&
        vblankPreroll_.sample.ordinal <= vblankMappingSupportPostroll_.sample.ordinal &&
        vblankObserver_.ring().overflowCount() == 0 && vblankObserver_.waitFailureCount() == 0 &&
        gpu::sameWindowOutput(vblankIdentityStart_, vblankIdentityEnd_);
    const QJsonObject physicalMappingSupportEnvelopeShadow{
        {"schema", "mvm-p2-d5-2-w2-c11-physical-mapping-support-envelope-1"},
        {"shadow_only", true},
        {"performance_semantics_connected", false},
        {"intent_satisfaction_connected", false},
        {"candidate_boundary_authority", "native Present capture lifecycle"},
        {"physical_boundary_authority", "window output physical VBlank observer"},
        {"capture_begin_qpc", mappingCaptureBeginQpc},
        {"capture_close_qpc", mappingCaptureCloseQpc},
        {"producer_teardown_completed", vblankMappingSupportTeardownCompleted_},
        {"postroll_boundary_qpc", vblankMappingSupportPostrollBoundaryQpc_},
        {"predecessor_valid", mappingSupportLowerClosed},
        {"predecessor_ordinal", vblankPreroll_.sample.ordinal},
        {"predecessor_qpc", vblankPreroll_.sample.qpc},
        {"successor_valid", mappingSupportUpperClosed},
        {"successor_ordinal", vblankMappingSupportPostroll_.sample.ordinal},
        {"successor_qpc", vblankMappingSupportPostroll_.sample.qpc},
        {"postroll_wait_completed", vblankMappingSupportPostroll_.completed},
        {"postroll_wait_timeout", vblankMappingSupportPostroll_.timedOut},
        {"postroll_wait_elapsed_qpc", vblankMappingSupportPostroll_.waitElapsedQpc},
        {"lower_closed_before_candidate_capture", mappingSupportLowerClosed},
        {"upper_closed_after_candidate_capture_and_teardown", mappingSupportUpperClosed},
        {"ring_overflow_count", static_cast<qint64>(vblankObserver_.ring().overflowCount())},
        {"wait_failure_count", static_cast<qint64>(vblankObserver_.waitFailureCount())},
        {"output_stable", gpu::sameWindowOutput(vblankIdentityStart_, vblankIdentityEnd_)},
        {"authority_valid", mappingSupportAuthorityValid}};
    QJsonArray incrementalTransitionJson;
    for (const auto& transition : incrementalMapperTransitions_) {
        incrementalTransitionJson.append(QJsonObject{
            {"event_type", transition.eventType},
            {"qpc", transition.qpc},
            {"vblank_ordinal", transition.vblankOrdinal},
            {"swap_ordinal", transition.swapOrdinal},
            {"source_frame", transition.sourceFrame},
            {"has_closed_records", transition.hasClosedRecords},
            {"current_solution_class",
             QString::fromLatin1(core::mappingSolutionClassName(transition.solutionClass))},
            {"closed_record_count", static_cast<qint64>(transition.closedRecordCount)},
            {"commit_watermark", static_cast<qint64>(transition.commitWatermark)},
            {"mapper_error",
             QString::fromLatin1(core::incrementalMappingErrorName(transition.error))}});
    }
    const auto& incrementalSnapshot = incrementalMapperShadow_.snapshot();
    QJsonArray incrementalRecordJson;
    long long lostPhysicalOpportunities = 0;
    long long previousMappedOpportunity = -1;
    std::size_t candidateSample = 0;
    while (candidateSample < vblankSamples.size() &&
           vblankSamples[candidateSample].ordinal < incrementalLastBeforeStart_.ordinal)
        ++candidateSample;
    std::size_t candidateLast = candidateSample;
    for (std::size_t index = 0; index < presentationSwaps.size(); ++index) {
        const auto& swap = presentationSwaps[index];
        while (candidateLast + 1 < vblankSamples.size() &&
               vblankSamples[candidateLast + 1].qpc <= swap.swapQpc)
            ++candidateLast;
        const long long mapped = index < incrementalSnapshot.committedAssignment.size()
                                     ? incrementalSnapshot.committedAssignment[index]
                                     : -1;
        if (previousMappedOpportunity >= 0 && mapped > previousMappedOpportunity + 1)
            lostPhysicalOpportunities += mapped - previousMappedOpportunity - 1;
        if (mapped >= 0)
            previousMappedOpportunity = mapped;
        incrementalRecordJson.append(QJsonObject{
            {"swap_ordinal", swap.swapOrdinal},
            {"swap_qpc", swap.swapQpc},
            {"source_frame", swap.presentedOutputFrame},
            {"candidate_first_opportunity_ordinal",
             incrementalMapperOriginSelected_ ? incrementalLastBeforeStart_.ordinal : -1},
            {"candidate_last_opportunity_ordinal",
             candidateLast < vblankSamples.size() ? vblankSamples[candidateLast].ordinal : -1},
            {"committed", mapped >= 0},
            {"commit_qpc", index < incrementalCommitQpc_.size() ? incrementalCommitQpc_[index] : 0},
            {"final_mapped_opportunity", mapped}});
    }
    std::vector<long long> sortedPresentedFrames(uniquePresentedFrames.begin(),
                                                 uniquePresentedFrames.end());
    std::sort(sortedPresentedFrames.begin(), sortedPresentedFrames.end());
    long long sourceFrameGapDrops = 0;
    long long nextSourceFrame = 0;
    for (const auto frame : sortedPresentedFrames) {
        if (frame >= nextSourceFrame) {
            sourceFrameGapDrops += frame - nextSourceFrame;
            nextSourceFrame = frame + 1;
        }
    }
    const long long tailSourceFrameDrops =
        std::max(0LL, requiredMeasurementFrameCount_ - nextSourceFrame);
    const bool sourceFrameAccountingExact = static_cast<long long>(uniquePresentedFrames.size()) +
                                                sourceFrameGapDrops + tailSourceFrameDrops ==
                                            requiredMeasurementFrameCount_;
    const QJsonObject incrementalMapperShadow{
        {"enabled", config_.incrementalMapperShadow},
        {"shadow_only", true},
        {"formal_counter_authority_changed", false},
        {"admissibility_relation", "VISIBLE_PREFIX: opportunity_start_qpc <= callback_qpc"},
        {"sync_interval_precondition", 1},
        {"qt_runtime_version", QString::fromLatin1(qVersion())},
        {"qt_source_tag", "v6.11.1"},
        {"qtbase_source_commit", "59c81a3c2247b821b9b84b4eb8d939b77e07e276"},
        {"qtdeclarative_source_commit", "a02bed441965ee1f18f856352c7d5ee5ba35d795"},
        {"qt_d3d11_source_path", "qtbase/src/gui/rhi/qrhid3d11.cpp"},
        {"qt_quick_source_path", "qtdeclarative/src/quick/scenegraph/qsgthreadedrenderloop.cpp"},
        {"requested_swap_interval",
         item_ && item_->window() ? item_->window()->requestedFormat().swapInterval() : -1},
        {"qsg_no_vsync_environment_set", qEnvironmentVariableIsSet("QSG_NO_VSYNC")},
        {"d3d11_backend_forced", true},
        {"present_sync_interval", 1},
        {"present_flags", 0},
        {"dxgi_present_restart_used", false},
        {"tearing_path_used", false},
        {"finalized", incrementalMapperFinalized_},
        {"mapper_pass", incrementalMapperPass_},
        {"mapper_error", QString::fromStdString(incrementalMapperError_)},
        {"final_solution_class",
         QString::fromLatin1(core::mappingSolutionClassName(incrementalSnapshot.solutionClass))},
        {"observed_swap_count", static_cast<qint64>(incrementalSwapRead_)},
        {"closed_record_count", static_cast<qint64>(incrementalSnapshot.closedRecordCount)},
        {"commit_watermark", static_cast<qint64>(incrementalSnapshot.committedAssignment.size())},
        {"origin_vblank_ordinal",
         incrementalMapperOriginSelected_ ? incrementalLastBeforeStart_.ordinal : -1},
        {"origin_vblank_qpc",
         incrementalMapperOriginSelected_ ? incrementalLastBeforeStart_.qpc : 0},
        {"measurement_domain_closed", incrementalMapperDomainClosed_},
        {"domain_boundary_vblank_ordinal",
         incrementalMapperDomainClosed_ ? incrementalDomainBoundary_.ordinal : -1},
        {"domain_boundary_vblank_qpc",
         incrementalMapperDomainClosed_ ? incrementalDomainBoundary_.qpc : 0},
        {"lost_physical_opportunity_count", lostPhysicalOpportunities},
        {"displayed_unique_source_frames", static_cast<qint64>(uniquePresentedFrames.size())},
        {"source_frame_gap_drops", sourceFrameGapDrops},
        {"tail_source_frame_drops", tailSourceFrameDrops},
        {"source_frame_accounting_exact", sourceFrameAccountingExact},
        {"transitions", incrementalTransitionJson},
        {"records", incrementalRecordJson}};
    QJsonObject presentationOpportunity{
        {"enabled", config_.presentationOpportunityRing},
        {"physical_vblank", physicalVBlank},
        {"physical_vblank_domain_shadow", physicalVBlankDomainShadow},
        {"physical_mapping_support_envelope_shadow", physicalMappingSupportEnvelopeShadow},
        {"measurement_start_qpc", state_->measurementStartQpc.load(std::memory_order_acquire)},
        {"measurement_end_qpc_exclusive",
         state_->measurementEndQpc.load(std::memory_order_acquire)},
        {"qpc_frequency", qpcFrequency},
        {"render_record_count", static_cast<qint64>(presentationRenders.size())},
        {"swap_record_count", static_cast<qint64>(presentationSwaps.size())},
        {"unique_presented_frame_count", static_cast<qint64>(uniquePresentedFrames.size())},
        {"render_overflow_count", state_->presentationOpportunityRing.renderOverflowCount()},
        {"swap_overflow_count", state_->presentationOpportunityRing.swapOverflowCount()},
        {"dwm_timing_start", dwmTimingJson(dwmTimingStart_)},
        {"dwm_timing_stop", dwmTimingJson(dwmTimingStop_)},
        {"render_records", presentationRenderJson},
        {"swap_records", presentationSwapJson},
        {"incremental_mapper_shadow", incrementalMapperShadow}};
    const NativePresentHookSnapshot nativePresentSnapshot =
        state_->nativePresentHook ? state_->nativePresentHook->snapshot()
                                  : NativePresentHookSnapshot{};
    QJsonArray nativePresentRecords;
    QJsonArray captureEnvelopeRecords;
    QJsonArray intentIdentityLedger;
    QJsonArray intentScopeLedgerJson;
    QJsonArray requiredIntentOrdinalsJson;
    for (const long long ordinal : formalOpportunitySnapshot.requiredIntentOrdinals)
        requiredIntentOrdinalsJson.append(QString::number(ordinal));
    std::vector<NativePresentIntentScopeRecord> intentScopeLedger;
    {
        std::lock_guard<std::mutex> lock(state_->nativePresentIntentScopeMutex);
        intentScopeLedger = state_->nativePresentIntentScopeLedger;
    }
    std::unordered_map<std::uint64_t, std::vector<NativePresentIntentScopeRecord>>
        intentScopeByToken;
    for (const auto& record : intentScopeLedger) {
        intentScopeByToken[record.tokenSerial].push_back(record);
        intentScopeLedgerJson.append(QJsonObject{
            {"token_serial", QString::number(record.tokenSerial)},
            {"intent_ordinal", QString::number(record.intentOrdinal)},
            {"intent_scope", nativePresentIntentScopeName(record.scope)},
            {"decision_qpc", record.decisionQpc},
            {"decision_qpc_exact", record.decisionQpcExact},
            {"required_current_membership", record.requiredCurrentMembership},
            {"required_current_membership_exact", record.requiredCurrentMembershipExact},
            {"measurement_boundary_relation",
             measurementBoundaryRelationName(record.measurementBoundaryRelation)},
            {"producer_semantics_exact", record.producerSemanticsExact},
            {"duplicate_callback", record.duplicateCallback},
            {"repeat", record.repeat},
            {"past_source_domain", record.pastSourceDomain},
            {"target_frame", record.targetFrame},
            {"last_finalized_opportunity_ordinal", record.lastFinalizedOpportunityOrdinal},
            {"render_begin_qpc", record.renderBeginQpc},
            {"formal_transport_eligible",
             record.transportDisposition == gpu::FormalIntentTransportDisposition::Transport},
            {"transport_disposition",
             gpu::formalIntentTransportDispositionName(record.transportDisposition)},
        });
    }
    // W4-C3 stop witness v3。render threadが記録した値をそのまま出力し、
    // controller側でcauseやclaim結果をQPC等から再構築しない。
    QJsonObject formalStopWitnessJson;
    {
        CompositorStopWitness witness;
        bool captured = state_->stopWitnessCaptured.load(std::memory_order_seq_cst);
        if (captured) {
            std::lock_guard<std::mutex> lock(state_->stopWitnessMutex);
            witness = state_->stopWitness;
        }
        formalStopWitnessJson = QJsonObject{
            {"schema", "mvm-p2-d5-2-w4-c3-stop-witness-3"},
            {"diagnostic_root_cause_capture",
             state_->formalSchedulerInvocationLedgerEnabled.load(std::memory_order_acquire)},
            {"canonical_performance_authority", false},
            {"captured", captured},
            {"witness_count", captured ? 1 : 0},
            {"duplicate_witness_count",
             static_cast<qint64>(state_->stopWitnessDuplicateCount.load(std::memory_order_seq_cst))},
            {"losing_stop_claim_count",
             static_cast<qint64>(state_->losingStopClaimCount.load(std::memory_order_seq_cst))},
            {"cause", QString::fromLatin1(stopArbitrationName(witness.cause))},
            {"render_callback_begin_qpc", witness.renderCallbackBeginQpc},
            {"scheduler_invocation_serial",
             static_cast<qint64>(witness.terminal.schedulerInvocationSerial)},
            {"terminal_intent_ordinal", witness.terminal.intentOrdinal},
            {"terminal_target_frame", witness.terminal.targetFrame},
            {"terminal_past_source_domain", witness.terminal.pastSourceDomain},
            {"terminal_required_intent_membership", witness.terminal.requiredIntentMembership},
            {"stop_arbitration",
             QJsonObject{
                 {"previous", QString::fromLatin1(stopArbitrationName(witness.arbitrationPrevious))},
                 {"claimed", QString::fromLatin1(stopArbitrationName(witness.arbitrationClaimed))},
                 {"claim_succeeded", witness.arbitrationClaimSucceeded},
                 {"measurement_start_state",
                  QString::fromLatin1(stopArbitrationName(witness.measurementStartState))},
                 {"reset_count_during_measurement",
                  static_cast<qint64>(witness.resetCountDuringMeasurement)}}},
            {"measurement_start",
             QJsonObject{{"explicit_stop_publish_serial",
                          static_cast<qint64>(
                              witness.measurementStartExplicitStopPublishSerial)},
                         {"fatal_publish_serial",
                          static_cast<qint64>(witness.measurementStartFatalPublishSerial)}}},
            {"pre",
             QJsonObject{
                 {"capture_gate_open", witness.preCaptureGateOpen},
                 {"explicit_stop_requested", witness.preExplicitStopRequested},
                 {"planned_window_end_reached", witness.prePlannedWindowEndReached},
                 {"fatal_latched", witness.preFatalLatched},
                 {"explicit_stop_publish_serial",
                  static_cast<qint64>(witness.preExplicitStopPublishSerial)},
                 {"fatal_publish_serial", static_cast<qint64>(witness.preFatalPublishSerial)}}},
            {"action",
             QJsonObject{{"formal_opportunity_domain_reached_published",
                          witness.terminal.formalOpportunityDomainReachedPublished},
                         {"finish_measurement_entered", witness.finishMeasurementEntered},
                         {"capture_gate_exchange_closed", witness.captureGateExchangeClosed},
                         {"measurement_stop_published", witness.measurementStopPublished}}},
            {"at_gate_close",
             QJsonObject{{"explicit_stop_publish_serial",
                          static_cast<qint64>(witness.gateCloseExplicitStopPublishSerial)},
                         {"fatal_publish_serial",
                          static_cast<qint64>(witness.gateCloseFatalPublishSerial)}}},
            {"post", QJsonObject{{"capture_gate_open", witness.postCaptureGateOpen},
                                 {"measurement_stop_qpc", witness.measurementStopQpc}}}};
    }
    QJsonArray formalSchedulerInvocationLedger;
    for (const auto& record : formalOpportunitySnapshot.invocationRecords) {
        formalSchedulerInvocationLedger.append(QJsonObject{
            {"scheduler_invocation_serial", static_cast<qint64>(record.invocationSerial)},
            {"invocation_qpc", record.invocationQpc},
            {"input_authority", presentationAuthorityJson(record.inputAuthority)},
            {"pre", presentationInvocationStateJson(record.pre)},
            {"result",
             QString::fromLatin1(gpu::presentationSchedulerInvocationResultName(record.result))},
            {"reason",
             QString::fromLatin1(gpu::presentationSchedulerInvocationReasonName(record.reason))},
            {"decision_valid", record.decision.valid},
            {"duplicate_callback", record.decision.duplicateCallback},
            {"intent_ordinal", record.decision.opportunityOrdinal},
            {"target_frame", record.decision.targetFrame},
            {"repeat", record.decision.repeat},
            {"past_source_domain", record.decision.pastSourceDomain},
            {"required_intent_membership", record.decision.requiredIntentMembership},
            {"required_intent_membership_exact", record.decision.requiredIntentMembershipExact},
            {"last_finalized_opportunity_ordinal", record.decision.lastFinalizedOpportunityOrdinal},
            {"formal_transport_disposition",
             QString::fromLatin1(
                 gpu::formalIntentTransportDispositionName(record.transportDisposition))},
            {"formal_transport_disposition_exact", record.transportDispositionExact},
            {"post", presentationInvocationStateJson(record.post)},
            {"state_transition_exact", record.stateTransitionExact}});
    }
    const auto scopeRecordMatchesNative = [](const NativePresentIntentScopeRecord& producer,
                                             const MvmNativePresentRecord& native) {
        if (producer.transportDisposition !=
            gpu::FormalIntentTransportDisposition::Transport) {
            const bool suppressionExact =
                producer.transportDisposition ==
                    gpu::FormalIntentTransportDisposition::SuppressDuplicateCallback ||
                producer.transportDisposition ==
                    gpu::FormalIntentTransportDisposition::SuppressOutsideRequiredSet;
            return suppressionExact && native.intentOrdinalValid == 0 &&
                   native.token.intentOrdinalValid == 0;
        }
        return producer.transportDisposition ==
                   gpu::FormalIntentTransportDisposition::Transport &&
               native.intentOrdinalValid == 1 && native.token.intentOrdinalValid == 1 &&
               producer.intentOrdinal == native.intentOrdinal;
    };
    long long intentScopeMissingCount = 0;
    long long intentScopeAmbiguousCount = 0;
    long long intentScopeMutationCount = 0;
    std::unordered_set<std::uint64_t> nativeIntentTokenSerials;
    for (const auto& record : nativePresentSnapshot.records) {
        if (record.tokenPresent == 0)
            continue;
        nativeIntentTokenSerials.insert(record.token.tokenSerial);
        const auto found = intentScopeByToken.find(record.token.tokenSerial);
        if (found == intentScopeByToken.end()) {
            ++intentScopeMissingCount;
        } else if (found->second.size() != 1) {
            ++intentScopeAmbiguousCount;
        } else if (!scopeRecordMatchesNative(found->second.front(), record)) {
            ++intentScopeMutationCount;
        }
    }
    long long intentScopeUnmatchedCount = 0;
    for (const auto& [tokenSerial, records] : intentScopeByToken) {
        if (tokenSerial == 0 || nativeIntentTokenSerials.count(tokenSerial) == 0)
            intentScopeUnmatchedCount += static_cast<long long>(records.size());
    }
    const bool intentScopeAuthorityPass =
        !intentScopeLedger.empty() && intentScopeMissingCount == 0 &&
        intentScopeAmbiguousCount == 0 && intentScopeMutationCount == 0 &&
        intentScopeUnmatchedCount == 0;
    std::unordered_set<std::uint64_t> nativePresentSerials;
    std::unordered_set<std::uint64_t> compositionTokenSerials;
    const bool formalIntentMode =
        state_->formalOpportunitySchedulerEnabled.load(std::memory_order_acquire);
    long long projectedMissingTokenCount = 0;
    long long projectedDuplicateIdentityCount = 0;
    long long projectedFailedPresentCount = 0;
    const long long b1MeasurementStartQpc =
        state_->measurementStartQpc.load(std::memory_order_acquire);
    const long long b1MeasurementEndQpc = state_->measurementEndQpc.load(std::memory_order_acquire);
    const auto nativePresentRecordJson = [&intentScopeByToken, &scopeRecordMatchesNative](
                                             const MvmNativePresentRecord& record) {
        QJsonArray sources;
        for (std::uint32_t index = 0; index < record.token.sourceCount; ++index) {
            const auto& source = record.token.sources[index];
            sources.append(QJsonObject{
                {"source_id", QString::number(source.sourceId)},
                {"source_generation", QString::number(source.sourceGeneration)},
                {"resource_epoch", QString::number(source.resourceEpoch)},
                {"frame_number", source.frameNumber},
            });
        }
        QJsonObject intentScope{{"token_serial", QString::number(record.token.tokenSerial)},
                                {"intent_ordinal", QString::number(record.intentOrdinal)},
                                {"intent_scope", "UNRESOLVED"},
                                {"match_count", 0},
                                {"exact", false},
                                {"formal_transport_eligible", false},
                                {"transport_disposition", "UNRESOLVED"}};
        const auto scope = intentScopeByToken.find(record.token.tokenSerial);
        if (scope != intentScopeByToken.end()) {
            intentScope["match_count"] = static_cast<qint64>(scope->second.size());
            if (scope->second.size() == 1) {
                const auto& producer = scope->second.front();
                intentScope["intent_ordinal"] = QString::number(producer.intentOrdinal);
                intentScope["intent_scope"] = nativePresentIntentScopeName(producer.scope);
                intentScope["exact"] = scopeRecordMatchesNative(producer, record);
                intentScope["formal_transport_eligible"] =
                    producer.transportDisposition ==
                    gpu::FormalIntentTransportDisposition::Transport;
                intentScope["transport_disposition"] =
                    gpu::formalIntentTransportDispositionName(producer.transportDisposition);
            }
        }
        return QJsonObject{
            {"present_serial", QString::number(record.presentSerial)},
            {"swapchain_identity", QString::number(record.swapchainIdentity)},
            {"thread_id", static_cast<qint64>(record.threadId)},
            {"present_enter_qpc", record.presentEnterQpc},
            {"present_return_qpc", record.presentReturnQpc},
            {"hresult", static_cast<qint64>(record.hresult)},
            {"sync_interval", static_cast<qint64>(record.syncInterval)},
            {"present_flags", static_cast<qint64>(record.presentFlags)},
            {"propagation_serial", QString::number(record.propagationSerial)},
            {"intent_ordinal", QString::number(record.intentOrdinal)},
            {"intent_ordinal_valid", record.intentOrdinalValid != 0},
            {"token_present", record.tokenPresent != 0},
            {"composition_token",
             QJsonObject{{"token_serial", QString::number(record.token.tokenSerial)},
                         {"propagation_serial", QString::number(record.token.propagationSerial)},
                         {"composition_epoch", QString::number(record.token.compositionEpoch)},
                         {"composition_state", QString::number(record.token.compositionState)},
                         {"output_frame", record.token.outputFrameNumber},
                         {"intent_ordinal", QString::number(record.token.intentOrdinal)},
                         {"intent_ordinal_valid", record.token.intentOrdinalValid != 0},
                         {"source_count", static_cast<qint64>(record.token.sourceCount)},
                         {"sources", sources}}},
            {"intent_scope_provenance", intentScope},
        };
    };
    bool everyIntentIdentityRecordExact = true;
    for (const auto& record : nativePresentSnapshot.records) {
        captureEnvelopeRecords.append(nativePresentRecordJson(record));
        // W2-B1/B2 scopeは従来どおりfreezeしたmeasurement投影だけに限定する。
        // capture envelopeのpre/post recordをtransportへ混ぜない。
        if (record.presentEnterQpc < b1MeasurementStartQpc ||
            record.presentReturnQpc >= b1MeasurementEndQpc)
            continue;
        const bool presentSerialUnique =
            record.presentSerial != 0 && nativePresentSerials.insert(record.presentSerial).second;
        const bool tokenSerialUnique =
            record.token.tokenSerial != 0 &&
            compositionTokenSerials.insert(record.token.tokenSerial).second;
        const bool identityExact = record.intentOrdinalValid == record.token.intentOrdinalValid &&
                                   record.intentOrdinal == record.token.intentOrdinal;
        const auto scope = intentScopeByToken.find(record.token.tokenSerial);
        const bool exactScope = scope != intentScopeByToken.end() && scope->second.size() == 1 &&
                                scopeRecordMatchesNative(scope->second.front(), record);
        const bool suppressed =
            exactScope && scope->second.front().transportDisposition !=
                              gpu::FormalIntentTransportDisposition::Transport;
        const bool modeValid = formalIntentMode
                                   ? (record.token.intentOrdinalValid == 1 || suppressed)
                                   : record.token.intentOrdinalValid == 0;
        everyIntentIdentityRecordExact = everyIntentIdentityRecordExact && presentSerialUnique &&
                                         tokenSerialUnique && identityExact && modeValid;
        if (record.tokenPresent == 0)
            ++projectedMissingTokenCount;
        if (!presentSerialUnique || !tokenSerialUnique)
            ++projectedDuplicateIdentityCount;
        if (FAILED(static_cast<HRESULT>(record.hresult)))
            ++projectedFailedPresentCount;
        intentIdentityLedger.append(QJsonObject{
            {"native_present_embedded_token_serial", QString::number(record.token.tokenSerial)},
            {"composition_token_intent_ordinal", QString::number(record.token.intentOrdinal)},
            {"composition_token_intent_valid", record.token.intentOrdinalValid != 0},
            {"native_present_serial", QString::number(record.presentSerial)},
            {"native_present_intent_ordinal", QString::number(record.intentOrdinal)},
            {"native_present_intent_valid", record.intentOrdinalValid != 0},
            {"formal_transport_eligible", exactScope && !suppressed},
            {"suppression_exact", suppressed},
            {"transport_disposition",
             exactScope
                 ? gpu::formalIntentTransportDispositionName(
                       scope->second.front().transportDisposition)
                 : "UNRESOLVED"},
        });
        nativePresentRecords.append(nativePresentRecordJson(record));
    }
    everyIntentIdentityRecordExact =
        everyIntentIdentityRecordExact && !nativePresentRecords.isEmpty();
    static constexpr const char* dirtyStageNames[MVM_DIRTY_STAGE_COUNT]{
        "renderer_update",   "node_schedule_update", "window_update",      "node_render",
        "compositor_render", "composition_token",    "dirty_material",     "texture_changed",
        "qsg_main_render",   "rhi_end_frame",        "successful_present", "target_pixel_toggle"};
    QJsonObject dirtyStageCounts;
    for (std::uint32_t stage = 0; stage < MVM_DIRTY_STAGE_COUNT; ++stage)
        dirtyStageCounts.insert(
            dirtyStageNames[stage],
            QString::number(nativePresentSnapshot.dirtyPropagationStageCounts[stage]));
    QJsonArray dirtyPropagationRecords;
    for (const auto& record : nativePresentSnapshot.dirtyPropagationRecords) {
        QJsonObject stages;
        for (std::uint32_t stage = 0; stage < MVM_DIRTY_STAGE_COUNT; ++stage)
            stages.insert(dirtyStageNames[stage], record.stageQpc[stage]);
        dirtyPropagationRecords.append(QJsonObject{
            {"propagation_serial", QString::number(record.propagationSerial)},
            {"composition_token_serial", QString::number(record.compositionTokenSerial)},
            {"present_serial", QString::number(record.presentSerial)},
            {"output_frame", record.outputFrameNumber},
            {"stage_qpc", stages},
        });
    }
    const bool nativePresentAuthorityPass =
        config_.nativePresentHook == NativePresentHookMode::OnDiagnostic &&
        state_->nativePresentHook && nativePresentSnapshot.available &&
        nativePresentSnapshot.captureStarted && nativePresentSnapshot.captureStopped &&
        nativePresentSnapshot.observedQtAbiVersion == MVM_NATIVE_PRESENT_HOOK_ABI_VERSION &&
        nativePresentSnapshot.layoutHandshakeAccepted &&
        state_->nativePresentHook->captureEnvelopeTransportValid() &&
        projectedMissingTokenCount == 0 && projectedDuplicateIdentityCount == 0 &&
        projectedFailedPresentCount == 0 && !nativePresentRecords.isEmpty();
    const bool intentIdentityTransportExact =
        nativePresentAuthorityPass && everyIntentIdentityRecordExact;
    const long long captureEnvelopeBeginQpc =
        state_->nativePresentEnvelopeBeginQpc.load(std::memory_order_acquire);
    const long long measurementArmQpc = state_->measurementArmQpc.load(std::memory_order_acquire);
    const long long captureEnvelopeCloseQpc =
        state_->nativePresentEnvelopeCloseQpc.load(std::memory_order_acquire);
    const bool captureEnvelopeLowerClosed = captureEnvelopeBeginQpc > 0 &&
                                            measurementArmQpc >= captureEnvelopeBeginQpc &&
                                            b1MeasurementStartQpc >= measurementArmQpc;
    const bool frozenMeasurementWindowUnchanged =
        frozenMeasurementEndQpc_ > b1MeasurementStartQpc &&
        frozenMeasurementEndQpc_ == b1MeasurementEndQpc;
    const bool captureEnvelopeUpperClosed =
        vblankSuccessor_.completed && vblankSuccessor_.sample.qpc >= frozenMeasurementEndQpc_ &&
        captureEnvelopeCloseQpc >= vblankSuccessor_.sample.qpc;
    const QJsonObject nativePresentHook{
        {"abi_version", static_cast<qint64>(MVM_NATIVE_PRESENT_HOOK_ABI_VERSION)},
        {"composition_token_size", static_cast<qint64>(sizeof(MvmNativePresentCompositionToken))},
        {"native_present_record_size", static_cast<qint64>(sizeof(MvmNativePresentRecord))},
        {"layout_signature", QString::number(mvmNativePresentHookLayoutSignature())},
        {"qt_abi_version_observed",
         static_cast<qint64>(nativePresentSnapshot.observedQtAbiVersion)},
        {"layout_handshake_accepted", nativePresentSnapshot.layoutHandshakeAccepted},
        {"requested_mode", nativePresentHookModeName(config_.nativePresentHook)},
        {"available", nativePresentSnapshot.available},
        {"hook_enabled", config_.nativePresentHook == NativePresentHookMode::OnDiagnostic},
        {"capture_started", nativePresentSnapshot.captureStarted},
        {"capture_stopped", nativePresentSnapshot.captureStopped},
        {"shadow_only", true},
        {"formal_counter_authority_changed", false},
        {"authority_pass", nativePresentAuthorityPass},
        {"record_count", static_cast<qint64>(nativePresentRecords.size())},
        {"overflow_count", static_cast<qint64>(nativePresentSnapshot.overflowCount)},
        {"missing_token_count", projectedMissingTokenCount},
        {"duplicate_token_count", projectedDuplicateIdentityCount},
        {"stale_token_count", static_cast<qint64>(nativePresentSnapshot.staleTokenCount)},
        {"token_set_failure_count", projectedMissingTokenCount},
        {"failed_present_count", projectedFailedPresentCount},
        {"submission_mode", static_cast<qint64>(nativePresentSnapshot.submissionMode)},
        {"configured_maximum_frame_latency",
         static_cast<qint64>(nativePresentSnapshot.configuredMaximumFrameLatency)},
        {"swapchain_maximum_frame_latency",
         static_cast<qint64>(nativePresentSnapshot.swapchainMaximumFrameLatency)},
        {"frame_latency_waitable_object_available",
         nativePresentSnapshot.frameLatencyWaitableObjectAvailable},
        {"dwm_flush_call_count", static_cast<qint64>(nativePresentSnapshot.dwmFlushCallCount)},
        {"dwm_flush_failure_count",
         static_cast<qint64>(nativePresentSnapshot.dwmFlushFailureCount)},
        {"authority_failure", !nativePresentAuthorityPass},
        {"capture_envelope",
         QJsonObject{
             {"schema", "mvm-p2-d5-2-w2-c01-capture-envelope-1"},
             {"enabled",
              state_->nativePresentCaptureEnvelopeEnabled.load(std::memory_order_acquire)},
             {"shadow_only", true},
             {"formal_counter_authority_changed", false},
             {"measurement_window_extended", false},
             {"lower_intent_producer", "formal opportunity scheduler preroll"},
             {"lower_intent_producer_started",
              state_->formalOpportunityEnvelopePrerollStarted.load(std::memory_order_acquire)},
             {"lower_intent_producer_completed_before_measurement",
              state_->formalOpportunityEnvelopePrerollCompleted.load(std::memory_order_acquire)},
             {"begin_qpc", captureEnvelopeBeginQpc},
             {"measurement_arm_qpc", measurementArmQpc},
             {"measurement_start_qpc", b1MeasurementStartQpc},
             {"frozen_measurement_end_qpc", frozenMeasurementEndQpc_},
             {"serialized_measurement_end_qpc", b1MeasurementEndQpc},
             {"successor_wait_completed", vblankSuccessor_.completed},
             {"successor_wait_timeout", vblankSuccessor_.timedOut},
             {"successor_sample_ordinal", vblankSuccessor_.sample.ordinal},
             {"successor_sample_qpc", vblankSuccessor_.sample.qpc},
             {"successor_wait_elapsed_qpc", vblankSuccessor_.waitElapsedQpc},
             {"close_qpc", captureEnvelopeCloseQpc},
             {"lower_closed_before_measurement_arm", captureEnvelopeLowerClosed},
             {"measurement_window_unchanged", frozenMeasurementWindowUnchanged},
             {"upper_closed_after_successor", captureEnvelopeUpperClosed},
             {"record_count", static_cast<qint64>(captureEnvelopeRecords.size())},
             {"overflow_count", static_cast<qint64>(nativePresentSnapshot.overflowCount)},
             {"missing_token_count", static_cast<qint64>(nativePresentSnapshot.missingTokenCount)},
             {"duplicate_token_count",
              static_cast<qint64>(nativePresentSnapshot.duplicateTokenCount)},
             {"stale_token_count", static_cast<qint64>(nativePresentSnapshot.staleTokenCount)},
             {"token_set_failure_count",
              state_->nativePresentTokenSetFailureCount.load(std::memory_order_relaxed)},
             {"authority_pass", nativePresentAuthorityPass && captureEnvelopeLowerClosed &&
                                    frozenMeasurementWindowUnchanged &&
                                    captureEnvelopeUpperClosed && intentScopeAuthorityPass},
         }},
        {"capture_envelope_records", captureEnvelopeRecords},
        {"intent_scope_provenance",
         QJsonObject{
             {"schema", "mvm-p2-d5-2-w2-c23-intent-authority-provenance-3"},
             {"shadow_only", true},
             {"abi_version_unchanged", true},
             {"join_key", "composition_token.token_serial"},
             {"scope_derived_from_present_qpc", false},
             {"scope_derived_from_source_frame", false},
             {"scope_derived_from_layer2_membership", false},
             {"required_intent_set_producer", "formal opportunity scheduler start"},
             {"required_intent_set_derived_from_presented", false},
             {"required_intent_set_exact", formalOpportunitySnapshot.requiredIntentSetExact},
             {"required_intent_ordinals", requiredIntentOrdinalsJson},
             {"duplicate_transport_suppressed_count",
              state_->formalDuplicateTransportSuppressedCount.load(std::memory_order_relaxed)},
             {"outside_required_transport_suppressed_count",
              state_->formalOutsideRequiredTransportSuppressedCount.load(
                  std::memory_order_relaxed)},
             {"record_count", static_cast<qint64>(intentScopeLedgerJson.size())},
             {"missing_scope_count", intentScopeMissingCount},
             {"ambiguous_scope_count", intentScopeAmbiguousCount},
             {"mutation_count", intentScopeMutationCount},
             {"unmatched_scope_count", intentScopeUnmatchedCount},
             {"authority_pass", intentScopeAuthorityPass},
             {"records", intentScopeLedgerJson},
         }},
        {"intent_identity_transport",
         QJsonObject{
             {"schema", "mvm-p2-d5-2-w2-b1-intent-identity-transport-2"},
             {"abi_version", static_cast<qint64>(MVM_NATIVE_PRESENT_HOOK_ABI_VERSION)},
             {"app_abi_version", static_cast<qint64>(MVM_NATIVE_PRESENT_HOOK_ABI_VERSION)},
             {"qt_abi_version_observed",
              static_cast<qint64>(nativePresentSnapshot.observedQtAbiVersion)},
             {"layout_handshake_accepted", nativePresentSnapshot.layoutHandshakeAccepted},
             {"composition_token_size",
              static_cast<qint64>(sizeof(MvmNativePresentCompositionToken))},
             {"native_present_record_size", static_cast<qint64>(sizeof(MvmNativePresentRecord))},
             {"layout_signature", QString::number(mvmNativePresentHookLayoutSignature())},
             {"shadow_only", true},
             {"performance_accounting_connected", false},
             {"formal_mode", formalIntentMode},
             {"record_count", static_cast<qint64>(intentIdentityLedger.size())},
             {"transport_exact", intentIdentityTransportExact},
             {"verdict", intentIdentityTransportExact ? "INTENT_IDENTITY_ABI_V4_TRANSPORT_EXACT"
                                                      : "INTENT_IDENTITY_ABI_V4_TRANSPORT_INVALID"},
             {"records", intentIdentityLedger},
         }},
        {"dirty_propagation",
         QJsonObject{
             {"schema", "mvm-p2-c3-a3-t2-dirty-propagation-1"},
             {"record_count",
              static_cast<qint64>(nativePresentSnapshot.dirtyPropagationRecords.size())},
             {"overflow_count",
              static_cast<qint64>(nativePresentSnapshot.dirtyPropagationOverflowCount)},
             {"duplicate_stage_count",
              static_cast<qint64>(nativePresentSnapshot.dirtyPropagationDuplicateStageCount)},
             {"stage_counts", dirtyStageCounts},
             {"records", dirtyPropagationRecords},
         }},
        {"qt_upstream_tag", "v6.11.1"},
        {"qt_upstream_commit", "59c81a3c2247b821b9b84b4eb8d939b77e07e276"},
        {"qt_source_path", "qtbase/src/gui/rhi/qrhid3d11.cpp"},
        {"hot_path_allocation", false},
        {"hot_path_mutex", false},
        {"hot_path_io", false},
        {"hot_path_logging", false},
        {"records", nativePresentRecords},
    };
    const QString mode = config_.mode == CompositorMode::Playback ? QStringLiteral("playback")
                         : config_.mode == CompositorMode::Seek   ? QStringLiteral("seek")
                                                                  : QStringLiteral("layout");
    const bool measurementDeltaAvailable =
        config_.mode != CompositorMode::Playback || measurementAvailable_;
    const auto measurementJson = [measurementDeltaAvailable](long long value) {
        return measurementDeltaAvailable ? QJsonValue(static_cast<qint64>(value))
                                         : QJsonValue(QJsonValue::Null);
    };
    const auto measurementDoubleJson = [measurementDeltaAvailable](double value) {
        return measurementDeltaAvailable ? QJsonValue(value) : QJsonValue(QJsonValue::Null);
    };
    QJsonObject o{
        {"schema", config_.diagnosticTiming ? "mvm-p2-diagnostic-1" : "mvm-p2-formal-2"},
        {"formal_contract_version", "P2-D5-2"},
        {"mode", mode},
        {"formal_preflight", config_.formalPreflight},
        {"process_id", static_cast<qint64>(GetCurrentProcessId())},
        {"process_exit_code", exitCode_},
        {"configured_seed", static_cast<qint64>(config_.seed)},
        {"configured_warmup_seconds", config_.warmupSeconds},
        {"configured_measure_seconds", config_.measureSeconds},
        {"configured_seek_count", config_.seekCount},
        {"diagnostic_target_rhiitem_pixel_toggle", config_.diagnosticTargetPixelToggle},
        {"configured_measurement_preroll_frames",
         static_cast<qint64>(gpu::kMeasurementPrerollFrames)},
        {"measurement_preroll_ok", measurementPrerollOk_},
        {"measurement_preroll_depth_a", measurementPrerollDepthA_},
        {"measurement_preroll_depth_b", measurementPrerollDepthB_},
        {"measurement_preroll_front_a", measurementPrerollFrontA_},
        {"measurement_preroll_front_b", measurementPrerollFrontB_},
        {"source_a_frame_count", sourceAFrameCount_},
        {"source_b_frame_count", sourceBFrameCount_},
        {"required_measurement_frame_count", requiredMeasurementFrameCount_},
        {"source_coverage_ok", sourceCoverageOk_},
        {"measurement_started", measurementStartCaptured_},
        {"measurement_stop_captured", measurementStopCaptured_},
        {"measurement_available", measurementAvailable_},
        {"measurement_elapsed_seconds", measurementDoubleJson(measureElapsedSeconds_)},
        {"same_device_a", a.decodeDevicePointer == state_->nativeDevicePointer.load()},
        {"same_device_b", b.decodeDevicePointer == state_->nativeDevicePointer.load()},
        {"actual_output_width", state_->actualOutputWidth.load()},
        {"actual_output_height", state_->actualOutputHeight.load()},
        {"actual_gpu_completion_backend",
         QString::fromStdString(state_->actualGpuCompletionBackend)},
        {"adapter_a", QString::fromStdString(a.adapter.description)},
        {"adapter_b", QString::fromStdString(b.adapter.description)},
        {"effective_fps", measurementDoubleJson(measureElapsedSeconds_ > 0
                                                    ? static_cast<double>(measurement.displayed) /
                                                          measureElapsedSeconds_
                                                    : 0)},
        {"drop_rate", measurementDoubleJson(measurement.scheduled > 0
                                                ? static_cast<double>(measurement.dropped) /
                                                      static_cast<double>(measurement.scheduled)
                                                : 0)},
        {"formal_opportunity_authority_valid", formalOpportunitySnapshot.valid &&
                                                   formalOpportunitySnapshot.closed &&
                                                   formalRefreshStable},
        {"formal_opportunity_error", QString::fromLatin1(gpu::presentationOpportunityErrorName(
                                         formalOpportunitySnapshot.error))},
        {"formal_refresh_numerator",
         state_->formalRefreshNumerator.load(std::memory_order_relaxed)},
        {"formal_refresh_denominator",
         state_->formalRefreshDenominator.load(std::memory_order_relaxed)},
        {"formal_source_fps_numerator", 60},
        {"formal_source_fps_denominator", 1},
        {"formal_qpc_frequency", qpcFrequency},
        {"formal_displayed_unique_count", formalOpportunitySnapshot.displayedUnique},
        {"formal_repeated_opportunity_count", formalOpportunitySnapshot.repeated},
        {"formal_gap_true_drop_count", formalOpportunitySnapshot.gapTrueDrop},
        {"tail_true_drop", formalOpportunitySnapshot.tailTrueDrop},
        {"formal_true_opportunity_drop_count", formalOpportunitySnapshot.trueDrop},
        {"formal_forward_reconciliation_count",
         formalOpportunitySnapshot.forwardReconciliationCount},
        {"formal_lost_opportunity_count", formalOpportunitySnapshot.lostOpportunityCount},
        {"formal_superseded_candidate_count", formalOpportunitySnapshot.supersededCandidateCount},
        {"formal_swapped_composition_count", formalOpportunitySnapshot.swappedCompositionCount},
        {"formal_finalized_opportunity_count",
         static_cast<qint64>(formalOpportunitySnapshot.records.size())},
        {"formal_opportunity_anchored", formalOpportunitySnapshot.anchored},
        {"formal_opportunity_origin_refresh_count",
         static_cast<qint64>(formalOpportunitySnapshot.originRefreshCount)},
        {"formal_first_reconciliation_event",
         presentationFirstEventJson(formalOpportunitySnapshot.firstEvent)},
        {"diagnostic_synthetic_deadline_drop_count",
         state_->diagnosticSyntheticDeadlineDropCount.load(std::memory_order_relaxed)},
        {"formal_opportunity_ledger", formalOpportunityLedger},
        {"formal_stop_witness", formalStopWitnessJson},
        {"formal_scheduler_invocation_ledger",
         QJsonObject{
             {"schema", "mvm-p2-d5-2-w4-c2-scheduler-invocation-ledger-1"},
             {"diagnostic_root_cause_capture", formalOpportunitySnapshot.invocationLedgerEnabled},
             {"canonical_performance_authority", false},
             {"physical_vblank_successor_required", false},
             {"physical_mapping_support_authority", false},
             {"measurement_stop_qpc", measurementStop_.qpc},
             {"native_envelope_close_qpc",
              state_->nativePresentEnvelopeCloseQpc.load(std::memory_order_acquire)},
             {"record_count", formalSchedulerInvocationLedger.size()},
             // W4-C3 amend 2。replay入力はscheduler instanceが使用したconfigそのもの。
             {"scheduler_config",
              QJsonObject{
                  {"source_frame_offset",
                   static_cast<qint64>(formalOpportunitySnapshot.config.sourceFrameOffset)},
                  {"source_fps_numerator",
                   static_cast<qint64>(formalOpportunitySnapshot.config.sourceFpsNumerator)},
                  {"source_fps_denominator",
                   static_cast<qint64>(formalOpportunitySnapshot.config.sourceFpsDenominator)},
                  {"refresh_numerator",
                   static_cast<qint64>(formalOpportunitySnapshot.config.refreshNumerator)},
                  {"refresh_denominator",
                   static_cast<qint64>(formalOpportunitySnapshot.config.refreshDenominator)},
                  {"required_frame_count",
                   static_cast<qint64>(formalOpportunitySnapshot.config.requiredFrameCount)}}},
             {"records", formalSchedulerInvocationLedger}}},
        {"measurement_composition_requested_count",
         measurementJson(measurement.compositionRequested)},
        {"measurement_composition_drawn_count", measurementJson(measurement.compositionDrawn)},
        {"measurement_gpu_submission_count", measurementJson(measurement.gpuSubmission)},
        {"measurement_layer_draw_count", measurementJson(measurement.layerDraw)},
        {"measurement_logical_clear_count", measurementJson(measurement.logicalClear)},
        {"measurement_scheduled_output_count", measurementJson(measurement.scheduled)},
        {"measurement_displayed_composition_count", measurementJson(measurement.displayed)},
        {"measurement_dropped_output_count", measurementJson(measurement.dropped)},
        {"measurement_missing_pair_count", measurementJson(measurement.missingPair)},
        {"measurement_source_a_eof_count", measurementJson(measurement.sourceAEof)},
        {"measurement_source_b_eof_count", measurementJson(measurement.sourceBEof)},
        {"measurement_first_output_frame",
         measurementJson(state_->measurementFirstOutputFrame.load())},
        {"measurement_drop_scheduler_deadline", measurementJson(measurement.dropSchedulerDeadline)},
        {"measurement_drop_missing_source_a", measurementJson(measurement.dropMissingSourceA)},
        {"measurement_drop_missing_source_b", measurementJson(measurement.dropMissingSourceB)},
        {"measurement_drop_missing_both", measurementJson(measurement.dropMissingBoth)},
        {"measurement_drop_stale_generation", measurementJson(measurement.dropStaleGeneration)},
        {"measurement_drop_future_generation", measurementJson(measurement.dropFutureGeneration)},
        {"measurement_drop_stale_composition_epoch",
         measurementJson(measurement.dropStaleCompositionEpoch)},
        {"measurement_drop_render_failure", measurementJson(measurement.dropRenderFailure)},
        {"measurement_present_callback_count", measurementJson(measurement.presentCallback)},
        {"measurement_repeated_present_count", measurementJson(measurement.repeatedPresent)},
        {"measurement_partial_gpu_issue_failure_count",
         measurementJson(measurement.partialGpuIssueFailure)},
        {"measurement_completion_poll_failure_count",
         measurementJson(measurement.completionPollFailure)},
        {"measurement_untracked_submission_count",
         measurementJson(measurement.untrackedSubmission)},
        {"scheduled_output_count", measurementJson(measurement.scheduled)},
        {"displayed_composition_count", measurementJson(measurement.displayed)},
        {"decoded_a_count", a.decodedFrameCount},
        {"decoded_b_count", b.decodedFrameCount},
        {"source_a_software_frame_reject_count", a.softwareFrameRejectCount},
        {"source_b_software_frame_reject_count", b.softwareFrameRejectCount},
        {"software_fallback_count", a.softwareFrameRejectCount + b.softwareFrameRejectCount},
        {"worker_join_leak_count", (!a.joined ? 1 : 0) + (!b.joined ? 1 : 0)},
        {"paired_count", measurementJson(measurement.compositionRequested)},
        {"composition_submitted_count", measurementJson(measurement.gpuSubmission)},
        {"composition_displayed_count", measurementJson(measurement.displayed)},
        {"present_callback_count", measurementJson(measurement.presentCallback)},
        {"dropped_output_count", measurementJson(measurement.dropped)},
        {"scheduler_deadline_drop_count", measurementJson(measurement.dropSchedulerDeadline)},
        {"missing_pair_drop_count", state_->missingPairDropCount.load()},
        {"missing_source_a_drop_count", state_->missingSourceADropCount.load()},
        {"missing_source_b_drop_count", state_->missingSourceBDropCount.load()},
        {"repeated_present_count", measurementJson(measurement.repeatedPresent)},
        {"pending_pair_count", static_cast<qint64>(a.bufferDepth + b.bufferDepth)},
        {"retired_not_completed", static_cast<qint64>(c.retirementDepthAfterDrain)},
        {"mixed_source_frame_count", state_->coordinator.mixedSourceFrameCount()},
        {"mixed_generation_count", state_->coordinator.mixedGenerationCount()},
        {"stale_composition_epoch_count", state_->coordinator.staleCompositionEpochCount()},
        {"dual_seek_decode_ready_ms", doubles(seekDecodeReadyMs_)},
        {"dual_seek_displayed_ms", doubles(seekDisplayedMs_)},
        {"dual_seek_displayed_p95_ms", p95},
        {"dual_seek_displayed_observed_max_ms", observedMax},
        {"seek_display_mismatch", seekMismatch_},
        {"seek_timeout_count", seekTimeout_},
        {"seek_stale_completion_count", seekStaleCompletion_},
        {"seek_busy_acceptance_count", 0},
        {"seek_completion_publish_reject_count",
         seekA.completionPublishRejectCount + seekB.completionPublishRejectCount},
        {"seek_completion_request_mismatch_count",
         seekA.completionRequestMismatchCount + seekB.completionRequestMismatchCount},
        {"seek_completion_stopped_superseded_count",
         seekA.completionStoppedSupersededCount + seekB.completionStoppedSupersededCount},
        {"parallel_dispatch_valid_count", parallelDispatchValidCount},
        {"execution_overlap_count", overlapCount},
        {"execution_nonoverlap_count",
         static_cast<qint64>(seekConcurrencySamples_.size()) - overlapCount},
        {"seek_concurrency_samples", concurrencySamples},
        {"layout_epoch_mismatch", layoutMismatch_},
        {"cpu_full_frame_readback_count", state_->readbacks.fullFrameReadbacks()},
        {"marker_band_readback_count", state_->readbacks.markerBandReadbacks()},
        {"output_probe_readback_count", state_->readbacks.outputProbeReadbacks()},
        {"marker_a_checked_count", state_->markerAChecked.load()},
        {"marker_b_checked_count", state_->markerBChecked.load()},
        {"marker_a_mismatch", state_->markerAMismatch.load()},
        {"marker_b_mismatch", state_->markerBMismatch.load()},
        {"gpu_submission_count", c.gpuSubmissionCount},
        {"untracked_submission_count", c.untrackedSubmissionCount},
        {"completion_poll_failure_count", c.completionPollFailureCount},
        {"retirement_depth_peak", static_cast<qint64>(c.retirementDepthPeak)},
        {"retirement_depth_after_drain", static_cast<qint64>(c.retirementDepthAfterDrain)},
        {"payloads_released_before_completion", c.payloadsReleasedBeforeCompletion},
        {"retirement_timeout_count", c.retirementTimeoutCount},
        {"device_lost_count", state_->deviceLostCount.load()},
        {"lifecycle_order_violation_count", state_->lifecycleOrderViolationCount.load()},
        {"teardown_success", state_->teardownComplete.load()},
        {"final_report_written_after_teardown", state_->teardownComplete.load()},
        {"gpu_passes_per_composition", measurement.displayed > 0
                                           ? static_cast<double>(measurement.gpuSubmission) /
                                                 static_cast<double>(measurement.displayed)
                                           : 0.0},
        {"full_frame_gpu_copy_count", c.fullFrameGpuCopyCount},
        {"logical_clear_count", measurement.logicalClear},
        {"actual_target_probe_checked_count", state_->actualTargetProbeChecked.load()},
        {"actual_target_probe_mismatch", state_->actualTargetProbeMismatch.load()},
        {"partial_gpu_issue_failure_count", c.partialGpuIssueFailureCount},
        {"compose_after_fatal_rejected_count", c.composeAfterFatalRejectedCount},
        {"shutdown_reason", shutdownReason_}};
    const HWND targetHwnd =
        item_ && item_->window() ? reinterpret_cast<HWND>(item_->window()->winId()) : nullptr;
    const auto rawEnvironment = [](const char* name) -> QJsonValue {
        return qEnvironmentVariableIsSet(name) ? QJsonValue(QString::fromUtf8(qgetenv(name)))
                                               : QJsonValue(QJsonValue::Null);
    };
    {
        PresentationEligibilityPreflight preflight;
        const bool captured =
            state_ && state_->eligibilityPreflightCaptured.load(std::memory_order_acquire);
        if (captured) {
            std::lock_guard<std::mutex> lock(state_->eligibilityPreflightMutex);
            preflight = state_->eligibilityPreflight;
        }
        const auto hex = [](std::uint64_t value) {
            return QStringLiteral("0x%1").arg(static_cast<qulonglong>(value), 0, 16);
        };
        QJsonObject swapchain{
            {"available", preflight.swapchain_desc_available},
            {"identity", hex(preflight.swapchain_identity)},
            {"width", static_cast<qint64>(preflight.width)},
            {"height", static_cast<qint64>(preflight.height)},
            {"format", static_cast<qint64>(preflight.format)},
            {"stereo", preflight.stereo},
            {"sample_count", static_cast<qint64>(preflight.sample_count)},
            {"sample_quality", static_cast<qint64>(preflight.sample_quality)},
            {"buffer_usage", static_cast<qint64>(preflight.buffer_usage)},
            {"buffer_count", static_cast<qint64>(preflight.buffer_count)},
            {"scaling", static_cast<qint64>(preflight.scaling)},
            {"swap_effect", static_cast<qint64>(preflight.swap_effect)},
            {"alpha_mode", static_cast<qint64>(preflight.alpha_mode)},
            {"flags", static_cast<qint64>(preflight.flags)},
            {"frame_latency_waitable_object", preflight.frame_latency_waitable_object},
            {"maximum_frame_latency_available", preflight.maximum_frame_latency_available},
            {"maximum_frame_latency", static_cast<qint64>(preflight.maximum_frame_latency)}};
        QJsonObject adapter{{"available", preflight.adapter_available},
                            {"luid_low", static_cast<qint64>(preflight.adapter_luid_low)},
                            {"luid_high", static_cast<qint64>(preflight.adapter_luid_high)},
                            {"description", QString::fromStdString(preflight.adapter_description)}};
        QJsonObject output{{"available", preflight.output_available},
                           {"monitor_handle", hex(preflight.monitor_handle)},
                           {"device_name", QString::fromStdString(preflight.output_device_name)},
                           {"desktop_left", preflight.desktop_left},
                           {"desktop_top", preflight.desktop_top},
                           {"desktop_right", preflight.desktop_right},
                           {"desktop_bottom", preflight.desktop_bottom},
                           {"attached_to_desktop", preflight.attached_to_desktop}};
        QJsonObject window{{"available", preflight.window_available},
                           {"handle", hex(preflight.window_handle)},
                           {"style", static_cast<qint64>(preflight.window_style)},
                           {"ex_style", static_cast<qint64>(preflight.window_ex_style)},
                           {"cloaked_available", preflight.cloaked_available},
                           {"cloaked", static_cast<qint64>(preflight.cloaked)},
                           {"window_left", preflight.window_left},
                           {"window_top", preflight.window_top},
                           {"window_right", preflight.window_right},
                           {"window_bottom", preflight.window_bottom},
                           {"client_width", preflight.client_width},
                           {"client_height", preflight.client_height}};
        // capability は eligibility の説明変数であり、その Present が
        // independent flip / MPO されたという証拠ではない。actual presentation path は
        // PresentMon/ETW 側の PresentMode / DisplayedQPC provenance で判定する。
        QJsonObject capability{{"tearing_support_available", preflight.tearing_support_available},
                               {"tearing_supported", preflight.tearing_supported},
                               {"hardware_composition_support_available",
                                preflight.hardware_composition_support_available},
                               {"hardware_composition_support_flags",
                                static_cast<qint64>(preflight.hardware_composition_support_flags)}};
        o.insert("presentation_eligibility_preflight",
                 QJsonObject{{"schema", "mvm-p2-c3-a3-t2-d1b0-eligibility-preflight-1"},
                             {"authority", "diagnostic_only"},
                             {"is_presentation_path_authority", false},
                             {"captured", preflight.captured},
                             {"error", QString::fromStdString(preflight.error)},
                             {"swapchain", swapchain},
                             {"adapter", adapter},
                             {"output", output},
                             {"window", window},
                             {"capability", capability}});
    }
    o.insert(
        "t2_preflight",
        QJsonObject{
            {"target_hwnd",
             QStringLiteral("0x%1").arg(
                 static_cast<qulonglong>(reinterpret_cast<std::uintptr_t>(targetHwnd)), 0, 16)},
            {"gwl_exstyle_raw", targetHwnd ? QString::number(static_cast<qulonglong>(
                                                 GetWindowLongPtrW(targetHwnd, GWL_EXSTYLE)))
                                           : QString()},
            {"QT_QPA_DISABLE_REDIRECTION_SURFACE",
             rawEnvironment("QT_QPA_DISABLE_REDIRECTION_SURFACE")},
            {"QT_D3D_NO_FLIP", rawEnvironment("QT_D3D_NO_FLIP")},
            {"QT_D3D_MAX_FRAME_LATENCY", rawEnvironment("QT_D3D_MAX_FRAME_LATENCY")},
            {"QSG_NO_VSYNC", rawEnvironment("QSG_NO_VSYNC")},
        });
    const long long decodedADelta =
        measurementStopA_.decodedFrameCount - measurementStartA_.decodedFrameCount;
    const long long decodedBDelta =
        measurementStopB_.decodedFrameCount - measurementStartB_.decodedFrameCount;
    const long long waitADelta =
        measurementStopA_.backpressureWaitCount - measurementStartA_.backpressureWaitCount;
    const long long waitBDelta =
        measurementStopB_.backpressureWaitCount - measurementStartB_.backpressureWaitCount;
    o.insert("diagnostic_case", diagnosticCaseName(config_.diagnosticCase));
    o.insert("diagnostic_timing", config_.diagnosticTiming);
    if (config_.schedulerPhaseRing) {
        o.insert("diagnostic_scheduler_phase_ring", true);
        o.insert("scheduler_phase_attribution", schedulerPhaseAttribution);
    }
    if (config_.presentationOpportunityRing) {
        o.insert("diagnostic_presentation_opportunity_ring", true);
        o.insert("presentation_opportunity", presentationOpportunity);
    }
    if (config_.nativePresentHook != NativePresentHookMode::Disabled)
        o.insert("native_present_hook", nativePresentHook);
    o.insert("effective_pair_rate",
             measureElapsedSeconds_ > 0
                 ? static_cast<double>(measurement.displayed) / measureElapsedSeconds_
                 : 0.0);
    o.insert("deadline_drop_rate", measurement.scheduled > 0
                                       ? static_cast<double>(measurement.dropSchedulerDeadline) /
                                             static_cast<double>(measurement.scheduled)
                                       : 0.0);
    o.insert("measurement_decoded_a_count", decodedADelta);
    o.insert("measurement_decoded_b_count", decodedBDelta);
    o.insert("measurement_wait_for_space_a_count", waitADelta);
    o.insert("measurement_wait_for_space_b_count", waitBDelta);
    o.insert("measurement_buffer_depth_a_start",
             static_cast<qint64>(measurementStartA_.bufferDepth));
    o.insert("measurement_buffer_depth_a_end", static_cast<qint64>(measurementStopA_.bufferDepth));
    o.insert("measurement_buffer_depth_b_start",
             static_cast<qint64>(measurementStartB_.bufferDepth));
    o.insert("measurement_buffer_depth_b_end", static_cast<qint64>(measurementStopB_.bufferDepth));
    o.insert("render_stage_timings", renderStages);
    o.insert("d3d11_lock_timings", lockTimings);
    o.insert("seek_stage_timings", seekStages);
    QSaveFile file(config_.metricsPath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return file.commit();
}

} // namespace mvm::app

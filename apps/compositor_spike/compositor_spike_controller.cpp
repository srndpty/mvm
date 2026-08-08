#include "compositor_spike_controller.h"

#include "media/gpu_preview/qpc_clock.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <random>

namespace mvm::app {
namespace {
std::vector<gpu::LayerLayout> layoutFor(size_t index) {
    const bool topLeft = index == 1 || index == 2;
    const float opacity = index < 2 ? 0.75f : 0.5f;
    return {{gpu::SourceId{1}, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0f, 0},
            {gpu::SourceId{2}, {topLeft ? 0.0f : 0.5f, topLeft ? 0.0f : 0.5f, 0.5f, 0.5f},
             {0, 0, 1, 1}, opacity, 1}};
}

QJsonArray doubles(const std::vector<double>& values) {
    QJsonArray out;
    for (double value : values)
        out.append(value);
    return out;
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
    phaseTimer_.start();
    timer_.start();
}

bool CompositorSpikeController::startWorkers() {
    workerA_ = std::make_shared<gpu::SourceDecodeWorker>(gpu::SourceId{1}, state_->device,
                                                        state_->readbacks, 16);
    workerB_ = std::make_shared<gpu::SourceDecodeWorker>(gpu::SourceId{2}, state_->device,
                                                        state_->readbacks, 16);
    std::string err;
    if (!workerA_->start(config_.sourceA.toUtf8().constData(), err) ||
        !workerB_->start(config_.sourceB.toUtf8().constData(), err)) {
        beginShutdown(QString::fromStdString(err), true);
        return false;
    }
    // open時点の設定値ではなく、actual decode textureが出た後のGetDevice結果を照合する。
    double initialAMs = 0;
    double initialBMs = 0;
    if (!workerA_->seekBlocking(0, initialAMs, err) ||
        !workerB_->seekBlocking(0, initialBMs, err)) {
        beginShutdown(QString::fromStdString(err), true);
        return false;
    }
    const auto a = workerA_->snapshot();
    const auto b = workerB_->snapshot();
    if (a.decodeDevicePointer != state_->nativeDevicePointer.load() ||
        b.decodeDevicePointer != state_->nativeDevicePointer.load() ||
        !a.adapter.sameAdapterAs(state_->qtAdapter) || !b.adapter.sameAdapterAs(state_->qtAdapter)) {
        beginShutdown(QStringLiteral("A/B decode texture deviceがQt deviceと一致しません"), true);
        return false;
    }
    if (state_->coordinator.configure(layoutFor(0),
                                      {{gpu::SourceId{1}, a.sourceGeneration},
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
    if (config_.testFault == "device_change")
        state_->testDeviceChange.store(true);
    else if (config_.testFault == "completion_fatal")
        state_->compositor.setTestFaults(
            {gpu::GpuCompositorInitializeFault::None, -1, true});
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
    }
    if (config_.formalPreflight && config_.mode != CompositorMode::Layout) {
        phase_ = Phase::MarkerStart;
    } else if (config_.mode == CompositorMode::Seek) {
        phase_ = Phase::SeekStart;
    } else {
        workerA_->play();
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

void CompositorSpikeController::tick() {
    if (!item_ || phase_ == Phase::Done)
        return;
    if (state_->fatal.load() && phase_ != Phase::ShutdownWait) {
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
    } else if (phase_ == Phase::Warmup &&
               phaseTimer_.elapsed() >= config_.warmupSeconds * 1000) {
        state_->measurementStartRequested.store(true, std::memory_order_release);
        state_->measurementStartCaptured.store(false, std::memory_order_release);
        phase_ = Phase::MeasureStartWait;
        item_->update();
    } else if (phase_ == Phase::MeasureStartWait) {
        if (state_->measurementStartCaptured.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(state_->measurementMutex);
            measurementStart_ = state_->measurementStart;
            phase_ = Phase::Measure;
        } else {
            item_->update();
        }
    } else if (phase_ == Phase::Measure &&
               gpu::qpcMsBetween(measurementStart_.qpc, gpu::qpcTicks()) >=
                   config_.measureSeconds * 1000.0) {
        state_->measurementStopRequested.store(true, std::memory_order_release);
        state_->measurementStopCaptured.store(false, std::memory_order_release);
        phase_ = Phase::MeasureStopWait;
        item_->update();
    } else if (phase_ == Phase::MeasureStopWait) {
        if (state_->measurementStopCaptured.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> lock(state_->measurementMutex);
                measurementStop_ = state_->measurementStop;
            }
            measureElapsedSeconds_ =
                gpu::qpcMsBetween(measurementStart_.qpc, measurementStop_.qpc) / 1000.0;
            beginShutdown(QStringLiteral("playback measurement完了"), false);
        } else {
            item_->update();
        }
    } else if (phase_ == Phase::SeekStart) {
        startSeek();
    } else if (phase_ == Phase::SeekWait) {
        pollSeek();
    } else if (phase_ == Phase::LayoutStart) {
        startLayoutChange();
    } else if (phase_ == Phase::LayoutWait) {
        pollLayoutChange();
    } else if (phase_ == Phase::ShutdownWait) {
        item_->update();
        if (state_->teardownComplete.load()) {
            if (!writeMetrics())
                exitCode_ = 6;
            phase_ = Phase::Done;
            timer_.stop();
            Q_EMIT finished();
        } else if (phaseTimer_.elapsed() > 15000) {
            exitCode_ = 6;
            phase_ = Phase::Done;
            timer_.stop();
            Q_EMIT finished();
        }
    }
}

void CompositorSpikeController::startSeek() {
    if (seekIndex_ >= seekTargets_.size()) {
        beginShutdown(QStringLiteral("dual seek完了"), seekMismatch_ != 0 || seekTimeout_ != 0);
        return;
    }
    const long long target = seekTargets_[seekIndex_];
    waitBaseline_ = state_->ledger.baseline();
    seekRequestStartQpc_ = gpu::qpcTicks();
    waitTimer_.restart();
    double aMs = 0, bMs = 0;
    std::string err;
    if (!workerA_->seekBlocking(target, aMs, err) || !workerB_->seekBlocking(target, bMs, err)) {
        ++seekMismatch_;
        beginShutdown(QString::fromStdString(err), true);
        return;
    }
    seekDecodeReadyMs_.push_back(gpu::qpcMsBetween(seekRequestStartQpc_, gpu::qpcTicks()));
    const auto a = workerA_->snapshot();
    const auto b = workerB_->snapshot();
    state_->coordinator.setSourceGeneration({1}, a.sourceGeneration);
    state_->coordinator.setSourceGeneration({2}, b.sourceGeneration);
    waitExpectation_ = {target, state_->coordinator.compositionEpoch(),
                        {{{1}, a.sourceGeneration, a.resourceEpoch, target},
                         {{2}, b.sourceGeneration, b.resourceEpoch, target}}};
    state_->requestedOutput.store(target);
    state_->scheduledOutputCount.fetch_add(1);
    item_->update();
    phase_ = Phase::SeekWait;
}

void CompositorSpikeController::pollSeek() {
    gpu::CompositionDisplayRecord found;
    if (state_->ledger.findAfter(waitBaseline_, waitExpectation_, found)) {
        seekDisplayedMs_.push_back(
            gpu::qpcMsBetween(seekRequestStartQpc_, found.displayedQpc));
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
    if (phase_ == Phase::ShutdownWait || phase_ == Phase::Done)
        return;
    shutdownReason_ = reason;
    if (failure)
        exitCode_ = 3;
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
    const auto& c = state_->compositor.counters();
    CompositorMeasurementCounters measurement;
    if (config_.mode == CompositorMode::Playback) {
        measurement = subtract(measurementStop_, measurementStart_);
    } else {
        measurement.compositionRequested = c.compositionRequestedCount;
        measurement.compositionDrawn = c.compositionDrawnCount;
        measurement.gpuSubmission = c.gpuSubmissionCount;
        measurement.layerDraw = c.layerDrawCount;
        measurement.logicalClear = state_->logicalClearCount.load();
        measurement.scheduled = state_->scheduledOutputCount.load();
        measurement.displayed = state_->displayedCompositionCount.load();
        measurement.dropped = state_->droppedOutputCount.load();
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
    const double p95 = sorted.empty()
                           ? -1.0
                           : sorted[static_cast<size_t>(
                                 std::ceil(static_cast<double>(sorted.size()) * 0.95) - 1)];
    const double observedMax = sorted.empty() ? -1.0 : sorted.back();
    const QString mode = config_.mode == CompositorMode::Playback
                             ? QStringLiteral("playback")
                             : config_.mode == CompositorMode::Seek ? QStringLiteral("seek")
                                                                    : QStringLiteral("layout");
    QJsonObject o{{"schema", "mvm-p2-formal-1"},
                  {"formal_contract_version", "P2-D1-1"},
                  {"mode", mode},
                  {"formal_preflight", config_.formalPreflight},
                  {"process_exit_code", exitCode_},
                  {"configured_seed", static_cast<qint64>(config_.seed)},
                  {"configured_warmup_seconds", config_.warmupSeconds},
                  {"configured_measure_seconds", config_.measureSeconds},
                  {"configured_seek_count", config_.seekCount},
                  {"measurement_elapsed_seconds", measureElapsedSeconds_},
                  {"same_device_a", a.decodeDevicePointer == state_->nativeDevicePointer.load()},
                  {"same_device_b", b.decodeDevicePointer == state_->nativeDevicePointer.load()},
                  {"actual_output_width", state_->actualOutputWidth.load()},
                  {"actual_output_height", state_->actualOutputHeight.load()},
                  {"actual_gpu_completion_backend",
                   QString::fromStdString(state_->actualGpuCompletionBackend)},
                  {"adapter_a", QString::fromStdString(a.adapter.description)},
                  {"adapter_b", QString::fromStdString(b.adapter.description)},
                  {"effective_fps", measureElapsedSeconds_ > 0
                                        ? static_cast<double>(measurement.displayed) /
                                              measureElapsedSeconds_
                                        : 0},
                  {"drop_rate", measurement.scheduled > 0
                                    ? static_cast<double>(measurement.dropped) /
                                          static_cast<double>(measurement.scheduled)
                                    : 0},
                  {"measurement_composition_requested_count",
                   measurement.compositionRequested},
                  {"measurement_composition_drawn_count", measurement.compositionDrawn},
                  {"measurement_gpu_submission_count", measurement.gpuSubmission},
                  {"measurement_layer_draw_count", measurement.layerDraw},
                  {"measurement_logical_clear_count", measurement.logicalClear},
                  {"measurement_scheduled_output_count", measurement.scheduled},
                  {"measurement_displayed_composition_count", measurement.displayed},
                  {"measurement_dropped_output_count", measurement.dropped},
                  {"measurement_drop_scheduler_deadline",
                   measurement.dropSchedulerDeadline},
                  {"measurement_drop_missing_source_a", measurement.dropMissingSourceA},
                  {"measurement_drop_missing_source_b", measurement.dropMissingSourceB},
                  {"measurement_drop_missing_both", measurement.dropMissingBoth},
                  {"measurement_drop_stale_generation", measurement.dropStaleGeneration},
                  {"measurement_drop_future_generation", measurement.dropFutureGeneration},
                  {"measurement_drop_stale_composition_epoch",
                   measurement.dropStaleCompositionEpoch},
                  {"measurement_drop_render_failure", measurement.dropRenderFailure},
                  {"measurement_present_callback_count", measurement.presentCallback},
                  {"measurement_repeated_present_count", measurement.repeatedPresent},
                  {"measurement_partial_gpu_issue_failure_count",
                   measurement.partialGpuIssueFailure},
                  {"measurement_completion_poll_failure_count",
                   measurement.completionPollFailure},
                  {"measurement_untracked_submission_count",
                   measurement.untrackedSubmission},
                  {"scheduled_output_count", measurement.scheduled},
                  {"displayed_composition_count", measurement.displayed},
                  {"decoded_a_count", a.decodedFrameCount},
                  {"decoded_b_count", b.decodedFrameCount},
                  {"paired_count", measurement.compositionRequested},
                  {"composition_submitted_count", measurement.gpuSubmission},
                  {"composition_displayed_count", measurement.displayed},
                  {"present_callback_count", measurement.presentCallback},
                  {"dropped_output_count", measurement.dropped},
                  {"scheduler_deadline_drop_count", measurement.dropSchedulerDeadline},
                  {"missing_pair_drop_count", state_->missingPairDropCount.load()},
                  {"missing_source_a_drop_count", state_->missingSourceADropCount.load()},
                  {"missing_source_b_drop_count", state_->missingSourceBDropCount.load()},
                  {"repeated_present_count", measurement.repeatedPresent},
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
                  {"gpu_passes_per_composition",
                   measurement.displayed > 0
                       ? static_cast<double>(measurement.gpuSubmission) /
                             static_cast<double>(measurement.displayed)
                       : 0.0},
                  {"full_frame_gpu_copy_count", c.fullFrameGpuCopyCount},
                  {"logical_clear_count", measurement.logicalClear},
                  {"actual_target_probe_checked_count",
                   state_->actualTargetProbeChecked.load()},
                  {"actual_target_probe_mismatch", state_->actualTargetProbeMismatch.load()},
                  {"partial_gpu_issue_failure_count", c.partialGpuIssueFailureCount},
                  {"compose_after_fatal_rejected_count", c.composeAfterFatalRejectedCount},
                  {"shutdown_reason", shutdownReason_}};
    QSaveFile file(config_.metricsPath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return file.commit();
}

} // namespace mvm::app

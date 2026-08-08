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
    if (phase_ == Phase::Warmup && phaseTimer_.elapsed() >= config_.warmupSeconds * 1000) {
        displayedAtMeasureStart_ = state_->displayedCompositionCount.load();
        droppedAtMeasureStart_ = state_->droppedOutputCount.load();
        scheduledAtMeasureStart_ = state_->scheduledOutputCount.load();
        clearsAtMeasureStart_ = state_->logicalClearCount.load();
        presentsAtMeasureStart_ = state_->presentCallbackCount.load();
        phase_ = Phase::Measure;
        phaseTimer_.restart();
    } else if (phase_ == Phase::Measure &&
               phaseTimer_.elapsed() >= config_.measureSeconds * 1000) {
        measureElapsedSeconds_ = static_cast<double>(phaseTimer_.elapsed()) / 1000.0;
        beginShutdown(QStringLiteral("playback smoke完了"), false);
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
    const long long displayed = config_.mode == CompositorMode::Playback
                                    ? state_->displayedCompositionCount.load() - displayedAtMeasureStart_
                                    : state_->displayedCompositionCount.load();
    const long long scheduled = config_.mode == CompositorMode::Playback
                                    ? state_->scheduledOutputCount.load() - scheduledAtMeasureStart_
                                    : state_->scheduledOutputCount.load();
    const long long dropped = config_.mode == CompositorMode::Playback
                                  ? state_->droppedOutputCount.load() - droppedAtMeasureStart_
                                  : state_->droppedOutputCount.load();
    std::vector<double> sorted = seekDisplayedMs_;
    std::sort(sorted.begin(), sorted.end());
    const double p95 = sorted.empty()
                           ? -1.0
                           : sorted[static_cast<size_t>(
                                 std::ceil(static_cast<double>(sorted.size()) * 0.95) - 1)];
    const double observedMax = sorted.empty() ? -1.0 : sorted.back();
    QJsonObject o{{"same_device_a", a.decodeDevicePointer == state_->nativeDevicePointer.load()},
                  {"same_device_b", b.decodeDevicePointer == state_->nativeDevicePointer.load()},
                  {"adapter_a", QString::fromStdString(a.adapter.description)},
                  {"adapter_b", QString::fromStdString(b.adapter.description)},
                  {"effective_fps", measureElapsedSeconds_ > 0
                                        ? static_cast<double>(displayed) / measureElapsedSeconds_
                                        : 0},
                  {"drop_rate", scheduled > 0
                                    ? static_cast<double>(dropped) /
                                          static_cast<double>(scheduled)
                                    : 0},
                  {"scheduled_output_count", scheduled},
                  {"displayed_composition_count", displayed},
                  {"decoded_a_count", a.decodedFrameCount},
                  {"decoded_b_count", b.decodedFrameCount},
                  {"paired_count", c.compositionRequestedCount},
                  {"composition_submitted_count", c.gpuSubmissionCount},
                  {"composition_displayed_count", displayed},
                  {"present_callback_count",
                   config_.mode == CompositorMode::Playback
                       ? state_->presentCallbackCount.load() - presentsAtMeasureStart_
                       : state_->presentCallbackCount.load()},
                  {"dropped_output_count", dropped},
                  {"scheduler_deadline_drop_count", state_->schedulerDeadlineDropCount.load()},
                  {"missing_pair_drop_count", state_->missingPairDropCount.load()},
                  {"missing_source_a_drop_count", state_->missingSourceADropCount.load()},
                  {"missing_source_b_drop_count", state_->missingSourceBDropCount.load()},
                  {"repeated_present_count", state_->repeatedPresentCount.load()},
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
                  {"gpu_passes_per_composition", displayed > 0 ? 1 : 0},
                  {"full_frame_gpu_copy_count", c.fullFrameGpuCopyCount},
                  {"logical_clear_count", config_.mode == CompositorMode::Playback
                                                ? state_->logicalClearCount.load() - clearsAtMeasureStart_
                                                : state_->logicalClearCount.load()},
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

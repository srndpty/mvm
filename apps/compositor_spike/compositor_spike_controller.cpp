#include "compositor_spike_controller.h"

#include "media/gpu_preview/measurement_preroll.h"
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

double nearestRank(const std::vector<double>& sorted, double percentile) {
    if (sorted.empty())
        return -1.0;
    const size_t index = static_cast<size_t>(
        std::ceil(static_cast<double>(sorted.size()) * percentile) - 1.0);
    return sorted[std::min(index, sorted.size() - 1)];
}

QJsonObject distribution(const std::vector<double>& values, const char* valuesField) {
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0.0;
    for (double value : values)
        sum += value;
    QJsonObject result{{"count", static_cast<qint64>(values.size())},
                       {"mean", values.empty()
                                    ? -1.0
                                    : sum / static_cast<double>(values.size())},
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
        state_->coordinator.configure(layoutFor(0),
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
    if (fixed) {
        gpu::DecodedGpuFrame frameA;
        gpu::DecodedGpuFrame frameB;
        if (!workerA_->buffer().takeExact(0, frameA) ||
            !workerB_->buffer().takeExact(0, frameB)) {
            beginShutdown(QStringLiteral("fixed texture診断用frameを取得できません"), true);
            return false;
        }
        state_->diagnosticFixedFrame =
            {0,
             state_->coordinator.compositionEpoch(),
             {{std::move(frameA), {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0f, 0},
              {std::move(frameB), {0.5f, 0.5f, 0.5f, 0.5f}, {0, 0, 1, 1}, 0.75f, 1}}};
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
    const bool validA = workerA_->buffer().peekFrontIdentity(frontA) &&
                        frontA.frameNumber == 0 && frontA.sourceGeneration == a.sourceGeneration;
    const bool validB = !workerB_ ||
                        (workerB_->buffer().peekFrontIdentity(frontB) &&
                         frontB.frameNumber == 0 &&
                         frontB.sourceGeneration == b.sourceGeneration);
    if (!validA || !validB) {
        beginShutdown(QStringLiteral("測定開始reset後のsource buffer先頭がframe 0ではありません"),
                      true);
        return false;
    }
    return true;
}

void CompositorSpikeController::requestMeasurementStart() {
    state_->measurementFirstOutputFrame.store(-1, std::memory_order_release);
    state_->measurementDurationQpc.store(
        static_cast<long long>(gpu::qpcFrequency()) * config_.measureSeconds,
        std::memory_order_release);
    state_->measurementStartRequested.store(true, std::memory_order_release);
    state_->measurementStartCaptured.store(false, std::memory_order_release);
    state_->measurementStopRequested.store(false, std::memory_order_release);
    state_->measurementStopCaptured.store(false, std::memory_order_release);
    phase_ = Phase::MeasureStartWait;
    item_->update();
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
        const gpu::MeasurementPrerollSourceState stateA{
            workerA_->buffer().depth(), hasA, frontA, a.sourceGeneration, a.eof, a.fatal};
        const gpu::MeasurementPrerollSourceState stateB{
            workerB_->buffer().depth(), hasB, frontB, b.sourceGeneration, b.eof, b.fatal};
        const auto result = gpu::evaluateMeasurementPreroll(
            stateA, stateB,
            state_->playbackSchedulerEnabled.load(std::memory_order_acquire),
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
    } else if (phase_ == Phase::MeasureStartWait) {
        if (state_->measurementStartCaptured.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(state_->measurementMutex);
            measurementStart_ = state_->measurementStart;
            measurementStartA_ = workerA_ ? workerA_->snapshot() : gpu::SourceDecoderSnapshot{};
            measurementStartB_ = workerB_ ? workerB_->snapshot() : gpu::SourceDecoderSnapshot{};
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
            measurementStopA_ = workerA_ ? workerA_->snapshot() : gpu::SourceDecoderSnapshot{};
            measurementStopB_ = workerB_ ? workerB_->snapshot() : gpu::SourceDecoderSnapshot{};
            beginShutdown(QStringLiteral("playback measurement完了"), false);
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
        beginShutdown(QStringLiteral("dual seek完了"),
                      seekMismatch_ != 0 || seekTimeout_ != 0 || seekStaleCompletion_ != 0);
        return;
    }
    const long long target = seekTargets_[seekIndex_];
    waitBaseline_ = state_->ledger.baseline();
    seekRequestStartQpc_ = gpu::qpcTicks();
    waitTimer_.restart();
    std::string err;
    seekAReady_ = false;
    seekBReady_ = false;
    const auto aResult = workerA_->requestSeek(target, seekTicketA_, err);
    const auto bResult = workerB_->requestSeek(target, seekTicketB_, err);
    if (aResult != gpu::SeekRequestResult::Accepted ||
        bResult != gpu::SeekRequestResult::Accepted) {
        ++seekMismatch_;
        beginShutdown(QString::fromStdString(err), true);
        return;
    }
    phase_ = Phase::SeekDecodeWait;
}

void CompositorSpikeController::pollSeekDecode() {
    const long long target = seekTargets_[seekIndex_];
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
        const std::string error = !seekCompletionA_.error.empty() ? seekCompletionA_.error
                                                                  : seekCompletionB_.error;
        beginShutdown(QString::fromStdString(error.empty() ? "dual exact seekが失敗しました"
                                                            : error),
                      true);
        return;
    }
    seekAMs_.push_back(gpu::qpcMsBetween(seekCompletionA_.requestQpc,
                                         seekCompletionA_.decodeReadyQpc));
    seekBMs_.push_back(gpu::qpcMsBetween(seekCompletionB_.requestQpc,
                                         seekCompletionB_.decodeReadyQpc));
    seekDecodeReadyQpc_ =
        std::max(seekCompletionA_.decodeReadyQpc, seekCompletionB_.decodeReadyQpc);
    seekDecodeReadyMs_.push_back(
        gpu::qpcMsBetween(seekRequestStartQpc_, seekDecodeReadyQpc_));
    seekConcurrencySamples_.push_back(
        {seekRequestStartQpc_, seekCompletionA_.beginQpc, seekCompletionA_.decodeReadyQpc,
         seekCompletionB_.beginQpc, seekCompletionB_.decodeReadyQpc});

    // A/B両completionのidentity検証が終わるまでcomposition stateを変更しない。
    state_->requestedOutput.store(-1, std::memory_order_release);
    state_->coordinator.setSourceGeneration({1}, seekCompletionA_.sourceGeneration);
    state_->coordinator.setSourceGeneration({2}, seekCompletionB_.sourceGeneration);
    waitExpectation_ = {target, state_->coordinator.compositionEpoch(),
                        {{{1}, seekCompletionA_.sourceGeneration,
                          seekCompletionA_.resourceEpoch, target},
                         {{2}, seekCompletionB_.sourceGeneration,
                          seekCompletionB_.resourceEpoch, target}}};
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
            seekDisplayedMs_.push_back(
                gpu::qpcMsBetween(seekRequestStartQpc_, found.displayedQpc));
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
    if (phase_ == Phase::ShutdownWait || phase_ == Phase::Done)
        return;
    shutdownReason_ = reason;
    if (failure)
        exitCode_ = 3;
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
    const auto seekA = workerA_ ? workerA_->seekDiagnosticSnapshot()
                                : gpu::SourceSeekDiagnosticSnapshot{};
    const auto seekB = workerB_ ? workerB_->seekDiagnosticSnapshot()
                                : gpu::SourceSeekDiagnosticSnapshot{};
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
    const double p95 = sorted.empty()
                           ? -1.0
                           : sorted[static_cast<size_t>(
                                 std::ceil(static_cast<double>(sorted.size()) * 0.95) - 1)];
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
    for (const auto& sample : seekConcurrencySamples_) {
        const bool overlaps = std::max(sample.aBeginQpc, sample.bBeginQpc) <
                              std::min(sample.aReadyQpc, sample.bReadyQpc);
        if (overlaps)
            ++overlapCount;
        concurrencySamples.append(
            QJsonObject{{"request_start_qpc", sample.requestStartQpc},
                        {"a_begin_qpc", sample.aBeginQpc},
                        {"a_ready_qpc", sample.aReadyQpc},
                        {"b_begin_qpc", sample.bBeginQpc},
                        {"b_ready_qpc", sample.bReadyQpc},
                        {"overlap", overlaps},
                        {"serial_equivalent_ms",
                         gpu::qpcMsBetween(sample.aBeginQpc, sample.aReadyQpc) +
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
        {"both_ready_to_pair_ms",
         distribution(seekDecodeReadyToPairMs_, "values_ms")},
        {"decode_ready_to_pair_ms", distribution(seekDecodeReadyToPairMs_, "values_ms")},
        {"pair_to_submission_ms", distribution(seekPairToSubmissionMs_, "values_ms")},
        {"submission_to_display_record_ms",
         distribution(seekSubmissionToDisplayMs_, "values_ms")},
        {"decode_ready_to_display_ms",
         distribution(seekDecodeReadyToDisplayMs_, "values_ms")},
        {"request_to_display_ms", distribution(seekDisplayedMs_, "values_ms")}};
    const QString mode = config_.mode == CompositorMode::Playback
                             ? QStringLiteral("playback")
                             : config_.mode == CompositorMode::Seek ? QStringLiteral("seek")
                                                                    : QStringLiteral("layout");
    QJsonObject o{{"schema", config_.diagnosticTiming ? "mvm-p2-diagnostic-1"
                                                       : "mvm-p2-formal-1"},
                  {"formal_contract_version", "P2-D4-2"},
                  {"mode", mode},
                  {"formal_preflight", config_.formalPreflight},
                  {"process_exit_code", exitCode_},
                  {"configured_seed", static_cast<qint64>(config_.seed)},
                  {"configured_warmup_seconds", config_.warmupSeconds},
                  {"configured_measure_seconds", config_.measureSeconds},
                  {"configured_seek_count", config_.seekCount},
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
                  {"measurement_missing_pair_count", measurement.missingPair},
                  {"measurement_source_a_eof_count", measurement.sourceAEof},
                  {"measurement_source_b_eof_count", measurement.sourceBEof},
                  {"measurement_first_output_frame",
                   state_->measurementFirstOutputFrame.load()},
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
                  {"source_a_software_frame_reject_count", a.softwareFrameRejectCount},
                  {"source_b_software_frame_reject_count", b.softwareFrameRejectCount},
                  {"software_fallback_count",
                   a.softwareFrameRejectCount + b.softwareFrameRejectCount},
                  {"worker_join_leak_count", (!a.joined ? 1 : 0) + (!b.joined ? 1 : 0)},
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
                  {"seek_stale_completion_count", seekStaleCompletion_},
                  {"seek_busy_acceptance_count", 0},
                  {"seek_completion_publish_reject_count",
                   seekA.completionPublishRejectCount + seekB.completionPublishRejectCount},
                  {"seek_completion_request_mismatch_count",
                   seekA.completionRequestMismatchCount +
                       seekB.completionRequestMismatchCount},
                  {"seek_completion_stopped_superseded_count",
                   seekA.completionStoppedSupersededCount +
                       seekB.completionStoppedSupersededCount},
                  {"seek_overlap_count", overlapCount},
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
    o.insert("effective_pair_rate", measureElapsedSeconds_ > 0
                                        ? static_cast<double>(measurement.displayed) /
                                              measureElapsedSeconds_
                                        : 0.0);
    o.insert("deadline_drop_rate",
             measurement.scheduled > 0
                 ? static_cast<double>(measurement.dropSchedulerDeadline) /
                       static_cast<double>(measurement.scheduled)
                 : 0.0);
    o.insert("measurement_decoded_a_count", decodedADelta);
    o.insert("measurement_decoded_b_count", decodedBDelta);
    o.insert("measurement_wait_for_space_a_count", waitADelta);
    o.insert("measurement_wait_for_space_b_count", waitBDelta);
    o.insert("measurement_buffer_depth_a_start",
             static_cast<qint64>(measurementStartA_.bufferDepth));
    o.insert("measurement_buffer_depth_a_end",
             static_cast<qint64>(measurementStopA_.bufferDepth));
    o.insert("measurement_buffer_depth_b_start",
             static_cast<qint64>(measurementStartB_.bufferDepth));
    o.insert("measurement_buffer_depth_b_end",
             static_cast<qint64>(measurementStopB_.bufferDepth));
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

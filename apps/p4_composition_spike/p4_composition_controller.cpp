#include "p4_composition_controller.h"

#include "media/audio_preview/audio_video_scheduler.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QQuickWindow>
#include <QSaveFile>
#include <QScreen>

#include <algorithm>
#include <cmath>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace mvm::app {
namespace {

constexpr auto kScheduleKind = gpu::Phase4ScheduleKind::Smoke;
// smoke schedule の boundary。frame 0 は initial state なので transition ではない。
constexpr long long kBoundaries[] = {200, 400};
constexpr int kRawSchemaVersion = 1;
constexpr const char* kFixtureASha256 =
    "d398114c38806f39670df51dfabb0095d462cbc35286ea1467901d4007cf0308";
constexpr const char* kFixtureBSha256 =
    "fe7cd1a45d101d363cb3930497601efd41dd55fd36c194acf8f24e3e4728b479";

double nearestRank(const std::vector<double>& sorted, double p) {
    if (sorted.empty())
        return 0.0;
    const auto index =
        static_cast<size_t>(std::ceil(static_cast<double>(sorted.size()) * p) - 1.0);
    return sorted[std::min(index, sorted.size() - 1)];
}

QJsonObject deltaDistribution(const std::vector<double>& values, bool absolute) {
    std::vector<double> sorted = values;
    if (absolute) {
        for (double& value : sorted)
            value = std::abs(value);
    }
    std::sort(sorted.begin(), sorted.end());
    QJsonObject result{{"count", static_cast<qint64>(values.size())},
                       {"p50", nearestRank(sorted, 0.50)},
                       {"p95", nearestRank(sorted, 0.95)},
                       {"min", sorted.empty() ? 0.0 : sorted.front()},
                       {"max", sorted.empty() ? 0.0 : sorted.back()}};
    if (absolute)
        result.insert("p99", nearestRank(sorted, 0.99));
    return result;
}

QString orientationName(Qt::ScreenOrientation orientation) {
    switch (orientation) {
    case Qt::LandscapeOrientation: return QStringLiteral("landscape");
    case Qt::PortraitOrientation: return QStringLiteral("portrait");
    case Qt::InvertedLandscapeOrientation: return QStringLiteral("inverted-landscape");
    case Qt::InvertedPortraitOrientation: return QStringLiteral("inverted-portrait");
    case Qt::PrimaryOrientation: return QStringLiteral("primary");
    }
    return QStringLiteral("unknown");
}

QJsonObject displayEnvironmentJson(const DisplayEnvironmentSnapshot& value) {
    return {{"screen_name", QString::fromStdString(value.screenName)},
            {"screen_orientation", QString::fromStdString(value.screenOrientation)},
            {"screen_geometry_width", value.screenGeometryWidth},
            {"screen_geometry_height", value.screenGeometryHeight},
            {"available_geometry_width", value.availableGeometryWidth},
            {"available_geometry_height", value.availableGeometryHeight},
            {"device_pixel_ratio", value.devicePixelRatio},
            {"window_logical_width", value.windowLogicalWidth},
            {"window_logical_height", value.windowLogicalHeight},
            {"compositor_surface_logical_width", value.compositorSurfaceLogicalWidth},
            {"compositor_surface_logical_height", value.compositorSurfaceLogicalHeight},
            {"rhi_target_pixel_width", value.rhiTargetPixelWidth},
            {"rhi_target_pixel_height", value.rhiTargetPixelHeight},
            {"native_window_outer_width", value.nativeWindowOuterWidth},
            {"native_window_outer_height", value.nativeWindowOuterHeight},
            {"native_window_client_width", value.nativeWindowClientWidth},
            {"native_window_client_height", value.nativeWindowClientHeight}};
}

QJsonValue stateNameJson(gpu::CompositionStateId state) {
    const char* name = gpu::phase4StateName(state);
    // 未知 state を "S0" などへ縮退させない。null のまま出して checker に弾かせる。
    return name ? QJsonValue(QString::fromLatin1(name)) : QJsonValue(QJsonValue::Null);
}

QJsonObject sourceIdentityJson(const gpu::SourceFrameIdentity& identity) {
    return {{"source_id", static_cast<qint64>(identity.sourceId.value)},
            {"frame", identity.frameNumber},
            {"source_generation", static_cast<qint64>(identity.sourceGeneration.value)},
            {"resource_epoch", static_cast<qint64>(identity.resourceEpoch.value)}};
}

} // namespace

P4CompositionController::P4CompositionController(P4Config config, QObject* parent)
    : QObject(parent), config_(std::move(config)) {
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(2);
    connect(&timer_, &QTimer::timeout, this, &P4CompositionController::tick);
}

void P4CompositionController::attach(CompositorRhiItem* item) {
    item_ = item;
    state_ = item->state();
    audioClock_ = std::make_shared<audio::AudioMasterClock>();
    state_->audioMasterClock = audioClock_;
    phaseTimer_.start();
    timer_.start();
}

bool P4CompositionController::validateSchedule() {
    const std::string canonical = gpu::phase4CanonicalScheduleString(kScheduleKind);
    if (canonical.empty()) {
        startShutdown(QStringLiteral("canonical schedule 文字列を構築できません"), true);
        return false;
    }
    canonicalSchedule_ = QString::fromStdString(canonical);
    canonicalScheduleSha256_ =
        QString::fromLatin1(QCryptographicHash::hash(QByteArray::fromStdString(canonical),
                                                     QCryptographicHash::Sha256)
                                .toHex());
    const QString expected =
        QString::fromLatin1(gpu::phase4ExpectedScheduleSha256(kScheduleKind));
    if (canonicalScheduleSha256_ != expected) {
        startShutdown(QStringLiteral("canonical schedule SHA-256 が freeze 値と一致しません: ") +
                          canonicalScheduleSha256_,
                      true);
        return false;
    }
    scheduleEntries_ = gpu::phase4ScheduleEntries(kScheduleKind);
    schedule_ = gpu::phase4Schedule(kScheduleKind);
    if (!schedule_) {
        startShutdown(QStringLiteral("smoke schedule が validation を通りません"), true);
        return false;
    }
    return true;
}

DisplayEnvironmentSnapshot P4CompositionController::captureDisplayEnvironment() const {
    DisplayEnvironmentSnapshot result;
    if (!item_)
        return result;
    const auto target = state_->actualOutputSizeSnapshot();
    result.rhiTargetPixelWidth = target.width;
    result.rhiTargetPixelHeight = target.height;
    result.compositorSurfaceLogicalWidth = static_cast<int>(std::lround(item_->width()));
    result.compositorSurfaceLogicalHeight = static_cast<int>(std::lround(item_->height()));
    auto* window = item_->window();
    if (!window)
        return result;
    result.windowLogicalWidth = window->width();
    result.windowLogicalHeight = window->height();
    if (auto* screen = window->screen()) {
        const QRect geometry = screen->geometry();
        const QRect available = screen->availableGeometry();
        result.screenName = screen->name().toStdString();
        result.screenOrientation = orientationName(screen->orientation()).toStdString();
        result.screenGeometryWidth = geometry.width();
        result.screenGeometryHeight = geometry.height();
        result.availableGeometryWidth = available.width();
        result.availableGeometryHeight = available.height();
        result.devicePixelRatio = screen->devicePixelRatio();
    }
#ifdef Q_OS_WIN
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    RECT outer{};
    RECT client{};
    if (hwnd && GetWindowRect(hwnd, &outer)) {
        result.nativeWindowOuterWidth = outer.right - outer.left;
        result.nativeWindowOuterHeight = outer.bottom - outer.top;
    }
    if (hwnd && GetClientRect(hwnd, &client)) {
        result.nativeWindowClientWidth = client.right - client.left;
        result.nativeWindowClientHeight = client.bottom - client.top;
    }
#endif
    return result;
}

bool P4CompositionController::openPipelines() {
    workerA_ = std::make_shared<gpu::SourceDecodeWorker>(gpu::kPhase4SourceA, state_->device,
                                                         state_->readbacks, 16);
    workerB_ = std::make_shared<gpu::SourceDecodeWorker>(gpu::kPhase4SourceB, state_->device,
                                                         state_->readbacks, 16);
    audioWorker_ = std::make_shared<audio::AudioDecodeWorker>(audio::SourceId{1});
    audioSink_ = std::make_shared<audio::WasapiAudioSink>(audioWorker_->queue(), *audioClock_);
    std::string error;
    if (!workerA_->start(config_.sourceA.toUtf8().constData(), error) ||
        !workerB_->start(config_.sourceB.toUtf8().constData(), error) ||
        !audioWorker_->start(config_.sourceA.toUtf8().constData(), error) ||
        !audioSink_->open(error)) {
        startShutdown(QString::fromStdString(error), true);
        return false;
    }
    const auto a = workerA_->snapshot();
    const auto b = workerB_->snapshot();
    if (a.info.frameCount <= 0 || b.info.frameCount <= 0) {
        startShutdown(QStringLiteral("A/B source frame count が不正です"), true);
        return false;
    }
    const long long requiredFrames =
        static_cast<long long>(config_.durationSeconds) * audio::kInternalSampleRate /
        audio::kSamplesPerVideoFrame;
    if (std::min(a.info.frameCount, b.info.frameCount) < requiredFrames) {
        startShutdown(QStringLiteral("measurement 区間に必要な frame 数が source にありません"),
                      true);
        return false;
    }
    state_->audioMasterVideoFrameCount.store(std::min(a.info.frameCount, b.info.frameCount));
    {
        std::lock_guard<std::mutex> lock(state_->workerMutex);
        state_->workerA = workerA_;
        state_->workerB = workerB_;
    }
    return true;
}

bool P4CompositionController::prepareCpuReferences() {
    std::string error;
    if (!gpu::buildPhase4SmokeCpuReferences(config_.sourceA.toUtf8().constData(),
                                            config_.sourceB.toUtf8().constData(),
                                            kFixtureASha256, kFixtureBSha256, cpuReferences_,
                                            error)) {
        startShutdown(QString::fromStdString(error), true);
        return false;
    }
    return true;
}

bool P4CompositionController::pollTransitionProbes() {
    if (!state_->transitionProbeReady.load(std::memory_order_acquire))
        return true;
    std::vector<gpu::TransitionProbeResult> completed;
    std::string error;
    if (!state_->transitionProbeReadback.poll(completed, error)) {
        startShutdown(QString::fromStdString(error), true);
        return false;
    }
    if (!completed.empty()) {
        std::lock_guard<std::mutex> lock(state_->transitionProbeResultMutex);
        state_->transitionProbeResults.insert(state_->transitionProbeResults.end(),
                                              completed.begin(), completed.end());
    }
    return true;
}

bool P4CompositionController::startAtFrameZero(bool measurementStart) {
    state_->audioMasterSchedulerEnabled.store(false, std::memory_order_release);
    state_->playbackSchedulerEnabled.store(false, std::memory_order_release);
    state_->requestedOutput.store(-1, std::memory_order_release);
    std::string error;
    if (measurementStart) {
        workerA_->pause();
        workerB_->pause();
        audioWorker_->pause();
        if (!audioSink_->pause(error) || !audioSink_->resetForSeek(error)) {
            startShutdown(QString::fromStdString(error), true);
            return false;
        }
    }

    constexpr long long kTargetFrame = 0;
    constexpr long long kTargetSample = 0;
    gpu::SeekTicket ticketA;
    gpu::SeekTicket ticketB;
    audio::AudioSeekTicket audioTicket;
    if (workerA_->requestSeek(kTargetFrame, ticketA, error) != gpu::SeekRequestResult::Accepted ||
        workerB_->requestSeek(kTargetFrame, ticketB, error) != gpu::SeekRequestResult::Accepted ||
        audioWorker_->requestSeek(kTargetSample, audioTicket, error) !=
            audio::AudioSeekRequestResult::Accepted) {
        startShutdown(QStringLiteral("frame0/sample0 seek request が拒否されました"), true);
        return false;
    }
    gpu::SeekCompletion completionA;
    gpu::SeekCompletion completionB;
    audio::AudioSeekCompletion completionAudio;
    bool readyA = false;
    bool readyB = false;
    bool readyAudio = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!(readyA && readyB && readyAudio) && std::chrono::steady_clock::now() < deadline) {
        if (!readyA)
            readyA = workerA_->waitSeek(ticketA, 0, completionA) == gpu::SeekWaitResult::Ready;
        if (!readyB)
            readyB = workerB_->waitSeek(ticketB, 0, completionB) == gpu::SeekWaitResult::Ready;
        if (!readyAudio)
            readyAudio = audioWorker_->waitSeek(audioTicket, 0, completionAudio) ==
                         audio::AudioSeekWaitResult::Ready;
        if (!(readyA && readyB && readyAudio))
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!(readyA && readyB && readyAudio)) {
        startShutdown(QStringLiteral("frame0/sample0 seek が 5 秒 deadline で完了しません"), true);
        return false;
    }
    if (completionA.status != gpu::SeekCompletionStatus::Completed ||
        completionB.status != gpu::SeekCompletionStatus::Completed || !completionAudio.completed ||
        completionA.decodedFrameNumber != kTargetFrame ||
        completionB.decodedFrameNumber != kTargetFrame ||
        completionAudio.firstOutputSample != kTargetSample) {
        startShutdown(QStringLiteral("frame0/sample0 seek completion が exact contract と不一致です"),
                      true);
        return false;
    }
    const auto snapshotA = workerA_->snapshot();
    const auto snapshotB = workerB_->snapshot();
    if (snapshotA.decodeDevicePointer != state_->nativeDevicePointer.load() ||
        snapshotB.decodeDevicePointer != state_->nativeDevicePointer.load() ||
        !snapshotA.adapter.sameAdapterAs(state_->qtAdapter) ||
        !snapshotB.adapter.sameAdapterAs(state_->qtAdapter)) {
        startShutdown(QStringLiteral("A/B decode texture device が Qt device と一致しません"), true);
        return false;
    }

    // canonical S0 layout で configure し、S0 を atomic snapshot として adopt する。
    // updateLayout + adoptCompositionState の 2 段階は Phase 4 path では使わない。
    if (!coordinatorConfigured_) {
        if (state_->coordinator.configure(
                gpu::phase4CanonicalLayout(gpu::kPhase4S0),
                {{gpu::kPhase4SourceA, completionA.sourceGeneration},
                 {gpu::kPhase4SourceB, completionB.sourceGeneration}}) !=
            gpu::ConfigureResult::Configured) {
            startShutdown(QStringLiteral("Phase 4 compositor coordinator を初期化できません"), true);
            return false;
        }
        coordinatorConfigured_ = true;
    } else if (!state_->coordinator.setSourceGeneration(gpu::kPhase4SourceA,
                                                        completionA.sourceGeneration) ||
               !state_->coordinator.setSourceGeneration(gpu::kPhase4SourceB,
                                                        completionB.sourceGeneration)) {
        startShutdown(QStringLiteral("seek generation を adopt できません"), true);
        return false;
    }
    // initial S0 adoption。measurement counter baseline から除外するため、
    // Phase 4 driver ではなく controller が直接行う。
    const auto initial = state_->coordinator.adoptCompositionSnapshot(
        gpu::kPhase4S0, gpu::phase4CanonicalLayout(gpu::kPhase4S0));
    if (initial == gpu::CompositionStateAdoptionResult::Rejected) {
        startShutdown(QStringLiteral("initial S0 snapshot を採用できません"), true);
        return false;
    }

    workerA_->play();
    workerB_->play();
    audioWorker_->play();
    const auto prerollDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((workerA_->buffer().depth() < 8 || workerB_->buffer().depth() < 8) &&
           std::chrono::steady_clock::now() < prerollDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (workerA_->buffer().depth() < 8 || workerB_->buffer().depth() < 8 ||
        !audioWorker_->queue().waitForSamples(audio::kAudioPrerollSamples, 5000)) {
        startShutdown(QStringLiteral("固定 video/audio pre-roll を満たせません"), true);
        return false;
    }

    if (measurementStart) {
        // initial S0 setup 後に counter baseline を確定する。
        const auto queue = audioWorker_->queue().snapshot();
        measurementBaseline_ = {state_->videoPairWaitCount.load(),
                                state_->videoTargetSupersededCount.load(),
                                state_->audioClockVideoStaleDiscardA.load(),
                                state_->audioClockVideoStaleDiscardB.load(),
                                static_cast<long long>(queue.underflowCount),
                                static_cast<long long>(queue.overflowRejectCount),
                                state_->markerAMismatch.load(),
                                state_->markerBMismatch.load(),
                                state_->coordinator.mixedSourceFrameCount(),
                                state_->coordinator.mixedGenerationCount(),
                                state_->coordinator.staleCompositionEpochCount(),
                                state_->videoAheadViolationCount.load(),
                                state_->videoClockRegressionCount.load(),
                                state_->videoQpcMasterFallbackCount.load(),
                                static_cast<long long>(audioClock_->snapshot().clockQueryFailureCount)};
        measurementBaselineEpoch_ = state_->coordinator.compositionEpoch();
        baselineGenerationA_ = completionA.sourceGeneration;
        baselineGenerationB_ = completionB.sourceGeneration;
        baselineResourceEpochA_ = completionA.resourceEpoch;
        baselineResourceEpochB_ = completionB.resourceEpoch;
        {
            std::lock_guard<std::mutex> lock(state_->applicationAvDeltaMutex);
            state_->applicationAvDeltaMs.clear();
        }
        const long long requiredSamples =
            static_cast<long long>(config_.durationSeconds) * audio::kInternalSampleRate;
        state_->p3MeasurementEndSampleExclusive.store(requiredSamples, std::memory_order_release);
        state_->p3MeasurementActive.store(true, std::memory_order_release);
        // driver を publish してから enable する。enable の release が publish を見せる。
        state_->phase4Driver = std::make_shared<gpu::Phase4CompositionDriver>(
            *schedule_, std::vector<gpu::SourceId>{gpu::kPhase4SourceA, gpu::kPhase4SourceB});
        measurementLedgerBaseline_ = state_->ledger.baseline();
        state_->phase4Enabled.store(true, std::memory_order_release);
    }

    if (!audioSink_->play(kTargetSample, completionAudio.seekGeneration, error)) {
        startShutdown(QString::fromStdString(error), true);
        return false;
    }
    if (!measurementStart)
        measurementLedgerBaseline_ = state_->ledger.baseline();
    displayExpectation_ = {kTargetFrame,
                           state_->coordinator.compositionEpoch(),
                           {{gpu::kPhase4SourceA, completionA.sourceGeneration,
                             completionA.resourceEpoch, kTargetFrame},
                            {gpu::kPhase4SourceB, completionB.sourceGeneration,
                             completionB.resourceEpoch, kTargetFrame}},
                           gpu::kPhase4S0};
    state_->audioMasterGeneration.store(completionAudio.seekGeneration.value,
                                        std::memory_order_release);
    state_->audioMasterLastDisplayed.store(kTargetFrame - 1, std::memory_order_release);
    state_->audioMasterLastRequested.store(-1, std::memory_order_release);
    state_->audioMasterMarkerProbePending.store(true, std::memory_order_release);
    state_->audioMasterSchedulerEnabled.store(true, std::memory_order_release);
    phaseTimer_.restart();
    phase_ = measurementStart ? Phase::WaitMeasurementDisplay : Phase::WaitWarmupDisplay;
    item_->update();
    return true;
}

bool P4CompositionController::pollFirstDisplay(bool measurementStart) {
    gpu::CompositionDisplayRecord display;
    if (!state_->ledger.findAfter(measurementLedgerBaseline_, displayExpectation_, display)) {
        if (phaseTimer_.elapsed() > config_.displayTimeoutMs)
            startShutdown(QStringLiteral("first integrated video display が timeout しました"), true);
        return false;
    }
    if (measurementStart) {
        // measurement の first actual display は frame 0 / S0 / E0 でなければならない。
        const auto records = state_->ledger.recordsAfter(measurementLedgerBaseline_);
        if (records.empty() || records.front().outputFrameNumber != 0 ||
            records.front().compositionState != gpu::kPhase4S0 ||
            records.front().compositionEpoch != measurementBaselineEpoch_) {
            startShutdown(QStringLiteral("measurement first display が frame0/S0/E0 ではありません"),
                          true);
            return false;
        }
        firstMeasurementDisplaySeen_ = true;
    }
    return true;
}

void P4CompositionController::startShutdown(const QString& reason, bool failure) {
    if (phase_ == Phase::ShutdownWait || phase_ == Phase::Done)
        return;
    shutdownReason_ = reason;
    displayEnvironmentEnd_ = captureDisplayEnvironment();
    if (failure)
        exitCode_ = 3;

    // 順序は docs/phase4-plan.md §7 に freeze されている。ここへ手書きせず
    // runFrozenShutdownSequence へ委ねる。worker join 前に render teardown を
    // 要求しないことも、その helper が fail-closed に保証する。
    gpu::ShutdownActions actions;
    actions.disableSchedulers = [this] {
        state_->phase4Enabled.store(false, std::memory_order_release);
        state_->audioMasterSchedulerEnabled.store(false, std::memory_order_release);
        state_->playbackSchedulerEnabled.store(false, std::memory_order_release);
        state_->p3MeasurementActive.store(false, std::memory_order_release);
        state_->requestedOutput.store(-1, std::memory_order_release);
    };
    if (audioSink_)
        actions.stopAudioSink = [this] {
            // pause は endpoint を止めるだけで join しない。stop() が
            // client stop / stop event / render thread join / device release を行う。
            std::string ignored;
            audioSink_->pause(ignored);
            audioSink_->stop();
        };
    if (audioWorker_)
        actions.stopAudioDecodeWorker = [this] { audioWorker_->stop(); };
    if (workerA_)
        actions.stopVideoWorkerA = [this] { workerA_->stop(); };
    if (workerB_)
        actions.stopVideoWorkerB = [this] { workerB_->stop(); };
    actions.detachSharedWorkerRefs = [this] {
        std::lock_guard<std::mutex> lock(state_->workerMutex);
        state_->workerA.reset();
        state_->workerB.reset();
    };
    // stop() は同期 join するので、要求直前に最終 snapshot で join を確認する。
    actions.allWorkersJoined = [this] {
        return (!audioSink_ || audioSink_->snapshot().joined) &&
               (!audioWorker_ || audioWorker_->snapshot().joined) &&
               (!workerA_ || workerA_->joined()) && (!workerB_ || workerB_->joined());
    };
    actions.requestRenderTeardown = [this] { item_->requestTeardown(); };

    shutdownSequence_ = gpu::runFrozenShutdownSequence(actions);
    if (!shutdownSequence_.renderTeardownRequested) {
        // join を確認できないまま teardown を要求しない。黙って続行もしない。
        exitCode_ = 3;
        shutdownReason_ =
            QStringLiteral("worker join を確認できないため render teardown を要求しません: ") +
            reason;
    }
    phaseTimer_.restart();
    phase_ = Phase::ShutdownWait;
}

void P4CompositionController::tick() {
    if (!state_)
        return;
    if (state_->fatal.load(std::memory_order_acquire)) {
        std::string reason;
        {
            std::lock_guard<std::mutex> lock(state_->errorMutex);
            reason = state_->fatalReason;
        }
        startShutdown(QString::fromStdString(reason), true);
    }
    if (phase_ != Phase::ShutdownWait && phase_ != Phase::Done && !pollTransitionProbes())
        return;
    switch (phase_) {
    case Phase::WaitDevice:
        if (state_->deviceReady.load(std::memory_order_acquire)) {
            phaseTimer_.restart();
            phase_ = Phase::DisplayPreflight;
        } else if (phaseTimer_.elapsed() > 10000) {
            startShutdown(QStringLiteral("Qt/D3D11 device ready timeout"), true);
        }
        break;
    case Phase::DisplayPreflight: {
        displayEnvironmentStart_ = captureDisplayEnvironment();
        const auto result = evaluateP3C2DisplayTarget(displayEnvironmentStart_);
        if (result.state == DisplayTargetPreflightState::Waiting) {
            if (phaseTimer_.elapsed() > 10000)
                startShutdown(QStringLiteral("display target preflight ready timeout"), true);
            break;
        }
        if (!result.workloadMayStart()) {
            startShutdown(QString::fromStdString(result.error), true);
            break;
        }
        displayPreflightPassed_ = true;
        phase_ = Phase::Start;
        break;
    }
    case Phase::Start:
        // schedule の検証は decoder / audio open より前に済ませる。
        // fixture hash検証とCPU候補reference生成もmeasurement開始前に完了させる。
        if (validateSchedule() && prepareCpuReferences() && openPipelines())
            startAtFrameZero(false);
        break;
    case Phase::WaitWarmupDisplay:
        if (pollFirstDisplay(false)) {
            phaseTimer_.restart();
            phase_ = Phase::Warmup;
        }
        break;
    case Phase::Warmup: {
        const auto clock = audioClock_->snapshot();
        if (clock.mediaSamplePosition >=
            static_cast<long long>(config_.warmupSeconds) * audio::kInternalSampleRate) {
            warmupComplete_ = true;
            startAtFrameZero(true);
        } else if (phaseTimer_.elapsed() > (config_.warmupSeconds + 10) * 1000) {
            startShutdown(QStringLiteral("warmup 中に audio clock が進みません"), true);
        }
        break;
    }
    case Phase::WaitMeasurementDisplay:
        if (pollFirstDisplay(true))
            phase_ = Phase::Measure;
        break;
    case Phase::Measure: {
        const auto clock = audioClock_->snapshot();
        const long long required =
            static_cast<long long>(config_.durationSeconds) * audio::kInternalSampleRate;
        if (clock.mediaSamplePosition >= required) {
            observedMeasurementEndSample_ = clock.mediaSamplePosition;
            state_->p3MeasurementActive.store(false, std::memory_order_release);
            state_->phase4Enabled.store(false, std::memory_order_release);
            state_->audioMasterSchedulerEnabled.store(false, std::memory_order_release);
            startShutdown(QStringLiteral("10 秒 integration sanity 区間完了"), false);
        }
        break;
    }
    case Phase::ShutdownWait:
        if (state_->teardownComplete.load(std::memory_order_acquire)) {
            if (!writeMetrics())
                exitCode_ = 4;
            phase_ = Phase::Done;
            timer_.stop();
            Q_EMIT finished();
        } else if (phaseTimer_.elapsed() > 10000) {
            exitCode_ = 3;
            shutdownReason_ = QStringLiteral("GPU teardown timeout");
            writeMetrics();
            phase_ = Phase::Done;
            timer_.stop();
            Q_EMIT finished();
        }
        break;
    case Phase::Done:
        break;
    }
}

bool P4CompositionController::writeMetrics() const {
    const auto audioDecoder =
        audioWorker_ ? audioWorker_->snapshot() : audio::AudioDecoderSnapshot{};
    const auto queue = audioWorker_ ? audioWorker_->queue().snapshot() : audio::AudioQueueSnapshot{};
    const auto sink = audioSink_ ? audioSink_->snapshot() : audio::WasapiSnapshot{};
    const auto a = workerA_ ? workerA_->snapshot() : gpu::SourceDecoderSnapshot{};
    const auto b = workerB_ ? workerB_->snapshot() : gpu::SourceDecoderSnapshot{};
    const auto clock = audioClock_ ? audioClock_->snapshot() : audio::AudioClockSnapshot{};
    const auto& compositor = state_->compositor.counters();
    const auto driverCounters = state_->phase4Driver ? state_->phase4Driver->counters()
                                                     : gpu::Phase4DriverCounters{};

    const long long requiredSamples =
        static_cast<long long>(config_.durationSeconds) * audio::kInternalSampleRate;
    const long long requiredFrames = requiredSamples / audio::kSamplesPerVideoFrame;
    const auto records = firstMeasurementDisplaySeen_
                             ? state_->ledger.recordsAfter(measurementLedgerBaseline_)
                             : std::vector<gpu::CompositionDisplayRecord>{};
    std::vector<gpu::TransitionProbeResult> probeResults;
    {
        std::lock_guard<std::mutex> lock(state_->transitionProbeResultMutex);
        probeResults = state_->transitionProbeResults;
    }
    const auto probeCounters = state_->transitionProbeReadback.counters();

    // producer 側の集計。checker はこれを信用せず ledger から再計算する。
    QJsonArray ledgerJson;
    std::vector<double> measurementDeltas;
    long long firstFrame = -1;
    long long lastFrame = -1;
    long long uniqueDisplayed = 0;
    long long skipped = 0;
    long long nonIncreasing = 0;
    long long stateMismatch = 0;
    long long oldStateAfterBoundary = 0;
    long long pairIdentityViolation = 0;
    long long generationMismatch = 0;
    for (const auto& record : records) {
        const auto expectedState = schedule_ ? schedule_->resolve(record.outputFrameNumber)
                                             : std::optional<gpu::CompositionStateId>{};
        size_t segment = 0;
        for (size_t i = 0; i < scheduleEntries_.size(); ++i)
            if (record.outputFrameNumber >= scheduleEntries_[i].boundaryOutputFrame)
                segment = i;
        const gpu::CompositionEpoch expectedEpoch{measurementBaselineEpoch_.value +
                                                  static_cast<unsigned long long>(segment)};
        const bool stateOk = expectedState && record.compositionState == *expectedState &&
                             record.compositionEpoch == expectedEpoch;
        if (!stateOk) {
            ++stateMismatch;
            for (const long long boundary : kBoundaries)
                if (record.outputFrameNumber >= boundary && expectedState &&
                    record.compositionState != *expectedState)
                    ++oldStateAfterBoundary;
        }
        if (record.sources.size() != 2 || record.sources[0].frameNumber != record.outputFrameNumber ||
            record.sources[1].frameNumber != record.outputFrameNumber ||
            record.sources[0].sourceId != gpu::kPhase4SourceA ||
            record.sources[1].sourceId != gpu::kPhase4SourceB)
            ++pairIdentityViolation;
        else if (record.sources[0].sourceGeneration != baselineGenerationA_ ||
                 record.sources[1].sourceGeneration != baselineGenerationB_ ||
                 record.sources[0].resourceEpoch != baselineResourceEpochA_ ||
                 record.sources[1].resourceEpoch != baselineResourceEpochB_)
            ++generationMismatch;

        if (firstFrame < 0)
            firstFrame = record.outputFrameNumber;
        if (lastFrame >= 0) {
            if (record.outputFrameNumber <= lastFrame)
                ++nonIncreasing;
            else
                skipped += record.outputFrameNumber - lastFrame - 1;
        }
        if (record.outputFrameNumber != lastFrame)
            ++uniqueDisplayed;
        lastFrame = record.outputFrameNumber;
        if (record.applicationAvProjectionValid)
            measurementDeltas.push_back(record.applicationAvDeltaMs);

        QJsonArray sourcesJson;
        for (const auto& identity : record.sources)
            sourcesJson.append(sourceIdentityJson(identity));
        ledgerJson.append(
            QJsonObject{{"output_frame", record.outputFrameNumber},
                        {"composition_state", stateNameJson(record.compositionState)},
                        {"composition_state_id",
                         static_cast<qint64>(record.compositionState.value)},
                        {"composition_epoch", static_cast<qint64>(record.compositionEpoch.value)},
                        {"display_record_qpc", record.displayRecordQpc},
                        {"application_av_projection_valid", record.applicationAvProjectionValid},
                        {"application_av_delta_ms", record.applicationAvDeltaMs},
                        {"sources", sourcesJson}});
    }
    if (lastFrame >= 0 && lastFrame < requiredFrames)
        skipped += requiredFrames - lastFrame - 1;

    QJsonArray boundaryJson;
    QJsonArray activationLagJson;
    bool activationLagInRange = true;
    for (const long long boundary : kBoundaries) {
        long long firstAfter = -1;
        const gpu::CompositionDisplayRecord* found = nullptr;
        for (const auto& record : records)
            if (record.outputFrameNumber >= boundary &&
                (firstAfter < 0 || record.outputFrameNumber < firstAfter)) {
                firstAfter = record.outputFrameNumber;
                found = &record;
            }
        const long long lag = firstAfter < 0 ? -1 : firstAfter - boundary;
        if (lag < 0 || lag > 2)
            activationLagInRange = false;
        activationLagJson.append(lag);
        QJsonArray firstDisplaySources;
        if (found)
            for (const auto& identity : found->sources)
                firstDisplaySources.append(sourceIdentityJson(identity));
        boundaryJson.append(QJsonObject{
            {"boundary", boundary},
            {"expected_state",
             stateNameJson(schedule_ ? schedule_->resolve(boundary).value_or(gpu::CompositionStateId{})
                                     : gpu::CompositionStateId{})},
            {"first_displayed_output_frame", firstAfter},
            {"activation_lag_frames", lag},
            {"first_display_state", found ? stateNameJson(found->compositionState)
                                          : QJsonValue(QJsonValue::Null)},
            {"first_display_composition_epoch",
             found ? QJsonValue(static_cast<qint64>(found->compositionEpoch.value))
                   : QJsonValue(QJsonValue::Null)},
            {"first_display_sources", firstDisplaySources}});
    }

    QJsonArray probeJson;
    long long probeMismatch = 0;
    for (const auto& probe : probeResults) {
        const auto* expected = cpuReferences_.find(probe.request.boundary,
                                                   probe.request.actualOutputFrame,
                                                   probe.request.point);
        const gpu::Rgba8 actual{probe.rgba[0], probe.rgba[1], probe.rgba[2], probe.rgba[3]};
        const bool matches = expected && expected->state == probe.request.compositionState &&
                             gpu::probeWithinTolerance(actual, expected->rgba);
        if (!matches)
            ++probeMismatch;
        QJsonArray actualJson{actual.r, actual.g, actual.b, actual.a};
        QJsonArray expectedJson;
        if (expected)
            expectedJson = {expected->rgba.r, expected->rgba.g, expected->rgba.b,
                            expected->rgba.a};
        QJsonArray sourcesJson;
        for (const auto& identity : probe.request.sources)
            sourcesJson.append(sourceIdentityJson(identity));
        probeJson.append(QJsonObject{
            {"boundary", probe.request.boundary},
            {"actual_output_frame", probe.request.actualOutputFrame},
            {"composition_state", stateNameJson(probe.request.compositionState)},
            {"cpu_reference_state",
             expected ? stateNameJson(expected->state) : QJsonValue(QJsonValue::Null)},
            {"composition_epoch", static_cast<qint64>(probe.request.compositionEpoch.value)},
            {"probe", QString::fromLatin1(gpu::transitionProbePointName(probe.request.point))},
            {"x", probe.request.x},
            {"y", probe.request.y},
            {"actual_rgba", actualJson},
            {"cpu_expected_rgba", expectedJson},
            {"gpu_ticket", static_cast<qint64>(probe.ticket)},
            {"gpu_completion_serial", static_cast<qint64>(probe.completionSerial)},
            {"completion_observed", probe.completionObserved},
            {"blocking_wait_count", probeCounters.renderThreadBlockingWaitCount},
            {"sources", sourcesJson}});
    }

    QJsonArray shutdownSequenceJson;
    for (const auto step : shutdownSequence_.executed)
        shutdownSequenceJson.append(QString::fromLatin1(gpu::toString(step)));

    QJsonArray scheduleJson;
    for (const auto& entry : scheduleEntries_)
        scheduleJson.append(QJsonObject{{"boundary", entry.boundaryOutputFrame},
                                        {"state", stateNameJson(entry.state)}});

    const auto delta = [](long long current, long long baseline) {
        return std::max(0LL, current - baseline);
    };
    const long long underflow =
        delta(static_cast<long long>(queue.underflowCount), measurementBaseline_.underflow);
    const long long overflow =
        delta(static_cast<long long>(queue.overflowRejectCount), measurementBaseline_.overflow);
    const long long markerMismatch =
        delta(state_->markerAMismatch.load(), measurementBaseline_.markerAMismatch) +
        delta(state_->markerBMismatch.load(), measurementBaseline_.markerBMismatch);
    const long long mixedPair =
        delta(state_->coordinator.mixedSourceFrameCount(), measurementBaseline_.mixedPair);
    const long long mixedGeneration =
        delta(state_->coordinator.mixedGenerationCount(), measurementBaseline_.mixedGeneration);
    const long long staleEpoch =
        delta(state_->coordinator.staleCompositionEpochCount(), measurementBaseline_.staleEpoch);
    const long long ahead =
        delta(state_->videoAheadViolationCount.load(), measurementBaseline_.ahead);
    const long long clockRegression =
        delta(state_->videoClockRegressionCount.load(), measurementBaseline_.clockRegression);
    const long long qpcFallback =
        delta(state_->videoQpcMasterFallbackCount.load(), measurementBaseline_.qpcFallback);
    const long long audioClockQueryFailure =
        delta(static_cast<long long>(clock.clockQueryFailureCount),
              measurementBaseline_.audioClockQueryFailure);

    const bool counterSelfConsistent =
        driverCounters.resolveCount ==
        driverCounters.adoptionCount + driverCounters.noopCount + driverCounters.rejectCount;
    std::vector<double> absoluteDeltas = measurementDeltas;
    for (double& value : absoluteDeltas)
        value = std::abs(value);
    std::sort(absoluteDeltas.begin(), absoluteDeltas.end());
    const double avAbsP95 = nearestRank(absoluteDeltas, 0.95);
    const double avAbsMax = absoluteDeltas.empty() ? 0.0 : absoluteDeltas.back();
    const double effectiveFps = static_cast<double>(uniqueDisplayed) / config_.durationSeconds;
    const double dropRate = static_cast<double>(skipped) / static_cast<double>(requiredFrames);
    // producer側の完了判定であり、smoke PASS authorityではない。
    const bool integrationSanityPass =
        exitCode_ == 0 && displayPreflightPassed_ && warmupComplete_ &&
        firstMeasurementDisplaySeen_ && counterSelfConsistent && !records.empty() &&
        firstFrame == 0 && nonIncreasing == 0 && uniqueDisplayed + skipped == requiredFrames &&
        driverCounters.adoptionCount == 2 && driverCounters.epochIncrementCount == 2 &&
        driverCounters.rejectCount == 0 && driverCounters.unresolvedFrameCount == 0 &&
        driverCounters.sourceGenerationChangeDueToLayoutCount == 0 && stateMismatch == 0 &&
        oldStateAfterBoundary == 0 && pairIdentityViolation == 0 && generationMismatch == 0 &&
        activationLagInRange && underflow == 0 && overflow == 0 && markerMismatch == 0 &&
        mixedPair == 0 && mixedGeneration == 0 && staleEpoch == 0 && ahead == 0 &&
        clockRegression == 0 && qpcFallback == 0 && audioClockQueryFailure == 0 &&
        shutdownSequence_.joinVerified && shutdownSequence_.renderTeardownRequested &&
        shutdownSequence_.orderViolationCount == 0 &&
        state_->phase4AdoptionFailureCount.load() == 0 && state_->deviceLostCount.load() == 0 &&
        state_->lifecycleOrderViolationCount.load() == 0 &&
        state_->readbacks.fullFrameReadbacks() == 0 && compositor.fullFrameGpuCopyCount == 0 &&
        probeResults.size() == 4 && probeMismatch == 0 && probeCounters.issuedCount == 4 &&
        probeCounters.completedCount == 4 &&
        probeCounters.renderThreadBlockingWaitCount == 0 &&
        probeCounters.untrackedSubmissionCount == 0 &&
        probeCounters.completionFailureCount == 0 &&
        probeCounters.retirementTimeoutCount == 0 &&
        probeCounters.pendingAfterDrainCount == 0 &&
        state_->transitionProbeIssueFailureCount.load() == 0 &&
        effectiveFps >= 55.0 && dropRate <= 0.02 && avAbsP95 <= 20.0 &&
        avAbsMax <= 33.334 &&
        a.softwareFrameRejectCount == 0 && b.softwareFrameRejectCount == 0 &&
        sink.audioRenderThreadJoinLeak == 0 && audioDecoder.audioDecodeThreadJoinLeak == 0 &&
        a.joined && b.joined && sink.joined && audioDecoder.joined &&
        state_->teardownComplete.load() &&
        sameDisplayEnvironment(displayEnvironmentStart_, displayEnvironmentEnd_);

    QJsonObject root{
        {"schema", "mvm-p4-smoke-1"},
        {"schema_version", kRawSchemaVersion},
        {"contract_version", "P4-C-smoke-frozen"},
        {"phase", "P4-C"},
        {"schedule_kind", gpu::phase4ScheduleKindName(kScheduleKind)},
        // Phase 4 formal / smoke の contract verdict はここでは出さない。
        {"formal_verdict", "NOT_RUN"},
        {"smoke_contract_verdict", "NOT_RUN"},
        {"producer_complete", integrationSanityPass},
        {"detail", shutdownReason_},
        {"canonical_schedule", canonicalSchedule_},
        {"canonical_schedule_sha256", canonicalScheduleSha256_},
        {"schedule", scheduleJson},
        {"audio_master_only", true},
        {"samples_per_video_frame", audio::kSamplesPerVideoFrame},
        {"configured_video_preroll_frames", 8},
        {"configured_audio_preroll_ms", audio::kAudioPrerollMs},
        {"warmup_seconds", config_.warmupSeconds},
        {"measurement_seconds", config_.durationSeconds},
        {"measurement_audio_start_sample", 0},
        {"measurement_audio_end_sample", requiredSamples},
        {"observed_audio_stop_sample", observedMeasurementEndSample_},
        {"required_video_frames", requiredFrames},
        {"measurement_baseline_composition_epoch",
         static_cast<qint64>(measurementBaselineEpoch_.value)},
        {"composition_state_resolve_count", driverCounters.resolveCount},
        {"composition_state_adoption_count", driverCounters.adoptionCount},
        {"composition_state_noop_count", driverCounters.noopCount},
        {"composition_state_reject_count", driverCounters.rejectCount},
        {"composition_state_unresolved_count", driverCounters.unresolvedFrameCount},
        {"composition_epoch_increment_count", driverCounters.epochIncrementCount},
        {"composition_counter_self_consistent", counterSelfConsistent},
        {"composition_state_display_mismatch_count", stateMismatch},
        {"old_state_after_boundary_count", oldStateAfterBoundary},
        {"composition_pair_identity_violation_count", pairIdentityViolation},
        {"composition_layer_generation_mismatch_count", generationMismatch},
        {"source_generation_change_due_to_layout_count",
         driverCounters.sourceGenerationChangeDueToLayoutCount},
        {"phase4_adoption_failure_count", state_->phase4AdoptionFailureCount.load()},
        {"transition_boundaries", boundaryJson},
        {"transition_activation_lag_frames", activationLagJson},
        {"measurement_display_ledger", ledgerJson},
        {"measurement_display_ledger_count", static_cast<qint64>(records.size())},
        {"transition_pixel_probe_status", "COMPLETE"},
        {"transition_probe_records", probeJson},
        {"transition_probe_checked_count", static_cast<qint64>(probeResults.size())},
        {"transition_probe_mismatch_count", probeMismatch},
        {"transition_probe_render_thread_blocking_wait_count",
         probeCounters.renderThreadBlockingWaitCount},
        {"transition_probe_not_ready_poll_count", probeCounters.notReadyPollCount},
        {"transition_probe_untracked_submission_count",
         probeCounters.untrackedSubmissionCount},
        {"transition_probe_completion_failure_count", probeCounters.completionFailureCount},
        {"transition_probe_retirement_timeout_count", probeCounters.retirementTimeoutCount},
        {"transition_probe_pending_after_drain_count",
         static_cast<qint64>(probeCounters.pendingAfterDrainCount)},
        {"transition_probe_issue_failure_count",
         state_->transitionProbeIssueFailureCount.load()},
        {"cpu_reference_pixel_status", "PRECOMPUTED"},
        {"fixture_a_sha256", QString::fromStdString(cpuReferences_.fixtureASha256)},
        {"fixture_b_sha256", QString::fromStdString(cpuReferences_.fixtureBSha256)},
        {"cpu_reference_candidate_frame_count", 6},
        {"cpu_reference_candidate_probe_count",
         static_cast<qint64>(cpuReferences_.candidates.size())},
        {"measurement_video_first_frame", firstFrame},
        {"measurement_video_last_frame", lastFrame},
        {"measurement_video_displayed_unique_count", uniqueDisplayed},
        {"measurement_video_skipped_frame_count", skipped},
        {"measurement_non_increasing_display_count", nonIncreasing},
        {"effective_video_fps", effectiveFps},
        {"drop_rate", dropRate},
        {"measurement_pair_wait_count",
         delta(state_->videoPairWaitCount.load(), measurementBaseline_.pairWait)},
        {"measurement_target_superseded_count",
         delta(state_->videoTargetSupersededCount.load(), measurementBaseline_.targetSuperseded)},
        {"measurement_stale_discard_a",
         delta(state_->audioClockVideoStaleDiscardA.load(), measurementBaseline_.staleA)},
        {"measurement_stale_discard_b",
         delta(state_->audioClockVideoStaleDiscardB.load(), measurementBaseline_.staleB)},
        {"measurement_audio_underflow_count", underflow},
        {"measurement_audio_overflow_count", overflow},
        {"measurement_marker_mismatch_count", markerMismatch},
        {"measurement_mixed_pair_count", mixedPair},
        {"measurement_mixed_generation_count", mixedGeneration},
        {"measurement_stale_composition_epoch_count", staleEpoch},
        {"measurement_video_ahead_violation_count", ahead},
        {"measurement_clock_regression_count", clockRegression},
        {"measurement_video_qpc_master_fallback_count", qpcFallback},
        {"measurement_audio_clock_query_failure_count", audioClockQueryFailure},
        {"application_av_delta_ms", deltaDistribution(measurementDeltas, false)},
        {"application_av_delta_abs_ms", deltaDistribution(measurementDeltas, true)},
        {"application_av_projection_failure_count",
         static_cast<qint64>(records.size() - measurementDeltas.size())},
        {"endpoint_prefill_frames", static_cast<qint64>(sink.endpointPrefillFrames)},
        {"endpoint_first_media_sample", sink.endpointFirstMediaSample},
        {"clock_anchor_media_sample", sink.clockAnchorMediaSample},
        {"baseline_source_generation_a", static_cast<qint64>(baselineGenerationA_.value)},
        {"baseline_source_generation_b", static_cast<qint64>(baselineGenerationB_.value)},
        {"baseline_resource_epoch_a", static_cast<qint64>(baselineResourceEpochA_.value)},
        {"baseline_resource_epoch_b", static_cast<qint64>(baselineResourceEpochB_.value)},
        {"cpu_full_frame_readback_count", state_->readbacks.fullFrameReadbacks()},
        {"full_frame_gpu_copy_count", compositor.fullFrameGpuCopyCount},
        {"software_video_fallback_count", a.softwareFrameRejectCount + b.softwareFrameRejectCount},
        {"device_lost_count", state_->deviceLostCount.load()},
        {"lifecycle_violation_count", state_->lifecycleOrderViolationCount.load()},
        {"audio_render_thread_join_leak", static_cast<qint64>(sink.audioRenderThreadJoinLeak)},
        {"audio_decode_thread_join_leak",
         static_cast<qint64>(audioDecoder.audioDecodeThreadJoinLeak)},
        {"video_worker_a_joined", a.joined},
        {"video_worker_b_joined", b.joined},
        {"teardown_success", state_->teardownComplete.load()},
        {"final_report_after_teardown", state_->teardownComplete.load()},
        {"shutdown_sequence", shutdownSequenceJson},
        {"shutdown_workers_joined_before_teardown", shutdownSequence_.joinVerified},
        {"shutdown_render_teardown_requested", shutdownSequence_.renderTeardownRequested},
        {"shutdown_order_violation_count", shutdownSequence_.orderViolationCount},
        {"display_target_preflight_pass", displayPreflightPassed_},
        {"requested_output_width", 1920},
        {"requested_output_height", 1080},
        {"display_environment_start", displayEnvironmentJson(displayEnvironmentStart_)},
        {"display_environment_end", displayEnvironmentJson(displayEnvironmentEnd_)},
        {"adapter", QString::fromStdString(a.adapter.description)},
        {"audio_endpoint_sample_rate", sink.deviceFormat.sampleRate},
        {"audio_endpoint_channels", sink.deviceFormat.channels},
        {"audio_endpoint_sample_format",
         QString::fromStdString(sink.deviceFormat.sampleFormat)}};

    QSaveFile file(config_.metricsPath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit() && integrationSanityPass;
}

} // namespace mvm::app

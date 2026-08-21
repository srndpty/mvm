#include "p3_av_sync_controller.h"

#include <cstdio>

#include "media/audio_preview/audio_video_scheduler.h"
#include "media/gpu_preview/exact_frame_pairer.h"
#include "media/gpu_preview/qpc_clock.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>
#include <QSaveFile>
#include <QScreen>

#include <algorithm>
#include <cmath>
#include <random>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace mvm::app {
namespace {

std::vector<gpu::LayerLayout> p3Layout() {
    return {{gpu::SourceId{1}, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0f, 0},
            {gpu::SourceId{2}, {0.5f, 0.5f, 0.5f, 0.5f}, {0, 0, 1, 1}, 0.75f, 1}};
}

double qpcMs(long long begin, long long end) {
    return gpu::qpcUsBetween(begin, end) / 1000.0;
}

double nearestRank(const std::vector<double>& sorted, double p) {
    if (sorted.empty())
        return 0.0;
    const auto index = static_cast<size_t>(
        std::ceil(static_cast<double>(sorted.size()) * p) - 1.0);
    return sorted[std::min(index, sorted.size() - 1)];
}

QJsonObject deltaDistribution(const std::vector<double>& values, bool absolute) {
    std::vector<double> sorted = values;
    if (absolute) {
        for (double& value : sorted)
            value = std::abs(value);
    }
    std::sort(sorted.begin(), sorted.end());
    QJsonArray raw;
    for (double value : values)
        raw.append(value);
    QJsonObject result{{"count", static_cast<qint64>(values.size())},
                       {"p50", nearestRank(sorted, 0.50)},
                       {"p95", nearestRank(sorted, 0.95)},
                       {"min", sorted.empty() ? 0.0 : sorted.front()},
                       {"max", sorted.empty() ? 0.0 : sorted.back()}};
    if (absolute)
        result.insert("p99", nearestRank(sorted, 0.99));
    else
        result.insert("values", raw);
    return result;
}

QString modeName(P3AvMode mode) {
    switch (mode) {
    case P3AvMode::Playback: return QStringLiteral("playback");
    case P3AvMode::Seek: return QStringLiteral("seek");
    case P3AvMode::PauseResume: return QStringLiteral("pause-resume");
    }
    return QStringLiteral("unknown");
}

QString scheduleActionName(int value) {
    switch (static_cast<audio::AudioVideoScheduleAction>(value)) {
    case audio::AudioVideoScheduleAction::Hold: return QStringLiteral("Hold");
    case audio::AudioVideoScheduleAction::Request: return QStringLiteral("Request");
    case audio::AudioVideoScheduleAction::CatchUp: return QStringLiteral("CatchUp");
    case audio::AudioVideoScheduleAction::End: return QStringLiteral("End");
    case audio::AudioVideoScheduleAction::ClockRegression:
        return QStringLiteral("ClockRegression");
    case audio::AudioVideoScheduleAction::Invalid: return QStringLiteral("Invalid");
    }
    return QStringLiteral("NotObserved");
}

QString pairResultName(int value) {
    switch (static_cast<gpu::PairResult>(value)) {
    case gpu::PairResult::Paired: return QStringLiteral("Paired");
    case gpu::PairResult::WaitingForSource: return QStringLiteral("WaitingForSource");
    case gpu::PairResult::MissingA: return QStringLiteral("MissingA");
    case gpu::PairResult::MissingB: return QStringLiteral("MissingB");
    case gpu::PairResult::MissingBoth: return QStringLiteral("MissingBoth");
    case gpu::PairResult::StaleGeneration: return QStringLiteral("StaleGeneration");
    case gpu::PairResult::FutureGeneration: return QStringLiteral("FutureGeneration");
    case gpu::PairResult::MixedFrame: return QStringLiteral("MixedFrame");
    case gpu::PairResult::Rejected: return QStringLiteral("Rejected");
    }
    return QStringLiteral("NotObserved");
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

audio::RuntimeAttributionMode attributionMode(P3AvMode mode) {
    switch (mode) {
    case P3AvMode::Playback: return audio::RuntimeAttributionMode::Playback;
    case P3AvMode::Seek: return audio::RuntimeAttributionMode::Seek;
    case P3AvMode::PauseResume: return audio::RuntimeAttributionMode::PauseResume;
    }
    return audio::RuntimeAttributionMode::Unknown;
}

QString attributionModeName(audio::RuntimeAttributionMode mode) {
    switch (mode) {
    case audio::RuntimeAttributionMode::Playback: return QStringLiteral("playback");
    case audio::RuntimeAttributionMode::Seek: return QStringLiteral("seek");
    case audio::RuntimeAttributionMode::PauseResume: return QStringLiteral("pause-resume");
    case audio::RuntimeAttributionMode::Unknown: break;
    }
    return QStringLiteral("unknown");
}

QString clockRegressionSiteName(audio::ClockRegressionSite site) {
    switch (site) {
    case audio::ClockRegressionSite::SchedulerProjectionInvalid:
        return QStringLiteral("SchedulerProjectionInvalid");
    case audio::ClockRegressionSite::SchedulerDecision:
        return QStringLiteral("SchedulerDecision");
    case audio::ClockRegressionSite::DisplayProjectionInvalid:
        return QStringLiteral("DisplayProjectionInvalid");
    case audio::ClockRegressionSite::Unknown: break;
    }
    return QStringLiteral("Unknown");
}

QJsonObject attributionContextJson(const audio::RuntimeAttributionContextSnapshot& value) {
    return {{"mode", attributionModeName(value.mode)},
            {"engine_state", value.engineState},
            {"seek_ordinal", value.seekOrdinal},
            {"current_seek_target", value.currentSeekTarget},
            {"seek_pending", value.seekPending},
            {"decode_ready", value.decodeReady},
            {"requested_frame_presented", value.requestedFramePresented},
            {"pause_resume_cycle_ordinal", value.pauseResumeCycleOrdinal},
            {"resume_start_qpc", value.resumeStartQpc},
            {"audio_master_video_frame_count", value.audioMasterVideoFrameCount},
            {"canonical_last_seek_target", value.canonicalLastSeekTarget},
            {"current_seek_target_sample", value.currentSeekTargetSample},
            {"audio_decoder_eof", value.audioDecoderEof},
            {"actual_audio_end_exclusive", value.actualAudioEndExclusive},
            {"audio_start_qpc", value.audioStartQpc},
            {"first_exact_video_display_qpc", value.firstExactVideoDisplayQpc}};
}

QJsonObject audioConsumeTraceJson(const audio::AudioConsumeTraceEntry& value) {
    return {{"occurrence_qpc", value.occurrenceQpc},
            {"requested_sample_start", value.requestedSampleStart},
            {"requested_sample_count", value.requestedSampleCount},
            {"queued_samples_before_consume", value.queuedSamplesBeforeConsume},
            {"actually_consumed_samples", value.actuallyConsumedSamples},
            {"queued_samples_after_consume", value.queuedSamplesAfterConsume},
            {"queue_last_available_sample_exclusive", value.queueLastAvailableSampleExclusive},
            {"source_generation", static_cast<qint64>(value.sourceGeneration)}};
}

QJsonValue firstAudioUnderflowJson(const audio::RuntimeAttributionState& state) {
    const auto value = state.firstAudioUnderflow.snapshot();
    if (!value)
        return QJsonValue::Null;
    QJsonArray trace;
    for (std::size_t index = 0; index < value->consumeTraceCount; ++index)
        trace.append(audioConsumeTraceJson(value->consumeTrace[index]));
    const double audioStartToUnderflowMs =
        value->context.audioStartQpc > 0 && value->occurrenceQpc >= value->context.audioStartQpc
            ? qpcMs(value->context.audioStartQpc, value->occurrenceQpc)
            : -1.0;
    const double audioStartToFirstDisplayMs =
        value->context.audioStartQpc > 0 &&
                value->context.firstExactVideoDisplayQpc >= value->context.audioStartQpc
            ? qpcMs(value->context.audioStartQpc, value->context.firstExactVideoDisplayQpc)
            : -1.0;
    return QJsonObject{
        {"occurrence_qpc", value->occurrenceQpc},
        {"context", attributionContextJson(value->context)},
        {"requested_sample_start", value->requestedSampleStart},
        {"requested_sample_count", value->requestedSampleCount},
        {"queued_samples_before_consume", value->queuedSamplesBeforeConsume},
        {"actually_consumed_samples", value->actuallyConsumedSamples},
        {"queued_samples_after_consume", value->queuedSamplesAfterConsume},
        {"queue_last_available_sample_exclusive", value->queueLastAvailableSampleExclusive},
        {"source_generation", static_cast<qint64>(value->sourceGeneration)},
        {"audio_master_sample_position", value->audioMasterSamplePosition},
        {"endpoint_buffer_frames", static_cast<int>(value->endpointBufferFrames)},
        {"audio_start_to_underflow_ms", audioStartToUnderflowMs},
        {"audio_start_to_first_exact_video_display_ms", audioStartToFirstDisplayMs},
        {"consume_trace", trace}};
}

QJsonValue firstClockRegressionJson(const audio::RuntimeAttributionState& state) {
    const auto value = state.firstClockRegression.snapshot();
    if (!value)
        return QJsonValue::Null;
    const double resumeElapsedMs =
        value->context.resumeStartQpc > 0 && value->occurrenceQpc >= value->context.resumeStartQpc
            ? qpcMs(value->context.resumeStartQpc, value->occurrenceQpc)
            : -1.0;
    return QJsonObject{{"site", clockRegressionSiteName(value->site)},
                       {"occurrence_qpc", value->occurrenceQpc},
                       {"context", attributionContextJson(value->context)},
                       {"previous_frame", value->previousFrame},
                       {"candidate_frame", value->candidateFrame},
                       {"raw_audio_master_sample_position",
                        value->rawAudioMasterSamplePosition},
                       {"scheduler_target_frame", value->schedulerTargetFrame},
                       {"current_displayed_frame", value->currentDisplayedFrame},
                       {"source_generation", static_cast<qint64>(value->sourceGeneration)},
                       {"resume_elapsed_ms", resumeElapsedMs}};
}
} // namespace

P3AvSyncController::P3AvSyncController(P3AvConfig config, QObject* parent)
    : QObject(parent), config_(std::move(config)) {
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(2);
    connect(&timer_, &QTimer::timeout, this, &P3AvSyncController::tick);
}

void P3AvSyncController::setPhase(Phase phase) {
    phase_ = phase;
    if (state_)
        state_->runtimeAttribution.context.engineState.store(static_cast<int>(phase),
                                                             std::memory_order_release);
}

void P3AvSyncController::attach(CompositorRhiItem* item) {
    item_ = item;
    state_ = item->state();
    audioClock_ = std::make_shared<audio::AudioMasterClock>();
    state_->audioMasterClock = audioClock_;
    state_->runtimeAttribution.context.mode.store(static_cast<int>(attributionMode(config_.mode)),
                                                  std::memory_order_release);
    state_->runtimeAttribution.context.engineState.store(static_cast<int>(phase_),
                                                         std::memory_order_release);
    phaseTimer_.start();
    timer_.start();
}

DisplayEnvironmentSnapshot P3AvSyncController::captureDisplayEnvironment() const {
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

bool P3AvSyncController::openPipelines() {
    workerA_ = std::make_shared<gpu::SourceDecodeWorker>(gpu::SourceId{1}, state_->device,
                                                        state_->readbacks, 16);
    workerB_ = std::make_shared<gpu::SourceDecodeWorker>(gpu::SourceId{2}, state_->device,
                                                        state_->readbacks, 16);
    audioWorker_ = std::make_shared<audio::AudioDecodeWorker>(audio::SourceId{1});
    audioSink_ = std::make_shared<audio::WasapiAudioSink>(audioWorker_->queue(), *audioClock_);
    audioSink_->setRuntimeAttribution(&state_->runtimeAttribution);
    audioWorker_->setRuntimeAttribution(&state_->runtimeAttribution);
    std::string error;
    if (!workerA_->start(config_.sourceA.toUtf8().constData(), error) ||
        !workerB_->start(config_.sourceB.toUtf8().constData(), error) ||
        !audioWorker_->start(config_.sourceA.toUtf8().constData(), error) ||
        !audioSink_->open(error, audio::kVerificationSessionVolume)) {
        startShutdown(QString::fromStdString(error), true);
        return false;
    }
    const auto a = workerA_->snapshot();
    const auto b = workerB_->snapshot();
    if (a.info.frameCount <= 0 || b.info.frameCount <= 0) {
        startShutdown(QStringLiteral("A/B source frame count が不正です"), true);
        return false;
    }
    state_->audioMasterVideoFrameCount.store(std::min(a.info.frameCount, b.info.frameCount));
    {
        std::lock_guard<std::mutex> lock(state_->workerMutex);
        state_->workerA = workerA_;
        state_->workerB = workerB_;
    }
    if (config_.mode == P3AvMode::Seek) {
        constexpr long long kRequiredVideoPreroll = 8;
        const long long lastTarget =
            state_->audioMasterVideoFrameCount.load() - kRequiredVideoPreroll;
        canonicalLastSeekTarget_ = lastTarget;
        state_->runtimeAttribution.context.audioMasterVideoFrameCount.store(
            state_->audioMasterVideoFrameCount.load(), std::memory_order_release);
        state_->runtimeAttribution.context.canonicalLastSeekTarget.store(lastTarget,
                                                                         std::memory_order_release);
        if (lastTarget < 0) {
            startShutdown(QStringLiteral("8 frame pre-roll を確保できる seek target がありません"),
                          true);
            return false;
        }
        if (config_.diagnosticFixedSeekTarget >= 0) {
            if (config_.diagnosticFixedSeekTarget > lastTarget) {
                startShutdown(QStringLiteral("diagnostic fixed seek target がcanonical domain外です"),
                              true);
                return false;
            }
            seekTargets_.assign(static_cast<std::size_t>(config_.seekCount),
                                config_.diagnosticFixedSeekTarget);
        } else {
            std::mt19937 random(config_.seed);
            std::uniform_int_distribution<long long> target(0, lastTarget);
            for (int index = 0; index < config_.seekCount; ++index)
                seekTargets_.push_back(target(random));
        }
    } else {
        seekTargets_.push_back(0);
    }
    return true;
}

bool P3AvSyncController::startAtFrame(long long targetFrame, bool measurementStart) {
    auto& attribution = state_->runtimeAttribution.context;
    attribution.seekOrdinal.store(static_cast<std::int64_t>(seeks_.size() + 1),
                                  std::memory_order_release);
    attribution.currentSeekTarget.store(targetFrame, std::memory_order_release);
    attribution.currentSeekTargetSample.store(targetFrame * audio::kSamplesPerVideoFrame,
                                              std::memory_order_release);
    attribution.seekPending.store(true, std::memory_order_release);
    attribution.decodeReady.store(false, std::memory_order_release);
    attribution.requestedFramePresented.store(false, std::memory_order_release);
    attribution.audioStartQpc.store(0, std::memory_order_release);
    attribution.firstExactVideoDisplayQpc.store(0, std::memory_order_release);
    state_->audioMasterSchedulerEnabled.store(false, std::memory_order_release);
    state_->playbackSchedulerEnabled.store(false, std::memory_order_release);
    state_->audioMasterPendingSeekFrame.store(-1, std::memory_order_release);
    state_->requestedOutput.store(-1, std::memory_order_release);
    std::string error;
    if (!seeks_.empty()) {
        workerA_->pause();
        workerB_->pause();
        audioWorker_->pause();
        if (!audioSink_->pause(error) || !audioSink_->resetForSeek(error)) {
            startShutdown(QString::fromStdString(error), true);
            return false;
        }
    }

    const long long targetSample = targetFrame * audio::kSamplesPerVideoFrame;
    const auto beforeA = workerA_->snapshot();
    const auto beforeB = workerB_->snapshot();
    const auto beforeAudio = audioWorker_->snapshot();
    seekTimeoutStageEvidence_ = {};
    state_->p3SeekDiagnostics.reset(targetFrame);
    seekStaleABaseline_ = state_->audioClockVideoStaleDiscardA.load();
    seekStaleBBaseline_ = state_->audioClockVideoStaleDiscardB.load();
    seekPairWaitBaseline_ = state_->videoPairWaitCount.load();
    requestStartQpc_ = gpu::qpcTicks();
    gpu::SeekTicket ticketA;
    gpu::SeekTicket ticketB;
    audio::AudioSeekTicket audioTicket;
    // 全 request を先に dispatch し、completion 待ちはその後に行う。
    const auto requestA = workerA_->requestSeek(targetFrame, ticketA, error);
    const auto requestB = workerB_->requestSeek(targetFrame, ticketB, error);
    const auto requestAudio = audioWorker_->requestSeek(targetSample, audioTicket, error);
    if (requestA != gpu::SeekRequestResult::Accepted ||
        requestB != gpu::SeekRequestResult::Accepted ||
        requestAudio != audio::AudioSeekRequestResult::Accepted) {
        ++seekBusyAcceptanceCount_;
        startShutdown(QStringLiteral("integrated seek request が拒否されました"), true);
        return false;
    }
    gpu::SeekCompletion completionA;
    gpu::SeekCompletion completionB;
    audio::AudioSeekCompletion completionAudio;
    bool readyA = false;
    bool readyB = false;
    bool readyAudio = false;
    const auto commonDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!(readyA && readyB && readyAudio) && std::chrono::steady_clock::now() < commonDeadline) {
        if (!readyA) {
            const auto result = workerA_->waitSeek(ticketA, 0, completionA);
            readyA = result == gpu::SeekWaitResult::Ready;
            if (result == gpu::SeekWaitResult::StaleTicket)
                ++seekStaleCompletionCount_;
        }
        if (!readyB) {
            const auto result = workerB_->waitSeek(ticketB, 0, completionB);
            readyB = result == gpu::SeekWaitResult::Ready;
            if (result == gpu::SeekWaitResult::StaleTicket)
                ++seekStaleCompletionCount_;
        }
        if (!readyAudio) {
            const auto result = audioWorker_->waitSeek(audioTicket, 0, completionAudio);
            readyAudio = result == audio::AudioSeekWaitResult::Ready;
            if (result == audio::AudioSeekWaitResult::StaleTicket)
                ++seekStaleCompletionCount_;
        }
        if (!(readyA && readyB && readyAudio))
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!(readyA && readyB && readyAudio)) {
        ++seekTimeoutCount_;
        const auto da = workerA_->seekDiagnosticSnapshot();
        const auto db = workerB_->seekDiagnosticSnapshot();
        seekTimeoutDiagnostic_ = QStringLiteral(
            "A(ticket=%1 phase=%2 generation=%3 depth=%4 ready=%5) "
            "B(ticket=%6 phase=%7 generation=%8 depth=%9 ready=%10) "
            "audio(ticket=%11 generation=%12 depth=%13 ready=%14)")
                                     .arg(ticketA.requestId)
                                     .arg(QString::fromLatin1(gpu::toString(da.phase)))
                                     .arg(workerA_->snapshot().sourceGeneration.value)
                                     .arg(workerA_->buffer().depth())
                                     .arg(readyA)
                                     .arg(ticketB.requestId)
                                     .arg(QString::fromLatin1(gpu::toString(db.phase)))
                                     .arg(workerB_->snapshot().sourceGeneration.value)
                                     .arg(workerB_->buffer().depth())
                                     .arg(readyB)
                                     .arg(audioTicket.requestId)
                                     .arg(audioWorker_->snapshot().sourceGeneration.value)
                                     .arg(audioWorker_->queue().snapshot().queuedSamples)
                                     .arg(readyAudio);
        startShutdown(QStringLiteral("integrated seek 共通 5 秒 deadline timeout: ") +
                          seekTimeoutDiagnostic_,
                      true);
        return false;
    }
    if (
        completionA.status != gpu::SeekCompletionStatus::Completed ||
        completionB.status != gpu::SeekCompletionStatus::Completed || !completionAudio.completed ||
        completionA.decodedFrameNumber != targetFrame ||
        completionB.decodedFrameNumber != targetFrame ||
        completionAudio.firstOutputSample != targetSample) {
        startShutdown(QStringLiteral("integrated seek completion が exact contract と不一致です"),
                      true);
        return false;
    }
    const long long allReadyQpc = gpu::qpcTicks();
    attribution.decodeReady.store(true, std::memory_order_release);
    seekTimeoutStageEvidence_ = {
        {"seek_request_qpc", requestStartQpc_},
        {"requested_frame", targetFrame},
        {"requested_audio_sample", targetSample},
        {"source_a",
         QJsonObject{{"requested_generation",
                      static_cast<qint64>(beforeA.sourceGeneration.value + 1)},
                     {"ready_generation",
                      static_cast<qint64>(completionA.sourceGeneration.value)},
                     {"ready_qpc", completionA.decodeReadyQpc}}},
        {"source_b",
         QJsonObject{{"requested_generation",
                      static_cast<qint64>(beforeB.sourceGeneration.value + 1)},
                     {"ready_generation",
                      static_cast<qint64>(completionB.sourceGeneration.value)},
                     {"ready_qpc", completionB.decodeReadyQpc}}},
        {"audio",
         QJsonObject{{"requested_generation",
                      static_cast<qint64>(beforeAudio.sourceGeneration.value + 1)},
                     {"ready_generation",
                      static_cast<qint64>(completionAudio.seekGeneration.value)},
                     {"first_ready_sample", completionAudio.firstOutputSample},
                     {"ready_qpc", completionAudio.readyQpc}}},
        {"all_media_ready_qpc", allReadyQpc}};
    const auto snapshotA = workerA_->snapshot();
    const auto snapshotB = workerB_->snapshot();
    if (snapshotA.decodeDevicePointer != state_->nativeDevicePointer.load() ||
        snapshotB.decodeDevicePointer != state_->nativeDevicePointer.load() ||
        !snapshotA.adapter.sameAdapterAs(state_->qtAdapter) ||
        !snapshotB.adapter.sameAdapterAs(state_->qtAdapter)) {
        startShutdown(QStringLiteral("A/B decode texture device が Qt device と一致しません"),
                      true);
        return false;
    }
    if (!coordinatorConfigured_) {
        if (state_->coordinator.configure(
                p3Layout(), {{gpu::SourceId{1}, completionA.sourceGeneration},
                             {gpu::SourceId{2}, completionB.sourceGeneration}}) !=
            gpu::ConfigureResult::Configured) {
            startShutdown(QStringLiteral("P3-B compositor coordinator を初期化できません"), true);
            return false;
        }
        coordinatorConfigured_ = true;
    } else if (!state_->coordinator.setSourceGeneration(gpu::SourceId{1},
                                                        completionA.sourceGeneration) ||
               !state_->coordinator.setSourceGeneration(gpu::SourceId{2},
                                                        completionB.sourceGeneration)) {
        startShutdown(QStringLiteral("integrated seek generation を adopt できません"), true);
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
        const auto queue = audioWorker_->queue().snapshot();
        measurementBaseline_ = {
            state_->videoPairWaitCount.load(),
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
        {
            std::lock_guard<std::mutex> lock(state_->p3MeasurementDisplayMutex);
            state_->p3MeasurementDisplays.clear();
        }
        const long long requiredSamples =
            static_cast<long long>(config_.durationSeconds) * audio::kInternalSampleRate;
        state_->p3MeasurementEndSampleExclusive.store(requiredSamples,
                                                       std::memory_order_release);
        state_->p3MeasurementActive.store(true, std::memory_order_release);
    }
    if (!audioSink_->play(targetSample, completionAudio.seekGeneration, error)) {
        startShutdown(QString::fromStdString(error), true);
        return false;
    }
    audioStartQpc_ = gpu::qpcTicks();
    const auto sink = audioSink_->snapshot();
    SeekRecord record;
    record.requestedFrame = targetFrame;
    record.requestedAudioSample = targetSample;
    record.audioGeneration = completionAudio.seekGeneration.value;
    record.videoGenerationA = completionA.sourceGeneration.value;
    record.videoGenerationB = completionB.sourceGeneration.value;
    record.firstAudioSample = sink.endpointFirstMediaSample;
    record.audioSeekReadyMs = completionAudio.latencyMs;
    record.videoAReadyMs = completionA.decodeReadyMs;
    record.videoBReadyMs = completionB.decodeReadyMs;
    record.allMediaReadyMs = qpcMs(requestStartQpc_, allReadyQpc);
    seeks_.push_back(record);

    displayBaseline_ = state_->ledger.baseline();
    displayExpectation_ =
        {targetFrame,
         state_->coordinator.compositionEpoch(),
         {{gpu::SourceId{1}, completionA.sourceGeneration, completionA.resourceEpoch, targetFrame},
          {gpu::SourceId{2}, completionB.sourceGeneration, completionB.resourceEpoch,
           targetFrame}}};
    state_->audioMasterGeneration.store(completionAudio.seekGeneration.value,
                                        std::memory_order_release);
    state_->audioMasterLastDisplayed.store(targetFrame - 1, std::memory_order_release);
    state_->audioMasterLastRequested.store(-1, std::memory_order_release);
    state_->audioMasterMarkerProbePending.store(true, std::memory_order_release);
    state_->audioMasterPendingSeekFrame.store(targetFrame, std::memory_order_release);
    attribution.engineState.store(static_cast<int>(Phase::WaitDisplay), std::memory_order_release);
    state_->audioMasterSchedulerEnabled.store(true, std::memory_order_release);
    phaseTimer_.restart();
    setPhase(Phase::WaitDisplay);
    item_->update();
    return true;
}

bool P3AvSyncController::pollFirstDisplay() {
    gpu::CompositionDisplayRecord display;
    if (!state_->ledger.findAfter(displayBaseline_, displayExpectation_, display)) {
        if (phaseTimer_.elapsed() > config_.displayTimeoutMs) {
            captureSeekTimeoutStageEvidence();
            startShutdown(QStringLiteral("first integrated video display が timeout しました"),
                          true);
        }
        return false;
    }
    SeekRecord& record = seeks_.back();
    record.firstDisplayedVideoFrame = display.outputFrameNumber;
    record.resumeToFirstVideoMs = qpcMs(audioStartQpc_, display.displayRecordQpc);
    record.requestToFirstVideoMs = qpcMs(requestStartQpc_, display.displayRecordQpc);
    record.firstDisplayApplicationAvProjectionValid = display.applicationAvProjectionValid;
    record.firstDisplayApplicationAvDeltaMs = display.applicationAvDeltaMs;
    state_->runtimeAttribution.context.firstExactVideoDisplayQpc.store(
        display.displayRecordQpc, std::memory_order_release);
    state_->p3SeekDiagnostics.active.store(false, std::memory_order_release);
    if (record.firstAudioSample != record.requestedAudioSample ||
        record.firstDisplayedVideoFrame != record.requestedFrame) {
        startShutdown(QStringLiteral("first audio/video identity が seek target と不一致です"),
                      true);
        return false;
    }
    state_->runtimeAttribution.context.requestedFramePresented.store(true,
                                                                     std::memory_order_release);
    state_->runtimeAttribution.context.seekPending.store(false, std::memory_order_release);
    return true;
}

void P3AvSyncController::captureSeekTimeoutStageEvidence() {
    if (!state_ || !workerA_ || !workerB_ || !audioWorker_ || !audioSink_ || !audioClock_)
        return;
    state_->p3SeekDiagnostics.active.store(false, std::memory_order_release);
    const auto sourceA = workerA_->snapshot();
    const auto sourceB = workerB_->snapshot();
    const auto bufferA = workerA_->buffer().snapshot();
    const auto bufferB = workerB_->buffer().snapshot();
    const auto audio = audioWorker_->snapshot();
    const auto sink = audioSink_->snapshot();
    const auto clock = audioClock_->snapshot();
    const long long nowQpc = gpu::qpcTicks();
    audio::Qpc100ns now100ns;
    const bool qpcConverted = audio::qpcTicksTo100ns(
        {static_cast<unsigned long long>(nowQpc)}, gpu::qpcFrequencyTicks(), now100ns);
    const audio::SourceGeneration expectedGeneration{
        state_->audioMasterGeneration.load(std::memory_order_acquire)};
    const auto projection = qpcConverted
                                ? audio::projectAtQpc100ns(clock, now100ns, expectedGeneration)
                                : audio::AudioClockProjection{};
    const auto& render = state_->p3SeekDiagnostics;
    auto sourceJson = [](const gpu::SourceDecoderSnapshot& worker,
                         const gpu::SourceFrameBufferSnapshot& buffer) {
        return QJsonObject{{"current_generation",
                            static_cast<qint64>(worker.sourceGeneration.value)},
                           {"resource_epoch", static_cast<qint64>(worker.resourceEpoch.value)},
                           {"buffer_generation",
                            static_cast<qint64>(buffer.generation.value)},
                           {"buffer_front_frame", buffer.frontFrame},
                           {"buffer_back_frame", buffer.backFrame},
                           {"buffer_depth", static_cast<qint64>(buffer.depth)},
                           {"eof", worker.eof}};
    };
    QJsonObject sourceAJson = seekTimeoutStageEvidence_.value("source_a").toObject();
    QJsonObject sourceBJson = seekTimeoutStageEvidence_.value("source_b").toObject();
    const auto sourceALive = sourceJson(sourceA, bufferA);
    const auto sourceBLive = sourceJson(sourceB, bufferB);
    for (auto it = sourceALive.begin(); it != sourceALive.end(); ++it)
        sourceAJson.insert(it.key(), it.value());
    for (auto it = sourceBLive.begin(); it != sourceBLive.end(); ++it)
        sourceBJson.insert(it.key(), it.value());
    seekTimeoutStageEvidence_.insert("source_a", sourceAJson);
    seekTimeoutStageEvidence_.insert("source_b", sourceBJson);
    QJsonObject audioJson = seekTimeoutStageEvidence_.value("audio").toObject();
    audioJson.insert("current_generation", static_cast<qint64>(audio.sourceGeneration.value));
    audioJson.insert("eof", audio.eof);
    seekTimeoutStageEvidence_.insert("audio", audioJson);
    seekTimeoutStageEvidence_.insert(
        "wasapi_clock",
        QJsonObject{{"sink_started", sink.running},
                    {"clock_generation", static_cast<qint64>(clock.generation.value)},
                    {"anchor_valid", sink.clockAnchorMediaSample >= 0 && clock.running},
                    {"current_media_sample", clock.mediaSamplePosition},
                    {"projected_media_sample", projection.mediaSample},
                    {"last_clock_query_result", projection.valid ? "VALID" : "INVALID"},
                    {"clock_query_failure_count",
                     static_cast<qint64>(clock.clockQueryFailureCount)}});
    seekTimeoutStageEvidence_.insert(
        "video_scheduler",
        QJsonObject{{"last_displayed", render.schedulerLastDisplayed.load()},
                    {"last_requested", render.schedulerLastRequested.load()},
                    {"target_frame", render.schedulerTargetFrame.load()},
                    {"last_action", scheduleActionName(render.schedulerLastAction.load())},
                    {"skipped_frames", render.schedulerSkippedFrames.load()},
                    {"first_target_frame", render.schedulerFirstTargetFrame.load()},
                    {"first_action", scheduleActionName(render.schedulerFirstAction.load())},
                    {"first_skipped_frames", render.schedulerFirstSkippedFrames.load()}});
    seekTimeoutStageEvidence_.insert(
        "pair_render",
        QJsonObject{{"render_callback_count_after_seek", render.renderCallbackCount.load()},
                    {"last_render_callback_qpc", render.lastRenderCallbackQpc.load()},
                    {"pair_attempt_count", render.pairAttemptCount.load()},
                    {"last_pair_result", pairResultName(render.lastPairResult.load())},
                    {"stale_discard_a",
                     state_->audioClockVideoStaleDiscardA.load() - seekStaleABaseline_},
                    {"stale_discard_b",
                     state_->audioClockVideoStaleDiscardB.load() - seekStaleBBaseline_},
                    {"pair_wait_count",
                     state_->videoPairWaitCount.load() - seekPairWaitBaseline_},
                    {"exact_pair_formed_qpc", render.exactPairFormedQpc.load()},
                    {"gpu_compose_submitted_qpc", render.gpuComposeSubmittedQpc.load()},
                    {"gpu_completion_observed_qpc", render.gpuCompletionObservedQpc.load()},
                    {"display_ledger_append_qpc", render.displayLedgerAppendQpc.load()}});
    seekTimeoutStageEvidence_.insert(
        "coordinator",
        QJsonObject{{"source_generation_a",
                     static_cast<qint64>(
                         state_->coordinator.sourceGeneration(gpu::SourceId{1}).value)},
                    {"source_generation_b",
                     static_cast<qint64>(
                         state_->coordinator.sourceGeneration(gpu::SourceId{2}).value)},
                    {"resource_epoch_a", static_cast<qint64>(sourceA.resourceEpoch.value)},
                    {"resource_epoch_b", static_cast<qint64>(sourceB.resourceEpoch.value)},
                    {"composition_epoch",
                     static_cast<qint64>(state_->coordinator.compositionEpoch().value)}});
    seekTimeoutStageEvidence_.insert("timeout_qpc", nowQpc);
}

void P3AvSyncController::startShutdown(const QString& reason, bool failure) {
    if (phase_ == Phase::ShutdownWait || phase_ == Phase::Done)
        return;
    shutdownEntered_ = true;
    shutdownEnteredFromPlayback_ = phase_ == Phase::Playback;
    shutdownReason_ = reason;
    if (config_.formalContractC2)
        displayEnvironmentEnd_ = captureDisplayEnvironment();
    if (failure) {
        exitCode_ = 3;
        // 理由を metrics JSON にしか残さないと、CTest の --output-on-failure では
        // 無言で落ちたようにしか見えない。P4 spike と同じ扱いに揃える。
        std::fprintf(stderr, "[p3] shutdown (failure): %s\n", reason.toUtf8().constData());
    }
    state_->audioMasterSchedulerEnabled.store(false, std::memory_order_release);
    state_->playbackSchedulerEnabled.store(false, std::memory_order_release);
    state_->audioMasterPendingSeekFrame.store(-1, std::memory_order_release);
    state_->requestedOutput.store(-1, std::memory_order_release);
    std::string ignored;
    if (audioSink_)
        audioSink_->pause(ignored);
    if (audioWorker_)
        audioWorker_->stop();
    if (audioSink_)
        audioSink_->stop();
    if (workerA_)
        workerA_->stop();
    if (workerB_)
        workerB_->stop();
    {
        std::lock_guard<std::mutex> lock(state_->workerMutex);
        state_->workerA.reset();
        state_->workerB.reset();
    }
    item_->requestTeardown();
    phaseTimer_.restart();
    setPhase(Phase::ShutdownWait);
}

void P3AvSyncController::tick() {
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
    if (audioSink_ && phase_ != Phase::ShutdownWait && phase_ != Phase::Done) {
        const auto sink = audioSink_->snapshot();
        if (sink.deviceFailureCount != 0) {
            startShutdown(QStringLiteral("WASAPI runtime failure: ") +
                              QString::fromStdString(sink.lastError),
                          true);
            return;
        }
        if (config_.injectRenderFaultAfterPlaying && !renderFaultInjectedAfterPlaying_ &&
            phase_ == Phase::Playback && sink.running) {
            // T0と同じrender thread -> recordFailure()経路を、再生開始後にだけ発火させる。
            renderFaultInjectedAfterPlaying_ = true;
            audioSink_->injectRenderFaultForTest();
        }
    }
    switch (phase_) {
    case Phase::WaitDevice:
        if (state_->deviceReady.load(std::memory_order_acquire)) {
            phaseTimer_.restart();
            setPhase(config_.formalContractC2 ? Phase::DisplayPreflight : Phase::Start);
        }
        else if (phaseTimer_.elapsed() > 10000)
            startShutdown(QStringLiteral("Qt/D3D11 device ready timeout"), true);
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
        formalWorkloadStarted_ = true;
        setPhase(Phase::Start);
        break;
    }
    case Phase::Start:
        if (openPipelines()) {
            seekIndex_ = 0;
            const bool measurementStart =
                config_.formalContract && config_.mode == P3AvMode::PauseResume;
            startAtFrame(seekTargets_.front(), measurementStart);
        }
        break;
    case Phase::WaitDisplay:
        if (!pollFirstDisplay())
            break;
        if (config_.mode == P3AvMode::Seek) {
            ++seekIndex_;
            if (seekIndex_ >= seekTargets_.size())
                startShutdown(QStringLiteral("integrated seek 完了"), false);
            else
                startAtFrame(seekTargets_[seekIndex_]);
        } else {
            runTimer_.restart();
            if (config_.formalContract && config_.mode == P3AvMode::Playback &&
                !warmupResetComplete_)
                setPhase(Phase::Warmup);
            else
                setPhase(config_.mode == P3AvMode::PauseResume ? Phase::PauseStart
                                                               : Phase::Playback);
        }
        break;
    case Phase::Warmup: {
        const auto clock = audioClock_->snapshot();
        if (clock.mediaSamplePosition >=
            static_cast<long long>(config_.warmupSeconds) * audio::kInternalSampleRate) {
            warmupResetComplete_ = true;
            startAtFrame(0, true);
        }
        break;
    }
    case Phase::Playback:
        if (config_.formalContract) {
            const auto clock = audioClock_->snapshot();
            const long long required =
                static_cast<long long>(config_.durationSeconds) * audio::kInternalSampleRate;
            if (clock.mediaSamplePosition >= required) {
                observedMeasurementEndSample_ = clock.mediaSamplePosition;
                state_->p3MeasurementActive.store(false, std::memory_order_release);
                state_->audioMasterSchedulerEnabled.store(false, std::memory_order_release);
                startShutdown(QStringLiteral("audio sample formal interval 完了"), false);
            }
        } else if (runTimer_.elapsed() >= config_.durationSeconds * 1000) {
            startShutdown(QStringLiteral("playback 完了"), false);
        }
        break;
    case Phase::PauseStart:
        if (runTimer_.elapsed() >= 2000) {
            state_->runtimeAttribution.context.pauseResumeCycleOrdinal.fetch_add(
                1, std::memory_order_acq_rel);
            state_->audioMasterSchedulerEnabled.store(false, std::memory_order_release);
            workerA_->pause();
            workerB_->pause();
            audioWorker_->pause();
            std::string error;
            if (!audioSink_->pause(error)) {
                startShutdown(QString::fromStdString(error), true);
                break;
            }
            pauseAudioSample_ = audioClock_->snapshot().mediaSamplePosition;
            pauseVideoFrame_ = state_->audioMasterLastDisplayed.load();
            pauseDisplayCount_ = state_->displayedCompositionCount.load();
            pauseGeneration_ = static_cast<long long>(state_->audioMasterGeneration.load());
            pauseQpcFallback_ = state_->videoQpcMasterFallbackCount.load();
            phaseTimer_.restart();
            setPhase(Phase::PauseWait);
        }
        break;
    case Phase::PauseWait:
        if (phaseTimer_.elapsed() >= 1000) {
            pauseFrozen_ = audioClock_->snapshot().mediaSamplePosition == pauseAudioSample_;
            pauseVideoAdvanceZero_ = state_->audioMasterLastDisplayed.load() == pauseVideoFrame_ &&
                                     state_->displayedCompositionCount.load() == pauseDisplayCount_;
            const auto audioGeneration = audioWorker_->snapshot().sourceGeneration.value;
            const auto aGeneration = workerA_->snapshot().sourceGeneration.value;
            const auto bGeneration = workerB_->snapshot().sourceGeneration.value;
            pauseGenerationStable_ = audioGeneration == state_->audioMasterGeneration.load() &&
                                     static_cast<long long>(audioGeneration) == pauseGeneration_ &&
                                     aGeneration == displayExpectation_.sources[0].sourceGeneration.value &&
                                     bGeneration == displayExpectation_.sources[1].sourceGeneration.value &&
                                     state_->videoQpcMasterFallbackCount.load() == pauseQpcFallback_;
            workerA_->play();
            workerB_->play();
            audioWorker_->play();
            std::string error;
            if (!audioSink_->play(pauseAudioSample_, audioWorker_->snapshot().sourceGeneration,
                                  error)) {
                startShutdown(QString::fromStdString(error), true);
                break;
            }
            auto& attribution = state_->runtimeAttribution.context;
            attribution.resumeStartQpc.store(gpu::qpcTicks(), std::memory_order_release);
            attribution.engineState.store(static_cast<int>(Phase::ResumePlayback),
                                          std::memory_order_release);
            state_->audioMasterSchedulerEnabled.store(true, std::memory_order_release);
            runTimer_.restart();
            setPhase(Phase::ResumePlayback);
        }
        break;
    case Phase::ResumePlayback:
        if (runTimer_.elapsed() >= 2000) {
            state_->p3MeasurementActive.store(false, std::memory_order_release);
            startShutdown(QStringLiteral("pause/resume 完了"),
                          !(pauseFrozen_ && pauseVideoAdvanceZero_ && pauseGenerationStable_));
        }
        break;
    case Phase::ShutdownWait:
        if (state_->teardownComplete.load(std::memory_order_acquire)) {
            if (!writeMetrics())
                exitCode_ = 4;
            setPhase(Phase::Done);
            timer_.stop();
            Q_EMIT finished();
        } else if (phaseTimer_.elapsed() > 10000) {
            exitCode_ = 3;
            shutdownReason_ = QStringLiteral("GPU teardown timeout");
            writeMetrics();
            setPhase(Phase::Done);
            timer_.stop();
            Q_EMIT finished();
        }
        break;
    case Phase::Done:
        break;
    }
}

bool P3AvSyncController::writeMetrics() const {
    const auto audioDecoder = audioWorker_ ? audioWorker_->snapshot() : audio::AudioDecoderSnapshot{};
    const auto queue = audioWorker_ ? audioWorker_->queue().snapshot() : audio::AudioQueueSnapshot{};
    const auto sink = audioSink_ ? audioSink_->snapshot() : audio::WasapiSnapshot{};
    const auto a = workerA_ ? workerA_->snapshot() : gpu::SourceDecoderSnapshot{};
    const auto b = workerB_ ? workerB_->snapshot() : gpu::SourceDecoderSnapshot{};
    std::vector<double> deltas;
    {
        std::lock_guard<std::mutex> lock(state_->applicationAvDeltaMutex);
        deltas = state_->applicationAvDeltaMs;
    }
    if (config_.formalContract) {
        std::vector<P3MeasurementDisplayRecord> measurementDisplays;
        {
            std::lock_guard<std::mutex> lock(state_->p3MeasurementDisplayMutex);
            measurementDisplays = state_->p3MeasurementDisplays;
        }
        const auto clock = audioClock_ ? audioClock_->snapshot() : audio::AudioClockSnapshot{};
        const auto& compositor = state_->compositor.counters();
        const long long requiredSamples =
            static_cast<long long>(config_.durationSeconds) * audio::kInternalSampleRate;
        const long long requiredFrames = requiredSamples / audio::kSamplesPerVideoFrame;
        long long firstFrame = -1;
        long long lastFrame = -1;
        long long uniqueDisplayed = 0;
        long long skipped = 0;
        long long duplicate = 0;
        long long nonIncreasing = 0;
        std::vector<double> measurementDeltas;
        QJsonArray displayJson;
        for (const auto& display : measurementDisplays) {
            displayJson.append(
                QJsonObject{{"frame", display.outputFrameNumber},
                            {"display_record_qpc", display.displayRecordQpc},
                            {"application_av_projection_valid",
                             display.applicationAvProjectionValid},
                            {"application_av_delta_ms", display.applicationAvDeltaMs}});
            if (firstFrame < 0)
                firstFrame = display.outputFrameNumber;
            if (lastFrame >= 0) {
                if (display.outputFrameNumber == lastFrame)
                    ++duplicate;
                if (display.outputFrameNumber <= lastFrame)
                    ++nonIncreasing;
                else
                    skipped += display.outputFrameNumber - lastFrame - 1;
            }
            if (display.outputFrameNumber != lastFrame)
                ++uniqueDisplayed;
            lastFrame = display.outputFrameNumber;
            if (display.applicationAvProjectionValid)
                measurementDeltas.push_back(display.applicationAvDeltaMs);
        }
        if (config_.mode == P3AvMode::Playback && lastFrame >= 0 && lastFrame < requiredFrames)
            skipped += requiredFrames - lastFrame - 1;

        std::vector<double> seekLatencies;
        std::vector<double> seekDeltas;
        QJsonArray seekJson;
        for (const auto& value : seeks_) {
            seekLatencies.push_back(value.requestToFirstVideoMs);
            if (value.firstDisplayApplicationAvProjectionValid)
                seekDeltas.push_back(value.firstDisplayApplicationAvDeltaMs);
            seekJson.append(QJsonObject{
                {"requested_frame", value.requestedFrame},
                {"requested_audio_sample", value.requestedAudioSample},
                {"audio_generation", static_cast<qint64>(value.audioGeneration)},
                {"video_generation_a", static_cast<qint64>(value.videoGenerationA)},
                {"video_generation_b", static_cast<qint64>(value.videoGenerationB)},
                {"first_audio_sample", value.firstAudioSample},
                {"first_displayed_video_frame", value.firstDisplayedVideoFrame},
                {"audio_ready_ms", value.audioSeekReadyMs},
                {"video_a_ready_ms", value.videoAReadyMs},
                {"video_b_ready_ms", value.videoBReadyMs},
                {"all_media_ready_ms", value.allMediaReadyMs},
                {"resume_to_display_ms", value.resumeToFirstVideoMs},
                {"request_to_first_display_ms", value.requestToFirstVideoMs},
                {"first_display_application_av_projection_valid",
                 value.firstDisplayApplicationAvProjectionValid},
                {"first_display_application_av_delta_ms",
                 value.firstDisplayApplicationAvDeltaMs}});
        }

        const auto delta = [&](long long current, long long baseline) {
            return std::max(0LL, current - baseline);
        };
        const bool playbackAccounting =
            config_.mode != P3AvMode::Playback ||
            (firstFrame == 0 && nonIncreasing == 0 && uniqueDisplayed + skipped == requiredFrames);
        const bool seekIdentity = std::all_of(seeks_.begin(), seeks_.end(), [](const auto& value) {
            return value.firstAudioSample == value.requestedAudioSample &&
                   value.firstDisplayedVideoFrame == value.requestedFrame &&
                   value.firstDisplayApplicationAvProjectionValid;
        });
        const bool modeCount = config_.mode != P3AvMode::Seek ||
                               seeks_.size() == static_cast<size_t>(config_.seekCount);
        const long long underflow = delta(static_cast<long long>(queue.underflowCount),
                                          measurementBaseline_.underflow);
        const long long overflow = delta(static_cast<long long>(queue.overflowRejectCount),
                                         measurementBaseline_.overflow);
        const long long markerMismatch =
            delta(state_->markerAMismatch.load(), measurementBaseline_.markerAMismatch) +
            delta(state_->markerBMismatch.load(), measurementBaseline_.markerBMismatch);
        const long long mixedPair = delta(state_->coordinator.mixedSourceFrameCount(),
                                          measurementBaseline_.mixedPair);
        const long long mixedGeneration = delta(state_->coordinator.mixedGenerationCount(),
                                                measurementBaseline_.mixedGeneration);
        const long long staleEpoch = delta(state_->coordinator.staleCompositionEpochCount(),
                                           measurementBaseline_.staleEpoch);
        const long long ahead =
            delta(state_->videoAheadViolationCount.load(), measurementBaseline_.ahead);
        const long long clockRegression =
            delta(state_->videoClockRegressionCount.load(), measurementBaseline_.clockRegression);
        const long long qpcFallback = delta(state_->videoQpcMasterFallbackCount.load(),
                                            measurementBaseline_.qpcFallback);
        const long long audioClockQueryFailure =
            delta(static_cast<long long>(clock.clockQueryFailureCount),
                  measurementBaseline_.audioClockQueryFailure);
        const bool displayCorrectness =
            !config_.formalContractC2 ||
            (displayPreflightPassed_ && formalWorkloadStarted_ &&
             sameDisplayEnvironment(displayEnvironmentStart_, displayEnvironmentEnd_));
        const bool correctness =
            exitCode_ == 0 && playbackAccounting && modeCount && seekIdentity && underflow == 0 &&
            overflow == 0 && markerMismatch == 0 && mixedPair == 0 && mixedGeneration == 0 &&
            staleEpoch == 0 && ahead == 0 && clockRegression == 0 && qpcFallback == 0 &&
            audioClockQueryFailure == 0 && seekTimeoutCount_ == 0 &&
            seekBusyAcceptanceCount_ == 0 && seekGenerationMismatchCount_ == 0 &&
            seekStaleCompletionCount_ == 0 && state_->deviceLostCount.load() == 0 &&
            state_->lifecycleOrderViolationCount.load() == 0 &&
            state_->readbacks.fullFrameReadbacks() == 0 &&
            compositor.fullFrameGpuCopyCount == 0 && a.softwareFrameRejectCount == 0 &&
            b.softwareFrameRejectCount == 0 && sink.deviceFailureCount == 0 &&
            sink.audioRenderThreadJoinLeak == 0 &&
            audioDecoder.audioDecodeThreadJoinLeak == 0 && a.joined && b.joined && sink.joined &&
            audioDecoder.joined && displayCorrectness &&
            (config_.mode != P3AvMode::PauseResume ||
             (pauseFrozen_ && pauseVideoAdvanceZero_ && pauseGenerationStable_));

        QJsonObject root{
            {"schema", config_.formalContractC2 ? "mvm-p3-formal-2" : "mvm-p3-formal-1"},
            {"contract_version", config_.formalContractC2 ? "P3-C-2" : "P3-C-1"},
            {"phase", "P3-C"},
            {"formal_verdict", "NOT_RUN"},
            {"mode", modeName(config_.mode)},
            {"pass", correctness},
            {"detail", shutdownReason_},
            {"audio_master_only", true},
            {"configured_video_preroll_frames", 8},
            {"configured_audio_preroll_ms", audio::kAudioPrerollMs},
            {"warmup_seconds", config_.warmupSeconds},
            {"measurement_seconds", config_.durationSeconds},
            {"measurement_audio_start_sample", 0},
            {"measurement_audio_end_sample", requiredSamples},
            {"observed_audio_stop_sample", observedMeasurementEndSample_},
            {"required_measurement_samples", requiredSamples},
            {"required_video_frames", requiredFrames},
            {"measurement_video_first_frame", firstFrame},
            {"measurement_video_last_frame", lastFrame},
            {"measurement_video_displayed_unique_count", uniqueDisplayed},
            {"measurement_video_skipped_frame_count", skipped},
            {"measurement_duplicate_display_identity_count", duplicate},
            {"measurement_non_increasing_display_count", nonIncreasing},
            {"measurement_display_records", displayJson},
            {"effective_video_fps",
             config_.mode == P3AvMode::Playback
                 ? static_cast<double>(uniqueDisplayed) / config_.durationSeconds
                 : 0.0},
            {"drop_rate", config_.mode == P3AvMode::Playback
                              ? static_cast<double>(skipped) / requiredFrames
                              : 0.0},
            {"measurement_pair_wait_count",
             delta(state_->videoPairWaitCount.load(), measurementBaseline_.pairWait)},
            {"measurement_target_superseded_count",
             delta(state_->videoTargetSupersededCount.load(),
                   measurementBaseline_.targetSuperseded)},
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
            {"measurement_audio_clock_query_failure_count",
             audioClockQueryFailure},
            {"application_av_delta_ms", deltaDistribution(measurementDeltas, false)},
            {"application_av_delta_abs_ms", deltaDistribution(measurementDeltas, true)},
            {"application_av_projection_failure_count",
             static_cast<qint64>(measurementDisplays.size() - measurementDeltas.size())},
            {"endpoint_prefill_frames", static_cast<qint64>(sink.endpointPrefillFrames)},
            {"endpoint_buffer_frames", static_cast<int>(sink.endpointBufferFrames)},
            {"endpoint_first_media_sample", sink.endpointFirstMediaSample},
            {"audio_decoder_eof", audioDecoder.eof},
            {"actual_audio_end_exclusive", audioDecoder.actualLastDecodedSampleExclusive},
            {"audio_master_video_frame_count", state_->audioMasterVideoFrameCount.load()},
            {"canonical_last_seek_target", canonicalLastSeekTarget_},
            {"diagnostic_fixed_seek_target", config_.diagnosticFixedSeekTarget},
            {"clock_anchor_media_sample", sink.clockAnchorMediaSample},
            {"integrated_seek_requested", config_.mode == P3AvMode::Seek ? config_.seekCount : 0},
            {"integrated_seek_exact",
             config_.mode == P3AvMode::Seek ? static_cast<int>(seeks_.size()) : 0},
            {"integrated_seek_timeout_count", seekTimeoutCount_},
            {"integrated_seek_busy_acceptance_count", seekBusyAcceptanceCount_},
            {"integrated_seek_stale_completion_count", seekStaleCompletionCount_},
            {"integrated_seek_generation_mismatch_count", seekGenerationMismatchCount_},
            {"seek_timeout_diagnostic", seekTimeoutDiagnostic_},
            {"seek_timeout_stage_evidence", seekTimeoutStageEvidence_},
            {"seek_request_to_first_display_ms", deltaDistribution(seekLatencies, false)},
            {"seek_first_display_application_av_delta_ms", deltaDistribution(seekDeltas, false)},
            {"seeks", seekJson},
            {"pause_clock_frozen", pauseFrozen_},
            {"pause_video_advance_zero", pauseVideoAdvanceZero_},
            {"pause_generation_stable", pauseGenerationStable_},
            {"cpu_full_frame_readback_count", state_->readbacks.fullFrameReadbacks()},
            {"full_frame_gpu_copy_count", compositor.fullFrameGpuCopyCount},
            {"software_video_fallback_count",
             a.softwareFrameRejectCount + b.softwareFrameRejectCount},
            {"device_lost_count", state_->deviceLostCount.load()},
            {"lifecycle_violation_count", state_->lifecycleOrderViolationCount.load()},
            {"audio_render_thread_join_leak",
             static_cast<qint64>(sink.audioRenderThreadJoinLeak)},
            {"audio_device_failure_count", static_cast<qint64>(sink.deviceFailureCount)},
            {"audio_device_last_error", QString::fromStdString(sink.lastError)},
            {"audio_sink_joined", sink.joined},
            {"shutdown_enter_observed", shutdownEntered_},
            {"shutdown_entered_from_playback", shutdownEnteredFromPlayback_},
            {"test_render_fault_injected_after_playing", renderFaultInjectedAfterPlaying_},
            {"audio_decode_thread_join_leak",
             static_cast<qint64>(audioDecoder.audioDecodeThreadJoinLeak)},
            {"video_worker_a_joined", a.joined},
            {"video_worker_b_joined", b.joined},
            {"teardown_success", state_->teardownComplete.load()},
            {"final_report_after_teardown", state_->teardownComplete.load()},
            {"adapter", QString::fromStdString(a.adapter.description)},
            {"audio_endpoint_sample_rate", sink.deviceFormat.sampleRate},
            {"audio_endpoint_channels", sink.deviceFormat.channels},
            {"audio_endpoint_sample_format", QString::fromStdString(sink.deviceFormat.sampleFormat)}};
        root.insert("first_audio_underflow_snapshot",
                    firstAudioUnderflowJson(state_->runtimeAttribution));
        root.insert("first_clock_regression_snapshot",
                    firstClockRegressionJson(state_->runtimeAttribution));
        if (config_.formalContractC2) {
            root.insert("requested_output_width", 1920);
            root.insert("requested_output_height", 1080);
            root.insert("display_target_preflight_pass", displayPreflightPassed_);
            root.insert("formal_workload_started", formalWorkloadStarted_);
            root.insert("display_environment_start",
                        displayEnvironmentJson(displayEnvironmentStart_));
            root.insert("display_environment_end", displayEnvironmentJson(displayEnvironmentEnd_));
        }
        QSaveFile file(config_.metricsPath);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        return file.commit() && correctness;
    }
    const bool seeksExact = std::all_of(seeks_.begin(), seeks_.end(), [](const SeekRecord& value) {
        return value.firstAudioSample == value.requestedAudioSample &&
               value.firstDisplayedVideoFrame == value.requestedFrame;
    });
    const bool requestedSeekCountOk =
        config_.mode != P3AvMode::Seek || seeks_.size() == static_cast<size_t>(config_.seekCount);
    const bool pass = exitCode_ == 0 && requestedSeekCountOk && seeksExact && !deltas.empty() &&
                      queue.underflowCount == 0 && queue.overflowRejectCount == 0 &&
                      state_->videoAheadViolationCount.load() == 0 &&
                      state_->videoClockRegressionCount.load() == 0 &&
                      state_->videoQpcMasterFallbackCount.load() == 0 &&
                      state_->coordinator.mixedSourceFrameCount() == 0 &&
                      state_->coordinator.mixedGenerationCount() == 0 &&
                      state_->coordinator.staleCompositionEpochCount() == 0 &&
                      state_->markerAMismatch.load() == 0 && state_->markerBMismatch.load() == 0 &&
                      state_->markerAChecked.load() > 0 && state_->markerBChecked.load() > 0 &&
                      sink.deviceFailureCount == 0 && sink.audioRenderThreadJoinLeak == 0 &&
                      audioDecoder.audioDecodeThreadJoinLeak == 0 && a.joined && b.joined &&
                      sink.joined && audioDecoder.joined && state_->deviceLostCount.load() == 0 &&
                      state_->lifecycleOrderViolationCount.load() == 0;

    QJsonArray seekJson;
    for (const auto& value : seeks_) {
        seekJson.append(QJsonObject{{"requested_frame", value.requestedFrame},
                                    {"requested_audio_sample", value.requestedAudioSample},
                                    {"audio_generation", static_cast<qint64>(value.audioGeneration)},
                                    {"video_generation_a", static_cast<qint64>(value.videoGenerationA)},
                                    {"video_generation_b", static_cast<qint64>(value.videoGenerationB)},
                                    {"first_audio_sample", value.firstAudioSample},
                                    {"first_displayed_video_frame", value.firstDisplayedVideoFrame},
                                    {"audio_seek_ready_ms", value.audioSeekReadyMs},
                                    {"video_a_ready_ms", value.videoAReadyMs},
                                    {"video_b_ready_ms", value.videoBReadyMs},
                                    {"all_media_ready_ms", value.allMediaReadyMs},
                                    {"resume_to_first_video_ms", value.resumeToFirstVideoMs},
                                    {"request_to_first_video_ms", value.requestToFirstVideoMs}});
    }
    QJsonObject root{
        {"schema_version", 1},
        {"phase", "P3-B"},
        {"formal_verdict", "NOT_RUN"},
        {"mode", modeName(config_.mode)},
        {"pass", pass},
        {"detail", shutdownReason_},
        {"audio_master_only", true},
        {"samples_per_video_frame", audio::kSamplesPerVideoFrame},
        {"video_preroll_frames", 8},
        {"audio_preroll_ms", audio::kAudioPrerollMs},
        {"endpoint_prefill_frames", static_cast<qint64>(sink.endpointPrefillFrames)},
        {"endpoint_buffer_frames", static_cast<int>(sink.endpointBufferFrames)},
        {"endpoint_first_media_sample", sink.endpointFirstMediaSample},
        {"audio_decoder_eof", audioDecoder.eof},
        {"actual_audio_end_exclusive", audioDecoder.actualLastDecodedSampleExclusive},
        {"audio_master_video_frame_count", state_->audioMasterVideoFrameCount.load()},
        {"canonical_last_seek_target", canonicalLastSeekTarget_},
        {"diagnostic_fixed_seek_target", config_.diagnosticFixedSeekTarget},
        {"endpoint_start_device_position", static_cast<qint64>(sink.endpointStartDevicePosition)},
        {"clock_anchor_media_sample", sink.clockAnchorMediaSample},
        {"clock_anchor_device_position", static_cast<qint64>(sink.clockAnchorDevicePosition)},
        {"audio_rendered_samples", static_cast<qint64>(sink.audioRenderedSamples)},
        {"audio_underflow_count", static_cast<qint64>(queue.underflowCount)},
        {"audio_overflow_count", static_cast<qint64>(queue.overflowRejectCount)},
        {"video_displayed_count", state_->displayedCompositionCount.load()},
        {"video_catchup_skip_count", state_->audioClockVideoCatchupSkipCount.load()},
        {"audio_clock_video_catchup_skip_count",
         state_->audioClockVideoCatchupSkipCount.load()},
        {"video_pair_wait_count", state_->videoPairWaitCount.load()},
        {"video_target_superseded_count", state_->videoTargetSupersededCount.load()},
        {"audio_clock_video_stale_discard_a", state_->audioClockVideoStaleDiscardA.load()},
        {"audio_clock_video_stale_discard_b", state_->audioClockVideoStaleDiscardB.load()},
        {"video_ahead_violation_count", state_->videoAheadViolationCount.load()},
        {"clock_regression_count", state_->videoClockRegressionCount.load()},
        {"video_qpc_master_fallback_count", state_->videoQpcMasterFallbackCount.load()},
        {"marker_a_checked", state_->markerAChecked.load()},
        {"marker_b_checked", state_->markerBChecked.load()},
        {"marker_a_mismatch", state_->markerAMismatch.load()},
        {"marker_b_mismatch", state_->markerBMismatch.load()},
        {"mixed_pair_mismatch", state_->coordinator.mixedSourceFrameCount()},
        {"mixed_generation_count", state_->coordinator.mixedGenerationCount()},
        {"stale_composition_epoch_count", state_->coordinator.staleCompositionEpochCount()},
        {"application_av_delta_ms", deltaDistribution(deltas, false)},
        {"application_av_delta_abs_ms", deltaDistribution(deltas, true)},
        {"integrated_seek_requested", config_.mode == P3AvMode::Seek ? config_.seekCount : 0},
        {"integrated_seek_exact", config_.mode == P3AvMode::Seek ? static_cast<int>(seeks_.size()) : 0},
        {"seek_timeout_stage_evidence", seekTimeoutStageEvidence_},
        {"seeks", seekJson},
        {"pause_clock_frozen", pauseFrozen_},
        {"pause_video_advance_zero", pauseVideoAdvanceZero_},
        {"pause_generation_stable", pauseGenerationStable_},
        {"device_lost_count", state_->deviceLostCount.load()},
        {"audio_render_thread_join_leak", static_cast<qint64>(sink.audioRenderThreadJoinLeak)},
        {"audio_device_failure_count", static_cast<qint64>(sink.deviceFailureCount)},
        {"audio_device_last_error", QString::fromStdString(sink.lastError)},
        {"audio_sink_joined", sink.joined},
        {"shutdown_enter_observed", shutdownEntered_},
        {"shutdown_entered_from_playback", shutdownEnteredFromPlayback_},
        {"test_render_fault_injected_after_playing", renderFaultInjectedAfterPlaying_},
        {"audio_decode_thread_join_leak", static_cast<qint64>(audioDecoder.audioDecodeThreadJoinLeak)},
        {"video_worker_a_joined", a.joined},
        {"video_worker_b_joined", b.joined},
        {"lifecycle_violation_count", state_->lifecycleOrderViolationCount.load()}};
    root.insert("first_audio_underflow_snapshot",
                firstAudioUnderflowJson(state_->runtimeAttribution));
    QSaveFile file(config_.metricsPath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit() && pass;
}

} // namespace mvm::app

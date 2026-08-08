#include "p3_av_sync_controller.h"

#include "media/audio_preview/audio_video_scheduler.h"
#include "media/gpu_preview/qpc_clock.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <random>
#include <thread>

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
} // namespace

P3AvSyncController::P3AvSyncController(P3AvConfig config, QObject* parent)
    : QObject(parent), config_(std::move(config)) {
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(2);
    connect(&timer_, &QTimer::timeout, this, &P3AvSyncController::tick);
}

void P3AvSyncController::attach(CompositorRhiItem* item) {
    item_ = item;
    state_ = item->state();
    audioClock_ = std::make_shared<audio::AudioMasterClock>();
    state_->audioMasterClock = audioClock_;
    phaseTimer_.start();
    timer_.start();
}

bool P3AvSyncController::openPipelines() {
    workerA_ = std::make_shared<gpu::SourceDecodeWorker>(gpu::SourceId{1}, state_->device,
                                                        state_->readbacks, 16);
    workerB_ = std::make_shared<gpu::SourceDecodeWorker>(gpu::SourceId{2}, state_->device,
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
        if (lastTarget < 0) {
            startShutdown(QStringLiteral("8 frame pre-roll を確保できる seek target がありません"),
                          true);
            return false;
        }
        std::mt19937 random(config_.seed);
        std::uniform_int_distribution<long long> target(0, lastTarget);
        for (int index = 0; index < config_.seekCount; ++index)
            seekTargets_.push_back(target(random));
    } else {
        seekTargets_.push_back(0);
    }
    return true;
}

bool P3AvSyncController::startAtFrame(long long targetFrame) {
    state_->audioMasterSchedulerEnabled.store(false, std::memory_order_release);
    state_->playbackSchedulerEnabled.store(false, std::memory_order_release);
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
        startShutdown(QStringLiteral("integrated seek request が拒否されました"), true);
        return false;
    }
    gpu::SeekCompletion completionA;
    gpu::SeekCompletion completionB;
    audio::AudioSeekCompletion completionAudio;
    if (workerA_->waitSeek(ticketA, 5000, completionA) != gpu::SeekWaitResult::Ready ||
        workerB_->waitSeek(ticketB, 5000, completionB) != gpu::SeekWaitResult::Ready ||
        audioWorker_->waitSeek(audioTicket, 5000, completionAudio) !=
            audio::AudioSeekWaitResult::Ready ||
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
    state_->audioMasterSchedulerEnabled.store(true, std::memory_order_release);
    phaseTimer_.restart();
    phase_ = Phase::WaitDisplay;
    item_->update();
    return true;
}

bool P3AvSyncController::pollFirstDisplay() {
    gpu::CompositionDisplayRecord display;
    if (!state_->ledger.findAfter(displayBaseline_, displayExpectation_, display)) {
        if (phaseTimer_.elapsed() > config_.displayTimeoutMs) {
            startShutdown(QStringLiteral("first integrated video display が timeout しました"),
                          true);
        }
        return false;
    }
    SeekRecord& record = seeks_.back();
    record.firstDisplayedVideoFrame = display.outputFrameNumber;
    record.resumeToFirstVideoMs = qpcMs(audioStartQpc_, display.displayRecordQpc);
    record.requestToFirstVideoMs = qpcMs(requestStartQpc_, display.displayRecordQpc);
    if (record.firstAudioSample != record.requestedAudioSample ||
        record.firstDisplayedVideoFrame != record.requestedFrame) {
        startShutdown(QStringLiteral("first audio/video identity が seek target と不一致です"),
                      true);
        return false;
    }
    return true;
}

void P3AvSyncController::startShutdown(const QString& reason, bool failure) {
    if (phase_ == Phase::ShutdownWait || phase_ == Phase::Done)
        return;
    shutdownReason_ = reason;
    if (failure)
        exitCode_ = 3;
    state_->audioMasterSchedulerEnabled.store(false, std::memory_order_release);
    state_->playbackSchedulerEnabled.store(false, std::memory_order_release);
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
    phase_ = Phase::ShutdownWait;
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
    switch (phase_) {
    case Phase::WaitDevice:
        if (state_->deviceReady.load(std::memory_order_acquire))
            phase_ = Phase::Start;
        else if (phaseTimer_.elapsed() > 10000)
            startShutdown(QStringLiteral("Qt/D3D11 device ready timeout"), true);
        break;
    case Phase::Start:
        if (openPipelines()) {
            seekIndex_ = 0;
            startAtFrame(seekTargets_.front());
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
            phase_ = config_.mode == P3AvMode::PauseResume ? Phase::PauseStart : Phase::Playback;
        }
        break;
    case Phase::Playback:
        if (runTimer_.elapsed() >= config_.durationSeconds * 1000)
            startShutdown(QStringLiteral("playback 完了"), false);
        break;
    case Phase::PauseStart:
        if (runTimer_.elapsed() >= 2000) {
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
            phaseTimer_.restart();
            phase_ = Phase::PauseWait;
        }
        break;
    case Phase::PauseWait:
        if (phaseTimer_.elapsed() >= 1000) {
            pauseFrozen_ = audioClock_->snapshot().mediaSamplePosition == pauseAudioSample_;
            pauseVideoAdvanceZero_ = state_->audioMasterLastDisplayed.load() == pauseVideoFrame_;
            const auto audioGeneration = audioWorker_->snapshot().sourceGeneration.value;
            const auto aGeneration = workerA_->snapshot().sourceGeneration.value;
            const auto bGeneration = workerB_->snapshot().sourceGeneration.value;
            pauseGenerationStable_ = audioGeneration == state_->audioMasterGeneration.load() &&
                                     aGeneration == displayExpectation_.sources[0].sourceGeneration.value &&
                                     bGeneration == displayExpectation_.sources[1].sourceGeneration.value;
            workerA_->play();
            workerB_->play();
            audioWorker_->play();
            std::string error;
            if (!audioSink_->play(pauseAudioSample_, audioWorker_->snapshot().sourceGeneration,
                                  error)) {
                startShutdown(QString::fromStdString(error), true);
                break;
            }
            state_->audioMasterSchedulerEnabled.store(true, std::memory_order_release);
            runTimer_.restart();
            phase_ = Phase::ResumePlayback;
        }
        break;
    case Phase::ResumePlayback:
        if (runTimer_.elapsed() >= 2000)
            startShutdown(QStringLiteral("pause/resume 完了"),
                          !(pauseFrozen_ && pauseVideoAdvanceZero_ && pauseGenerationStable_));
        break;
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
        {"endpoint_first_media_sample", sink.endpointFirstMediaSample},
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
        {"seeks", seekJson},
        {"pause_clock_frozen", pauseFrozen_},
        {"pause_video_advance_zero", pauseVideoAdvanceZero_},
        {"pause_generation_stable", pauseGenerationStable_},
        {"device_lost_count", state_->deviceLostCount.load()},
        {"audio_render_thread_join_leak", static_cast<qint64>(sink.audioRenderThreadJoinLeak)},
        {"audio_decode_thread_join_leak", static_cast<qint64>(audioDecoder.audioDecodeThreadJoinLeak)},
        {"video_worker_a_joined", a.joined},
        {"video_worker_b_joined", b.joined},
        {"lifecycle_violation_count", state_->lifecycleOrderViolationCount.load()}};
    QSaveFile file(config_.metricsPath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit() && pass;
}

} // namespace mvm::app

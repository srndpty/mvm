// P5-D2 audio-master transport の実行可能検証。
//
// P5-C smoke は video-only 経路の回帰であり、audio-master play/pause の証拠には
// ならない (preview-engine-contract.md §11)。このsmokeは WASAPI / IAudioClock を
// masterにした状態でだけ成立する性質を確認する。
#include "app/preview/preview_engine_rhi_item.h"
#include "media/audio_preview/audio_types.h"
#include "preview_engine/preview_engine.h"
#include "preview_engine/preview_engine_internal.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

#include <QGuiApplication>
#include <QMetaObject>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>

namespace {

class QtDispatcher final : public mvm::preview::PreviewEventDispatcher {
public:
    explicit QtDispatcher(QObject* context) : context_(context) {}

    bool post(std::function<void()> task) override {
        return QMetaObject::invokeMethod(context_, std::move(task), Qt::QueuedConnection);
    }

private:
    QObject* context_;
};

class SmokeSink final : public mvm::preview::PreviewEventSink {
public:
    void stateChanged(mvm::preview::PreviewEngineState state) override { terminal = state; }

    void positionChanged(mvm::preview::PreviewPosition position) override {
        if (lastPosition >= 0 && position.outputFrame < lastPosition)
            positionRegression = true;
        lastPosition = position.outputFrame;
    }

    void framePresented(mvm::preview::PresentedFrameInfo frame) override {
        if (presented != 0 && frame.presentationSequence <= lastSequence)
            sequenceViolation = true;
        lastSequence = frame.presentationSequence;
        ++presented;
    }

    void errorOccurred(mvm::preview::PreviewError error) override {
        errors.push_back(std::move(error));
    }

    void deviceChanged(mvm::preview::PreviewDeviceInfo) override {}

    std::vector<mvm::preview::PreviewError> errors;
    mvm::preview::PreviewEngineState terminal = mvm::preview::PreviewEngineState::Uninitialized;
    std::uint64_t presented = 0;
    std::uint64_t lastSequence = 0;
    std::int64_t lastPosition = -1;
    bool sequenceViolation = false;
    bool positionRegression = false;
};

enum class Stage { WaitDevice, WaitInitial, PauseHold, WaitResume, WaitShutdown };
enum class Fault { None, AudioClockStall, AudioSinkFatal, AudioPauseFault };

} // namespace

int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    const QStringList arguments = app.arguments();
    if (arguments.size() < 2 || arguments.size() > 3) {
        std::fprintf(stderr, "使い方: mvm_p5d_preview_smoke <fixture-path> "
                             "[--fault-audio-clock|--fault-audio-sink"
                             "|--fault-audio-pause]\n");
        return 2;
    }
    Fault fault = Fault::None;
    if (arguments.size() == 3) {
        if (arguments[2] == "--fault-audio-clock")
            fault = Fault::AudioClockStall;
        else if (arguments[2] == "--fault-audio-sink")
            fault = Fault::AudioSinkFatal;
        else if (arguments[2] == "--fault-audio-pause")
            fault = Fault::AudioPauseFault;
        else
            return 2;
    }

    auto engine = std::make_shared<mvm::preview::PreviewEngine>();
    auto dispatcher = std::make_shared<QtDispatcher>(&app);
    auto sink = std::make_shared<SmokeSink>();
    if (!engine->initialize({{{60, 1}}}, dispatcher) || !engine->attachEventSink(sink))
        return 3;
    // fixtureに1 kHz markerが入っているため、検証時は endpoint session volume を下げる。
    if (!mvm::preview::internal::PreviewRenderPort::setVerificationAudioVolume(
            *engine, mvm::audio::kVerificationSessionVolume)) {
        std::fprintf(stderr, "検証用session volumeを設定できません\n");
        return 3;
    }

    // audio統合後もqualified capabilityを実体として公開していること。
    const auto capabilities = engine->capabilities();
    if (capabilities.maxQualifiedActiveAudioSources != 1 ||
        capabilities.qualifiedAudioSampleRate != 48000 ||
        capabilities.qualifiedAudioChannelCount != 2) {
        std::fprintf(stderr, "qualified audio capabilityが公開されていません\n");
        return 3;
    }

    QQuickWindow window;
    window.setWidth(1280);
    window.setHeight(720);
    auto* surface = new mvm::app::PreviewEngineRhiItem(window.contentItem());
    surface->setWidth(1280);
    surface->setHeight(720);
    surface->setEngine(engine);
    window.show();

    Stage stage = Stage::WaitDevice;
    mvm::preview::AcceptedComposition accepted;
    std::uint64_t pausePresentedCount = 0;
    std::int64_t pausePosition = -1;
    mvm::preview::internal::P5CRuntimeDiagnostics activeDiagnostics;
    const auto started = std::chrono::steady_clock::now();
    auto stageStarted = started;
    int exitCode = 0;

    QTimer timer;
    timer.setInterval(10);
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        const auto now = std::chrono::steady_clock::now();
        if (now - started > std::chrono::seconds(40)) {
            std::fprintf(stderr, "P5-D smokeが40秒以内に完了しませんでした\n");
            exitCode = 10;
            app.quit();
            return;
        }
        const auto status = engine->status();
        const auto telemetry = engine->telemetry();

        if (stage == Stage::WaitDevice &&
            status.state == mvm::preview::PreviewEngineState::ReadyPaused) {
            mvm::preview::PreviewSourceDescriptor descriptor;
            descriptor.mediaPath = arguments[1].toStdWString();
            descriptor.videoEnabled = true;
            descriptor.audioEnabled = true;
            const auto source = engine->addSource(descriptor);
            if (!source) {
                std::fprintf(stderr, "audio付きsourceのopenに失敗: %s\n",
                             source.error().detail.c_str());
                exitCode = 4;
                app.quit();
                return;
            }
            // 2件目のactive audio sourceはfail-closedで拒否する。
            mvm::preview::PreviewSourceDescriptor audioOnly;
            audioOnly.mediaPath = arguments[1].toStdWString();
            audioOnly.audioEnabled = true;
            const auto secondAudio = engine->addSource(audioOnly);
            const auto secondVideo = engine->addSource(descriptor);
            // seekはP5-D3のscopeであり、D2では受理しない。
            const auto seek = engine->seek({0});
            if (secondAudio || secondVideo || seek ||
                secondAudio.error().category !=
                    mvm::preview::PreviewErrorCategory::UnsupportedCapability ||
                secondVideo.error().category !=
                    mvm::preview::PreviewErrorCategory::UnsupportedCapability ||
                seek.error().category !=
                    mvm::preview::PreviewErrorCategory::UnsupportedCapability) {
                std::fprintf(stderr, "audio/video capability負例が期待どおりに落ちません\n");
                exitCode = 11;
                app.quit();
                return;
            }
            // endpoint formatがdevice infoへ載っていること。
            const auto device = engine->deviceInfo();
            if (device.audioSampleRate == 0 || device.audioChannelCount == 0) {
                std::fprintf(stderr, "audio endpoint formatがdevice infoに載っていません\n");
                exitCode = 11;
                app.quit();
                return;
            }

            auto snapshot = std::make_shared<mvm::preview::CompositionSnapshot>();
            snapshot->layers.push_back({source.value(), {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0F});
            const auto composition = engine->submitComposition(snapshot);
            if (!composition || !engine->play()) {
                std::fprintf(stderr, "audio-master playを開始できません\n");
                exitCode = 5;
                app.quit();
                return;
            }
            accepted = composition.value();
            stage = Stage::WaitInitial;
            stageStarted = now;
            return;
        }

        if (stage == Stage::WaitInitial && telemetry.presentedFrameCount >= 20) {
            activeDiagnostics =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            if (!activeDiagnostics.audioMasterActive) {
                std::fprintf(stderr, "再生中にaudio masterが有効になっていません\n");
                exitCode = 12;
                app.quit();
                return;
            }
            if (fault == Fault::AudioPauseFault) {
                // sink pauseが失敗したのに`ReadyPaused`を公開しないことを検査する。
                if (!mvm::preview::internal::PreviewRenderPort::injectAudioSinkPauseFaultForTest(
                        *engine)) {
                    exitCode = 13;
                    app.quit();
                    return;
                }
                const auto paused = engine->pause();
                const auto afterPause = engine->status().state;
                if (paused ||
                    paused.error().category != mvm::preview::PreviewErrorCategory::AudioFailure ||
                    paused.error().severity !=
                        mvm::preview::PreviewErrorSeverity::FatalToSession ||
                    afterPause == mvm::preview::PreviewEngineState::ReadyPaused) {
                    std::fprintf(stderr,
                                 "sink pause失敗時にReadyPausedを公開しました (state=%d)\n",
                                 static_cast<int>(afterPause));
                    exitCode = 14;
                    app.quit();
                    return;
                }
                stage = Stage::WaitShutdown;
                return;
            }
            if (fault != Fault::None) {
                const bool injected =
                    fault == Fault::AudioClockStall
                        ? static_cast<bool>(
                              mvm::preview::internal::PreviewRenderPort::
                                  injectAudioClockStallForTest(*engine))
                        : static_cast<bool>(
                              mvm::preview::internal::PreviewRenderPort::
                                  injectAudioSinkRenderFaultForTest(*engine));
                if (!injected) {
                    exitCode = 13;
                    app.quit();
                    return;
                }
                stage = Stage::WaitShutdown;
                return;
            }
            if (!engine->pause()) {
                std::fprintf(stderr, "audio-master pauseに失敗しました\n");
                exitCode = 6;
                app.quit();
                return;
            }
            // pauseが成功を返した以上、公開stateはReadyPausedでなければならない。
            if (engine->status().state != mvm::preview::PreviewEngineState::ReadyPaused) {
                std::fprintf(stderr, "pause成功後のstateがReadyPausedではありません\n");
                exitCode = 6;
                app.quit();
                return;
            }
            // pause前に取得したsnapshotをcheckpointにすると、取得からscheduler停止
            // までにrender threadが1 frame提示しただけで正しい実装でもFAILする。
            // pause成功後の実測値をcheckpointにする。
            const auto pausedTelemetry = engine->telemetry();
            pausePresentedCount = pausedTelemetry.presentedFrameCount;
            pausePosition = pausedTelemetry.status.position.outputFrame;
            stage = Stage::PauseHold;
            stageStarted = now;
            return;
        }

        if (stage == Stage::PauseHold && now - stageStarted > std::chrono::milliseconds(500)) {
            // audio masterが止まっている間、video presentationもpositionも進まない。
            if (telemetry.presentedFrameCount != pausePresentedCount ||
                status.position.outputFrame != pausePosition) {
                std::fprintf(stderr, "pause中にpresentation/positionが進みました\n");
                exitCode = 7;
                app.quit();
                return;
            }
            if (!engine->play()) {
                std::fprintf(stderr, "resumeに失敗しました\n");
                exitCode = 7;
                app.quit();
                return;
            }
            stage = Stage::WaitResume;
            stageStarted = now;
            return;
        }

        if (stage == Stage::WaitResume &&
            telemetry.presentedFrameCount >= pausePresentedCount + 12) {
            if (status.position.outputFrame < pausePosition) {
                std::fprintf(stderr, "resume後にpositionが巻き戻りました\n");
                exitCode = 8;
                app.quit();
                return;
            }
            activeDiagnostics =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            if (!engine->requestShutdown()) {
                exitCode = 8;
                app.quit();
                return;
            }
            stage = Stage::WaitShutdown;
            return;
        }

        if (stage == Stage::WaitShutdown &&
            (status.state == mvm::preview::PreviewEngineState::Shutdown ||
             status.state == mvm::preview::PreviewEngineState::Error)) {
            const bool failureFault = fault != Fault::None;
            if (failureFault && sink->errors.empty())
                return;
            const auto terminalTelemetry = engine->telemetry();
            const auto diagnostics =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);

            const bool expectedTerminal =
                failureFault ? status.state == mvm::preview::PreviewEngineState::Error
                             : status.state == mvm::preview::PreviewEngineState::Shutdown;
            // audio failureはAudioFailureとしてfatalに出る。QPCへ退避して再生を
            // 続けない (projection失敗が記録され、Playingへ戻らない)。
            const bool audioFaultPass =
                !failureFault ||
                (sink->errors.size() == 1 &&
                 sink->errors.front().category ==
                     mvm::preview::PreviewErrorCategory::AudioFailure &&
                 sink->errors.front().severity ==
                     mvm::preview::PreviewErrorSeverity::FatalToSession &&
                 (fault != Fault::AudioClockStall ||
                  diagnostics.audioMasterProjectionFailureCount >= 1) &&
                 // sink faultは完成errorの注入ではなく、sink自身のdevice failureを
                 // product側が検知して昇格した結果でなければならない。
                 // pause失敗はengine側transport failureちょうど1件として数える。
                 // shutdown threadが再度pause()を呼ぶためsink側は1件以上になる。
                 (fault != Fault::AudioPauseFault ||
                  (diagnostics.audioTransportFailureCount == 1 &&
                   diagnostics.audioSinkDeviceFailureCount >= 1 &&
                   diagnostics.audioDomainRejectCount == 0 &&
                   diagnostics.audioMasterProjectionFailureCount == 0 &&
                   sink->errors.front().detail.find("pause fault") != std::string::npos)) &&
                 // sink render faultはsink自身のdevice failureだけが立ち、engine側の
                 // transport操作は失敗していない (polling経由で昇格したことの証拠)。
                 (fault != Fault::AudioSinkFatal ||
                  (diagnostics.audioSinkDeviceFailureCount >= 1 &&
                   diagnostics.audioTransportFailureCount == 0 &&
                   diagnostics.audioDomainRejectCount == 0 &&
                   diagnostics.audioMasterProjectionFailureCount == 0 &&
                   sink->errors.front().detail.find("runtime failure") != std::string::npos)) &&
                 // clock stallはprojection失敗だけで、sink/transportは無傷である。
                 (fault != Fault::AudioClockStall ||
                  (diagnostics.audioSinkDeviceFailureCount == 0 &&
                   diagnostics.audioTransportFailureCount == 0 &&
                   diagnostics.audioDomainRejectCount == 0)));
            const bool cleanRunPass =
                failureFault ||
                (diagnostics.audioMasterProjectionFailureCount == 0 &&
                 diagnostics.audioGenerationMismatchCount == 0 &&
                 diagnostics.audioSinkDeviceFailureCount == 0 &&
                 diagnostics.audioTransportFailureCount == 0 &&
                 diagnostics.audioDomainRejectCount == 0 && !sink->positionRegression &&
                 sink->errors.empty() && terminalTelemetry.presentedFrameCount >= 32 &&
                 activeDiagnostics.audioMasterActive &&
                 activeDiagnostics.registeredAudioSourceCount == 1 &&
                 activeDiagnostics.d3d11vaActive && activeDiagnostics.decodeRenderSameDevice &&
                 // 要求しただけでなく endpoint へ適用されたことまで確認する。
                 activeDiagnostics.audioSessionVolume ==
                     mvm::audio::kVerificationSessionVolume);

            // 最終状態だけでなく、contract §12 の停止順序そのものを固定する。
            using mvm::preview::internal::ShutdownStep;
            const std::vector<ShutdownStep> expectedSequence{
                ShutdownStep::DisableSchedulers,
                ShutdownStep::StopAudioSink,
                ShutdownStep::StopAudioDecodeWorker,
                ShutdownStep::StopVideoWorkers,
                ShutdownStep::DetachRenderVisibleWorkerRefs,
                ShutdownStep::VerifyJoins,
                ShutdownStep::RequestRenderTeardown,
                ShutdownStep::FiniteGpuRetirementDrain,
                ShutdownStep::ReleaseRenderTargetDevice,
                ShutdownStep::PublishShutdownComplete};
            const bool shutdownOrderPass = diagnostics.shutdownSequence == expectedSequence;
            if (!shutdownOrderPass) {
                std::fprintf(stderr, "shutdown orderingがcontract §12と一致しません (steps=%zu)\n",
                             diagnostics.shutdownSequence.size());
            }

            const bool pass =
                expectedTerminal && audioFaultPass && cleanRunPass && shutdownOrderPass &&
                !sink->sequenceViolation &&
                !diagnostics.audioMasterActive && diagnostics.audioSinkJoined &&
                diagnostics.audioWorkerJoined && diagnostics.workerJoined &&
                diagnostics.renderTeardownComplete && diagnostics.deviceReleased &&
                !diagnostics.unsafeGpuResourcesRetained &&
                diagnostics.staleSubstitutionCount == 0 &&
                diagnostics.lifecycleViolationCount == 0 &&
                diagnostics.fullCpuReadbackCount == 0 &&
                diagnostics.fullFrameGpuCopyCount == 0 &&
                diagnostics.softwareFallbackCount == 0 &&
                diagnostics.untrackedSubmissionCount == 0 &&
                diagnostics.earlyPayloadReleaseCount == 0 &&
                diagnostics.retirementTimeoutCount == 0 &&
                terminalTelemetry.eventDeliveryFailureCount == 0 &&
                (failureFault || terminalTelemetry.status.lastPresentedComposition == accepted);

            std::printf("{\"verdict\":\"%s\",\"presented\":%llu,\"underflow\":%llu,"
                        "\"audio_projection_failures\":%llu,\"audio_generation_mismatch\":%llu,"
                        "\"audio_sink_device_failures\":%llu,\"audio_transport_failures\":%llu,"
                        "\"audio_domain_rejects\":%llu,\"shutdown_steps\":%zu,"
                        "\"errors\":%zu,\"terminal\":%d}\n",
                        pass ? "PASS" : "FAIL",
                        static_cast<unsigned long long>(terminalTelemetry.presentedFrameCount),
                        static_cast<unsigned long long>(terminalTelemetry.audioUnderflowCount),
                        static_cast<unsigned long long>(
                            diagnostics.audioMasterProjectionFailureCount),
                        static_cast<unsigned long long>(diagnostics.audioGenerationMismatchCount),
                        static_cast<unsigned long long>(diagnostics.audioSinkDeviceFailureCount),
                        static_cast<unsigned long long>(diagnostics.audioTransportFailureCount),
                        static_cast<unsigned long long>(diagnostics.audioDomainRejectCount),
                        diagnostics.shutdownSequence.size(), sink->errors.size(),
                        static_cast<int>(status.state));
            exitCode = pass ? 0 : 9;
            app.quit();
        }
    });
    timer.start();
    app.exec();
    return exitCode;
}

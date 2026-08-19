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
    void stateChanged(mvm::preview::PreviewEngineState state) override {
        terminal = state;
        states.push_back(state);
    }

    // 「一度もPlayingを公開していない」ことを検査するために全遷移を残す。
    bool sawStateAfter(std::size_t from, mvm::preview::PreviewEngineState wanted) const {
        for (std::size_t i = from; i < states.size(); ++i) {
            if (states[i] == wanted)
                return true;
        }
        return false;
    }

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

    std::vector<mvm::preview::PreviewEngineState> states;
    std::vector<mvm::preview::PreviewError> errors;
    mvm::preview::PreviewEngineState terminal = mvm::preview::PreviewEngineState::Uninitialized;
    std::uint64_t presented = 0;
    std::uint64_t lastSequence = 0;
    std::int64_t lastPosition = -1;
    bool sequenceViolation = false;
    bool positionRegression = false;
};

enum class Stage { WaitDevice, WaitInitial, PauseHold, SeekWait, SeekSettle,
                   WaitResume, WaitShutdown };
// fixture 65 秒 / 60 fps。先頭からも末尾からも十分離れた frame を選ぶ。
constexpr std::int64_t kSeekTargetFrame = 900;

enum class Fault { None, AudioClockStall, AudioSinkFatal, AudioPauseFault, GpuDrain, SeekPaused, SeekPresentationStall, SeekPlaying,
                   SeekResumeFault, SeekAudioGenerationStall,
                   SeekResumeBarrier, SeekShutdownRace };

} // namespace

int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    const QStringList arguments = app.arguments();
    if (arguments.size() < 2 || arguments.size() > 3) {
        std::fprintf(stderr, "使い方: mvm_p5d_preview_smoke <fixture-path> "
                             "[--fault-audio-clock|--fault-audio-sink"
                             "|--fault-audio-pause|--fault-gpu-drain|--seek-paused|--seek-playing|--fault-seek-presentation"
                             "|--fault-seek-resume|--fault-seek-audio-generation"
                             "|--seek-resume-barrier|--seek-shutdown-race]\n");
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
        else if (arguments[2] == "--fault-gpu-drain")
            fault = Fault::GpuDrain;
        else if (arguments[2] == "--seek-paused")
            fault = Fault::SeekPaused;
        else if (arguments[2] == "--fault-seek-presentation")
            fault = Fault::SeekPresentationStall;
        else if (arguments[2] == "--seek-playing")
            fault = Fault::SeekPlaying;
        else if (arguments[2] == "--fault-seek-resume")
            fault = Fault::SeekResumeFault;
        else if (arguments[2] == "--fault-seek-audio-generation")
            fault = Fault::SeekAudioGenerationStall;
        else if (arguments[2] == "--seek-resume-barrier")
            fault = Fault::SeekResumeBarrier;
        else if (arguments[2] == "--seek-shutdown-race")
            fault = Fault::SeekShutdownRace;
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
    std::uint64_t seekPresentedCount = 0;
    std::int64_t seekPosition = -1;
    std::size_t seekStateBaseline = 0;
    bool barrierObserved = false;
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
            const auto stuck =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            std::fprintf(stderr,
                         "P5-D smokeが40秒以内に完了しませんでした "
                         "(stage=%d state=%d requests=%llu decodeReady=%llu completed=%llu "
                         "awaiting=%llu staleReject=%llu)\n",
                         static_cast<int>(stage), static_cast<int>(engine->status().state),
                         static_cast<unsigned long long>(stuck.seekRequestCount),
                         static_cast<unsigned long long>(stuck.seekDecodeReadyCount),
                         static_cast<unsigned long long>(stuck.seekCompletedCount),
                         static_cast<unsigned long long>(stuck.seekAwaitingPresentationCount),
                         static_cast<unsigned long long>(stuck.seekStaleGenerationRejectCount));
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
            // P5-D3でseekは受理対象になった。ただしaccepted composition前は
            // fail-closedで拒否する。負のframeはSeekFailureで拒否する。
            const auto seek = engine->seek({0});
            const auto negativeSeek = engine->seek({-1});
            if (secondAudio || secondVideo || seek || negativeSeek ||
                secondAudio.error().category !=
                    mvm::preview::PreviewErrorCategory::UnsupportedCapability ||
                secondVideo.error().category !=
                    mvm::preview::PreviewErrorCategory::UnsupportedCapability ||
                seek.error().category != mvm::preview::PreviewErrorCategory::InvalidState ||
                negativeSeek ||
                negativeSeek.error().category !=
                    mvm::preview::PreviewErrorCategory::SeekFailure) {
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

        if (stage == Stage::WaitInitial && telemetry.presentedFrameCount >= 20 &&
            (fault == Fault::SeekPlaying || fault == Fault::SeekResumeFault ||
             fault == Fault::SeekAudioGenerationStall ||
             fault == Fault::SeekResumeBarrier || fault == Fault::SeekShutdownRace)) {
            // Playingのままseekする。resume成功までPlayingを公開しないことを検査する。
            activeDiagnostics =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            if (fault == Fault::SeekResumeFault) {
                // seek後のresumeでWASAPI playを失敗させる。
                if (!mvm::preview::internal::PreviewRenderPort::injectAudioSinkPlayFaultForTest(
                        *engine)) {
                    exitCode = 35;
                    app.quit();
                    return;
                }
            }
            if (fault == Fault::SeekAudioGenerationStall &&
                !mvm::preview::internal::PreviewRenderPort::
                    injectSeekAudioGenerationMismatchForTest(*engine)) {
                exitCode = 36;
                app.quit();
                return;
            }
            if ((fault == Fault::SeekResumeBarrier || fault == Fault::SeekShutdownRace) &&
                !mvm::preview::internal::PreviewRenderPort::armAudioPlayBarrierForTest(*engine)) {
                exitCode = 39;
                app.quit();
                return;
            }
            seekStateBaseline = sink->states.size();
            const auto requested = engine->seek({kSeekTargetFrame});
            if (!requested || engine->status().state !=
                                  mvm::preview::PreviewEngineState::Seeking) {
                std::fprintf(stderr, "Playingからのseek requestが受理されませんでした\n");
                exitCode = 37;
                app.quit();
                return;
            }
            stage = Stage::SeekWait;
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
            if (fault == Fault::GpuDrain) {
                // audio登録済みでGPU drainを失敗させ、quarantine経路 (異常teardown) の
                // terminal state / resource quarantine / shutdown sequenceを固定する。
                // `DetachedWorkers`のdestruction orderそのものを検出するtestではない
                // (このUAFは無音で、testでは捕まえられないため構造で担保している)。
                if (!mvm::preview::internal::PreviewRenderPort::injectGpuDrainFailureForTest(
                        *engine) ||
                    !engine->requestShutdown()) {
                    exitCode = 13;
                    app.quit();
                    return;
                }
                stage = Stage::WaitShutdown;
                return;
            }
            // SeekPausedはfault注入ではなく、pause -> exact seek の正常経路である。
            if (fault != Fault::None && fault != Fault::SeekPaused &&
                fault != Fault::SeekPresentationStall && fault != Fault::SeekPlaying &&
                fault != Fault::SeekResumeFault && fault != Fault::SeekAudioGenerationStall &&
                fault != Fault::SeekResumeBarrier && fault != Fault::SeekShutdownRace) {
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

        if (stage == Stage::SeekWait) {
            const auto seekDiag =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            if (fault == Fault::SeekResumeBarrier || fault == Fault::SeekShutdownRace) {
                if (!barrierObserved) {
                    // resume の play() が barrier に入るまで待つ。
                    if (!mvm::preview::internal::PreviewRenderPort::
                            waitAudioPlayBarrierEnteredForTest(*engine, 0))
                        return;
                    barrierObserved = true;
                    // resume 中は Seeking のままであり、Playing を公開していない。
                    if (engine->status().state != mvm::preview::PreviewEngineState::Seeking ||
                        sink->sawStateAfter(seekStateBaseline,
                                            mvm::preview::PreviewEngineState::Playing)) {
                        std::fprintf(stderr,
                                     "resume 完了前に Playing を公開しました (state=%d)\n",
                                     static_cast<int>(engine->status().state));
                        exitCode = 40;
                        app.quit();
                        return;
                    }
                    if (fault == Fault::SeekShutdownRace) {
                        // resume を止めたまま shutdown を要求する。
                        if (!engine->requestShutdown()) {
                            exitCode = 41;
                            app.quit();
                            return;
                        }
                    }
                    mvm::preview::internal::PreviewRenderPort::releaseAudioPlayBarrierForTest(
                        *engine);
                    return;
                }
                if (fault == Fault::SeekShutdownRace) {
                    if (status.state != mvm::preview::PreviewEngineState::Shutdown &&
                        status.state != mvm::preview::PreviewEngineState::Error)
                        return;
                    stage = Stage::WaitShutdown;
                    return;
                }
                if (status.state == mvm::preview::PreviewEngineState::Seeking)
                    return;
                if (status.state != mvm::preview::PreviewEngineState::Playing) {
                    std::fprintf(stderr, "barrier 解放後に Playing へ戻りません (state=%d)\n",
                                 static_cast<int>(status.state));
                    exitCode = 42;
                    app.quit();
                    return;
                }
                activeDiagnostics =
                    mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
                if (!engine->requestShutdown()) {
                    exitCode = 43;
                    app.quit();
                    return;
                }
                stage = Stage::WaitShutdown;
                return;
            }
            if (fault == Fault::SeekResumeFault || fault == Fault::SeekAudioGenerationStall) {
                if (status.state == mvm::preview::PreviewEngineState::Seeking)
                    return;
                stage = Stage::WaitShutdown;
                return;
            }
            if (fault == Fault::SeekPlaying) {
                if (status.state == mvm::preview::PreviewEngineState::Seeking)
                    return;
                if (status.state != mvm::preview::PreviewEngineState::Playing) {
                    std::fprintf(stderr, "Playing originのseek後にPlayingへ戻りません (state=%d)\n",
                                 static_cast<int>(status.state));
                    exitCode = 38;
                    app.quit();
                    return;
                }
                stage = Stage::WaitResume;
                stageStarted = now;
                pausePresentedCount = engine->telemetry().presentedFrameCount;
                return;
            }
            if (fault == Fault::SeekPresentationStall) {
                if (status.state == mvm::preview::PreviewEngineState::Seeking)
                    return;
                stage = Stage::WaitShutdown;
                return;
            }
            if (status.state == mvm::preview::PreviewEngineState::Seeking) {
                // seek受理直後はまだ完了していない。decode readyだけでcompleteに
                // していないことを、この待ちの間に観測する。
                return;
            }
            if (status.state != mvm::preview::PreviewEngineState::ReadyPaused) {
                std::fprintf(stderr, "seek後にReadyPausedへ戻りませんでした (state=%d)\n",
                             static_cast<int>(status.state));
                exitCode = 30;
                app.quit();
                return;
            }
            // completionは「要求frameを実際に提示した」ことでなければならない。
            if (seekDiag.seekCompletedCount != 1 ||
                seekDiag.lastSeekPresentedFrame != kSeekTargetFrame ||
                seekDiag.lastSeekTargetFrame != kSeekTargetFrame ||
                status.position.outputFrame != kSeekTargetFrame) {
                std::fprintf(stderr,
                             "exact seek completionが成立していません "
                             "(completed=%llu target=%lld presented=%lld position=%lld)\n",
                             static_cast<unsigned long long>(seekDiag.seekCompletedCount),
                             static_cast<long long>(seekDiag.lastSeekTargetFrame),
                             static_cast<long long>(seekDiag.lastSeekPresentedFrame),
                             static_cast<long long>(status.position.outputFrame));
                exitCode = 31;
                app.quit();
                return;
            }
            seekPresentedCount = telemetry.presentedFrameCount;
            seekPosition = status.position.outputFrame;
            stage = Stage::SeekSettle;
            stageStarted = now;
            return;
        }
        if (stage == Stage::SeekSettle && now - stageStarted > std::chrono::milliseconds(400)) {
            // pausedのままseekしたので、完了後もtransportは進まない。
            if (telemetry.presentedFrameCount != seekPresentedCount ||
                status.position.outputFrame != seekPosition) {
                std::fprintf(stderr, "paused seek後にtransportが進みました\n");
                exitCode = 32;
                app.quit();
                return;
            }
            activeDiagnostics =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            if (!engine->requestShutdown()) {
                exitCode = 33;
                app.quit();
                return;
            }
            stage = Stage::WaitShutdown;
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
            if (fault == Fault::SeekPaused ||
                fault == Fault::SeekPresentationStall) {
                // pausedのままexact seekを要求する。returnはacceptanceであり、
                // ここではまだ完了していないことを次のstageで確認する。
                if (fault == Fault::SeekPresentationStall &&
                    !mvm::preview::internal::PreviewRenderPort::
                        injectSeekPresentationStallForTest(*engine)) {
                    exitCode = 34;
                    app.quit();
                    return;
                }
                const auto requested = engine->seek({kSeekTargetFrame});
                if (!requested) {
                    std::fprintf(stderr, "seek requestが受理されませんでした: %s\n",
                                 requested.error().detail.c_str());
                    exitCode = 29;
                    app.quit();
                    return;
                }
                if (engine->status().state != mvm::preview::PreviewEngineState::Seeking) {
                    std::fprintf(stderr, "seek受理後にSeekingへ遷移していません\n");
                    exitCode = 29;
                    app.quit();
                    return;
                }
                stage = Stage::SeekWait;
                stageStarted = now;
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
            const bool failureFault = fault != Fault::None && fault != Fault::SeekPaused &&
                fault != Fault::SeekPlaying && fault != Fault::SeekResumeBarrier &&
                fault != Fault::SeekShutdownRace;
            // presentation stallはSeekFailureでterminal Errorになる。
            if (failureFault && sink->errors.empty())
                return;
            // GPU drain faultはShutdownFailureであり、audio failureではない。
            const bool gpuDrainFault = fault == Fault::GpuDrain;
            const bool seekVariant = fault == Fault::SeekPaused;
            const bool seekStallVariant = fault == Fault::SeekPresentationStall;
            const bool seekPlayingVariant = fault == Fault::SeekPlaying;
            const bool seekResumeFaultVariant = fault == Fault::SeekResumeFault;
            const bool seekGenerationVariant = fault == Fault::SeekAudioGenerationStall;
            const bool seekBarrierVariant = fault == Fault::SeekResumeBarrier;
            const bool seekShutdownRaceVariant = fault == Fault::SeekShutdownRace;
            const bool seekFailClosedVariant =
                seekStallVariant || seekResumeFaultVariant || seekGenerationVariant;
            const auto terminalTelemetry = engine->telemetry();
            const auto diagnostics =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);

            const bool expectedTerminal =
                failureFault ? status.state == mvm::preview::PreviewEngineState::Error
                             : status.state == mvm::preview::PreviewEngineState::Shutdown;
            // audio failureはAudioFailureとしてfatalに出る。QPCへ退避して再生を
            // 続けない (projection失敗が記録され、Playingへ戻らない)。
            // seek presentation stallはSeekFailureであり、audio failureではない。
            const bool audioFaultPass =
                !failureFault || gpuDrainFault || seekStallVariant || seekGenerationVariant ||
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
            const bool seekPass =
                !seekVariant ||
                (diagnostics.seekRequestCount == 1 && diagnostics.seekCompletedCount == 1 &&
                 diagnostics.seekDecodeReadyCount == 1 &&
                 diagnostics.lastSeekPresentedFrame == kSeekTargetFrame &&
                 diagnostics.seekStaleGenerationRejectCount == 0 && sink->errors.empty());
            // decode readyでもcompleteにしないこと。completionは0のまま、
            // deadlineでSeekFailureとしてterminal Errorへ落ちる。
            const bool seekStallPass =
                !seekStallVariant ||
                (diagnostics.seekRequestCount == 1 && diagnostics.seekDecodeReadyCount == 1 &&
                 diagnostics.seekCompletedCount == 0 &&
                 diagnostics.seekAwaitingPresentationCount >= 1 &&
                 diagnostics.lastSeekPresentedFrame == -1 && sink->errors.size() == 1 &&
                 sink->errors.front().category ==
                     mvm::preview::PreviewErrorCategory::SeekFailure &&
                 sink->errors.front().severity ==
                     mvm::preview::PreviewErrorSeverity::FatalToSession);
            // Playing originのseekは、resume成功後にだけPlayingへ戻る。
            const bool seekPlayingPass =
                !seekPlayingVariant ||
                (diagnostics.seekRequestCount == 1 && diagnostics.seekCompletedCount == 1 &&
                 diagnostics.lastSeekPresentedFrame == kSeekTargetFrame && sink->errors.empty());
            // resume失敗時は`Playing`を一度も公開せず、AudioFailureで落ちる。
            const bool seekResumeFaultPass =
                !seekResumeFaultVariant ||
                (diagnostics.seekCompletedCount == 1 &&
                 !sink->sawStateAfter(seekStateBaseline,
                                      mvm::preview::PreviewEngineState::Playing) &&
                 sink->errors.size() == 1 &&
                 sink->errors.front().category ==
                     mvm::preview::PreviewErrorCategory::AudioFailure &&
                 sink->errors.front().severity ==
                     mvm::preview::PreviewErrorSeverity::FatalToSession);
            // audio generationが揃わない限り提示せず、deadlineでfail-closedにする。
            const bool seekGenerationPass =
                !seekGenerationVariant ||
                (diagnostics.seekDecodeReadyCount == 1 && diagnostics.seekCompletedCount == 0 &&
                 diagnostics.seekStaleGenerationRejectCount >= 1 &&
                 diagnostics.staleSubstitutionCount == 0 &&
                 !sink->sawStateAfter(seekStateBaseline,
                                      mvm::preview::PreviewEngineState::Playing) &&
                 sink->errors.size() == 1 &&
                 sink->errors.front().category ==
                     mvm::preview::PreviewErrorCategory::SeekFailure);
            // barrier で resume を止めている間 Playing を公開せず、解放後に Playing。
            const bool seekBarrierPass =
                !seekBarrierVariant ||
                (diagnostics.seekCompletedCount == 1 &&
                 diagnostics.seekCancelledByShutdownCount == 0 && sink->errors.empty());
            // resume 中の shutdown は cancellation であり、Error にしない。
            const bool seekShutdownRacePass =
                !seekShutdownRaceVariant ||
                (status.state == mvm::preview::PreviewEngineState::Shutdown &&
                 !sink->sawStateAfter(seekStateBaseline,
                                      mvm::preview::PreviewEngineState::Playing) &&
                 diagnostics.lifecycleViolationCount == 0 && sink->errors.empty());
            const bool cleanRunPass =
                failureFault || seekVariant || seekFailClosedVariant || seekPlayingVariant ||
                seekBarrierVariant || seekShutdownRaceVariant ||
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
            const std::vector<ShutdownStep> expectedGpuDrainSequence{
                ShutdownStep::DisableSchedulers,
                ShutdownStep::StopAudioSink,
                ShutdownStep::StopAudioDecodeWorker,
                ShutdownStep::StopVideoWorkers,
                ShutdownStep::DetachRenderVisibleWorkerRefs,
                ShutdownStep::VerifyJoins,
                ShutdownStep::RequestRenderTeardown,
                ShutdownStep::FiniteGpuRetirementDrain,
                ShutdownStep::PublishShutdownComplete};
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
            const bool shutdownOrderPass =
                diagnostics.shutdownSequence ==
                (gpuDrainFault ? expectedGpuDrainSequence : expectedSequence);
            if (!shutdownOrderPass) {
                std::fprintf(stderr, "shutdown orderingがcontract §12と一致しません (steps=%zu)\n",
                             diagnostics.shutdownSequence.size());
            }

            const bool pass =
                expectedTerminal && audioFaultPass && cleanRunPass && seekPass && seekStallPass && seekPlayingPass &&
                seekResumeFaultPass && seekGenerationPass && seekBarrierPass &&
                seekShutdownRacePass && shutdownOrderPass &&
                !sink->sequenceViolation &&
                !diagnostics.audioMasterActive && diagnostics.renderVisibleWorkersDetached &&
                diagnostics.audioSinkJoined && diagnostics.audioWorkerJoined &&
                diagnostics.workerJoined &&
                // GPU完了を確認できない場合はresourceを解放せずquarantineする。
                (gpuDrainFault ? !diagnostics.renderTeardownComplete &&
                                     !diagnostics.deviceReleased &&
                                     diagnostics.unsafeGpuResourcesRetained &&
                                     sink->errors.size() == 1 &&
                                     sink->errors.front().category ==
                                         mvm::preview::PreviewErrorCategory::ShutdownFailure
                               : diagnostics.renderTeardownComplete &&
                                     diagnostics.deviceReleased &&
                                     !diagnostics.unsafeGpuResourcesRetained) &&
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
                        "\"seek_requests\":%llu,\"seek_decode_ready\":%llu,"
                        "\"seek_completed\":%llu,\"seek_awaiting_presentation\":%llu,"
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
                        diagnostics.shutdownSequence.size(),
                        static_cast<unsigned long long>(diagnostics.seekRequestCount),
                        static_cast<unsigned long long>(diagnostics.seekDecodeReadyCount),
                        static_cast<unsigned long long>(diagnostics.seekCompletedCount),
                        static_cast<unsigned long long>(diagnostics.seekAwaitingPresentationCount),
                        sink->errors.size(),
                        static_cast<int>(status.state));
            if (!pass) {
                // 理由をJSONだけに残すと、CTestからは無言の失敗に見える。
                for (const auto& reported : sink->errors) {
                    std::fprintf(stderr, "[p5d] error: category=%d severity=%d detail=%s\n",
                                 static_cast<int>(reported.category),
                                 static_cast<int>(reported.severity), reported.detail.c_str());
                }
            }
            exitCode = pass ? 0 : 9;
            app.quit();
        }
    });
    timer.start();
    app.exec();
    return exitCode;
}

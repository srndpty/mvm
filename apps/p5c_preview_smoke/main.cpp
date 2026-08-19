#include "app/preview/preview_engine_rhi_item.h"
#include "preview_engine/preview_engine.h"
#include "preview_engine/preview_engine_internal.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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

    void positionChanged(mvm::preview::PreviewPosition) override {}

    void framePresented(mvm::preview::PresentedFrameInfo frame) override {
        if (!frames.empty() && frame.presentationSequence <= frames.back().presentationSequence)
            sequenceViolation = true;
        frames.push_back(frame);
    }

    void errorOccurred(mvm::preview::PreviewError error) override {
        errors.push_back(std::move(error));
    }

    void deviceChanged(mvm::preview::PreviewDeviceInfo) override {}

    std::vector<mvm::preview::PresentedFrameInfo> frames;
    std::vector<mvm::preview::PreviewError> errors;
    mvm::preview::PreviewEngineState terminal = mvm::preview::PreviewEngineState::Uninitialized;
    bool sequenceViolation = false;
};

enum class Stage {
    WaitDevice,
    WaitInitial,
    PauseHold,
    WaitResume,
    WaitShutdown,
    WaitEngineRelease,
    WaitRenderIdle,
    WaitRendererInvalidation,
    WaitRendererResume,
    WaitEofIdle
};
enum class Fault {
    None,
    GpuDrain,
    Device,
    Decoder,
    SourceStartupShutdown,
    PreAttachShutdown,
    EngineLifetimeDetach,
    RendererRecreation,
    DecoderEof
};

} // namespace

int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    const QStringList arguments = app.arguments();
    if (arguments.size() < 2 || arguments.size() > 3) {
        std::fprintf(stderr, "使い方: mvm_p5c_preview_smoke <fixture-a-path> "
                             "[--fault-gpu-drain|--fault-device|--fault-decoder|"
                             "--shutdown-source-startup|"
                             "--shutdown-before-attach|--engine-lifetime-detach|"
                             "--renderer-recreation|--decoder-eof]\n");
        return 2;
    }
    Fault fault = Fault::None;
    if (arguments.size() == 3) {
        if (arguments[2] == "--fault-gpu-drain")
            fault = Fault::GpuDrain;
        else if (arguments[2] == "--fault-device")
            fault = Fault::Device;
        else if (arguments[2] == "--fault-decoder")
            fault = Fault::Decoder;
        else if (arguments[2] == "--shutdown-source-startup")
            fault = Fault::SourceStartupShutdown;
        else if (arguments[2] == "--shutdown-before-attach")
            fault = Fault::PreAttachShutdown;
        else if (arguments[2] == "--engine-lifetime-detach")
            fault = Fault::EngineLifetimeDetach;
        else if (arguments[2] == "--renderer-recreation")
            fault = Fault::RendererRecreation;
        else if (arguments[2] == "--decoder-eof")
            fault = Fault::DecoderEof;
        else
            return 2;
    }
    QFile fixture(arguments[1]);
    QCryptographicHash fixtureHash(QCryptographicHash::Sha256);
    if (!fixture.open(QIODevice::ReadOnly) || !fixtureHash.addData(&fixture) ||
        fixtureHash.result().toHex() !=
            "d398114c38806f39670df51dfabb0095d462cbc35286ea1467901d4007cf0308") {
        std::fprintf(stderr, "fixture AのSHA-256がauthoritative値と一致しません\n");
        return 2;
    }

    auto engine = std::make_shared<mvm::preview::PreviewEngine>();
    auto dispatcher = std::make_shared<QtDispatcher>(&app);
    auto sink = std::make_shared<SmokeSink>();
    auto initialized = engine->initialize({{{60, 1}}}, dispatcher);
    if (!initialized || !engine->attachEventSink(sink))
        return 3;
    const auto beforeAttachPlay = engine->play();
    const auto beforeAttachSource = engine->addSource({arguments[1].toStdWString(), true, false});
    if (beforeAttachPlay ||
        beforeAttachPlay.error().category != mvm::preview::PreviewErrorCategory::InvalidState ||
        beforeAttachSource ||
        beforeAttachSource.error().category != mvm::preview::PreviewErrorCategory::InvalidState) {
        return 3;
    }

    QQuickWindow window;
    std::atomic_uint64_t renderPassCount{0};
    std::atomic_uint64_t sceneGraphInvalidationCount{0};
    QObject::connect(&window, &QQuickWindow::afterRendering, &app,
                     [&] { renderPassCount.fetch_add(1, std::memory_order_relaxed); },
                     Qt::DirectConnection);
    QObject::connect(
        &window, &QQuickWindow::sceneGraphInvalidated, &app,
        [&] { sceneGraphInvalidationCount.fetch_add(1, std::memory_order_release); },
        Qt::DirectConnection);
    window.setWidth(1920);
    window.setHeight(1080);
    auto* surface = new mvm::app::PreviewEngineRhiItem(window.contentItem());
    surface->setWidth(1920);
    surface->setHeight(1080);
    surface->setEngine(engine);
    if (fault == Fault::PreAttachShutdown && !engine->requestShutdown())
        return 15;
    window.show();

    Stage stage =
        fault == Fault::PreAttachShutdown ? Stage::WaitShutdown : Stage::WaitDevice;
    mvm::preview::AcceptedComposition accepted;
    std::uint64_t pausePresentedCount = 0;
    std::uint64_t decodeFailureCountBeforeFault = 0;
    std::weak_ptr<mvm::preview::PreviewEngine> detachedEngine;
    std::uint64_t idleRenderPassCount = 0;
    std::uint64_t eofDroppedFrameCount = 0;
    std::int64_t pausePosition = -1;
    mvm::preview::internal::P5CRuntimeDiagnostics activeDiagnostics;
    const auto started = std::chrono::steady_clock::now();
    auto stageStarted = started;
    int exitCode = 0;

    QTimer timer;
    timer.setInterval(10);
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        const auto now = std::chrono::steady_clock::now();
        if (now - started > std::chrono::seconds(30)) {
            std::fprintf(stderr, "P5-C smokeが30秒以内に完了しませんでした\n");
            exitCode = 10;
            app.quit();
            return;
        }
        if (stage == Stage::WaitEngineRelease) {
            if (!detachedEngine.expired())
                return;
            idleRenderPassCount = renderPassCount.load(std::memory_order_relaxed);
            stage = Stage::WaitRenderIdle;
            stageStarted = now;
            return;
        }
        if (stage == Stage::WaitRenderIdle) {
            if (now - stageStarted < std::chrono::milliseconds(300))
                return;
            const bool idle = renderPassCount.load(std::memory_order_relaxed) ==
                              idleRenderPassCount;
            std::printf("{\"verdict\":\"%s\",\"engine_lifetime_retained\":true,"
                        "\"detached_render_idle\":%s}\n",
                        idle ? "PASS" : "FAIL", idle ? "true" : "false");
            exitCode = idle ? 0 : 17;
            app.quit();
            return;
        }
        if (stage == Stage::WaitRendererInvalidation) {
            if (sceneGraphInvalidationCount.load(std::memory_order_acquire) == 0)
                return;
            window.show();
            stage = Stage::WaitRendererResume;
            stageStarted = now;
            return;
        }
        if (stage == Stage::WaitEofIdle) {
            if (now - stageStarted < std::chrono::milliseconds(300))
                return;
            const auto eofTelemetry = engine->telemetry();
            const auto eofDiagnostics =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            const bool pass = eofTelemetry.droppedFrameCount == eofDroppedFrameCount &&
                              renderPassCount.load(std::memory_order_relaxed) ==
                                  idleRenderPassCount &&
                              eofTelemetry.status.state ==
                                  mvm::preview::PreviewEngineState::Shutdown &&
                              eofDiagnostics.renderTeardownComplete &&
                              eofDiagnostics.deviceReleased && sink->errors.empty();
            std::printf("{\"verdict\":\"%s\",\"eof_render_idle\":%s,"
                        "\"dropped_stable\":%s}\n",
                        pass ? "PASS" : "FAIL", pass ? "true" : "false",
                        pass ? "true" : "false");
            exitCode = pass ? 0 : 20;
            app.quit();
            return;
        }
        const auto status = engine->status();
        const auto telemetry = engine->telemetry();
        if (stage == Stage::WaitDevice &&
            status.state == mvm::preview::PreviewEngineState::ReadyPaused) {
            const QFileInfo fixtureInfo(arguments[1]);
            const std::filesystem::path missing =
                (fixtureInfo.absoluteFilePath() + ".missing").toStdWString();
            const std::filesystem::path noVideo =
                fixtureInfo.dir().filePath("manifest.json").toStdWString();
            const auto missingResult = engine->addSource({missing, true, false});
            const auto noVideoResult = engine->addSource({noVideo, true, false});
            // P5-D2でaudio sourceは受理されるようになったため、ここでの負例は
            // 「存在しないpathのaudio source」に置き換える。P5-C smokeはvideo-only
            // 経路の回帰であり、audio masterは p5d smoke 側で検証する。
            const auto missingAudioResult = engine->addSource({missing, false, true});
            if (missingResult || noVideoResult || missingAudioResult ||
                missingResult.error().category !=
                    mvm::preview::PreviewErrorCategory::DecodeFailure ||
                noVideoResult.error().category !=
                    mvm::preview::PreviewErrorCategory::DecodeFailure ||
                missingAudioResult.error().category !=
                    mvm::preview::PreviewErrorCategory::DecodeFailure) {
                exitCode = 11;
                app.quit();
                return;
            }
            mvm::preview::PreviewSourceDescriptor descriptor;
            descriptor.mediaPath = arguments[1].toStdWString();
            descriptor.videoEnabled = true;
            auto source = engine->addSource(descriptor);
            if (!source) {
                std::fprintf(stderr, "source open失敗: %s\n", source.error().detail.c_str());
                exitCode = 4;
                app.quit();
                return;
            }
            const auto second = engine->addSource(descriptor);
            const auto empty =
                engine->submitComposition(std::make_shared<mvm::preview::CompositionSnapshot>());
            auto unknownSnapshot = std::make_shared<mvm::preview::CompositionSnapshot>();
            unknownSnapshot->layers.push_back(
                {{source.value().value + 100}, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0F});
            const auto unknown = engine->submitComposition(unknownSnapshot);
            auto twoLayerSnapshot = std::make_shared<mvm::preview::CompositionSnapshot>();
            twoLayerSnapshot->layers.push_back({source.value(), {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0F});
            twoLayerSnapshot->layers.push_back({source.value(), {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0F});
            const auto twoLayer = engine->submitComposition(twoLayerSnapshot);
            const auto playWithoutComposition = engine->play();
            const auto pauseOutsidePlaying = engine->pause();
            if (second || empty || unknown || twoLayer || playWithoutComposition ||
                pauseOutsidePlaying ||
                second.error().category !=
                    mvm::preview::PreviewErrorCategory::UnsupportedCapability ||
                empty.error().category != mvm::preview::PreviewErrorCategory::CompositionFailure ||
                unknown.error().category != mvm::preview::PreviewErrorCategory::InvalidSource ||
                twoLayer.error().category !=
                    mvm::preview::PreviewErrorCategory::UnsupportedCapability ||
                playWithoutComposition.error().category !=
                    mvm::preview::PreviewErrorCategory::InvalidState ||
                pauseOutsidePlaying.error().category !=
                    mvm::preview::PreviewErrorCategory::InvalidState) {
                exitCode = 12;
                app.quit();
                return;
            }
            if (fault == Fault::SourceStartupShutdown) {
                if (!engine->requestShutdown()) {
                    exitCode = 14;
                    app.quit();
                    return;
                }
                stage = Stage::WaitShutdown;
                return;
            }
            auto snapshot = std::make_shared<mvm::preview::CompositionSnapshot>();
            snapshot->layers.push_back({source.value(), {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0F});
            auto composition = engine->submitComposition(snapshot);
            // P5-D3でseekは受理対象になった。P5-C smokeはvideo-only経路の回帰なので
            // seekは行わず、引数検査がfail-closedであることだけを確認する。
            const auto seek = engine->seek({-1});
            if (!composition || seek ||
                seek.error().category != mvm::preview::PreviewErrorCategory::SeekFailure ||
                !engine->play()) {
                exitCode = 5;
                app.quit();
                return;
            }
            accepted = composition.value();
            stage = Stage::WaitInitial;
            stageStarted = now;
        } else if (stage == Stage::WaitInitial && telemetry.presentedFrameCount >= 12) {
            if (fault == Fault::EngineLifetimeDetach) {
                detachedEngine = engine;
                surface->setEngine({});
                engine.reset();
                if (detachedEngine.expired()) {
                    exitCode = 16;
                    app.quit();
                    return;
                }
                stage = Stage::WaitEngineRelease;
                return;
            }
            if (fault == Fault::RendererRecreation) {
                pausePresentedCount = telemetry.presentedFrameCount;
                window.setPersistentSceneGraph(false);
                window.hide();
                stage = Stage::WaitRendererInvalidation;
                stageStarted = now;
                return;
            }
            if (fault == Fault::DecoderEof) {
                if (!mvm::preview::internal::PreviewRenderPort::injectDecoderEofForTest(*engine)) {
                    exitCode = 21;
                    app.quit();
                    return;
                }
                stage = Stage::WaitShutdown;
                return;
            }
            if (fault != Fault::None) {
                bool injected = false;
                if (fault == Fault::GpuDrain) {
                    injected = static_cast<bool>(
                        mvm::preview::internal::PreviewRenderPort::injectGpuDrainFailureForTest(
                            *engine));
                    if (injected)
                        injected = static_cast<bool>(engine->requestShutdown());
                } else if (fault == Fault::Device) {
                    mvm::preview::PreviewError error;
                    error.category = mvm::preview::PreviewErrorCategory::DeviceFailure;
                    error.severity = mvm::preview::PreviewErrorSeverity::FatalToSession;
                    error.operation = mvm::preview::PreviewOperation::RenderDeviceAttach;
                    error.detail = "P5-C injected device fatal";
                    injected = static_cast<bool>(
                        mvm::preview::internal::PreviewRenderPort::injectFatal(*engine, error));
                } else {
                    decodeFailureCountBeforeFault = telemetry.decodeFailureCount;
                    injected = static_cast<bool>(
                        mvm::preview::internal::PreviewRenderPort::injectDecoderFatalForTest(
                            *engine, "P5-C injected decoder fatal"));
                }
                if (!injected) {
                    exitCode = 13;
                    app.quit();
                    return;
                }
                stage = Stage::WaitShutdown;
                return;
            }
            if (!engine->pause()) {
                exitCode = 6;
                app.quit();
                return;
            }
            pausePresentedCount = telemetry.presentedFrameCount;
            pausePosition = status.position.outputFrame;
            stage = Stage::PauseHold;
            stageStarted = now;
        } else if (stage == Stage::PauseHold &&
                   now - stageStarted > std::chrono::milliseconds(500)) {
            if (telemetry.presentedFrameCount != pausePresentedCount ||
                status.position.outputFrame != pausePosition || !engine->play()) {
                exitCode = 7;
                app.quit();
                return;
            }
            stage = Stage::WaitResume;
        } else if (stage == Stage::WaitRendererResume) {
            if (status.state == mvm::preview::PreviewEngineState::Error) {
                exitCode = 18;
                app.quit();
                return;
            }
            if (telemetry.presentedFrameCount >= pausePresentedCount + 8) {
                activeDiagnostics =
                    mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
                if (!engine->requestShutdown()) {
                    exitCode = 19;
                    app.quit();
                    return;
                }
                stage = Stage::WaitShutdown;
            }
        } else if (stage == Stage::WaitResume &&
                   telemetry.presentedFrameCount >= pausePresentedCount + 8) {
            activeDiagnostics =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            if (!engine->requestShutdown()) {
                exitCode = 8;
                app.quit();
                return;
            }
            stage = Stage::WaitShutdown;
        } else if (stage == Stage::WaitShutdown &&
                   (status.state == mvm::preview::PreviewEngineState::Shutdown ||
                    status.state == mvm::preview::PreviewEngineState::Error)) {
            if (fault == Fault::DecoderEof) {
                if (status.state != mvm::preview::PreviewEngineState::Shutdown) {
                    exitCode = 22;
                    app.quit();
                    return;
                }
                eofDroppedFrameCount = telemetry.droppedFrameCount;
                idleRenderPassCount = renderPassCount.load(std::memory_order_relaxed);
                stage = Stage::WaitEofIdle;
                stageStarted = now;
                return;
            }
            const bool failureFault = fault == Fault::GpuDrain || fault == Fault::Device ||
                                      fault == Fault::Decoder;
            if (failureFault && sink->errors.empty())
                return;
            const auto terminalTelemetry = engine->telemetry();
            const auto diagnostics =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            const bool expectedTerminal =
                failureFault ? status.state == mvm::preview::PreviewEngineState::Error
                             : status.state == mvm::preview::PreviewEngineState::Shutdown;
            const bool preAttachShutdown = fault == Fault::PreAttachShutdown;
            const bool needsPresentedFrames =
                fault != Fault::SourceStartupShutdown && !preAttachShutdown;
            const bool runtimeTeardownPass =
                preAttachShutdown || (fault == Fault::GpuDrain
                                          ? diagnostics.workerJoined &&
                                                !diagnostics.renderTeardownComplete &&
                                                !diagnostics.deviceReleased &&
                                                diagnostics.unsafeGpuResourcesRetained
                                          : diagnostics.workerJoined &&
                                                diagnostics.renderTeardownComplete &&
                                                diagnostics.deviceReleased &&
                                                !diagnostics.unsafeGpuResourcesRetained);
            const bool decoderErrorPass =
                fault != Fault::Decoder ||
                (sink->errors.size() == 1 &&
                 terminalTelemetry.decodeFailureCount == decodeFailureCountBeforeFault + 1 &&
                 sink->errors.front().category ==
                     mvm::preview::PreviewErrorCategory::DecodeFailure &&
                 sink->errors.front().severity ==
                     mvm::preview::PreviewErrorSeverity::FatalToSession &&
                 sink->errors.front().detail.find("P5-C injected decoder fatal") !=
                     std::string::npos);
            const bool gpuDrainRetentionPass =
                fault != Fault::GpuDrain ||
                (sink->errors.size() == 1 &&
                 sink->errors.front().category ==
                     mvm::preview::PreviewErrorCategory::ShutdownFailure &&
                 sink->errors.front().severity ==
                     mvm::preview::PreviewErrorSeverity::FatalToSession &&
                 sink->errors.front().detail.find("shutdown completion poll failure") !=
                     std::string::npos);
            const bool pass =
                expectedTerminal && decoderErrorPass && gpuDrainRetentionPass &&
                !sink->sequenceViolation &&
                (!needsPresentedFrames ||
                 (!sink->frames.empty() && sink->frames.back().composition == accepted)) &&
                diagnostics.distinctPresentedSourceFrameCount >=
                    (fault == Fault::None ? 20U : (failureFault ? 12U : 0U)) &&
                (fault != Fault::None ||
                 (activeDiagnostics.nativeDeviceAttached && activeDiagnostics.d3d11vaActive &&
                  activeDiagnostics.decodeRenderSameDevice &&
                  activeDiagnostics.registeredVideoSourceCount == 1 &&
                  activeDiagnostics.gpuCompositionPassCount >= 20 &&
                  activeDiagnostics.deviceLostCount == 0)) &&
                diagnostics.fullCpuReadbackCount == 0 && diagnostics.fullFrameGpuCopyCount == 0 &&
                diagnostics.softwareFallbackCount == 0 && diagnostics.staleSubstitutionCount == 0 &&
                diagnostics.untrackedSubmissionCount == 0 &&
                diagnostics.earlyPayloadReleaseCount == 0 &&
                diagnostics.retirementTimeoutCount == 0 &&
                diagnostics.lifecycleViolationCount == 0 && runtimeTeardownPass &&
                terminalTelemetry.eventDeliveryFailureCount == 0;
            const double seconds = std::chrono::duration<double>(now - started).count();
            std::printf(
                "{\"verdict\":\"%s\",\"presented\":%llu,\"dropped\":%llu,"
                "\"decode_failures\":%llu,\"errors\":%zu,"
                "\"fps\":%.3f,\"distinct\":%llu,\"terminal\":%d}\n",
                pass ? "PASS" : "FAIL",
                static_cast<unsigned long long>(terminalTelemetry.presentedFrameCount),
                static_cast<unsigned long long>(terminalTelemetry.droppedFrameCount),
                static_cast<unsigned long long>(terminalTelemetry.decodeFailureCount),
                sink->errors.size(),
                seconds > 0.0
                    ? static_cast<double>(terminalTelemetry.presentedFrameCount) / seconds
                    : 0.0,
                static_cast<unsigned long long>(diagnostics.distinctPresentedSourceFrameCount),
                static_cast<int>(status.state));
            exitCode = pass ? 0 : 9;
            app.quit();
        }
    });
    timer.start();
    app.exec();
    return exitCode;
}

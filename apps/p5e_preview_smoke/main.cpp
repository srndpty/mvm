// P5-E product composition の検証アプリ。
//
// P5-E1 時点では capability が 1/1 のままなので、ここで固定するのは
// 「product render path が `CompositorCoordinator` を composition epoch の
// authority として通っていること」である。二 source / 二 layer は P5-E3 で足す。
//
// 特に `--fault-stale-composition-epoch` は、compose 成立後・提示前に
// `CompositionEpoch` だけを進め、`validateForDisplay()` が製品経路で実際に
// 効いていることを固定する。固定するのは reject branch を踏んだことだけではなく、
// **その output frame を実際に提示しなかったこと**である。counter だけでは
// 「reject を数えたうえでそのまま描画する」bug を検出できない。
#include "app/preview/preview_engine_rhi_item.h"
#include "preview_engine/preview_engine.h"
#include "preview_engine/preview_engine_internal.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

#include <QCryptographicHash>
#include <QFile>
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
    void stateChanged(mvm::preview::PreviewEngineState) override {}

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
    bool sequenceViolation = false;
};

enum class Stage { WaitDevice, WaitInitial, WaitRecovered, WaitShutdown };
enum class Fault { None, StaleCompositionEpoch };

} // namespace

int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    const QStringList arguments = app.arguments();
    if (arguments.size() < 2 || arguments.size() > 3) {
        std::fprintf(stderr, "使い方: mvm_p5e_preview_smoke <fixture-a-path> "
                             "[--fault-stale-composition-epoch]\n");
        return 2;
    }
    Fault fault = Fault::None;
    if (arguments.size() == 3) {
        if (arguments[2] == "--fault-stale-composition-epoch")
            fault = Fault::StaleCompositionEpoch;
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
    if (!engine->initialize({{{60, 1}}}, dispatcher) || !engine->attachEventSink(sink))
        return 3;

    QQuickWindow window;
    window.setWidth(1920);
    window.setHeight(1080);
    auto* surface = new mvm::app::PreviewEngineRhiItem(window.contentItem());
    surface->setWidth(1920);
    surface->setHeight(1080);
    surface->setEngine(engine);
    window.show();

    Stage stage = Stage::WaitDevice;
    mvm::preview::AcceptedComposition accepted;
    std::uint64_t presentedAtInjection = 0;
    std::int64_t rejectedFrame = -1;
    mvm::preview::internal::P5CRuntimeDiagnostics activeDiagnostics;
    const auto started = std::chrono::steady_clock::now();
    int exitCode = 0;

    QTimer timer;
    timer.setInterval(10);
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        const auto now = std::chrono::steady_clock::now();
        if (now - started > std::chrono::seconds(30)) {
            std::fprintf(stderr, "P5-E smokeが30秒以内に完了しませんでした\n");
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
            auto source = engine->addSource(descriptor);
            if (!source) {
                std::fprintf(stderr, "source open失敗: %s\n", source.error().detail.c_str());
                exitCode = 4;
                app.quit();
                return;
            }
            // composition runtimeが未構成の間はepoch advance seamも成立しない。
            // seam自体がfail-closedであることをここで固定する。
            const auto tooEarly =
                mvm::preview::internal::PreviewRenderPort::injectCompositionEpochAdvanceForTest(
                    *engine);
            if (tooEarly ||
                tooEarly.error().category != mvm::preview::PreviewErrorCategory::InvalidState) {
                exitCode = 11;
                app.quit();
                return;
            }
            auto snapshot = std::make_shared<mvm::preview::CompositionSnapshot>();
            snapshot->layers.push_back({source.value(), {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0F});
            auto composition = engine->submitComposition(snapshot);
            if (!composition || !engine->play()) {
                exitCode = 5;
                app.quit();
                return;
            }
            accepted = composition.value();
            stage = Stage::WaitInitial;
            return;
        }
        if (stage == Stage::WaitInitial && telemetry.presentedFrameCount >= 12) {
            if (fault == Fault::StaleCompositionEpoch) {
                presentedAtInjection = telemetry.presentedFrameCount;
                if (!mvm::preview::internal::PreviewRenderPort::
                        injectCompositionEpochAdvanceForTest(*engine)) {
                    exitCode = 12;
                    app.quit();
                    return;
                }
                stage = Stage::WaitRecovered;
                return;
            }
            activeDiagnostics =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            if (!engine->requestShutdown()) {
                exitCode = 6;
                app.quit();
                return;
            }
            stage = Stage::WaitShutdown;
            return;
        }
        if (stage == Stage::WaitRecovered) {
            const auto probe =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            if (probe.staleCompositionEpochRejectCount == 0) {
                // 進めたepochが提示前に弾かれないまま提示が進み続けたなら、
                // 製品経路にvalidateForDisplay()が効いていない。timeoutではなく
                // 即座にFAILとして落とす。
                if (telemetry.presentedFrameCount >= presentedAtInjection + 60) {
                    std::fprintf(
                        stderr, "stale composition epochが提示前に拒否されませんでした\n");
                    exitCode = 8;
                    app.quit();
                }
                return;
            }
            // rejectはskipであってfatalではない。engineは次のrender callbackで
            // 実tokenのstateへ戻り、提示を再開できなければならない。
            if (telemetry.presentedFrameCount < presentedAtInjection + 8)
                return;
            activeDiagnostics = probe;
            rejectedFrame = probe.lastStaleCompositionRejectedFrame;
            if (rejectedFrame < 0) {
                std::fprintf(stderr, "拒否したoutput frameを記録していません\n");
                exitCode = 13;
                app.quit();
                return;
            }
            if (!engine->requestShutdown()) {
                exitCode = 7;
                app.quit();
                return;
            }
            stage = Stage::WaitShutdown;
            return;
        }
        if (stage == Stage::WaitShutdown &&
            (status.state == mvm::preview::PreviewEngineState::Shutdown ||
             status.state == mvm::preview::PreviewEngineState::Error)) {
            const auto terminalTelemetry = engine->telemetry();
            const auto diagnostics =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            // 拒否したframeが実際に提示されていないことを、frame identityで確認する。
            // rejectを数えたうえでそのまま描画するbugは、counterだけでは通ってしまう。
            bool rejectedFramePresented = false;
            bool resumedAfterRejectedFrame = false;
            for (const auto& presented : sink->frames) {
                if (presented.position.outputFrame == rejectedFrame)
                    rejectedFramePresented = true;
                if (presented.position.outputFrame > rejectedFrame)
                    resumedAfterRejectedFrame = true;
            }
            const bool staleEpochPass =
                fault == Fault::StaleCompositionEpoch
                    ? diagnostics.staleCompositionEpochRejectCount == 1 &&
                          diagnostics.lifecycleViolationCount == 1 && rejectedFrame >= 0 &&
                          diagnostics.lastStaleCompositionRejectedFrame == rejectedFrame &&
                          !rejectedFramePresented && resumedAfterRejectedFrame
                    : diagnostics.staleCompositionEpochRejectCount == 0 &&
                          diagnostics.lifecycleViolationCount == 0 &&
                          diagnostics.lastStaleCompositionRejectedFrame == -1;
            const bool pass =
                status.state == mvm::preview::PreviewEngineState::Shutdown && staleEpochPass &&
                sink->errors.empty() && !sink->sequenceViolation && !sink->frames.empty() &&
                // PresentedFrameInfoはactual accepted tokenとactual layer数を運ぶ。
                sink->frames.back().composition == accepted &&
                sink->frames.back().activeLayerCount == 1 &&
                diagnostics.distinctPresentedSourceFrameCount >= 12 &&
                activeDiagnostics.nativeDeviceAttached && activeDiagnostics.d3d11vaActive &&
                activeDiagnostics.decodeRenderSameDevice &&
                activeDiagnostics.registeredVideoSourceCount == 1 &&
                diagnostics.fullCpuReadbackCount == 0 && diagnostics.fullFrameGpuCopyCount == 0 &&
                diagnostics.softwareFallbackCount == 0 && diagnostics.staleSubstitutionCount == 0 &&
                diagnostics.untrackedSubmissionCount == 0 &&
                diagnostics.earlyPayloadReleaseCount == 0 &&
                diagnostics.retirementTimeoutCount == 0 && diagnostics.workerJoined &&
                diagnostics.renderTeardownComplete && diagnostics.deviceReleased &&
                !diagnostics.unsafeGpuResourcesRetained &&
                terminalTelemetry.eventDeliveryFailureCount == 0;
            std::printf("{\"verdict\":\"%s\",\"presented\":%llu,"
                        "\"stale_composition_epoch_rejects\":%llu,"
                        "\"rejected_frame\":%lld,\"rejected_frame_presented\":%s,"
                        "\"lifecycle_violations\":%llu,\"errors\":%zu,\"terminal\":%d}\n",
                        pass ? "PASS" : "FAIL",
                        static_cast<unsigned long long>(terminalTelemetry.presentedFrameCount),
                        static_cast<unsigned long long>(
                            diagnostics.staleCompositionEpochRejectCount),
                        static_cast<long long>(diagnostics.lastStaleCompositionRejectedFrame),
                        rejectedFramePresented ? "true" : "false",
                        static_cast<unsigned long long>(diagnostics.lifecycleViolationCount),
                        sink->errors.size(), static_cast<int>(status.state));
            exitCode = pass ? 0 : 9;
            app.quit();
        }
    });
    timer.start();
    app.exec();
    return exitCode;
}

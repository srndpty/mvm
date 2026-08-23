#include "app/preview/compositor_rhi_item.h"
#include "compositor_spike_controller.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSurfaceFormat>

#include <cstdio>
#include <cstring>

using namespace mvm::app;

namespace {
void usage() {
    std::fprintf(stderr,
                 "使い方: mvm_compositor_spike --source-a <path> --source-b <path> "
                 "--metrics <json> [options]\n"
                 "  --warmup-seconds <n> --measure-seconds <n> --seed <n>\n"
                 "  --seek-count <n> --display-timeout-ms <n>\n"
                 "  --formal-preflight\n"
                 "  --diagnostic-timing --diagnostic-case a|b|c|d\n"
                 "  --scheduler-phase-ring\n"
                 "  --presentation-opportunity-ring\n"
                 "  --incremental-mapper-shadow\n"
                 "  --native-present-hook off|on\n"
                 "  --target-rhiitem-pixel-toggle\n"
                 "  --gpu-completion fence|event_query --mode playback|seek|layout\n");
}

bool parse(const QStringList& args, CompositorSpikeConfig& config) {
    for (int i = 1; i < args.size(); ++i) {
        const QString arg = args[i];
        auto value = [&]() -> QString {
            return i + 1 < args.size() ? args[++i] : QString{};
        };
        if (arg == "--source-a") config.sourceA = value();
        else if (arg == "--source-b") config.sourceB = value();
        else if (arg == "--metrics") config.metricsPath = value();
        else if (arg == "--warmup-seconds") config.warmupSeconds = value().toInt();
        else if (arg == "--measure-seconds") config.measureSeconds = value().toInt();
        else if (arg == "--seed") config.seed = value().toUInt();
        else if (arg == "--seek-count") config.seekCount = value().toInt();
        else if (arg == "--display-timeout-ms") config.displayTimeoutMs = value().toInt();
        else if (arg == "--gpu-completion") {
            const QString v = value();
            if (v == "fence") config.completion = mvm::gpu::GpuCompletionBackend::Fence;
            else if (v == "event_query") config.completion = mvm::gpu::GpuCompletionBackend::EventQuery;
            else return false;
        } else if (arg == "--mode") {
            const QString v = value();
            if (v == "playback") config.mode = CompositorMode::Playback;
            else if (v == "seek") config.mode = CompositorMode::Seek;
            else if (v == "layout") config.mode = CompositorMode::Layout;
            else return false;
        } else if (arg == "--test-fault") {
            config.testFault = value();
            if (config.testFault != "device_change" && config.testFault != "completion_fatal")
                return false;
        } else if (arg == "--formal-preflight") config.formalPreflight = true;
        else if (arg == "--diagnostic-timing") config.diagnosticTiming = true;
        else if (arg == "--scheduler-phase-ring") config.schedulerPhaseRing = true;
        else if (arg == "--vblank-observer") config.vblankObserver = true;
        else if (arg == "--presentation-opportunity-ring")
            config.presentationOpportunityRing = true;
        else if (arg == "--incremental-mapper-shadow")
            config.incrementalMapperShadow = true;
        else if (arg == "--native-present-hook") {
            const QString v = value().toLower();
            if (v == "off") config.nativePresentHook = NativePresentHookMode::OffControl;
            else if (v == "on") config.nativePresentHook = NativePresentHookMode::OnDiagnostic;
            else return false;
        }
        else if (arg == "--target-rhiitem-pixel-toggle")
            config.diagnosticTargetPixelToggle = true;
        else if (arg == "--diagnostic-case") {
            const QString v = value().toLower();
            if (v == "a") config.diagnosticCase = CompositorDiagnosticCase::SingleDecode;
            else if (v == "b") config.diagnosticCase = CompositorDiagnosticCase::PairOnly;
            else if (v == "c") config.diagnosticCase = CompositorDiagnosticCase::FixedTextures;
            else if (v == "d") config.diagnosticCase = CompositorDiagnosticCase::FullPath;
            else return false;
            config.diagnosticTiming = true;
        }
        else return false;
    }
    const bool mapperDependencies = !config.incrementalMapperShadow ||
                                    (config.presentationOpportunityRing && config.vblankObserver &&
                                     config.mode == CompositorMode::Playback);
    const bool nativePresentDependencies =
        config.nativePresentHook == NativePresentHookMode::Disabled ||
        (config.presentationOpportunityRing && config.vblankObserver &&
         config.mode == CompositorMode::Playback);
    return !config.sourceA.isEmpty() && !config.sourceB.isEmpty() &&
           !config.metricsPath.isEmpty() && config.warmupSeconds >= 0 &&
           config.measureSeconds > 0 && config.seekCount > 0 && config.displayTimeoutMs > 0 &&
           mapperDependencies && nativePresentDependencies;
}
} // namespace

int main(int argc, char** argv) {
    bool mapperShadowRequested = false;
    bool nativePresentHookRequested = false;
    for (int index = 1; index < argc; ++index)
        if (std::strcmp(argv[index], "--incremental-mapper-shadow") == 0)
            mapperShadowRequested = true;
        else if (std::strcmp(argv[index], "--native-present-hook") == 0)
            nativePresentHookRequested = true;
    if ((mapperShadowRequested || nativePresentHookRequested) &&
        qEnvironmentVariableIsSet("QSG_NO_VSYNC")) {
        std::fprintf(stderr, "formal presentation pathではQSG_NO_VSYNCを使用できません\n");
        return 6;
    }
    if (mapperShadowRequested || nativePresentHookRequested) {
        auto surfaceFormat = QSurfaceFormat::defaultFormat();
        surfaceFormat.setSwapInterval(1);
        QSurfaceFormat::setDefaultFormat(surfaceFormat);
    }
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication app(argc, argv);
    CompositorSpikeConfig config;
    if (!parse(app.arguments(), config)) {
        usage();
        return 2;
    }
    qmlRegisterType<CompositorRhiItem>("mvm.compositor", 1, 0, "CompositorSurface");
    CompositorSpikeController controller(config);
    QQmlApplicationEngine engine;
    engine.load(QUrl(QStringLiteral("qrc:/mvm/compositor_spike/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 5;
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    auto* surface = window ? window->findChild<CompositorRhiItem*>("compositorSurface") : nullptr;
    if (!surface)
        return 5;
    surface->setPreferredCompletionBackend(config.completion);
    if (config.presentationOpportunityRing ||
        (config.formalPreflight && config.mode == CompositorMode::Playback &&
         config.diagnosticCase == CompositorDiagnosticCase::None)) {
        QObject::connect(window, &QQuickWindow::frameSwapped, surface,
                         &CompositorRhiItem::recordFrameSwapped, Qt::DirectConnection);
    }
    controller.attach(surface);
    // Main.qmlはvisible:falseで生成される。configが全て確定した後にだけ
    // render threadを起動させ、initialize()がattach()を追い越す順序を排除する。
    window->setVisible(true);
    QObject::connect(&controller, &CompositorSpikeController::finished, &app,
                     [&] { app.exit(controller.exitCode()); });
    return app.exec();
}

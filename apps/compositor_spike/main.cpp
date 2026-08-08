#include "app/preview/compositor_rhi_item.h"
#include "compositor_spike_controller.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>

#include <cstdio>

using namespace mvm::app;

namespace {
void usage() {
    std::fprintf(stderr,
                 "使い方: mvm_compositor_spike --source-a <path> --source-b <path> "
                 "--metrics <json> [options]\n"
                 "  --warmup-seconds <n> --measure-seconds <n> --seed <n>\n"
                 "  --seek-count <n> --display-timeout-ms <n>\n"
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
        } else return false;
    }
    return !config.sourceA.isEmpty() && !config.sourceB.isEmpty() &&
           !config.metricsPath.isEmpty() && config.warmupSeconds >= 0 &&
           config.measureSeconds > 0 && config.seekCount > 0 && config.displayTimeoutMs > 0;
}
} // namespace

int main(int argc, char** argv) {
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
    controller.attach(surface);
    QObject::connect(&controller, &CompositorSpikeController::finished, &app,
                     [&] { app.exit(controller.exitCode()); });
    return app.exec();
}

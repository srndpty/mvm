#include "app/preview/compositor_rhi_item.h"
#include "p3_av_sync_controller.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>

#include <cstdio>

using namespace mvm::app;

namespace {
void usage() {
    std::fprintf(stderr,
                 "使い方: mvm_p3_av_sync_spike --source-a <path> --source-b <path> "
                 "--metrics <json> --mode playback|seek|pause-resume [options]\n"
                 "  --formal-contract | --formal-contract-c2\n"
                 "  --inject-render-fault-after-playing (test専用)\n"
                 "  --diagnostic-fixed-seek-target <frame> (診断専用)\n");
}

bool parse(const QStringList& args, P3AvConfig& config) {
    for (int i = 1; i < args.size(); ++i) {
        const QString key = args[i];
        auto value = [&]() { return i + 1 < args.size() ? args[++i] : QString{}; };
        if (key == "--source-a") config.sourceA = value();
        else if (key == "--source-b") config.sourceB = value();
        else if (key == "--metrics") config.metricsPath = value();
        else if (key == "--duration-seconds") config.durationSeconds = value().toInt();
        else if (key == "--seek-count") config.seekCount = value().toInt();
        else if (key == "--seed") config.seed = value().toUInt();
        else if (key == "--display-timeout-ms") config.displayTimeoutMs = value().toInt();
        else if (key == "--warmup-seconds") config.warmupSeconds = value().toInt();
        else if (key == "--inject-render-fault-after-playing")
            config.injectRenderFaultAfterPlaying = true;
        else if (key == "--diagnostic-fixed-seek-target") {
            bool ok = false;
            config.diagnosticFixedSeekTarget = value().toLongLong(&ok);
            if (!ok || config.diagnosticFixedSeekTarget < 0)
                return false;
        }
        else if (key == "--formal-contract") config.formalContract = true;
        else if (key == "--formal-contract-c2") {
            config.formalContract = true;
            config.formalContractC2 = true;
        }
        else if (key == "--mode") {
            const QString mode = value();
            if (mode == "playback") config.mode = P3AvMode::Playback;
            else if (mode == "seek") config.mode = P3AvMode::Seek;
            else if (mode == "pause-resume") config.mode = P3AvMode::PauseResume;
            else return false;
        } else {
            return false;
        }
    }
    return !config.sourceA.isEmpty() && !config.sourceB.isEmpty() &&
           !config.metricsPath.isEmpty() && config.durationSeconds > 0 && config.seekCount > 0 &&
           config.displayTimeoutMs > 0 && config.warmupSeconds >= 0 &&
           (config.diagnosticFixedSeekTarget < 0 || config.mode == P3AvMode::Seek);
}
} // namespace

int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication app(argc, argv);
    P3AvConfig config;
    if (!parse(app.arguments(), config)) {
        usage();
        return 2;
    }
    qmlRegisterType<CompositorRhiItem>("mvm.compositor", 1, 0, "CompositorSurface");
    P3AvSyncController controller(config);
    QQmlApplicationEngine engine;
    engine.load(QUrl(QStringLiteral("qrc:/mvm/p3_av_sync_spike/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 5;
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    auto* surface = window ? window->findChild<CompositorRhiItem*>("compositorSurface") : nullptr;
    if (!surface)
        return 5;
    controller.attach(surface);
    QObject::connect(&controller, &P3AvSyncController::finished, &app,
                     [&] { app.exit(controller.exitCode()); });
    return app.exec();
}

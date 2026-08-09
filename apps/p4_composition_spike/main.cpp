#include "app/preview/compositor_rhi_item.h"
#include "p4_composition_controller.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>

#include <cstdio>

using namespace mvm::app;

namespace {
void usage() {
    std::fprintf(stderr,
                 "使い方: mvm_p4_composition_spike --source-a <path> --source-b <path> "
                 "--metrics <json> [options]\n"
                 "  --warmup-seconds <n>     warmup 秒数 (既定 1)\n"
                 "  --display-timeout-ms <n> 初回表示待ち (既定 3000)\n"
                 "schedule は smoke 固定である。CLI から上書きできない。\n");
}

bool parse(const QStringList& args, P4Config& config) {
    for (int i = 1; i < args.size(); ++i) {
        const QString key = args[i];
        auto value = [&]() { return i + 1 < args.size() ? args[++i] : QString{}; };
        if (key == "--source-a") config.sourceA = value();
        else if (key == "--source-b") config.sourceB = value();
        else if (key == "--metrics") config.metricsPath = value();
        else if (key == "--warmup-seconds") config.warmupSeconds = value().toInt();
        else if (key == "--display-timeout-ms") config.displayTimeoutMs = value().toInt();
        else return false;
    }
    return !config.sourceA.isEmpty() && !config.sourceB.isEmpty() &&
           !config.metricsPath.isEmpty() && config.durationSeconds == 10 &&
           config.warmupSeconds >= 0 && config.displayTimeoutMs > 0;
}
} // namespace

int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication app(argc, argv);
    P4Config config;
    if (!parse(app.arguments(), config)) {
        usage();
        return 2;
    }
    qmlRegisterType<CompositorRhiItem>("mvm.compositor", 1, 0, "CompositorSurface");
    P4CompositionController controller(config);
    QQmlApplicationEngine engine;
    engine.load(QUrl(QStringLiteral("qrc:/mvm/p4_composition_spike/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 5;
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    auto* surface = window ? window->findChild<CompositorRhiItem*>("compositorSurface") : nullptr;
    if (!surface)
        return 5;
    controller.attach(surface);
    QObject::connect(&controller, &P4CompositionController::finished, &app,
                     [&] { app.exit(controller.exitCode()); });
    return app.exec();
}

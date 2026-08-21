#include "app/preview/compositor_rhi_item.h"
#include "app/preview/display_target_contract.h"

#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>

#include <cmath>
#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace mvm::app;

namespace {
QString orientationName(Qt::ScreenOrientation orientation) {
    switch (orientation) {
    case Qt::PortraitOrientation: return QStringLiteral("portrait");
    case Qt::InvertedPortraitOrientation: return QStringLiteral("inverted-portrait");
    case Qt::LandscapeOrientation: return QStringLiteral("landscape");
    case Qt::InvertedLandscapeOrientation: return QStringLiteral("inverted-landscape");
    default: return QStringLiteral("unknown");
    }
}

DisplayEnvironmentSnapshot capture(CompositorRhiItem* item) {
    DisplayEnvironmentSnapshot result;
    const auto state = item->state();
    const auto target = state->actualOutputSizeSnapshot();
    result.rhiTargetPixelWidth = target.width;
    result.rhiTargetPixelHeight = target.height;
    result.compositorSurfaceLogicalWidth = static_cast<int>(std::lround(item->width()));
    result.compositorSurfaceLogicalHeight = static_cast<int>(std::lround(item->height()));
    auto* window = item->window();
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

QJsonObject environmentJson(const DisplayEnvironmentSnapshot& value) {
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

bool writeResult(const QString& path, const DisplayEnvironmentSnapshot& environment,
                 bool passed, const QString& error) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QJsonObject root{{"schema", "mvm-display-preflight-probe-1"},
                           {"preflight_pass", passed},
                           {"error", error},
                           {"display_environment", environmentJson(environment)}};
    return file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) >= 0;
}
} // namespace

int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication app(argc, argv);
    const QStringList args = app.arguments();
    const qsizetype metricsIndex = args.indexOf(QStringLiteral("--metrics"));
    const qsizetype timeoutIndex = args.indexOf(QStringLiteral("--timeout-ms"));
    if (metricsIndex < 0 || metricsIndex + 1 >= args.size() || timeoutIndex < 0 ||
        timeoutIndex + 1 >= args.size()) {
        std::fprintf(stderr, "使い方: mvm_display_preflight_probe --metrics <json> --timeout-ms <ms>\n");
        return 2;
    }
    bool timeoutOk = false;
    const int timeoutMs = args[timeoutIndex + 1].toInt(&timeoutOk);
    if (!timeoutOk || timeoutMs <= 0)
        return 2;

    qmlRegisterType<CompositorRhiItem>("mvm.compositor", 1, 0, "CompositorSurface");
    QQmlApplicationEngine engine;
    engine.load(QUrl(QStringLiteral("qrc:/mvm/display_preflight_probe/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 5;
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    auto* item = window ? window->findChild<CompositorRhiItem*>("compositorSurface") : nullptr;
    if (!item)
        return 5;

    const QString metricsPath = args[metricsIndex + 1];
    const auto state = item->state();
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer poll;
    poll.setInterval(50);
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        const auto snapshot = capture(item);
        const auto result = evaluateP3C2DisplayTarget(snapshot);
        if (result.state == DisplayTargetPreflightState::Waiting && elapsed.elapsed() < timeoutMs)
            return;
        const bool passed = result.state == DisplayTargetPreflightState::Passed;
        const QString error = passed ? QString{} : QString::fromStdString(result.error);
        if (!writeResult(metricsPath, snapshot, passed, error)) {
            app.exit(6);
            return;
        }
        poll.stop();
        item->requestTeardown();
        QTimer* teardown = new QTimer(&app);
        teardown->setInterval(25);
        const qint64 teardownStart = elapsed.elapsed();
        QObject::connect(teardown, &QTimer::timeout, &app, [&, teardown, passed, teardownStart] {
            if (state->teardownComplete.load(std::memory_order_acquire)) {
                teardown->stop();
                app.exit(passed ? 0 : 4);
            } else if (elapsed.elapsed() - teardownStart >= timeoutMs) {
                teardown->stop();
                app.exit(7);
            }
        });
        teardown->start();
    });
    poll.start();
    return app.exec();
}

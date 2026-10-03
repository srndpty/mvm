#include "app/preview/test_window_mode.h"

#include <QGuiApplication>
#include <QScreen>
#include <QWindow>

namespace mvm::app {

bool testFixedWindowRequested() {
    return qEnvironmentVariable("MVM_TEST_FIXED_WINDOW") == QStringLiteral("1");
}

void prepareTestFixedWindowEnvironment() {
    if (!testFixedWindowRequested())
        return;
    // Windows の拡大率 (125% など) と、利用者が設定した Qt の倍率の両方を打ち消す。
    qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
    qputenv("QT_SCALE_FACTOR", "1");
    qunsetenv("QT_SCREEN_SCALE_FACTORS");
}

Qt::WindowFlags testFixedWindowFlags() {
    return Qt::Window | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus |
           Qt::WindowTransparentForInput;
}

namespace {
QPoint testFixedWindowPosition() {
    const QScreen* screen = QGuiApplication::primaryScreen();
    return screen ? screen->geometry().topLeft() : QPoint{};
}
} // namespace

QVariantMap testFixedWindowInitialProperties() {
    if (!testFixedWindowRequested())
        return {};
    const QPoint position = testFixedWindowPosition();
    return {{QStringLiteral("flags"), QVariant::fromValue(testFixedWindowFlags())},
            {QStringLiteral("x"), position.x()},
            {QStringLiteral("y"), position.y()}};
}

void applyTestFixedWindow(QWindow& window) {
    if (!testFixedWindowRequested())
        return;
    window.setFlags(testFixedWindowFlags());
    window.setPosition(testFixedWindowPosition());
}

} // namespace mvm::app

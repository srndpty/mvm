// 試験用の固定 window (app/preview/test_window_mode.h) が、利用者の画面の向き・解像度・
// 拡大率によらず要求どおりの大きさの描画先を作ることを確かめる。
//
// 縦向きの画面 (1200x1920) では 1920x1080 の window が作れず、画面に依存する試験が落ちた。
// 利用者に画面の向きを変えてもらわずに同じ状況を作るため、今の画面より大きい window を要求する。
// 対照として、同じ大きさの枠付き window が画面に合わせて縮められることも確かめる
// (縮められないなら、この試験は枠なしにした効果を確かめられていない)。
// 拡大率は、利用者の Qt の倍率 (QT_SCALE_FACTOR=1.5) を模擬して打ち消せることを確かめる。

#include "app/preview/test_window_mode.h"
#include "test_event_wait.h"
#include "test_window_isolation.h"

#include <cmath>
#include <cstdio>
#include <memory>

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScreen>
#include <QTest>
#include <QTimer>

namespace {
int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

bool waitForFrame(QQuickWindow& window) {
    bool swapped = false;
    QObject::connect(
        &window, &QQuickWindow::frameSwapped, &window, [&] { swapped = true; },
        Qt::QueuedConnection);
    window.update();
    QElapsedTimer timer;
    timer.start();
    while (!swapped && timer.elapsed() < 10000)
        QTest::qWait(10);
    return swapped;
}
} // namespace

int main(int argc, char** argv) {
    if (!mvm::app::testFixedWindowRequested()) {
        std::fprintf(stderr, "PROTOCOL_INVALID: MVM_TEST_FIXED_WINDOW=1 で実行してください\n");
        return 2;
    }
    // 利用者が Qt の倍率を 150% にしている状態を模擬する。prepare が打ち消すはず。
    qputenv("QT_SCALE_FACTOR", "1.5");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    mvm::app::prepareTestFixedWindowEnvironment();
    QGuiApplication app(argc, argv);

    // 対照: 待ち条件の成立直後に次のイベントが状態を戻しても、成立の観測を取り消さない。
    // 旧 helper は成立後にも processEvents() して再判定するので、この検査は失敗する。
    bool ready = true;
    QTimer::singleShot(0, &app, [&] { ready = false; });
    check(mvm::test::pumpUntil([&] { return ready; }, 50),
          "待ち条件の成立を後続イベントの状態で取り消しました");
    QCoreApplication::processEvents();
    check(!ready, "対照の状態変更イベントが実行されていません");

    const QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen) {
        std::fprintf(stderr, "PROTOCOL_INVALID: 画面がありません\n");
        return 2;
    }
    const QSize screenSize = screen->geometry().size();
    const QSize requested(std::max(1920, screenSize.width() + 400),
                          std::max(1080, screenSize.height() + 300));
    std::printf("screen %dx%d、要求 %dx%d\n", screenSize.width(), screenSize.height(),
                requested.width(), requested.height());

    {
        QQuickWindow window;
        mvm::app::applyTestFixedWindow(window);
        window.resize(requested);
        window.setColor(QColor(QStringLiteral("#235789")));
        window.show();
        check(QTest::qWaitForWindowExposed(&window), "固定 window が expose されません");
        check(waitForFrame(window), "固定 window が描画されません");
        QString reason;
        const bool isolated = mvm::test::backgroundWindowIsolated(window, reason);
        check(isolated, qPrintable(reason));
        check(std::abs(window.devicePixelRatio() - 1.0) < 1e-6,
              "固定 window の devicePixelRatio が 1.0 ではありません");
        check(window.size() == requested, "固定 window の logical size が要求と違います");
        const QImage grabbed = window.grabWindow();
        check(grabbed.size() == requested, "固定 window の描画先の pixel size が要求と違います");
        check(!grabbed.isNull() && grabbed.pixelColor(10, 10) == QColor(QStringLiteral("#235789")),
              "背面の固定 window が指定した色を描画していません");
        check(window.flags().testFlag(Qt::WindowDoesNotAcceptFocus) &&
                  window.flags().testFlag(Qt::WindowTransparentForInput),
              "固定 window が focus を取るか、入力を受けます");
        std::printf("固定 window: logical %dx%d、描画先 %dx%d、dpr %.3f\n", window.width(),
                    window.height(), grabbed.width(), grabbed.height(), window.devicePixelRatio());
    }
    {
        // QML の visible: true より先に指定しないと、生成時に前面へ出てしまう。
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nWindow { width: 320; height: 180; visible: true; "
                          "Rectangle { anchors.fill: parent; color: '#1d5793' } }",
                          QUrl());
        std::unique_ptr<QObject> root(
            component.createWithInitialProperties(mvm::app::testFixedWindowInitialProperties()));
        auto* window = qobject_cast<QQuickWindow*>(root.get());
        check(window != nullptr, "背面 QML window を生成できません");
        if (window) {
            check(QTest::qWaitForWindowExposed(window) && waitForFrame(*window),
                  "背面 QML window が描画されません");
            QString reason;
            const bool isolated = mvm::test::backgroundWindowIsolated(*window, reason);
            check(isolated, qPrintable(reason));
            const QImage image = window->grabWindow();
            check(!image.isNull() && image.pixelColor(10, 10) == QColor(QStringLiteral("#1d5793")),
                  "背面 QML window が指定した色を描画していません");
        }
        // 対照は表示しない。背面指定の欠落を検査が拒否することを確かめる。
        for (const auto missing : {Qt::WindowStaysOnBottomHint, Qt::WindowDoesNotAcceptFocus,
                                   Qt::WindowTransparentForInput}) {
            QQuickWindow invalid;
            invalid.setFlags(mvm::app::testBackgroundWindowFlags() & ~missing);
            QString reason;
            check(!mvm::test::backgroundWindowIsolated(invalid, reason) && !reason.isEmpty(),
                  "背面・非フォーカス・入力透過の指定が欠けた window を検査が受理しました");
        }
    }
    {
        // 対照: 枠付きの window は画面に合わせて縮められる。
        QQuickWindow window;
        window.setFlags(mvm::app::testBackgroundWindowFlags());
        window.resize(requested);
        window.show();
        check(QTest::qWaitForWindowExposed(&window), "枠付き window が expose されません");
        check(waitForFrame(window), "枠付き window が描画されません");
        std::printf("枠付き window: logical %dx%d\n", window.width(), window.height());
        check(
            window.size() != requested,
            "枠付き window が縮められませんでした (この試験は枠なしの効果を確かめられていません)");
    }

    if (failures != 0)
        return 1;
    std::printf("PASS\n");
    return 0;
}

// 試験用の固定 window (app/preview/test_window_mode.h) が、利用者の画面の向き・解像度・
// 拡大率によらず要求どおりの大きさの描画先を作ることを確かめる。
//
// 縦向きの画面 (1200x1920) では 1920x1080 の window が作れず、画面に依存する試験が落ちた。
// 利用者に画面の向きを変えてもらわずに同じ状況を作るため、今の画面より大きい window を要求する。
// 対照として、同じ大きさの枠付き window が画面に合わせて縮められることも確かめる
// (縮められないなら、この試験は枠なしにした効果を確かめられていない)。
// 拡大率は、利用者の Qt の倍率 (QT_SCALE_FACTOR=1.5) を模擬して打ち消せることを確かめる。

#include "app/preview/test_window_mode.h"

#include <cmath>
#include <cstdio>

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScreen>
#include <QTest>

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
        window.show();
        check(QTest::qWaitForWindowExposed(&window), "固定 window が expose されません");
        check(waitForFrame(window), "固定 window が描画されません");
        check(std::abs(window.devicePixelRatio() - 1.0) < 1e-6,
              "固定 window の devicePixelRatio が 1.0 ではありません");
        check(window.size() == requested, "固定 window の logical size が要求と違います");
        const QImage grabbed = window.grabWindow();
        check(grabbed.size() == requested, "固定 window の描画先の pixel size が要求と違います");
        check(window.flags().testFlag(Qt::WindowDoesNotAcceptFocus) &&
                  window.flags().testFlag(Qt::WindowTransparentForInput),
              "固定 window が focus を取るか、入力を受けます");
        std::printf("固定 window: logical %dx%d、描画先 %dx%d、dpr %.3f\n", window.width(),
                    window.height(), grabbed.width(), grabbed.height(), window.devicePixelRatio());
    }
    {
        // 対照: 枠付きの window は画面に合わせて縮められる。
        QQuickWindow window;
        window.setFlags(Qt::Window | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
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

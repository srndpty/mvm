#ifndef MVM_TEST_WINDOW_FOCUS_H
#define MVM_TEST_WINDOW_FOCUS_H

// GUI 試験の window を、利用者の作業を止めずに操作するための補助。
//
// 試験の window を OS の前面 (foreground) にすると、利用者が他の window を少しでも触ると
// Windows が試験の window を非アクティブにし、試験は判定できなくなる (PROTOCOL_INVALID)。
// 利用者の打ったキーが試験の window へ入る危険もある。
// そこで試験の window は OS のフォーカスを受けない window として出し (前面を奪わない)、
// Qt の中でだけ「この window がフォーカスを持つ」と知らせる。入力は QTest の合成 event を
// window へ直接送るので、OS の前面がどこにあっても届く。
//
// QWindowSystemInterface は Qt の QPA (platform の内部 interface) である。
// 製品のコードからは使わず、この試験の補助だけが使う。

#include <windows.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QPoint>
#include <QString>
#include <QVariant>
#include <QWindow>
#include <qpa/qwindowsysteminterface.h>

namespace mvm::test {

// 試験の window の flags。OS のフォーカスを受けず、表示しても前面を奪わない。
// OS のマウス入力も透過させる: 画面上にある試験の window の上を利用者の実際のマウスが
// 通っても、その移動 event は背後の window へ行き、試験の合成 event (hover の検査) に
// 混ざらない (実測: 透過させないと hover の検査が 3 回に 1 回乱れた)。画面の外へ置くと
// OS が描画の対象にしない (expose されない) ので、画面の上に置いたまま透過させる。
// QML の window は読み込みと同時に表示されるので、読み込み前の初期 property で渡す。
inline QVariant backgroundWindowFlags() {
    return QVariant::fromValue(
        Qt::WindowFlags(Qt::Window | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput));
}

// Qt の中でだけ window にフォーカスを持たせる。OS の前面は変えない。
inline bool focusWithoutForeground(QWindow* window) {
    QWindowSystemInterface::handleFocusWindowChanged<QWindowSystemInterface::SynchronousDelivery>(
        window, Qt::ActiveWindowFocusReason);
    QCoreApplication::processEvents();
    return QGuiApplication::focusWindow() == window && window->isActive();
}

// 利用者の操作から切り離せているか。OS の前面になっておらず、画面上の位置で OS の
// マウス入力を受けない (その位置の実際のマウスは背後の window へ行く) ことを確かめる。
// 切り離せていなければ、試験の結果が利用者の操作に左右される。
inline bool isolatedFromUserInput(QWindow* window, QString& reason) {
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    if (GetForegroundWindow() == hwnd) {
        reason = QStringLiteral("試験の window が OS の前面になっています");
        return false;
    }
    const auto center = window->mapToGlobal(QPoint(window->width() / 2, window->height() / 2)) *
                        window->devicePixelRatio();
    const HWND hit = WindowFromPoint(POINT{center.x(), center.y()});
    if (hit == hwnd || (hit && GetAncestor(hit, GA_ROOT) == hwnd)) {
        reason = QStringLiteral("試験の window が OS のマウス入力を受けます");
        return false;
    }
    return true;
}

} // namespace mvm::test

#endif

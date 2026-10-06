#ifndef MVM_TEST_WINDOW_ISOLATION_H
#define MVM_TEST_WINDOW_ISOLATION_H

#include <windows.h>
#include <cstdio>

#include <QString>
#include <QWindow>

namespace mvm::test {

// Qt の指定だけでなく、実際の HWND も入力を受けず、利用者の前面 window より後ろにあるか。
inline bool backgroundWindowIsolated(QWindow& window, QString& reason) {
    const auto flags = window.flags();
    if (!flags.testFlag(Qt::WindowStaysOnBottomHint) ||
        !flags.testFlag(Qt::WindowDoesNotAcceptFocus) ||
        !flags.testFlag(Qt::WindowTransparentForInput)) {
        reason = QStringLiteral("試験の window に背面・非フォーカス・入力透過の指定がありません");
        return false;
    }
    const auto hwnd = reinterpret_cast<HWND>(window.winId());
    const LONG_PTR styles = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const LONG_PTR required = WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW;
    if ((styles & required) != required || (styles & WS_EX_TOPMOST)) {
        reason =
            QStringLiteral("試験の HWND が入力を受けるか、タスクバーまたは最前面に表示されます");
        return false;
    }
    const HWND foreground = GetForegroundWindow();
    if (foreground == hwnd) {
        reason = QStringLiteral("試験の window が OS の前面になっています");
        return false;
    }
    // デスクトップ自体は背面 window より下にある。比較できない場合も style は検査し、
    // Z 順の比較を省略したことを出力に残す。
    if (foreground && foreground != GetShellWindow()) {
        bool found = false;
        for (HWND above = GetWindow(hwnd, GW_HWNDPREV); above;
             above = GetWindow(above, GW_HWNDPREV)) {
            if (above == foreground) {
                found = true;
                break;
            }
        }
        if (!found) {
            reason = QStringLiteral("試験の window が利用者の前面 window より手前にあります");
            return false;
        }
    } else {
        std::printf("背面検査: 前面が未設定またはデスクトップのため Z 順比較を省略しました。"
                    "背面指定と HWND の入力隔離設定は検査済みです\n");
    }
    return true;
}

} // namespace mvm::test

#endif

#ifndef MVM_APPS_MVM_FOCUS_RELEASE_FILTER_H
#define MVM_APPS_MVM_FOCUS_RELEASE_FILTER_H

#include <QEvent>
#include <QMouseEvent>
#include <QObject>
#include <QQuickItem>
#include <QQuickWindow>

namespace mvm::app {

// 文字入力欄 (TextField / TextArea / SpinBox の入力部) に focus があるとき、
// その外側を押したら focus を window へ戻す。
//
// Space の再生や V / T の tool 切り替えは、文字入力欄が focus を持つ間は
// 止めている (Main.qml の keyboardFocusTakesKeys)。一方で Qt Quick は
// timeline や preview を押しても focus を移さないので、inspector の数値や本文を
// 一度触ると、別の場所を押しても単キー操作が効かないままになっていた。
// 押した先の item が自分で focus を取る場合 (別の入力欄) は、その処理が後から
// 走るので妨げない。event は消費しない。
class FocusReleaseFilter final : public QObject {
public:
    explicit FocusReleaseFilter(QQuickWindow* window) : QObject(window), window_(window) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched != window_ || event->type() != QEvent::MouseButtonPress)
            return QObject::eventFilter(watched, event);
        QQuickItem* focused = window_->activeFocusItem();
        if (!focused || !isTextInput(focused))
            return QObject::eventFilter(watched, event);
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const QPointF local = focused->mapFromScene(mouse->scenePosition());
        if (!focused->contains(local))
            focusTarget()->forceActiveFocus(Qt::MouseFocusReason);
        return QObject::eventFilter(watched, event);
    }

private:
    // QQuickWindow::contentItem() は root item であり、focus を与えても入力欄の
    // focus が外れない。ApplicationWindow の contentItem (QML の root.contentItem と
    // 同じもの) へ移す。
    QQuickItem* focusTarget() const {
        auto* item = window_->property("contentItem").value<QQuickItem*>();
        return item ? item : window_->contentItem();
    }

    static bool isTextInput(const QQuickItem* item) {
        return item->inherits("QQuickTextInput") || item->inherits("QQuickTextEdit");
    }

    QQuickWindow* window_;
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_FOCUS_RELEASE_FILTER_H

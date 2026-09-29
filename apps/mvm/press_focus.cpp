#include "press_focus.h"

#include <QMouseEvent>
#include <QQuickWindow>

namespace mvm::app {

PressFocus::PressFocus(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, false);
    setAcceptedMouseButtons(Qt::NoButton);
}

PressFocus::~PressFocus() {
    watch(nullptr);
}

void PressFocus::itemChange(ItemChange change, const ItemChangeData& value) {
    QQuickItem::itemChange(change, value);
    if (change == ItemSceneChange)
        watch(value.window);
}

void PressFocus::watch(QQuickWindow* window) {
    if (window_ == window)
        return;
    if (window_)
        window_->removeEventFilter(this);
    window_ = window;
    if (window_)
        window_->installEventFilter(this);
}

bool PressFocus::pressIsOnOverlay(const QPointF& scenePosition) const {
    // overlay (QQuickOverlay) は window の root item の子にある。開いている popup の範囲だけを見る
    // (overlay 自体は window 全面を覆う)。
    for (QQuickItem* child : window_->contentItem()->childItems()) {
        if (!child->inherits("QQuickOverlay"))
            continue;
        for (QQuickItem* popup : child->childItems()) {
            if (popup->isVisible() && popup->contains(popup->mapFromScene(scenePosition)))
                return true;
        }
    }
    return false;
}

bool PressFocus::eventFilter(QObject* watched, QEvent* event) {
    if (watched != window_ || event->type() != QEvent::MouseButtonPress)
        return QQuickItem::eventFilter(watched, event);
    QQuickItem* target = parentItem();
    if (!target || !isEnabled() || !target->isVisible())
        return QQuickItem::eventFilter(watched, event);
    const auto* mouse = static_cast<const QMouseEvent*>(event);
    if (target->contains(target->mapFromScene(mouse->scenePosition())) &&
        !pressIsOnOverlay(mouse->scenePosition()))
        target->forceActiveFocus(Qt::MouseFocusReason);
    return QQuickItem::eventFilter(watched, event);
}

} // namespace mvm::app

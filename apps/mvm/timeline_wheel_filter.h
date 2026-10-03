#ifndef MVM_TIMELINE_WHEEL_FILTER_H
#define MVM_TIMELINE_WHEEL_FILTER_H
#include <QQuickItem>
#include <QQuickWindow>
#include <QVariant>
#include <QWheelEvent>

class TimelineWheelEventFilter final : public QObject {
public:
    TimelineWheelEventFilter(QQuickWindow* window, QQuickItem* timelinePanel)
        : window_(window), timelinePanel_(timelinePanel) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched != window_)
            return QObject::eventFilter(watched, event);
        if (event->type() != QEvent::Wheel || !timelinePanel_)
            return QObject::eventFilter(watched, event);
        // ポップアップの上でtimelineへ入力を横取りしない。Qt Quick側で配送する。
        if (window_->property("timelineWheelBlocked").toBool())
            return false;
        const auto* wheel = static_cast<QWheelEvent*>(event);
        // modal でない popup (フォント一覧などの ComboBox・メニュー) が背面の timeline に
        // 重なって開いていることがある。popup の上のホイールは popup の一覧へ渡す。
        if (popupAt(wheel->position()))
            return false;
        const QPointF local = timelinePanel_->mapFromScene(wheel->position());
        if (!timelinePanel_->contains(local))
            return QObject::eventFilter(watched, event);

        const QPoint angleDelta = wheel->angleDelta();
        const QPoint pixelDelta = wheel->pixelDelta();
        // Windowsやmouse driverによってはAlt+縦wheelが横wheelへ変換される。
        // 縦成分だけを見るとdelta=0をzoom-outと誤認し、最小zoomから戻れなくなる。
        const int delta = angleDelta.y() != 0   ? angleDelta.y()
                          : angleDelta.x() != 0 ? angleDelta.x()
                          : pixelDelta.y() != 0 ? pixelDelta.y()
                                                : pixelDelta.x();
        const char* method = nullptr;
        QVariantList arguments;
        if (wheel->modifiers().testFlag(Qt::AltModifier)) {
            method = "handleNativeAltWheel";
            arguments = {delta, local.x()};
        } else if (wheel->modifiers().testFlag(Qt::ControlModifier)) {
            method = "handleNativeCtrlWheel";
            arguments = {delta};
        } else if (wheel->modifiers().testFlag(Qt::ShiftModifier)) {
            method = "handleNativeShiftWheel";
            arguments = {delta};
        } else {
            method = "handleNativePlainWheel";
            arguments = {delta};
        }

        if (delta == 0)
            return true;

        if (arguments.size() == 2) {
            QMetaObject::invokeMethod(timelinePanel_, method, Qt::DirectConnection,
                                      Q_ARG(QVariant, arguments[0]), Q_ARG(QVariant, arguments[1]));
        } else {
            QMetaObject::invokeMethod(timelinePanel_, method, Qt::DirectConnection,
                                      Q_ARG(QVariant, arguments[0]));
        }
        // modifier付きwheelはFlickableへ流さず、通常scrollへ化ける挙動を止める。
        return true;
    }

private:
    // Popup は window の overlay 層 (QQuickOverlay) に置かれる。overlay は contentItem の子にも
    // 兄弟にもなりうるので両方を見る。overlay の子で見えているもの (popup 本体と modal の dim) が
    // scene 座標を含むか。
    bool popupAt(QPointF scenePosition) const {
        auto* content = window_->contentItem();
        if (!content)
            return false;
        auto layers = content->childItems();
        if (content->parentItem())
            layers += content->parentItem()->childItems();
        for (auto* layer : layers) {
            if (!layer->inherits("QQuickOverlay") || !layer->isVisible())
                continue;
            for (auto* popup : layer->childItems()) {
                if (popup->isVisible() && popup->contains(popup->mapFromScene(scenePosition)))
                    return true;
            }
        }
        return false;
    }

    QQuickWindow* window_ = nullptr;
    QQuickItem* timelinePanel_ = nullptr;
};

#endif

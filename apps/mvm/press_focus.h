#ifndef MVM_APPS_MVM_PRESS_FOCUS_H
#define MVM_APPS_MVM_PRESS_FOCUS_H

#include <QPointer>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

namespace mvm::app {

// 親 item の範囲を押したら、キーボードの宛先を親へ移す。press は消費しない。
//
// 以前は親の全面を覆う最前面の MouseArea (press を受けて accepted = false で下へ流す) で
// 行っていた。しかし Qt 6 では mouse を受ける item の下へ hover とカーソルの探索が届かず、
// timeline の clip 端のカーソルや、hover で出す表示が一切効かなくなっていた。子の MouseArea が
// press を受けると親の pointer handler へは渡らないので、handler でも代われない。
// そこで window へ届く press を event filter で先に見る (覆いと同じく子より先に動く)。
//
// popup (dialog・menu) は overlay にあり、親の範囲に重なっていても親の操作ではないので、
// overlay の上の press では移さない。
class PressFocus : public QQuickItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(PressFocus)

public:
    explicit PressFocus(QQuickItem* parent = nullptr);
    ~PressFocus() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;

private:
    void watch(QQuickWindow* window);
    bool pressIsOnOverlay(const QPointF& scenePosition) const;

    QPointer<QQuickWindow> window_;
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_PRESS_FOCUS_H

#ifndef MVM_APPS_MVM_ALT_REPEAT_FILTER_H
#define MVM_APPS_MVM_ALT_REPEAT_FILTER_H

#include <QEvent>
#include <QKeyEvent>
#include <QObject>
#include <QQuickWindow>

namespace mvm::app {

// Alt を押したままの間に Windows が送る、Alt の自動反復の押下を捨てる。
//
// Qt Quick の Menu は、開いている間に Alt が押されると閉じる。自動反復も押下として届くので、
// Alt を押したまま ファイル(F) → 新規プロジェクト(N) と選ぶ途中 (約 0.5 秒後) で menu が
// 閉じてしまう。自動反復の Alt は押し直しではなく意味を持たないので、window に届く前に捨てる。
// 最初の押下と離した event は通す (menubar の Alt+英字・Alt だけの操作はそのまま)。
class AltRepeatFilter final : public QObject {
public:
    explicit AltRepeatFilter(QQuickWindow* window) : QObject(window), window_(window) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == window_ && event->type() == QEvent::KeyPress) {
            const auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Alt && key->isAutoRepeat())
                return true;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QQuickWindow* window_;
};

} // namespace mvm::app

#endif

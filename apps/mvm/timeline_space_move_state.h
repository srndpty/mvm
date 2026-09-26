#ifndef MVM_APPS_MVM_TIMELINE_SPACE_MOVE_STATE_H
#define MVM_APPS_MVM_TIMELINE_SPACE_MOVE_STATE_H

#include <QCoreApplication>
#include <QEvent>
#include <QInputMethodQueryEvent>
#include <QObject>

namespace mvm::app {

class TimelineSpaceMoveState final {
public:
    enum class Result { Pass, Consume, Activate, Deactivate };

    Result handleKey(QEvent::Type type, int key, Qt::KeyboardModifiers modifiers, bool autoRepeat,
                     bool timelineHovered, bool textInputFocused) {
        if (key != Qt::Key_Space)
            return Result::Pass;

        if (type == QEvent::KeyPress) {
            if (active_)
                return Result::Consume;
            if (autoRepeat || modifiers != Qt::NoModifier || !timelineHovered || textInputFocused)
                return Result::Pass;
            active_ = true;
            return Result::Activate;
        }

        if (type == QEvent::KeyRelease && active_) {
            if (autoRepeat)
                return Result::Consume;
            active_ = false;
            return Result::Deactivate;
        }
        return Result::Pass;
    }

    bool deactivate() {
        const bool wasActive = active_;
        active_ = false;
        return wasActive;
    }

    bool active() const { return active_; }

private:
    bool active_ = false;
};

inline bool acceptsTextInput(QObject* focusObject) {
    if (!focusObject)
        return false;
    QInputMethodQueryEvent query(Qt::ImEnabled);
    QCoreApplication::sendEvent(focusObject, &query);
    return query.value(Qt::ImEnabled).toBool();
}

} // namespace mvm::app

#endif // MVM_APPS_MVM_TIMELINE_SPACE_MOVE_STATE_H

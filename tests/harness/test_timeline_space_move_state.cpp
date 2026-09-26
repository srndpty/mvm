#include "timeline_space_move_state.h"

#include <cstdio>
#include <string>

#include <QCoreApplication>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

class TextInputProbe final : public QObject {
protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::InputMethodQuery) {
            auto* query = static_cast<QInputMethodQueryEvent*>(event);
            query->setValue(Qt::ImEnabled, true);
            return true;
        }
        return QObject::event(event);
    }
};

using Result = mvm::app::TimelineSpaceMoveState::Result;

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);

    TextInputProbe textInput;
    check(mvm::app::acceptsTextInput(&textInput),
          "テキスト入力focusをinput method queryで検出できません");

    mvm::app::TimelineSpaceMoveState textState;
    std::string enteredText;
    for (const char character : std::string("hello world")) {
        const int key = character == ' ' ? Qt::Key_Space : Qt::Key_A;
        const auto pressed =
            textState.handleKey(QEvent::KeyPress, key, Qt::NoModifier, false, true, true);
        if (pressed == Result::Pass)
            enteredText.push_back(character);
        textState.handleKey(QEvent::KeyRelease, key, Qt::NoModifier, false, true, true);
    }
    check(enteredText == "hello world" && !textState.active(),
          "テキスト入力中のSpaceをhand toolがconsumeしました");

    mvm::app::TimelineSpaceMoveState timelineState;
    check(timelineState.handleKey(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, false, true,
                                  false) == Result::Activate &&
              timelineState.active(),
          "timeline上のSpace downでhand toolが有効になりません");
    check(timelineState.handleKey(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier, false, true,
                                  false) == Result::Deactivate &&
              !timelineState.active(),
          "timeline上のSpace upでhand toolが解除されません");

    mvm::app::TimelineSpaceMoveState shortcutState;
    check(shortcutState.handleKey(QEvent::KeyPress, Qt::Key_Space, Qt::ControlModifier, false, true,
                                  false) == Result::Pass &&
              shortcutState.handleKey(QEvent::KeyRelease, Qt::Key_Space, Qt::ControlModifier, false,
                                      true, false) == Result::Pass &&
              !shortcutState.active(),
          "Ctrl+Spaceをplayback shortcutへ渡していません");

    mvm::app::TimelineSpaceMoveState outsideState;
    check(outsideState.handleKey(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, false, false,
                                 false) == Result::Pass &&
              !outsideState.active(),
          "timeline外のSpaceをconsumeしました");

    mvm::app::TimelineSpaceMoveState modifierReleaseState;
    check(modifierReleaseState.handleKey(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, false,
                                         true, false) == Result::Activate &&
              modifierReleaseState.handleKey(QEvent::KeyRelease, Qt::Key_Space, Qt::ShiftModifier,
                                             false, true, false) == Result::Deactivate &&
              !modifierReleaseState.active(),
          "Space down後のmodifier付きSpace upでhand toolが解除されません");

    if (failures != 0)
        return 1;
    std::puts("timeline Space move state: PASS");
    return 0;
}

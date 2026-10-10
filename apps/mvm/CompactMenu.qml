import QtQuick
import QtQuick.Controls

Menu {
    id: menu
    implicitWidth: Math.max(218, contentItem.implicitWidth + leftPadding + rightPadding)
    padding: 4
    // Alt を保持したメニューバー操作でも、キーイベントの送り先を項目一覧へ移す。
    onOpened: contentItem.forceActiveFocus()

    function mnemonicItem(event) {
        if (!opened || (event.modifiers & ~Qt.AltModifier) !== 0
                || event.key < Qt.Key_A || event.key > Qt.Key_Z)
            return null;
        const letter = String.fromCharCode(event.key);
        for (let i = 0; i < count; ++i) {
            const entry = itemAt(i);
            if (entry && entry.visible && entry.mnemonic === letter)
                return entry;
        }
        return null;
    }

    contentItem: BoundedListView {
        implicitHeight: contentHeight
        model: menu.contentModel
        interactive: contentHeight + menu.topPadding + menu.bottomPadding > menu.height
        clip: true
        currentIndex: menu.currentIndex
        // Shortcut 解決より先に受理し、背面 Action と menubar の同じキーを止める。
        Keys.onShortcutOverride: event => {
            if (menu.mnemonicItem(event))
                event.accepted = true;
        }
        Keys.onPressed: event => {
            const entry = menu.mnemonicItem(event);
            if (!entry)
                return;
            event.accepted = true;
            if (entry.enabled && !event.isAutoRepeat)
                entry.click();
        }
        ScrollIndicator.vertical: ScrollIndicator {}
    }

    background: Rectangle {
        color: "#292b2f"
        border.color: "#4a4d53"
        border.width: 1
        radius: 6
    }
}

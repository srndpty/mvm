import QtQuick
import QtQuick.Controls

Dialog {
    id: dialog
    modal: true
    padding: 18
    topPadding: 16
    bottomPadding: 18
    font.pixelSize: 12
    // Windowのnative filterが前面のモーダル入力を横取りしないための共通契約。
    readonly property var hostWindow: parent ? parent.Window.window : null
    property var registeredWindow: null
    function updateWheelBlock() {
        const candidate = visible && modal && hostWindow
                          && typeof hostWindow.activeModalDialogs === "number" ? hostWindow : null;
        if (candidate === registeredWindow)
            return;
        if (registeredWindow)
            registeredWindow.activeModalDialogs--;
        registeredWindow = candidate;
        if (registeredWindow)
            registeredWindow.activeModalDialogs++;
    }
    onVisibleChanged: updateWheelBlock()
    onModalChanged: updateWheelBlock()
    onHostWindowChanged: updateWheelBlock()
    Component.onDestruction: {
        if (registeredWindow)
            registeredWindow.activeModalDialogs--;
    }

    Overlay.modal: Rectangle {
        color: "#a8000000"
    }

    header: Item {
        implicitHeight: 48

        Label {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 18
            anchors.rightMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            text: dialog.title
            color: "#f0f1f3"
            font.pixelSize: 14
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: "#41454b"
        }
    }

    background: Rectangle {
        color: "#292b2f"
        border.color: "#555960"
        border.width: 1
        radius: 8
    }
}

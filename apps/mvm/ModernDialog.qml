import QtQuick
import QtQuick.Controls

Dialog {
    id: dialog
    modal: true
    padding: 18
    topPadding: 16
    bottomPadding: 18
    font.pixelSize: 12

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

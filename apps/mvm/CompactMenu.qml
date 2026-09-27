import QtQuick
import QtQuick.Controls

Menu {
    id: menu
    implicitWidth: Math.max(218, contentItem.implicitWidth + leftPadding + rightPadding)
    padding: 4

    background: Rectangle {
        color: "#292b2f"
        border.color: "#4a4d53"
        border.width: 1
        radius: 6
    }
}

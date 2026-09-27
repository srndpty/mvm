import QtQuick
import QtQuick.Controls

ItemDelegate {
    id: option
    implicitHeight: 30
    leftPadding: 10
    rightPadding: 10
    font.pixelSize: 12

    contentItem: Label {
        text: option.text
        font: option.font
        color: "#f0f1f3"
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        color: option.highlighted || option.hovered ? "#414750" : "transparent"
        radius: 4
    }
}

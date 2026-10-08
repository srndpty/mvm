import QtQuick
import QtQuick.Controls.Basic

Button {
    id: button
    property bool prominent: false
    property bool destructive: false
    implicitWidth: Math.max(88, contentItem.implicitWidth + 28)
    implicitHeight: 31
    font.pixelSize: 12

    contentItem: Label {
        text: button.text
        font: button.font
        color: button.enabled ? "#f0f1f3" : "#80848b"
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        radius: 4
        color: !button.enabled ? "#32363c"
               : button.prominent ? (button.down ? "#2e5a8b"
                                     : button.hovered ? "#477fb8" : "#376fa8")
               : button.destructive && button.hovered ? "#684143"
               : button.down ? "#343a42"
               : button.hovered ? "#4a515a" : "#3a4048"
        border.color: button.activeFocus ? "#9bc8ff"
                      : button.prominent ? "#5a8fc6" : "#565d66"
        border.width: 1
    }
}

import QtQuick
import QtQuick.Controls.Basic

CheckBox {
    id: control
    implicitHeight: 30
    spacing: 8
    font.pixelSize: 12
    indicator: Rectangle {
        x: control.leftPadding
        y: (control.height - height) / 2
        width: 18
        height: 18
        radius: 4
        color: control.checked ? "#376fa8" : "#1d2127"
        border.color: control.activeFocus ? "#9bc8ff" : "#565d66"
        Label {
            anchors.centerIn: parent
            text: "✓"
            color: "#ffffff"
            visible: control.checked
        }
    }
    contentItem: Label {
        leftPadding: control.indicator.width + control.spacing
        text: control.text
        font: control.font
        color: control.enabled ? "#c8cbd1" : "#80848b"
        verticalAlignment: Text.AlignVCenter
    }
}

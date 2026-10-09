import QtQuick
import QtQuick.Controls.Basic

TextField {
    id: field
    implicitHeight: 34
    leftPadding: 10
    rightPadding: 10
    font.pixelSize: 12
    color: field.enabled ? "#f0f1f3" : "#8c9097"
    placeholderTextColor: "#858a92"
    selectionColor: "#376fa8"
    selectedTextColor: "#ffffff"

    background: Rectangle {
        color: field.readOnly ? "#24282e" : "#1d2127"
        border.color: field.activeFocus ? "#6ca9e6" : "#51565e"
        border.width: 1
        radius: 4
    }
}

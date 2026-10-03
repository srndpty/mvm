import QtQuick
import QtQuick.Controls

TextArea {
    id: field
    padding: 10
    font.pixelSize: 12
    color: enabled ? "#f0f1f3" : "#8c9097"
    placeholderTextColor: "#858a92"
    selectionColor: "#376fa8"
    selectedTextColor: "#ffffff"
    wrapMode: TextEdit.Wrap
    background: Rectangle {
        color: "#1d2127"
        border.color: field.activeFocus ? "#6ca9e6" : "#51565e"
        radius: 4
    }
}

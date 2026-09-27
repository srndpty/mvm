import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

MenuItem {
    id: item
    readonly property string shortcutLabel: {
        const parts = text.split("\t");
        if (parts.length > 1)
            return parts[1];
        return action && action.shortcut ? String(action.shortcut) : "";
    }

    implicitWidth: Math.max(206, contentItem.implicitWidth + leftPadding + rightPadding)
    implicitHeight: 27
    leftPadding: 12
    rightPadding: 12
    font.pixelSize: 12

    contentItem: RowLayout {
        spacing: 20
        Label {
            Layout.fillWidth: true
            text: item.text.split("\t")[0]
            color: item.enabled ? "#f0f1f3" : "#85888f"
            font: item.font
            elide: Text.ElideRight
        }
        Label {
            visible: item.shortcutLabel.length > 0
            text: item.shortcutLabel
            color: item.enabled ? "#b8bbc2" : "#74777e"
            font: item.font
        }
    }

    background: Rectangle {
        color: item.highlighted && item.enabled ? "#414750" : "transparent"
        radius: 4
    }
}

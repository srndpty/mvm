pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

ComboBox {
    id: combo
    implicitHeight: 34
    font.pixelSize: 12

    delegate: ModernDialogOption {
        required property int index
        width: combo.width - 8
        text: combo.textAt(index)
        highlighted: combo.highlightedIndex === index
    }
    contentItem: Label {
        leftPadding: 10
        rightPadding: 30
        text: combo.displayText
        color: "#f0f1f3"
        font: combo.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    indicator: Label {
        x: combo.width - width - 12
        y: (combo.height - height) / 2
        text: "⌄"
        color: "#c8cbd1"
        font.pixelSize: 17
    }
    background: Rectangle {
        color: combo.pressed ? "#353b43" : "#1d2127"
        border.color: combo.activeFocus ? "#6ca9e6" : "#51565e"
        border.width: 1
        radius: 4
    }
    popup: Popup {
        y: combo.height + 4
        width: combo.width
        implicitHeight: Math.min(options.contentHeight + 8, 240)
        padding: 4
        contentItem: ListView {
            id: options
            clip: true
            implicitHeight: contentHeight
            model: combo.popup.visible ? combo.delegateModel : null
            currentIndex: combo.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator { }
        }
        background: Rectangle {
            color: "#292b2f"
            border.color: "#555960"
            border.width: 1
            radius: 6
        }
    }
}

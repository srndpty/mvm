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
    indicator: ChevronIcon {
        x: combo.width - width - 11
        y: (combo.height - height) / 2
        direction: "updown"
        width: 8
        height: 12
        color: combo.enabled ? "#c8cbd1" : "#6b7079"
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
        contentItem: BoundedListView {
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

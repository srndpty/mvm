pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    required property MvmController mvmController
    property var clipData: root.mvmController.selectedTextClip
    readonly property string clipId: root.clipData.clipId || ""
    spacing: 5

    function setValue(key, value) {
        if (root.clipId.length > 0)
            root.mvmController.updateTextClip(root.clipId, {[key]: value});
    }

    Label { text: "文字"; color: "#e6e8ec" }
    TextArea {
        Layout.fillWidth: true
        Layout.preferredHeight: 70
        text: root.clipData.content || ""
        wrapMode: TextEdit.NoWrap
        onActiveFocusChanged: {
            if (!activeFocus && text !== root.clipData.content)
                root.setValue("content", text);
        }
    }
    RowLayout {
        Layout.fillWidth: true
        Label { text: "フォント"; color: "#aeb6c2" }
        TextField {
            Layout.fillWidth: true
            text: root.clipData.fontFamily || ""
            onEditingFinished: root.setValue("fontFamily", text)
        }
        SpinBox {
            from: 1
            to: root.mvmController.outputHeight
            value: root.clipData.fontSize || 64
            onValueModified: root.setValue("fontSize", value)
        }
    }
    RowLayout {
        CheckBox {
            text: "太字"
            checked: !!root.clipData.bold
            onToggled: root.setValue("bold", checked)
        }
        ComboBox {
            model: [{label: "左", value: "left"},
                    {label: "中央", value: "center"},
                    {label: "右", value: "right"}]
            textRole: "label"
            valueRole: "value"
            currentIndex: root.clipData.alignment === "right" ? 2
                          : root.clipData.alignment === "center" ? 1 : 0
            onActivated: root.setValue("alignment", currentValue)
        }
    }
    RowLayout {
        Label { text: "X"; color: "#aeb6c2" }
        SpinBox {
            from: 0; to: root.mvmController.outputWidth - 1
            value: root.clipData.x || 0
            onValueModified: root.setValue("x", value)
        }
        Label { text: "Y"; color: "#aeb6c2" }
        SpinBox {
            from: 0; to: root.mvmController.outputHeight - 1
            value: root.clipData.y || 0
            onValueModified: root.setValue("y", value)
        }
    }
    GridLayout {
        Layout.fillWidth: true
        columns: 2
        Label { text: "文字色 #AARRGGBB"; color: "#aeb6c2" }
        TextField {
            Layout.fillWidth: true
            text: root.clipData.color || "#FFFFFFFF"
            onEditingFinished: root.setValue("color", text)
        }
        Label { text: "縁色"; color: "#aeb6c2" }
        TextField {
            Layout.fillWidth: true
            text: root.clipData.outlineColor || "#FF000000"
            onEditingFinished: root.setValue("outlineColor", text)
        }
        Label { text: "縁幅"; color: "#aeb6c2" }
        SpinBox {
            from: 0; to: 32
            value: root.clipData.outlineWidth || 0
            onValueModified: root.setValue("outlineWidth", value)
        }
        Label { text: "背景色"; color: "#aeb6c2" }
        TextField {
            Layout.fillWidth: true
            text: root.clipData.backgroundColor || "#00000000"
            onEditingFinished: root.setValue("backgroundColor", text)
        }
    }
}

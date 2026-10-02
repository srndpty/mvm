pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// 数字のシングルクリックで入力。無音は -inf / -∞ のどちらでも指定できる。
Item {
    id: root
    property real value: 0
    property real minimumValue: -96
    property real maximumValue: 15
    property bool silenceEnabled: true
    property string suffix: ""
    signal valueEdited(real value)
    implicitWidth: 48
    implicitHeight: 24
    function formatted() {
        return root.silenceEnabled && root.value <= root.minimumValue
                ? "−∞" : root.value.toFixed(1);
    }
    function beginEditing() {
        editor.text = root.silenceEnabled && root.value <= root.minimumValue ? "-inf" : root.value.toFixed(1);
        editor.visible = true;
        editor.forceActiveFocus();
        editor.selectAll();
    }
    function finishEditing() {
        if (!editor.visible) return;
        const body = editor.text.trim().toLowerCase();
        const silent = root.silenceEnabled && ["-inf", "-∞", "−∞"].indexOf(body) >= 0;
        const candidate = silent ? root.minimumValue : Number(body);
        if (body === "" || !isFinite(candidate) || candidate < root.minimumValue || candidate > root.maximumValue) {
            editor.forceActiveFocus();
            editor.selectAll();
            return;
        }
        editor.visible = false;
        root.valueEdited(candidate);
    }
    Label {
        anchors.fill: parent
        text: root.formatted() + root.suffix
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        font.pixelSize: 11
        color: numberMouse.containsMouse ? "#a9d9ff" : "#65b8f0"
        visible: !editor.visible
        MouseArea {
            id: numberMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.IBeamCursor
            onClicked: root.beginEditing()
        }
    }
    TextField {
        id: editor
        objectName: "audioValueEditor"
        anchors.fill: parent
        visible: false
        padding: 2
        font.pixelSize: 11
        horizontalAlignment: Text.AlignHCenter
        selectByMouse: true
        onAccepted: root.finishEditing()
        onEditingFinished: root.finishEditing()
        Keys.onEscapePressed: event => { visible = false; event.accepted = true; }
        ToolTip.visible: activeFocus
        ToolTip.text: root.minimumValue + " ～ " + root.maximumValue + (root.silenceEnabled ? "、無音: -inf" : "")
    }
}

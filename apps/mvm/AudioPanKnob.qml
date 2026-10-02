pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

Column {
    id: root
    property real value: 0
    signal valueEdited(real value, bool commit)
    signal editCanceled()
    width: 78
    spacing: 0
    Item {
        width: parent.width
        height: 62
        Rectangle {
            anchors.centerIn: parent
            width: 42; height: 42; radius: 21
            color: knobMouse.containsMouse ? "#363b43" : "#292d34"
            border.color: "#8a929d"; border.width: 2
            Item {
                anchors.fill: parent
                rotation: root.value * 135
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: 5; width: 2; height: 15; color: "#cdd4dd"
                }
            }
        }
        Label { x: 2; anchors.bottom: parent.bottom; text: "L"; color: "#65b8f0"; font.pixelSize: 10 }
        Label { anchors.right: parent.right; anchors.rightMargin: 2; anchors.bottom: parent.bottom; text: "R"; color: "#65b8f0"; font.pixelSize: 10 }
        MouseArea {
            id: knobMouse
            objectName: "audioPanDrag"
            anchors.fill: parent
            hoverEnabled: true
            preventStealing: true
            cursorShape: Qt.SizeAllCursor
            property real lastX: 0
            property real lastY: 0
            property real dragValue: 0
            onPressed: mouse => { lastX = mouse.x; lastY = mouse.y; dragValue = root.value; }
            onPositionChanged: mouse => {
                if (!pressed) return;
                const scale = mouse.modifiers & Qt.ControlModifier ? 0.001 : 0.01;
                dragValue = Math.max(-1, Math.min(1, dragValue + ((mouse.x - lastX) - (mouse.y - lastY)) * scale));
                lastX = mouse.x; lastY = mouse.y;
                root.valueEdited(dragValue, false);
            }
            onReleased: root.valueEdited(dragValue, true)
            onDoubleClicked: { dragValue = 0; root.valueEdited(0, true); }
            onCanceled: root.editCanceled()
            ToolTip.visible: containsMouse && !pressed
            ToolTip.text: "上下・左右ドラッグでパン、Ctrlで微調整、ダブルクリックで中央"
        }
    }
    AudioValueField {
        objectName: "audioPanValue"
        width: parent.width
        value: root.value * 100
        minimumValue: -100; maximumValue: 100; silenceEnabled: false
        onValueEdited: value => root.valueEdited(value / 100, true)
    }
}

pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

Item {
    id: root
    property real value: 0
    signal valueEdited(real value, bool commit)
    signal editCanceled()
    implicitWidth: 43
    implicitHeight: 190
    readonly property var dbPoints: [15, 6, 0, -6, -12, -24, -48, -96]
    readonly property var positions: [0, 0.14, 0.25, 0.38, 0.5, 0.65, 0.82, 1]
    function positionFor(db) {
        const value = Math.max(-96, Math.min(15, db));
        for (let i = 1; i < dbPoints.length; ++i)
            if (value >= dbPoints[i])
                return positions[i-1] + (dbPoints[i-1] - value) / (dbPoints[i-1] - dbPoints[i]) * (positions[i] - positions[i-1]);
        return 1;
    }
    function dbFor(position) {
        const p = Math.max(0, Math.min(1, position));
        for (let i = 1; i < positions.length; ++i)
            if (p <= positions[i])
                return dbPoints[i-1] + (p - positions[i-1]) / (positions[i] - positions[i-1]) * (dbPoints[i] - dbPoints[i-1]);
        return -96;
    }
    Item {
        id: rail
        x: 25; y: 10; width: 15; height: Math.max(1, parent.height - 20)
        Rectangle { x: 6; width: 3; height: parent.height; color: "#0d1014"; border.color: "#454b55" }
        Repeater {
            model: [15, 6, 0, -6, -12, -24, -48, -96]
            Item {
                required property real modelData
                y: root.positionFor(modelData) * rail.height
                Rectangle { x: -6; width: 5; height: 1; color: "#6d7580" }
                Label { x: -26; y: -6; width: 19; horizontalAlignment: Text.AlignRight; text: modelData === -96 ? "−∞" : modelData; color: "#8a929d"; font.pixelSize: 8 }
            }
        }
        Rectangle {
            y: root.positionFor(root.value) * rail.height - height / 2
            width: 15; height: 25; radius: 2
            color: faderMouse.containsMouse ? "#747d89" : "#505761"
            border.color: "#abb3bd"
            Rectangle { anchors.centerIn: parent; width: 9; height: 2; color: "#e5e9ee" }
        }
    }
    MouseArea {
        id: faderMouse
        objectName: "audioFaderDrag"
        anchors.fill: parent
        hoverEnabled: true
        preventStealing: true
        cursorShape: Qt.SizeVerCursor
        property real lastY: 0
        property real dragPosition: 0
        onPressed: mouse => { lastY = mouse.y; dragPosition = root.positionFor(root.value); }
        onPositionChanged: mouse => {
            if (!pressed) return;
            const scale = mouse.modifiers & Qt.ControlModifier ? 0.1 : 1;
            dragPosition = Math.max(0, Math.min(1, dragPosition + (mouse.y - lastY) / rail.height * scale));
            lastY = mouse.y;
            root.valueEdited(root.dbFor(dragPosition), false);
        }
        onReleased: root.valueEdited(root.dbFor(dragPosition), true)
        onDoubleClicked: { dragPosition = root.positionFor(0); root.valueEdited(0, true); }
        onCanceled: root.editCanceled()
        ToolTip.visible: containsMouse && !pressed
        ToolTip.text: "上下ドラッグで音量、Ctrlで微調整、ダブルクリックで0 dB"
    }
}

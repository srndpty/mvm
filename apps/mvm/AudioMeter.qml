pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// 黄色線は直近1秒の最大ピーク。クリップ表示はクリックで解除する。
Item {
    id: root
    property real dbLeft: -96
    property real dbRight: -96
    property bool clipped: false
    property real shownLeft: -96
    property real shownRight: -96
    property real heldLeft: -96
    property real heldRight: -96
    property var history: []
    property bool showScale: true
    signal clipCleared()
    implicitWidth: showScale ? 57 : 29
    implicitHeight: 190
    function fillRatio(db) { return Math.max(0, Math.min(1, (db + 60) / 60)); }
    function dbText(db) { return db <= -96 ? "−∞" : db.toFixed(1); }
    function sample(now) {
        shownLeft = Math.max(root.dbLeft, shownLeft - 1.8);
        shownRight = Math.max(root.dbRight, shownRight - 1.8);
        const recent = history.filter(entry => now - entry.time < 1000);
        recent.push({time: now, left: root.dbLeft, right: root.dbRight});
        history = recent;
        heldLeft = Math.max(...recent.map(entry => entry.left));
        heldRight = Math.max(...recent.map(entry => entry.right));
    }
    Timer { interval: 50; running: root.visible; repeat: true; onTriggered: root.sample(Date.now()) }
    Rectangle {
        id: clipLamp
        objectName: "audioClipLamp"
        x: 1; width: 27; height: 6; radius: 1
        color: root.clipped ? "#ff4545" : "#48292d"
        MouseArea {
            anchors.fill: parent; anchors.margins: -3
            cursorShape: Qt.PointingHandCursor
            onClicked: root.clipCleared()
            ToolTip.visible: containsMouse
            ToolTip.text: "0 dB超過。クリックでクリップ表示を解除"
            hoverEnabled: true
        }
    }
    Item {
        id: bars
        y: 10; width: parent.width; height: Math.max(1, parent.height - 26)
        Row {
            spacing: 3
            Repeater {
                model: 2
                Rectangle {
                    id: channel
                    required property int index
                    readonly property real level: index === 0 ? root.shownLeft : root.shownRight
                    readonly property real held: index === 0 ? root.heldLeft : root.heldRight
                    width: 12; height: bars.height
                    color: "#0c1115"; border.color: "#454b55"
                    Item {
                        x: 1; y: 1; width: parent.width - 2; height: parent.height - 2
                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: parent.height * root.fillRatio(channel.level)
                            gradient: Gradient {
                                GradientStop { position: 0; color: channel.level > -6 ? "#e5c947" : "#5fd65e" }
                                GradientStop { position: 1; color: "#36a855" }
                            }
                        }
                        Rectangle { width: parent.width; height: 2; y: Math.min(parent.height - 2, (1 - root.fillRatio(channel.held)) * parent.height); color: "#f3cf43" }
                    }
                }
            }
        }
        Repeater {
            model: [0, -6, -12, -24, -36, -48, -60]
            Item {
                required property real modelData
                visible: root.showScale
                x: 29; y: (1 - root.fillRatio(modelData)) * bars.height
                Rectangle { width: 4; height: 1; color: "#6d7580" }
                Label { x: 6; y: -6; text: modelData; color: "#8a929d"; font.pixelSize: 8 }
            }
        }
    }
    Label {
        anchors.bottom: parent.bottom
        width: 28; horizontalAlignment: Text.AlignHCenter
        text: "L   R"; color: "#8a929d"; font.pixelSize: 9
    }
}

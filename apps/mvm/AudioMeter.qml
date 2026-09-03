import QtQuick
import QtQuick.Controls

// L/R の peak メーター。表示は dBFS のみで、目盛りも -60..0 に固定する。
// 値の減衰は WASAPI sink 側で行っており、ここでは表示だけを担う。
Item {
    id: root

    property real dbLeft: -60
    property real dbRight: -60
    readonly property real minimumDb: -60

    implicitWidth: 92
    implicitHeight: 120

    function fillRatio(db) {
        const clamped = Math.max(root.minimumDb, Math.min(0, db));
        return (clamped - root.minimumDb) / (0 - root.minimumDb);
    }

    function dbText(db) {
        return db <= root.minimumDb ? "-inf" : db.toFixed(1);
    }

    Label {
        id: title
        text: "Audio"
        color: "#9aa2ad"
        font.pixelSize: 10
    }

    Row {
        id: bars
        y: title.height + 4
        height: parent.height - title.height - 22
        spacing: 8

        Repeater {
            model: [{ "name": "L", "db": root.dbLeft }, { "name": "R", "db": root.dbRight }]

            Column {
                required property var modelData
                spacing: 3

                Rectangle {
                    width: 18
                    height: bars.height - 16
                    radius: 2
                    color: "#15181d"
                    border.color: "#3c424c"

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.margins: 2
                        height: Math.max(0, (parent.height - 4) * root.fillRatio(modelData.db))
                        radius: 1
                        // -6dB を超えたら黄、0dB 近傍は赤。判断を色で出す。
                        color: modelData.db >= -1 ? "#e05a5a"
                             : modelData.db >= -6 ? "#e0c05a"
                                                  : "#5ac07a"
                    }
                }
                Label {
                    width: 18
                    horizontalAlignment: Text.AlignHCenter
                    text: modelData.name
                    color: "#9aa2ad"
                    font.pixelSize: 9
                }
            }
        }
    }

    Label {
        anchors.bottom: parent.bottom
        text: root.dbText(root.dbLeft) + " / " + root.dbText(root.dbRight) + " dB"
        color: "#c9ccd2"
        font.pixelSize: 10
    }
}

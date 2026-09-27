import QtQuick

// プロジェクトパネルの種別アイコン。画像アセットを持たず、矩形の組み合わせで描く。
Item {
    id: root

    // "folder" / "video" / "audio" / "image"
    property string kind: "video"

    implicitWidth: 16
    implicitHeight: 14

    // --- folder: 見出しタブ付きの箱 ---
    Item {
        anchors.fill: parent
        visible: root.kind === "folder"
        Rectangle {
            x: 1
            y: 1
            width: 6
            height: 3
            radius: 1
            color: "#c9a55c"
        }
        Rectangle {
            x: 1
            y: 3
            width: 14
            height: 10
            radius: 1.5
            color: "#c9a55c"
        }
    }

    // --- video: フィルムの穴が並んだ帯 ---
    Rectangle {
        anchors.fill: parent
        anchors.margins: 1
        visible: root.kind === "video"
        radius: 1.5
        color: "#5b8fd1"
        Column {
            x: 1.5
            anchors.verticalCenter: parent.verticalCenter
            spacing: 1.5
            Repeater {
                model: 3
                Rectangle { width: 2; height: 2; color: "#1b1f25" }
            }
        }
        Column {
            x: parent.width - 3.5
            anchors.verticalCenter: parent.verticalCenter
            spacing: 1.5
            Repeater {
                model: 3
                Rectangle { width: 2; height: 2; color: "#1b1f25" }
            }
        }
    }

    // --- audio: 波形の縦線 ---
    Rectangle {
        anchors.fill: parent
        anchors.margins: 1
        visible: root.kind === "audio"
        radius: 1.5
        color: "#4fae7d"
        Row {
            anchors.centerIn: parent
            height: 9
            spacing: 1
            Repeater {
                model: [4, 8, 5, 9, 3]
                // Repeater が作った直後の delegate は parent が null なので、parent へ
                // anchor せず Row の固定の高さに対して中央へ置く。
                Rectangle {
                    required property int modelData
                    y: (9 - modelData) / 2
                    width: 1.5
                    height: modelData
                    color: "#1b1f25"
                }
            }
        }
    }

    // --- image: 山と太陽 ---
    Rectangle {
        anchors.fill: parent
        anchors.margins: 1
        visible: root.kind === "image"
        radius: 1.5
        color: "#b67ac9"
        clip: true
        Rectangle {
            x: 8
            y: 2
            width: 3
            height: 3
            radius: 1.5
            color: "#1b1f25"
        }
        Rectangle {
            x: 1
            y: 7
            width: 8
            height: 8
            rotation: 45
            color: "#1b1f25"
        }
    }
}

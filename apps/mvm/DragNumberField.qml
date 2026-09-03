import QtQuick
import QtQuick.Controls

// 数値を左右ドラッグで変更できるフィールド。ダブルクリックで直接入力へ切り替わる。
// ドラッグ中は commit=false で通知し、離した時だけ commit=true にする。
// 保存のたびに Project を書き直さないための区別であり、表示上の都合ではない。
Item {
    id: root

    property string labelText
    property real value: 0
    property real minimumValue: -100000
    property real maximumValue: 100000
    // 1 px ドラッグあたりの変化量。
    property real stepPerPixel: 0.5
    property int decimals: 0
    property string suffix: ""
    // enabled は Item から継承したものをそのまま使う。同名 property を足すと
    // 親 (GridLayout) の enabled が伝わらず、無効化しても drag できてしまう。

    signal valueEdited(real newValue, bool commit)
    // grab を奪われた等で release が来なかった場合。編集を確定させない。
    signal editCanceled()

    implicitWidth: 116
    implicitHeight: 38

    function clampValue(candidate) {
        return Math.max(root.minimumValue, Math.min(root.maximumValue, candidate));
    }

    function formatted(candidate) {
        return candidate.toFixed(root.decimals) + root.suffix;
    }

    Label {
        id: caption
        x: 2
        width: parent.width - 4
        text: root.labelText
        color: "#9aa2ad"
        font.pixelSize: 10
        elide: Text.ElideRight
    }

    Rectangle {
        id: box
        y: caption.height + 2
        width: parent.width
        height: parent.height - caption.height - 2
        radius: 3
        color: root.enabled ? (dragArea.pressed ? "#2f3945" : "#232830") : "#1c2026"
        border.color: dragArea.containsMouse || editor.visible ? "#5b9bd5" : "#3c424c"

        Label {
            anchors.fill: parent
            anchors.leftMargin: 8
            visible: !editor.visible
            verticalAlignment: Text.AlignVCenter
            text: root.formatted(root.value)
            color: root.enabled ? "#e6e8ec" : "#6a707a"
            font.pixelSize: 13
        }

        TextField {
            id: editor
            anchors.fill: parent
            visible: false
            selectByMouse: true
            font.pixelSize: 13
            onAccepted: {
                const parsed = parseFloat(text);
                if (!isNaN(parsed))
                    root.valueEdited(root.clampValue(parsed), true);
                editor.visible = false;
            }
            onActiveFocusChanged: {
                if (!activeFocus)
                    editor.visible = false;
            }
        }

        MouseArea {
            id: dragArea
            anchors.fill: parent
            enabled: root.enabled && !editor.visible
            hoverEnabled: true
            cursorShape: Qt.SizeHorCursor
            preventStealing: true

            property real pressX: 0
            property real pressValue: 0
            property bool dragged: false

            onPressed: mouse => {
                pressX = mouse.x;
                pressValue = root.value;
                dragged = false;
            }
            onPositionChanged: mouse => {
                if (!pressed)
                    return;
                const delta = mouse.x - pressX;
                if (!dragged && Math.abs(delta) < 3)
                    return;
                dragged = true;
                root.valueEdited(root.clampValue(pressValue + delta * root.stepPerPixel), false);
            }
            onReleased: {
                if (dragged)
                    root.valueEdited(root.value, true);
                dragged = false;
            }
            onCanceled: {
                if (dragged)
                    root.editCanceled();
                dragged = false;
            }
            onDoubleClicked: {
                editor.text = root.value.toFixed(root.decimals);
                editor.visible = true;
                editor.forceActiveFocus();
                editor.selectAll();
            }
        }
    }
}

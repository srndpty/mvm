pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 選択中の文字 clip の書式。値の変更は 1 回ごとに updateTextClip で確定する
// (Undo 1 回分)。数値のドラッグ中は表示だけを追従させ、離したときに確定する。
// フォント・サイズ・色などの書式は字幕の共通書式と同じ TextStyleEditor を使う。
ColumnLayout {
    id: root
    required property MvmController mvmController
    property var clipData: root.mvmController.selectedTextClip
    readonly property string clipId: root.clipData.clipId || ""
    spacing: 4

    function setValue(key, value) {
        if (root.clipId.length > 0 && root.clipData[key] !== value)
            root.mvmController.updateTextClip(root.clipId, {[key]: value});
    }

    function preview(values) {
        if (root.clipId.length > 0)
            root.mvmController.previewTextClip(root.clipId, values);
    }

    // 入力欄を抜けたら window へ focus を戻す。残すと Space や V などの単キー操作が止まる。
    function releaseFocus() {
        if (root.Window.window)
            root.Window.window.contentItem.forceActiveFocus();
    }

    // テロップの定位置へ置くボタン。押すたびに下から 15% の高さの左・中央・右へ置き直し、
    // 行揃えも同じ向きにする (textPresetPlacement)。置いた後は X / Y やドラッグで微調整できる。
    // 状態を持つ排他ボタンではないので、押下中だけ強調する。
    component PlaceButton: AbstractButton {
        id: placeButton
        required property string alignment
        required property string tip
        objectName: "textPlaceButton_" + placeButton.alignment
        implicitWidth: 30
        implicitHeight: 24
        focusPolicy: Qt.NoFocus
        onClicked: {
            if (root.clipId.length > 0)
                root.mvmController.placeTextClip(root.clipId, placeButton.alignment);
        }
        ToolTip.visible: hovered
        ToolTip.delay: 400
        ToolTip.text: placeButton.tip
        background: Rectangle {
            radius: 3
            color: placeButton.down ? "#2f4a66" : (placeButton.hovered ? "#2a2f37" : "transparent")
            border.color: placeButton.hovered ? "#5b9bd5" : "#3c424c"
        }
        // 画面の枠と、下寄りに置かれるテロップの帯。
        contentItem: Item {
            Rectangle {
                x: 6
                y: 5
                width: 18
                height: 14
                radius: 1
                color: "transparent"
                border.color: "#8b939e"
            }
            Rectangle {
                width: 8
                height: 2
                y: 14
                x: placeButton.alignment === "left" ? 8
                   : placeButton.alignment === "right" ? 14 : 11
                color: placeButton.hovered ? "#8cc4ff" : "#e6e8ec"
            }
        }
    }

    // 位置の数値。書式と同じく、ドラッグ中は preview だけを描き直し、離したときに保存する。
    component PositionField: StyleNumberField {
        styleData: root.clipData
        Layout.fillWidth: true
        stepPerPixel: 1
        onPreviewed: (key, value) => root.preview({[key]: value})
        onCommitted: (key, value) => root.setValue(key, value)
        onCanceled: root.mvmController.cancelTextPreview()
    }

    // 本文。枠で入力範囲を示す。Ctrl+Enter で確定、Esc で取り消し。
    TextArea {
        id: contentEditor
        objectName: "textContentEditor"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.max(44, contentHeight + topPadding + bottomPadding)
        Layout.maximumHeight: 96
        text: root.clipData.content || ""
        wrapMode: TextEdit.NoWrap
        font.pixelSize: 13
        color: "#e6e8ec"
        padding: 6
        placeholderText: "文字を入力"
        background: Rectangle {
            radius: 3
            color: "#1c2026"
            border.color: contentEditor.activeFocus ? "#5b9bd5" : "#4a515c"
        }
        Keys.onPressed: event => {
            if ((event.modifiers & Qt.ControlModifier)
                    && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
                root.releaseFocus();
                event.accepted = true;
            } else if (event.key === Qt.Key_Escape) {
                contentEditor.text = root.clipData.content || "";
                root.releaseFocus();
                event.accepted = true;
            }
        }
        onActiveFocusChanged: {
            if (!activeFocus && text !== root.clipData.content && text.length > 0)
                root.setValue("content", text);
        }
    }

    TextStyleEditor {
        Layout.fillWidth: true
        styleData: root.clipData
        namePrefix: "text"
        maximumFontSize: root.mvmController.outputHeight
        onValueCommitted: (key, value) => root.setValue(key, value)
        onValuesPreviewed: values => root.preview(values)
        onPreviewCanceled: root.mvmController.cancelTextPreview()
        boldRowItems: [
            PlaceButton {
                alignment: "left"
                tip: "テロップ位置 (左下) へ置く"
            },
            PlaceButton {
                alignment: "center"
                tip: "テロップ位置 (中央下) へ置く"
            },
            PlaceButton {
                alignment: "right"
                tip: "テロップ位置 (右下) へ置く"
            }
        ]
        middleItems: [
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                PositionField {
                    key: "x"
                    labelText: "X"
                    minimumValue: 0
                    maximumValue: root.mvmController.outputWidth - 1
                }
                PositionField {
                    key: "y"
                    labelText: "Y"
                    minimumValue: 0
                    maximumValue: root.mvmController.outputHeight - 1
                }
            }
        ]
    }
}

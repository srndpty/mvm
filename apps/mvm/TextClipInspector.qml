pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 選択中の文字 clip の書式。値の変更は 1 回ごとに updateTextClip で確定する
// (Undo 1 回分)。数値のドラッグ中は表示だけを追従させ、離したときに確定する。
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

    // 確定前の値で preview だけを描き直す (Project は変えない)。色のドラッグやフォントの
    // hover は短い間に何度も来る。最初の値はすぐ描き、その後は動かしている間も一定間隔で
    // 最新の値を描く (throttle)。止まるのを待つ debounce にすると、ドラッグ中に色が追従しない。
    function schedulePreview(key, value) {
        if (root.clipId.length === 0)
            return;
        previewTimer.values = {[key]: value};
        if (previewTimer.running) {
            previewTimer.pending = true;
            return;
        }
        root.sendPreview();
        previewTimer.start();
    }

    function sendPreview() {
        previewTimer.pending = false;
        if (root.clipId.length > 0)
            root.mvmController.previewTextClip(root.clipId, previewTimer.values);
    }

    // preview 中の値を確定する。元の値と同じなら保存せず preview を取り消す。
    function commitPreview(key, value) {
        previewTimer.stop();
        previewTimer.pending = false;
        if (root.clipData[key] === value)
            root.mvmController.cancelTextPreview();
        else
            root.setValue(key, value);
    }

    function cancelPreview() {
        previewTimer.stop();
        previewTimer.pending = false;
        root.mvmController.cancelTextPreview();
    }

    // 描き直しの最短間隔。1080p の文字画像の描画と preview の seek を毎回行うので、
    // マウスの move ごとには描かない。間隔中に来た最新の値は間隔の終わりに描く。
    Timer {
        id: previewTimer
        property var values: ({})
        property bool pending: false
        interval: 200
        onTriggered: {
            if (!previewTimer.pending)
                return;
            root.sendPreview();
            previewTimer.start();
        }
    }

    // 入力欄を抜けたら window へ focus を戻す。残すと Space や V などの単キー操作が止まる。
    function releaseFocus() {
        if (root.Window.window)
            root.Window.window.contentItem.forceActiveFocus();
    }

    component SectionLabel: Label {
        color: "#9aa2ad"
        font.pixelSize: 11
        Layout.preferredWidth: 44
    }

    // 数値。ドラッグ中は pending を表示しつつ preview だけを描き直し (Project は変えない)、
    // 離したとき (commit) に保存する。値が元に戻っていれば preview を取り消すだけにする。
    component TextNumberField: DragNumberField {
        id: numberField
        required property string key
        objectName: "textNumberField_" + numberField.key
        property var pending: undefined
        inlineLabelWidth: 20
        value: numberField.pending !== undefined ? numberField.pending
                                                 : (root.clipData[numberField.key] || 0)
        onValueEdited: (newValue, commit) => {
            const rounded = Math.round(newValue);
            if (!commit) {
                numberField.pending = rounded;
                if (root.clipId.length > 0)
                    root.mvmController.previewTextClip(root.clipId, {[numberField.key]: rounded});
                return;
            }
            numberField.pending = undefined;
            if (root.clipData[numberField.key] === rounded)
                root.mvmController.cancelTextPreview();
            else
                root.setValue(numberField.key, rounded);
        }
        onEditCanceled: {
            numberField.pending = undefined;
            root.mvmController.cancelTextPreview();
        }
    }

    // 色の見本。押すと color picker を開く。
    component ColorSwatch: Rectangle {
        id: swatch
        required property string key
        required property string title
        objectName: "textColorSwatch_" + swatch.key
        implicitWidth: 34
        implicitHeight: 20
        radius: 2
        color: "#2a2f37"
        border.color: swatchArea.containsMouse ? "#5b9bd5" : "#5a616c"
        Rectangle {
            anchors.fill: parent
            anchors.margins: 2
            color: root.clipData[swatch.key] || "transparent"
        }
        // 完全に透明な色は見本が背景と区別できないので斜線で示す。
        Rectangle {
            anchors.centerIn: parent
            width: parent.width - 6
            height: 1
            rotation: -30
            color: "#d05050"
            // 保存形式は "#AARRGGBB" なので、先頭 2 桁が alpha。
            visible: String(root.clipData[swatch.key] || "#00").substring(1, 3) === "00"
        }
        MouseArea {
            id: swatchArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                colorPicker.targetKey = swatch.key;
                colorPicker.title = swatch.title;
                colorPicker.initialColor = root.clipData[swatch.key] || "#FFFFFFFF";
                colorPicker.anchorItem = swatch;
                colorPicker.open();
            }
        }
    }

    // 16 進の直接入力。見本と同じ値を編集する。
    component ColorText: TextField {
        id: colorField
        required property string key
        Layout.fillWidth: true
        implicitHeight: 24
        topPadding: 0
        bottomPadding: 0
        font.pixelSize: 12
        font.family: "Consolas"
        color: "#e6e8ec"
        background: Rectangle {
            radius: 3
            color: "#232830"
            border.color: colorField.activeFocus ? "#5b9bd5" : "#3c424c"
        }
        text: root.clipData[colorField.key] || ""
        onEditingFinished: root.setValue(colorField.key, text.toUpperCase())
        Keys.onReturnPressed: root.releaseFocus()
        Keys.onEscapePressed: {
            text = root.clipData[colorField.key] || "";
            root.releaseFocus();
        }
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

    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        ComboBox {
            id: fontBox
            objectName: "textFontBox"
            Layout.fillWidth: true
            implicitHeight: 26
            model: Qt.fontFamilies()
            displayText: root.clipData.fontFamily || ""
            currentIndex: fontBox.find(root.clipData.fontFamily || "")
            font.pixelSize: 12
            focusPolicy: Qt.NoFocus
            // Photoshop と同じく、一覧で hover した書体を preview へ仮に反映する。
            // 選べば確定、選ばずに閉じれば元の書体へ戻す。
            onActivated: index => root.commitPreview("fontFamily", fontBox.textAt(index))
            onHighlightedIndexChanged: {
                if (fontBox.popup.visible && fontBox.highlightedIndex >= 0)
                    root.schedulePreview("fontFamily", fontBox.textAt(fontBox.highlightedIndex));
            }
            Connections {
                target: fontBox.popup
                function onClosed() {
                    // activated の後にも来る。確定済みなら override は無いので何もしない。
                    root.cancelPreview();
                }
            }
            // Premiere と同じく、名前の右にその書体の見本を出す。
            delegate: ItemDelegate {
                id: fontItem
                required property int index
                required property string modelData
                width: ListView.view ? ListView.view.width : fontBox.width
                height: 26
                highlighted: fontBox.highlightedIndex === fontItem.index
                contentItem: RowLayout {
                    spacing: 8
                    Label {
                        Layout.fillWidth: true
                        text: fontItem.modelData
                        elide: Text.ElideRight
                        font.pixelSize: 12
                        color: "#e6e8ec"
                    }
                    Label {
                        text: "文字もじモジ"
                        font.family: fontItem.modelData
                        font.pixelSize: 14
                        color: "#c8ced6"
                    }
                }
            }
            popup.height: Math.min(420, fontBox.count * 26 + 8)
            // ComboBox 自身は focus を取らない (取ると単キー操作が止まる) ので、開いた一覧に
            // focus を渡す。渡さないと Esc で閉じられない。
            popup.focus: true
        }
        TextNumberField {
            key: "fontSize"
            labelText: ""
            inlineLabelWidth: 1
            Layout.preferredWidth: 58
            minimumValue: 1
            maximumValue: root.mvmController.outputHeight
            stepPerPixel: 0.5
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        Button {
            id: boldButton
            checkable: true
            checked: !!root.clipData.bold
            implicitWidth: 28
            implicitHeight: 24
            focusPolicy: Qt.NoFocus
            text: "B"
            font.bold: true
            font.pixelSize: 13
            onClicked: root.setValue("bold", boldButton.checked)
            ToolTip.visible: hovered
            ToolTip.text: "太字"
        }
        Item { implicitWidth: 6 }
        PlaceButton { alignment: "left"; tip: "テロップ位置 (左下) へ置く" }
        PlaceButton { alignment: "center"; tip: "テロップ位置 (中央下) へ置く" }
        PlaceButton { alignment: "right"; tip: "テロップ位置 (右下) へ置く" }
        Item { Layout.fillWidth: true }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 6
        TextNumberField {
            key: "x"
            labelText: "X"
            Layout.fillWidth: true
            minimumValue: 0
            maximumValue: root.mvmController.outputWidth - 1
            stepPerPixel: 1
        }
        TextNumberField {
            key: "y"
            labelText: "Y"
            Layout.fillWidth: true
            minimumValue: 0
            maximumValue: root.mvmController.outputHeight - 1
            stepPerPixel: 1
        }
    }

    GridLayout {
        Layout.fillWidth: true
        columns: 3
        columnSpacing: 6
        rowSpacing: 4
        SectionLabel { text: "文字色" }
        ColorSwatch { key: "color"; title: "文字色" }
        ColorText { key: "color" }

        SectionLabel { text: "縁" }
        ColorSwatch { key: "outlineColor"; title: "縁の色" }
        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            ColorText { key: "outlineColor" }
            TextNumberField {
                key: "outlineWidth"
                labelText: "幅"
                Layout.preferredWidth: 70
                minimumValue: 0
                maximumValue: 32
                stepPerPixel: 0.2
            }
        }

        SectionLabel { text: "背景" }
        ColorSwatch { key: "backgroundColor"; title: "背景色" }
        ColorText { key: "backgroundColor" }
    }

    ModernColorPicker {
        id: colorPicker
        objectName: "textColorPicker"
        property string targetKey: ""
        onColorEdited: argb => root.schedulePreview(colorPicker.targetKey, argb)
        onColorAccepted: argb => root.commitPreview(colorPicker.targetKey, argb)
        onColorCanceled: root.cancelPreview()
    }
}

pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    required property var mvmController
    readonly property var clipData: root.mvmController.selectedMathClip
    readonly property string clipId: root.clipData.clipId || ""
    property string editingId: ""
    property string savedSource: ""
    property bool loading: false
    property bool logExpanded: false
    spacing: 6
    enabled: !root.mvmController.busy && !root.mvmController.playing

    // 描画の通知は未確定の入力を上書きしない。同じ clip の Undo は非編集中に反映する。
    function synchronize() {
        if (root.loading)
            return;
        if (root.editingId === root.clipId && sourceEditor.activeFocus)
            return;
        root.loading = true;
        if (root.editingId && root.editingId !== root.clipId) {
            previewTimer.stop();
            if (sourceEditor.text !== root.savedSource)
                root.mvmController.updateMathClip(root.editingId, {source: sourceEditor.text});
            else
                root.mvmController.cancelMathPreview();
        }
        root.editingId = root.clipId;
        root.savedSource = root.clipData.source || "";
        sourceEditor.text = root.savedSource;
        root.loading = false;
    }
    onClipDataChanged: synchronize()
    onClipIdChanged: synchronize()
    Component.onCompleted: synchronize()

    function commitSource() {
        previewTimer.stop();
        if (root.editingId !== root.clipId || !root.editingId)
            return;
        if (sourceEditor.text !== root.savedSource) {
            if (root.mvmController.updateMathClip(root.editingId, {source: sourceEditor.text})) {
                root.savedSource = sourceEditor.text.trim();
                root.loading = true;
                sourceEditor.text = root.savedSource;
                root.loading = false;
            }
        } else {
            root.mvmController.cancelMathPreview();
        }
    }
    function releaseFocus() {
        if (root.Window.window)
            root.Window.window.contentItem.forceActiveFocus();
    }
    function focusSource() {
        sourceEditor.forceActiveFocus();
        sourceEditor.selectAll();
    }
    function setValue(key, value) {
        root.mvmController.updateMathClip(root.clipId, {[key]: value});
    }

    Label { text: "数式（LaTeX）"; color: "#e6e8ec" }
    ModernDialogTextArea {
        id: sourceEditor
        objectName: "mathSourceEditor"
        Layout.fillWidth: true
        // 長い式も折り返し、親パネルの縦スクロールで全文へ到達できる。
        Layout.preferredHeight: Math.max(76, implicitHeight)
        font.family: "Consolas"
        placeholderText: "例: ax^2 + bx + c = 0"
        onTextChanged: {
            if (!root.loading && activeFocus)
                previewTimer.restart();
        }
        onActiveFocusChanged: {
            if (!activeFocus)
                root.commitSource();
        }
        Keys.onPressed: event => {
            if ((event.modifiers & Qt.ControlModifier)
                    && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
                root.commitSource();
                root.releaseFocus();
                event.accepted = true;
            } else if (event.key === Qt.Key_Escape) {
                previewTimer.stop();
                root.loading = true;
                sourceEditor.text = root.savedSource;
                root.loading = false;
                root.mvmController.cancelMathPreview();
                root.releaseFocus();
                event.accepted = true;
            }
        }
    }
    Timer {
        id: previewTimer
        interval: 600
        onTriggered: {
            if (root.editingId === root.clipId && sourceEditor.activeFocus)
                root.mvmController.previewMathClip(root.clipId, {source: sourceEditor.text});
        }
    }
    Label {
        Layout.fillWidth: true
        text: "Ctrl+Enter または入力欄を離れて確定 / Esc で戻す"
        color: "#9aa2ad"
        font.pixelSize: 11
        wrapMode: Text.Wrap
    }
    StyleNumberField {
        Layout.fillWidth: true
        styleData: root.clipData
        namePrefix: "math"
        key: "fontSize"
        labelText: "文字サイズ"
        inlineLabelWidth: 72
        minimumValue: 1
        maximumValue: root.mvmController.outputHeight
        stepPerPixel: 1
        onPreviewed: (key, value) => root.mvmController.previewMathClip(root.clipId, {[key]: value})
        onCommitted: (key, value) => root.setValue(key, value)
        onCanceled: root.mvmController.cancelMathPreview()
    }
    Flow {
        Layout.fillWidth: true
        spacing: 6
        Repeater {
            model: [{key: "color", title: "文字色"}, {key: "backgroundColor", title: "背景色"}]
            delegate: ModernDialogButton {
                id: colorButton
                required property var modelData
                objectName: "mathColor_" + modelData.key
                text: modelData.title + "  " + (root.clipData[modelData.key] || "")
                onClicked: {
                    colorPicker.valueKey = modelData.key;
                    colorPicker.initialColor = root.clipData[modelData.key];
                    colorPicker.open();
                }
            }
        }
    }
    ModernColorPicker {
        id: colorPicker
        property string valueKey: "color"
        onColorEdited: argb => root.mvmController.previewMathClip(root.clipId, {[valueKey]: argb})
        onColorAccepted: argb => root.setValue(valueKey, argb)
        onColorCanceled: root.mvmController.cancelMathPreview()
    }
    Label {
        objectName: "mathRenderState"
        Layout.fillWidth: true
        text: (({checking: "準備中", rendering: "描画中", ready: "完了",
                stale: "更新中（古い表示）", error: "エラー", unavailable: "利用不可"})[root.clipData.state] || "準備中")
              + (root.clipData.showingPrevious && root.clipData.state !== "stale" ? "（古い表示）" : "")
        color: root.clipData.state === "error" || root.clipData.state === "unavailable" ? "#f2c66d" : "#a8d5a2"
        wrapMode: Text.Wrap
    }
    Label {
        Layout.fillWidth: true
        visible: text.length > 0
        text: root.clipData.message || ""
        color: "#f2c66d"
        wrapMode: Text.Wrap
    }
    Label {
        Layout.fillWidth: true
        visible: root.clipData.state === "unavailable"
        text: "Manim と MiKTeX を導入し、latex / dvisvgm が利用できる状態にして再試行してください。MiKTeX の不足パッケージは自動導入を有効にしてください。"
        color: "#aeb4bf"
        wrapMode: Text.Wrap
    }
    Flow {
        Layout.fillWidth: true
        spacing: 6
        ModernDialogButton {
            objectName: "mathRetryButton"
            text: "再試行"
            onClicked: root.mvmController.retryMathRendering()
        }
        ModernDialogButton {
            objectName: "mathLogToggle"
            visible: (root.clipData.log || "").length > 0
            text: root.logExpanded ? "ログを閉じる" : "ログを表示"
            onClicked: root.logExpanded = !root.logExpanded
        }
    }
    ModernDialogTextArea {
        objectName: "mathRenderLog"
        Layout.fillWidth: true
        Layout.preferredHeight: implicitHeight
        visible: root.logExpanded && text.length > 0
        text: root.clipData.log || ""
        readOnly: true
        font.family: "Consolas"
    }
    Label {
        Layout.fillWidth: true
        text: root.clipData.toolchain || "描画環境を確認中"
        color: "#9aa2ad"
        font.pixelSize: 11
        wrapMode: Text.WrapAnywhere
    }
    Label {
        objectName: "mathInspectorNotes"
        Layout.fillWidth: true
        text: "式中の \\color は反映されません。色は上の文字色で指定してください。\n鮮明に大きくするには文字サイズを上げてください。位置・拡大・回転は下のエフェクトで調整できます。"
        color: "#9aa2ad"
        font.pixelSize: 11
        wrapMode: Text.Wrap
    }
}

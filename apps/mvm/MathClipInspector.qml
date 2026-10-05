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
    property bool commitRejected: false
    property bool logExpanded: false
    spacing: 6
    enabled: !root.mvmController.busy && !root.mvmController.playing

    // 描画の通知は未確定の入力を上書きしない。同じ clip の Undo は非編集中に反映する。
    function synchronize() {
        if (root.loading)
            return;
        if (root.commitRejected || (root.editingId === root.clipId && sourceEditor.activeFocus))
            return;
        if (root.editingId && root.editingId !== root.clipId) {
            // 確定できない入力は旧 clip の欄に残す。描画通知で再試行・上書きしない。
            if (!root.commitSource())
                return;
        }
        root.loading = true;
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
        if (!root.editingId || root.loading)
            return true;
        root.loading = true;
        if (sourceEditor.text !== root.savedSource) {
            if (!root.mvmController.updateMathClip(root.editingId, {source: sourceEditor.text})) {
                root.commitRejected = true;
                root.loading = false;
                root.mvmController.cancelMathPreview();
                return false;
            }
            root.savedSource = sourceEditor.text.trim();
            sourceEditor.text = root.savedSource;
        } else {
            root.mvmController.cancelMathPreview();
        }
        root.commitRejected = false;
        root.loading = false;
        return true;
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
    Label {
        objectName: "mathDraftRejection"
        Layout.fillWidth: true
        visible: root.commitRejected
        text: root.editingId !== root.clipId
              ? "前に選択した数式の入力を確定できず、この欄に保持しています。修正して Ctrl+Enter で確定するか、Esc で取り消すと選択中の数式へ移ります。"
              : "入力を確定できませんでした。この欄に保持しています。修正して確定するか、Esc で取り消してください。"
        color: "#f2c66d"
        wrapMode: Text.Wrap
    }
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
            if (!activeFocus && !root.loading && root.commitSource() && root.editingId !== root.clipId)
                root.synchronize();
        }
        Keys.onPressed: event => {
            if ((event.modifiers & Qt.ControlModifier)
                    && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
                if (root.commitSource()) {
                    root.synchronize();
                    root.releaseFocus();
                }
                event.accepted = true;
            } else if (event.key === Qt.Key_Escape) {
                previewTimer.stop();
                root.loading = true;
                sourceEditor.text = root.savedSource;
                root.loading = false;
                root.commitRejected = false;
                root.mvmController.cancelMathPreview();
                root.synchronize();
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
        enabled: root.editingId === root.clipId && !root.commitRejected
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
        enabled: root.editingId === root.clipId && !root.commitRejected
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
    // Write: clip の先頭で式を書いていく (Manim の Write)。尺は秒で見せ、clip の尺までに収める。
    ModernDialogCheckBox {
        id: writeToggle
        objectName: "mathWriteToggle"
        enabled: root.editingId === root.clipId && !root.commitRejected
        Layout.fillWidth: true
        text: "Write（先頭で式を書く）"
        checked: root.clipData.intro === "write"
        onToggled: {
            root.setValue("intro", writeToggle.checked ? "write" : "none");
            // 確定を拒否されたときも Project の値に戻す (クリックで切れた binding を張り直す)。
            writeToggle.checked = Qt.binding(() => root.clipData.intro === "write");
        }
    }
    DragNumberField {
        id: writeSeconds
        objectName: "mathWriteSeconds"
        property var pending: undefined
        visible: root.clipData.intro === "write"
        enabled: root.editingId === root.clipId && !root.commitRejected
        Layout.fillWidth: true
        labelText: "Write の長さ"
        inlineLabelWidth: 92
        decimals: 2
        suffix: " 秒"
        stepPerPixel: 0.01
        minimumValue: 0.01
        maximumValue: Math.max(0.01, Number(root.clipData.introMaxSeconds) || 0.01)
        value: writeSeconds.pending !== undefined ? writeSeconds.pending
                                                  : (Number(root.clipData.introSeconds) || 0)
        // 尺を変えると連番を描き直すので、ドラッグ中は表示だけを変え、離したときに確定する。
        onValueEdited: (newValue, commit) => {
            if (!commit) {
                writeSeconds.pending = newValue;
                return;
            }
            writeSeconds.pending = undefined;
            if (Math.abs(newValue - (Number(root.clipData.introSeconds) || 0)) > 0.0001)
                root.setValue("introSeconds", newValue);
        }
        onEditCanceled: writeSeconds.pending = undefined
    }
    Label {
        objectName: "mathWriteState"
        Layout.fillWidth: true
        visible: root.clipData.intro === "write"
        text: (({checking: "Write: 準備中", rendering: "Write: 描画中（書き終えた式を表示）",
                 ready: "Write: 完了", error: "Write: エラー", unavailable: "Write: 利用不可"})[root.clipData.writeState]
               || "Write: 準備中")
              + (root.clipData.writeMessage ? "\n" + root.clipData.writeMessage : "")
              // 書き出しには使えるが、preview の memory の上限で Write を表示しない場合。
              + (root.clipData.writePreview === "loading" ? "（preview を準備中）" : "")
              + (root.clipData.writePreview === "memory" && root.clipData.writePreviewMessage
                 ? "\n" + root.clipData.writePreviewMessage : "")
        color: root.clipData.writeState === "error" || root.clipData.writeState === "unavailable"
               || root.clipData.writePreview === "memory" ? "#f2c66d" : "#a8d5a2"
        wrapMode: Text.Wrap
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
    MathDependencyGuidance {
        objectName: "mathDependencyGuidance"
        visible: root.clipData.unavailableReason === "backend"
    }
    Flow {
        Layout.fillWidth: true
        spacing: 6
        ModernDialogButton {
            objectName: "mathRetryButton"
            visible: root.clipData.canRetry === true
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
        text: "式中の \\color は反映されません。色は上の文字色で指定してください。\n鮮明に大きくするには文字サイズを上げてください。位置・拡大・回転・フェードは下のエフェクトで調整できます。\nWrite は確定した式を描いてから表示します。入力中は書き終えた式を表示します。"
        color: "#9aa2ad"
        font.pixelSize: 11
        wrapMode: Text.Wrap
    }
}

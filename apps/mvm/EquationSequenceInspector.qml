pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 数式 sequence (EquationSequence) の authoring (P3-5)。
// 表示の値 (editor.view / editor.status) を読み、利用者の意図を editor の操作として呼ぶだけ。
// TeX・部分式の参照・時間・compile の状態・byte offset はここで解釈しない。部分式の範囲は
// 編集欄の UTF-16 の選択 (selectionStart / selectionEnd) をそのまま渡し、変換は controller が行う。
// Project の確定は操作 1 回 = Undo 1 回。式の入力は focus を離す・Ctrl+Enter で 1 回だけ確定する。
ColumnLayout {
    id: root
    objectName: "equationSequenceInspector"
    required property var mvmController
    required property var editor
    readonly property var view: root.editor ? root.editor.view : ({})
    readonly property var status: root.editor ? root.editor.status : ({})
    readonly property string clipId: root.view.clipId || ""
    readonly property var current: root.view.state || ({})
    readonly property var selectedPart: root.view.selectedPart || ({})
    readonly property var selectedAction: root.view.selectedAction || ({})
    readonly property string editKey: root.clipId + "/" + (root.current.id || "")
    property string loadedKey: ""
    property string savedSource: ""
    property bool loading: false
    property bool sourceRejected: false
    property bool confirmDeleteState: false
    property bool confirmDeletePart: false
    property bool detailExpanded: false
    spacing: 8
    enabled: !root.mvmController.busy && !root.mvmController.playing

    // ---- 式の編集欄と Project の同期 (MathClipInspector と同じ規則) ----
    // 入力中 (focus がある同じ状態) は Project の通知で上書きしない。別の状態へ移るときは
    // 前の状態の入力を先に確定する。確定できなければ破棄し、理由は editor の message に残る。
    function synchronize() {
        if (root.loading)
            return;
        if (sourceEditor.activeFocus && root.loadedKey === root.editKey)
            return;
        if (root.loadedKey !== root.editKey) {
            if (root.loadedKey !== "" && sourceEditor.text !== root.savedSource) {
                const result = root.editor.commitSourceEdit(sourceEditor.text);
                if (!result.ok)
                    root.editor.cancelSourceEdit();
            } else {
                root.editor.cancelSourceEdit();
            }
        }
        root.loading = true;
        root.loadedKey = root.editKey;
        root.savedSource = root.current.source || "";
        // 同じ文字なら書き直さない (選択を保ったまま部分式の操作へ進める)。
        if (sourceEditor.text !== root.savedSource)
            sourceEditor.text = root.savedSource;
        root.sourceRejected = false;
        root.loading = false;
    }
    onEditKeyChanged: {
        root.confirmDeleteState = false;
        root.confirmDeletePart = false;
        root.synchronize();
    }
    onCurrentChanged: root.synchronize()
    Component.onCompleted: root.synchronize()

    function commitSource() {
        if (root.loading || root.loadedKey === "")
            return true;
        if (sourceEditor.text === root.savedSource) {
            root.editor.cancelSourceEdit();
            root.sourceRejected = false;
            return true;
        }
        const result = root.editor.commitSourceEdit(sourceEditor.text);
        if (!result.ok) {
            if (result.discarded) {
                root.loadedKey = "";
                root.synchronize();
                return true;
            }
            root.sourceRejected = true;
            return false;
        }
        root.sourceRejected = false;
        root.savedSource = sourceEditor.text;
        if (sourceEditor.activeFocus)
            root.editor.beginSourceEdit(sourceEditor.textDocument);
        return true;
    }
    function cancelSource() {
        // 記録を先に止めてから文字を戻す (戻す変更を編集として記録しない)。
        root.editor.cancelSourceEdit();
        root.loading = true;
        sourceEditor.text = root.savedSource;
        root.loading = false;
        root.sourceRejected = false;
    }
    function releaseFocus() {
        if (root.Window.window)
            root.Window.window.contentItem.forceActiveFocus();
    }
    function focusSource() {
        sourceEditor.forceActiveFocus();
    }

    // 読むだけの状態の問い合わせ。描画は要求しない (再生位置に掛かる clip の合成だけが要求する)。
    Timer {
        id: statusPoll
        objectName: "equationStatusPoll"
        interval: 250
        repeat: true
        running: root.visible && root.clipId !== ""
        triggeredOnStart: true
        onTriggered: root.editor.refreshStatus()
    }

    component SectionTitle: Label {
        Layout.fillWidth: true
        Layout.topMargin: 4
        color: "#e6e8ec"
        font.bold: true
        font.pixelSize: 12
        elide: Text.ElideRight
    }
    component Note: Label {
        Layout.fillWidth: true
        color: "#9aa2ad"
        font.pixelSize: 11
        wrapMode: Text.Wrap
    }
    component StatusRow: RowLayout {
        id: statusRow
        property string caption
        property string value
        property bool warn: false
        Layout.fillWidth: true
        spacing: 8
        Label {
            Layout.preferredWidth: 76
            text: statusRow.caption
            color: "#9aa2ad"
            font.pixelSize: 11
            elide: Text.ElideRight
        }
        Label {
            Layout.fillWidth: true
            text: statusRow.value
            color: statusRow.warn ? "#f2c66d" : "#c8d3df"
            font.pixelSize: 11
            wrapMode: Text.Wrap
        }
    }

    Label {
        Layout.fillWidth: true
        text: "数式 sequence"
        color: "#e6e8ec"
        font.bold: true
        font.pixelSize: 13
    }
    Note {
        objectName: "equationSequenceSummary"
        text: (root.view.stateCount || 0) + " 状態  |  全長 " + (root.view.lengthText || "")
              + "  |  " + (root.view.fpsText || "")
    }

    // ---- 状態の表示 (Compile / Renderer / Artifact / Residency / Preview を分けて出す) ----
    Rectangle {
        objectName: "equationStatusPanel"
        Layout.fillWidth: true
        implicitHeight: statusColumn.implicitHeight + 16
        radius: 4
        color: "#1d2127"
        border.color: root.status.category ? "#7a6233" : "#343840"
        ColumnLayout {
            id: statusColumn
            x: 8
            y: 8
            width: parent.width - 16
            spacing: 3
            StatusRow {
                objectName: "equationStatusCompile"
                caption: "式の解析"
                value: root.status.compileText || "確認中"
                warn: root.status.compile === "failed"
            }
            StatusRow {
                objectName: "equationStatusRenderer"
                caption: "描画環境"
                value: root.status.rendererText || "確認中"
                warn: root.status.renderer === "unavailable"
            }
            StatusRow {
                objectName: "equationStatusArtifact"
                caption: "描画結果"
                value: root.status.artifactText || "—"
                warn: root.status.artifact === "failed" || root.status.artifact === "blocked"
            }
            StatusRow {
                objectName: "equationStatusResidency"
                caption: "メモリ"
                value: root.status.residencyText || "—"
                warn: root.status.residency === "over_budget" || root.status.residency === "failed"
            }
            StatusRow {
                objectName: "equationStatusPreview"
                caption: "表示中"
                value: root.status.previewText || "—"
                warn: root.status.preview === "current_state_only"
            }
            Label {
                objectName: "equationStatusHeadline"
                Layout.fillWidth: true
                visible: text.length > 0
                text: root.status.headline || ""
                color: "#f2c66d"
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }
            MathDependencyGuidance {
                objectName: "equationDependencyGuidance"
                visible: root.status.renderer === "unavailable"
                font.pixelSize: 11
            }
            Flow {
                Layout.fillWidth: true
                spacing: 6
                ModernDialogButton {
                    objectName: "equationRetryButton"
                    visible: root.status.renderer === "unavailable" || root.status.artifact === "failed"
                    text: "再試行"
                    onClicked: root.mvmController.retryMathRendering()
                }
                ModernDialogButton {
                    objectName: "equationStatusDetailToggle"
                    visible: (root.status.detail || "").length > 0
                    text: root.detailExpanded ? "詳細を閉じる" : "詳細"
                    onClicked: root.detailExpanded = !root.detailExpanded
                }
            }
            Label {
                objectName: "equationStatusDetail"
                Layout.fillWidth: true
                visible: root.detailExpanded && text.length > 0
                text: root.status.detail || ""
                color: "#9aa2ad"
                font.family: "Consolas"
                font.pixelSize: 10
                wrapMode: Text.WrapAnywhere
            }
            Label {
                Layout.fillWidth: true
                text: root.status.exportText || ""
                visible: text.length > 0
                color: "#7f8792"
                font.pixelSize: 10
                wrapMode: Text.Wrap
            }
        }
    }

    Label {
        objectName: "equationMessage"
        Layout.fillWidth: true
        visible: text.length > 0
        text: root.view.message || ""
        color: root.view.messageError ? "#f2c66d" : "#a8d5a2"
        wrapMode: Text.Wrap
    }

    // ---- 状態の一覧 (Project の vector の順。選択は UI の状態) ----
    SectionTitle {
        text: "状態"
    }
    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(Math.max(1, stateList.count), 6) * 30 + 4
        color: "#1d2127"
        border.color: stateList.activeFocus ? "#6ca9e6" : "#343840"
        radius: 4
    BoundedListView {
        id: stateList
        objectName: "equationStateList"
        anchors.fill: parent
        anchors.margins: 2
        model: root.view.states || []
        activeFocusOnTab: true
        keyNavigationEnabled: false
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        // 一覧に focus がある間は Delete・上下を timeline の shortcut より優先する。Delete は何もしない
        // (clip を誤って消さない。状態の削除は確認付きのボタンだけ)。上下は選択の移動で、並べ替えではない。
        Keys.onShortcutOverride: event => {
            event.accepted = event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace
                || event.key === Qt.Key_Up || event.key === Qt.Key_Down;
        }
        Keys.onPressed: event => {
            const states = root.view.states || [];
            const index = root.current.index || 0;
            if (event.key === Qt.Key_Up && index > 0) {
                root.editor.selectState(states[index - 1].id);
            } else if (event.key === Qt.Key_Down && index + 1 < states.length) {
                root.editor.selectState(states[index + 1].id);
            }
            if (event.key === Qt.Key_Up || event.key === Qt.Key_Down || event.key === Qt.Key_Delete
                    || event.key === Qt.Key_Backspace)
                event.accepted = true;
        }
        delegate: Rectangle {
            id: stateRow
            required property var modelData
            objectName: "equationStateRow_" + modelData.index
            width: ListView.view.width
            height: 30
            color: modelData.selected ? "#2c4058" : (stateMouse.containsMouse ? "#262b33" : "transparent")
            radius: 3
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 10
                spacing: 6
                Label {
                    text: "[" + stateRow.modelData.index + "]"
                    color: "#9aa2ad"
                    font.pixelSize: 11
                    Layout.preferredWidth: 28
                }
                Rectangle {
                    visible: stateRow.modelData.problem
                    width: 6
                    height: 6
                    radius: 3
                    color: "#f2c66d"
                }
                Label {
                    Layout.fillWidth: true
                    text: stateRow.modelData.summary
                    color: "#e6e8ec"
                    font.family: "Consolas"
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                Label {
                    text: "hold " + stateRow.modelData.holdFrames + "f"
                    color: "#9aa2ad"
                    font.pixelSize: 11
                }
            }
            MouseArea {
                id: stateMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: {
                    stateList.forceActiveFocus();
                    root.editor.selectState(stateRow.modelData.id);
                }
            }
        }
    }
    }
    Flow {
        objectName: "equationStateButtons"
        Layout.fillWidth: true
        spacing: 6
        ModernDialogButton {
            objectName: "equationInsertBefore"
            text: "前に挿入"
            onClicked: root.editor.insertState(false)
        }
        ModernDialogButton {
            objectName: "equationInsertAfter"
            text: "後に挿入"
            onClicked: root.editor.insertState(true)
        }
        ModernDialogButton {
            objectName: "equationMoveUp"
            text: "上へ"
            enabled: root.current.canMoveUp === true
            onClicked: root.editor.moveSelectedState(-1)
        }
        ModernDialogButton {
            objectName: "equationMoveDown"
            text: "下へ"
            enabled: root.current.canMoveDown === true
            onClicked: root.editor.moveSelectedState(1)
        }
        ModernDialogButton {
            objectName: "equationDeleteState"
            text: "削除…"
            destructive: true
            enabled: root.current.canDelete === true
            onClicked: root.confirmDeleteState = true
        }
    }
    Note {
        objectName: "equationLastStateNote"
        visible: root.current.canDelete === false
        text: "状態が 1 つだけなので削除できません (sequence には状態が 1 つ以上必要です)。"
    }
    Rectangle {
        objectName: "equationDeleteStateConfirm"
        Layout.fillWidth: true
        visible: root.confirmDeleteState
        implicitHeight: deleteStateColumn.implicitHeight + 16
        radius: 4
        color: "#2a2224"
        border.color: "#684143"
        ColumnLayout {
            id: deleteStateColumn
            x: 8
            y: 8
            width: parent.width - 16
            spacing: 6
            Label {
                objectName: "equationDeleteStateText"
                Layout.fillWidth: true
                text: "状態 [" + root.current.index + "] を削除します。この状態の強調 "
                      + (root.current.deleteActions || 0) + " 件と、隣接する変形 "
                      + (root.current.deleteTransitions || 0) + " 件 (明示の対応 "
                      + (root.current.deleteCorrespondence || 0) + " 件) も一緒に削除されます。"
                      + (root.current.deleteJoinsNeighbors
                         ? "前後の状態は新しい変形 (対応なし) でつながります。" : "")
                color: "#f0d0d0"
                wrapMode: Text.Wrap
            }
            Flow {
                Layout.fillWidth: true
                spacing: 6
                ModernDialogButton {
                    objectName: "equationDeleteStateConfirmButton"
                    text: "削除する"
                    destructive: true
                    onClicked: {
                        root.confirmDeleteState = false;
                        root.editor.deleteSelectedState();
                    }
                }
                ModernDialogButton {
                    objectName: "equationDeleteStateCancel"
                    text: "やめる"
                    onClicked: root.confirmDeleteState = false
                }
            }
        }
    }

    // ---- 選んだ状態の式と hold ----
    SectionTitle {
        objectName: "equationStateTitle"
        text: "状態 [" + (root.current.index ?? 0) + "] の式"
    }
    Label {
        objectName: "equationSourceRejection"
        Layout.fillWidth: true
        visible: root.sourceRejected
        text: "入力を確定できませんでした。この欄に保持しています。修正して確定するか、Esc で取り消してください。"
        color: "#f2c66d"
        wrapMode: Text.Wrap
    }
    BoundedScrollView {
        id: sourceScroll
        objectName: "equationSourceScroll"
        Layout.fillWidth: true
        // 長い式は折り返し、欄の中で縦にスクロールする (パネルを式の長さで伸ばさない)。
        Layout.preferredHeight: Math.min(Math.max(76, sourceEditor.implicitHeight), 180)
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ModernDialogTextArea {
            id: sourceEditor
            objectName: "equationSourceEditor"
            font.family: "Consolas"
            // 部分式の追加・再設定は確定後の選択を使う。focus が外れても選択を残す。
            persistentSelection: true
            placeholderText: "例: ax^2 + bx + c = 0"
            onActiveFocusChanged: {
                if (root.loading)
                    return;
                if (activeFocus) {
                    root.editor.beginSourceEdit(sourceEditor.textDocument);
                } else if (root.commitSource()) {
                    root.synchronize();
                }
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
                    root.cancelSource();
                    root.releaseFocus();
                    event.accepted = true;
                } else if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
                    // TeX の入力欄に tab 文字を入れず、focus を次 / 前の操作へ移す (離れると確定)。
                    const next = sourceEditor.nextItemInFocusChain(event.key === Qt.Key_Tab);
                    if (next)
                        next.forceActiveFocus(event.key === Qt.Key_Tab ? Qt.TabFocusReason
                                                                       : Qt.BacktabFocusReason);
                    event.accepted = true;
                }
            }
        }
    }
    Note {
        text: "Ctrl+Enter または入力欄を離れて確定 / Esc で戻す。部分式の範囲は編集の位置に合わせて移り、部分式の中を編集すると範囲は無効になります (同じ文字を探して付け直すことはしません)。"
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        DragNumberField {
            id: holdField
            objectName: "equationHoldField"
            property var pending: undefined
            Layout.preferredWidth: 150
            labelText: "hold"
            inlineLabelWidth: 40
            suffix: " f"
            stepPerPixel: 0.5
            minimumValue: 1
            maximumValue: root.current.maximumFrames || 100000
            value: holdField.pending !== undefined ? holdField.pending : (Number(root.current.holdFrames) || 1)
            onValueEdited: (newValue, commit) => {
                if (!commit) {
                    holdField.pending = Math.round(newValue);
                    return;
                }
                holdField.pending = undefined;
                root.editor.setHoldFrames(Math.round(newValue));
            }
            onEditCanceled: holdField.pending = undefined
        }
        Label {
            Layout.fillWidth: true
            text: root.current.holdText || ""
            color: "#9aa2ad"
            font.pixelSize: 11
            elide: Text.ElideRight
        }
    }

    // ---- 前後の変形 (長さと明示の対応) ----
    SectionTitle {
        text: "変形"
    }
    Note {
        objectName: "equationNoTransitions"
        visible: (root.view.transitions || []).length === 0
        text: "状態が 1 つだけなので変形はありません。状態を追加すると、前後の状態の間に変形ができます。"
    }
    Repeater {
        model: root.view.transitions || []
        delegate: Rectangle {
            id: transitionCard
            required property var modelData
            required property int index
            objectName: "equationTransition_" + modelData.role
            Layout.fillWidth: true
            implicitHeight: transitionColumn.implicitHeight + 16
            radius: 4
            color: "#1d2127"
            border.color: "#343840"
            ColumnLayout {
                id: transitionColumn
                x: 8
                y: 8
                width: parent.width - 16
                spacing: 6
                Label {
                    Layout.fillWidth: true
                    text: transitionCard.modelData.title
                    color: "#c8d3df"
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    DragNumberField {
                        id: transitionField
                        objectName: "equationTransitionFrames_" + transitionCard.modelData.role
                        property var pending: undefined
                        Layout.preferredWidth: 150
                        labelText: "長さ"
                        inlineLabelWidth: 40
                        suffix: " f"
                        stepPerPixel: 0.5
                        minimumValue: 1
                        maximumValue: root.current.maximumFrames || 100000
                        value: transitionField.pending !== undefined ? transitionField.pending
                                                                     : (Number(transitionCard.modelData.frames) || 1)
                        onValueEdited: (newValue, commit) => {
                            if (!commit) {
                                transitionField.pending = Math.round(newValue);
                                return;
                            }
                            transitionField.pending = undefined;
                            root.editor.setTransitionFrames(transitionCard.modelData.id, Math.round(newValue));
                        }
                        onEditCanceled: transitionField.pending = undefined
                    }
                    Label {
                        Layout.fillWidth: true
                        text: transitionCard.modelData.framesText
                        color: "#9aa2ad"
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                }
                Note {
                    visible: transitionCard.modelData.correspondence.length === 0
                    text: "明示の対応はありません。指定しない部分は変形のときに自動で照合します (自動の照合は保存されません)。"
                }
                Repeater {
                    model: transitionCard.modelData.correspondence
                    delegate: RowLayout {
                        id: pairRow
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 6
                        Label {
                            Layout.fillWidth: true
                            text: pairRow.modelData.fromLabel + "  →  " + pairRow.modelData.toLabel
                            color: "#e6e8ec"
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }
                        ModernDialogButton {
                            objectName: "equationRemovePair"
                            text: "外す"
                            implicitWidth: 56
                            onClicked: root.editor.removeCorrespondence(transitionCard.modelData.id,
                                                                        pairRow.modelData.fromPart,
                                                                        pairRow.modelData.toPart)
                        }
                    }
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    visible: transitionCard.modelData.fromCandidates.length > 0
                             && transitionCard.modelData.toCandidates.length > 0
                    ModernDialogComboBox {
                        id: fromCombo
                        objectName: "equationPairFrom_" + transitionCard.modelData.role
                        width: Math.min(200, transitionColumn.width)
                        model: transitionCard.modelData.fromCandidates
                        textRole: "label"
                        valueRole: "id"
                    }
                    ModernDialogComboBox {
                        id: toCombo
                        objectName: "equationPairTo_" + transitionCard.modelData.role
                        width: Math.min(200, transitionColumn.width)
                        model: transitionCard.modelData.toCandidates
                        textRole: "label"
                        valueRole: "id"
                    }
                    ModernDialogButton {
                        objectName: "equationAddPair_" + transitionCard.modelData.role
                        text: "対応を追加"
                        onClicked: root.editor.addCorrespondence(transitionCard.modelData.id,
                                                                 fromCombo.currentValue || "",
                                                                 toCombo.currentValue || "")
                    }
                }
                Note {
                    visible: transitionCard.modelData.fromCandidates.length === 0
                             || transitionCard.modelData.toCandidates.length === 0
                    text: "対応を追加するには、前と後の状態に有効な部分式 (まだ対応に使っていないもの) が必要です。"
                }
            }
        }
    }

    // ---- 部分式 ----
    SectionTitle {
        text: "部分式"
    }
    Note {
        objectName: "equationNoParts"
        visible: (root.view.parts || []).length === 0
        text: "部分式はまだありません。上の式の一部を選択して「選択範囲を部分式に追加」を押してください。"
    }
    Rectangle {
        Layout.fillWidth: true
        visible: partList.count > 0
        Layout.preferredHeight: Math.min(partList.count, 5) * 46 + 4
        color: "#1d2127"
        border.color: partList.activeFocus ? "#6ca9e6" : "#343840"
        radius: 4
    BoundedListView {
        id: partList
        objectName: "equationPartList"
        anchors.fill: parent
        anchors.margins: 2
        model: root.view.parts || []
        activeFocusOnTab: true
        keyNavigationEnabled: false
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        Keys.onShortcutOverride: event => {
            event.accepted = event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace
                || event.key === Qt.Key_Up || event.key === Qt.Key_Down;
        }
        Keys.onPressed: event => {
            const parts = root.view.parts || [];
            let index = -1;
            for (let i = 0; i < parts.length; ++i)
                if (parts[i].selected)
                    index = i;
            if (event.key === Qt.Key_Up && index > 0)
                root.editor.selectPart(parts[index - 1].id);
            else if (event.key === Qt.Key_Down && index + 1 < parts.length)
                root.editor.selectPart(parts[index + 1].id);
            if (event.key === Qt.Key_Up || event.key === Qt.Key_Down || event.key === Qt.Key_Delete
                    || event.key === Qt.Key_Backspace)
                event.accepted = true;
        }
        delegate: Rectangle {
            id: partRow
            required property var modelData
            required property int index
            objectName: "equationPartRow_" + index
            width: ListView.view.width
            height: 46
            radius: 3
            color: modelData.selected ? "#2c4058" : (partMouse.containsMouse ? "#262b33" : "transparent")
            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 10
                anchors.topMargin: 4
                anchors.bottomMargin: 4
                spacing: 2
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Label {
                        Layout.fillWidth: true
                        text: partRow.modelData.displayLabel
                        color: "#e6e8ec"
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    Rectangle {
                        implicitWidth: chip.implicitWidth + 10
                        implicitHeight: 16
                        radius: 3
                        color: partRow.modelData.status === "bound" ? "#2d4a36" : "#5a4520"
                        Label {
                            id: chip
                            anchors.centerIn: parent
                            text: ({bound: "対応済み", invalid_binding: "範囲が無効",
                                    missing_target: "見つかりません",
                                    unsupported_tex_boundary: "分離できない範囲",
                                    unsupported_empty_target: "描く文字なし"})[partRow.modelData.status]
                                  || partRow.modelData.status
                            color: "#f0f1f3"
                            font.pixelSize: 10
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: partRow.modelData.rangeText + "  ·  強調 " + partRow.modelData.actionCount
                          + "  ·  対応 " + partRow.modelData.correspondenceCount
                    color: "#9aa2ad"
                    font.pixelSize: 10
                    elide: Text.ElideRight
                }
            }
            MouseArea {
                id: partMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: {
                    partList.forceActiveFocus();
                    root.confirmDeletePart = false;
                    root.editor.selectPart(partRow.modelData.id);
                }
            }
        }
    }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 6
        ModernDialogField {
            id: partLabelField
            objectName: "equationPartLabelField"
            Layout.fillWidth: true
            Layout.minimumWidth: 60
            placeholderText: "名前 (省略可)"
        }
    }
    Flow {
        Layout.fillWidth: true
        spacing: 6
        ModernDialogButton {
            objectName: "equationAddPart"
            text: "選択範囲を部分式に追加"
            onClicked: {
                if (root.editor.addPart(sourceEditor.selectionStart, sourceEditor.selectionEnd,
                                        sourceEditor.text, partLabelField.text))
                    partLabelField.text = "";
            }
        }
    }
    // 選んだ部分式の詳細と修復
    Rectangle {
        objectName: "equationPartDetail"
        Layout.fillWidth: true
        visible: root.selectedPart.id !== undefined
        implicitHeight: partDetail.implicitHeight + 16
        radius: 4
        color: "#1d2127"
        border.color: root.selectedPart.repairable ? "#7a6233" : "#343840"
        ColumnLayout {
            id: partDetail
            x: 8
            y: 8
            width: parent.width - 16
            spacing: 6
            Label {
                Layout.fillWidth: true
                text: root.selectedPart.displayLabel || ""
                color: "#e6e8ec"
                font.pixelSize: 12
                wrapMode: Text.WrapAnywhere
            }
            Label {
                objectName: "equationPartStatusText"
                Layout.fillWidth: true
                text: root.selectedPart.statusText || ""
                color: root.selectedPart.repairable ? "#f2c66d" : "#a8d5a2"
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                text: (root.selectedPart.rangeText || "") + "  ·  強調 " + (root.selectedPart.actionCount || 0)
                      + " 件  ·  対応 " + (root.selectedPart.correspondenceCount || 0) + " 件"
                color: "#9aa2ad"
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }
            Label {
                objectName: "equationPartIdDetail"
                Layout.fillWidth: true
                text: "ID: " + (root.selectedPart.id || "")
                color: "#6f7782"
                font.pixelSize: 10
                elide: Text.ElideMiddle
            }
            Flow {
                Layout.fillWidth: true
                spacing: 6
                ModernDialogButton {
                    objectName: "equationShowPart"
                    visible: root.selectedPart.hasRange === true
                    text: "式で範囲を表示"
                    onClicked: sourceEditor.select(root.selectedPart.rangeStart, root.selectedPart.rangeEnd)
                }
                ModernDialogButton {
                    objectName: "equationRebindPart"
                    prominent: root.selectedPart.repairable === true
                    text: root.selectedPart.exists === false ? "選択範囲で作り直す" : "選択範囲で再設定"
                    onClicked: root.editor.rebindSelectedPart(sourceEditor.selectionStart,
                                                              sourceEditor.selectionEnd, sourceEditor.text)
                }
                ModernDialogButton {
                    objectName: "equationRenamePart"
                    visible: root.selectedPart.exists === true
                    text: "名前を変更"
                    onClicked: {
                        if (root.editor.renameSelectedPart(partLabelField.text))
                            partLabelField.text = "";
                    }
                }
                ModernDialogButton {
                    objectName: "equationDeletePart"
                    visible: root.selectedPart.exists === true
                    text: "削除…"
                    destructive: true
                    onClicked: root.confirmDeletePart = true
                }
            }
            Note {
                visible: root.selectedPart.repairable === true
                text: root.selectedPart.exists === false
                      ? "式の中で範囲を選んで「選択範囲で作り直す」を押すと、同じ部分式として作り直し、参照している強調が有効に戻ります。変形の対応は自動では戻りません。"
                      : "式の中で範囲を選んで「選択範囲で再設定」を押すと、同じ部分式のまま範囲を結び直します。変形の対応は自動では戻りません。"
            }
            ColumnLayout {
                objectName: "equationDeletePartConfirm"
                Layout.fillWidth: true
                visible: root.confirmDeletePart
                spacing: 6
                Label {
                    Layout.fillWidth: true
                    text: "部分式を削除します。参照している強調 " + (root.selectedPart.actionCount || 0)
                          + " 件は「見つからない部分式」になり (後で作り直せます)、変形の対応 "
                          + (root.selectedPart.correspondenceCount || 0) + " 件は外れます。"
                    color: "#f0d0d0"
                    wrapMode: Text.Wrap
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    ModernDialogButton {
                        objectName: "equationDeletePartConfirmButton"
                        text: "削除する"
                        destructive: true
                        onClicked: {
                            root.confirmDeletePart = false;
                            root.editor.deleteSelectedPart();
                        }
                    }
                    ModernDialogButton {
                        text: "やめる"
                        onClicked: root.confirmDeletePart = false
                    }
                }
            }
        }
    }

    // ---- 強調 (outline / pulse) ----
    SectionTitle {
        text: "強調"
    }
    Note {
        objectName: "equationNoActions"
        visible: (root.view.actions || []).length === 0
        text: (root.view.actionTargets || []).length === 0
              ? "強調には部分式が必要です。先に部分式を追加してください。"
              : "強調はまだありません。下で対象と種類を選んで追加できます。"
    }
    Repeater {
        model: root.view.actions || []
        delegate: Rectangle {
            id: actionRow
            required property var modelData
            required property int index
            objectName: "equationActionRow_" + index
            Layout.fillWidth: true
            implicitHeight: actionColumn.implicitHeight + 10
            radius: 3
            color: modelData.selected ? "#2c4058" : (actionMouse.containsMouse ? "#262b33" : "#1d2127")
            border.color: modelData.targetStatus === "missing" ? "#7a6233" : "#343840"
            ColumnLayout {
                id: actionColumn
                x: 8
                y: 5
                width: parent.width - 16
                spacing: 2
                Label {
                    Layout.fillWidth: true
                    text: actionRow.modelData.operationText + "  ·  " + actionRow.modelData.partLabel
                    color: "#e6e8ec"
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                Label {
                    Layout.fillWidth: true
                    text: actionRow.modelData.intervalText
                    color: "#9aa2ad"
                    font.pixelSize: 10
                    elide: Text.ElideRight
                }
                Label {
                    Layout.fillWidth: true
                    visible: actionRow.modelData.targetStatus === "missing" || actionRow.modelData.unsampled
                    text: actionRow.modelData.targetStatus === "missing"
                          ? "対象の部分式が見つかりません (部分式の一覧で作り直すか、対象を選び直してください)"
                          : "今の出力 fps では 1 frame も表示されません"
                    color: "#f2c66d"
                    font.pixelSize: 10
                    wrapMode: Text.Wrap
                }
            }
            MouseArea {
                id: actionMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: root.editor.selectAction(actionRow.modelData.selected ? "" : actionRow.modelData.id)
            }
        }
    }
    // 追加・変更の入力 (確定は「追加」「変更を適用」の 1 回ずつ)。区間の最終的な判定は domain。
    Rectangle {
        id: actionForm
        objectName: "equationActionForm"
        Layout.fillWidth: true
        visible: (root.view.actionTargets || []).length > 0 || root.selectedAction.id !== undefined
        implicitHeight: actionFormColumn.implicitHeight + 16
        radius: 4
        color: "#1d2127"
        border.color: "#343840"
        property string targetId: ""
        property string operation: "outline"
        property real start: 0
        property real duration: 1
        readonly property real hold: Number(root.current.holdFrames) || 1
        readonly property bool fits: actionForm.start >= 0 && actionForm.duration >= 1
                                     && actionForm.start + actionForm.duration <= actionForm.hold
        // 選んだ action を入力へ写す。選んでいなければ選んだ部分式を対象の既定にする。
        function load() {
            const a = root.selectedAction;
            if (a.id !== undefined) {
                actionForm.targetId = a.partId;
                actionForm.operation = a.operation;
                actionForm.start = a.start;
                actionForm.duration = a.duration;
            } else {
                const targets = root.view.actionTargets || [];
                let target = targets.length > 0 ? targets[0].id : "";
                for (let i = 0; i < targets.length; ++i)
                    if (targets[i].id === root.selectedPart.id)
                        target = targets[i].id;
                actionForm.targetId = target;
                actionForm.operation = "outline";
                actionForm.start = 0;
                actionForm.duration = Math.max(1, Math.min(actionForm.hold, 30));
            }
        }
        Connections {
            target: root
            function onSelectedActionChanged() { actionForm.load(); }
            function onEditKeyChanged() { actionForm.load(); }
        }
        Component.onCompleted: actionForm.load()
        ColumnLayout {
            id: actionFormColumn
            x: 8
            y: 8
            width: parent.width - 16
            spacing: 6
            Label {
                Layout.fillWidth: true
                text: root.selectedAction.id !== undefined ? "選んだ強調を変更" : "強調を追加"
                color: "#c8d3df"
                font.pixelSize: 12
            }
            Flow {
                Layout.fillWidth: true
                spacing: 6
                ModernDialogComboBox {
                    id: targetCombo
                    objectName: "equationActionTarget"
                    width: Math.min(220, actionFormColumn.width)
                    textRole: "label"
                    valueRole: "id"
                    // 欠落した対象の action を選んでいるときは、その対象も候補に残す (選び直すまで)。
                    model: {
                        const list = (root.view.actionTargets || []).slice();
                        if (root.selectedAction.targetStatus === "missing")
                            list.push({id: root.selectedAction.partId, label: "(見つからない部分式)"});
                        return list;
                    }
                    currentIndex: {
                        const list = targetCombo.model || [];
                        for (let i = 0; i < list.length; ++i)
                            if (list[i].id === actionForm.targetId)
                                return i;
                        return -1;
                    }
                    onActivated: actionForm.targetId = targetCombo.currentValue
                }
                ModernDialogComboBox {
                    id: operationCombo
                    objectName: "equationActionOperation"
                    width: Math.min(180, actionFormColumn.width)
                    textRole: "label"
                    valueRole: "id"
                    model: [{id: "outline", label: "outline (囲み線)"}, {id: "pulse", label: "pulse (拡大と強調色)"}]
                    currentIndex: actionForm.operation === "pulse" ? 1 : 0
                    onActivated: actionForm.operation = operationCombo.currentValue
                }
            }
            Flow {
                Layout.fillWidth: true
                spacing: 8
                DragNumberField {
                    objectName: "equationActionStart"
                    width: 130
                    labelText: "開始"
                    inlineLabelWidth: 40
                    suffix: " f"
                    minimumValue: 0
                    maximumValue: Math.max(0, actionForm.hold - 1)
                    stepPerPixel: 0.5
                    value: actionForm.start
                    onValueEdited: newValue => actionForm.start = Math.round(newValue)
                }
                DragNumberField {
                    objectName: "equationActionDuration"
                    width: 130
                    labelText: "長さ"
                    inlineLabelWidth: 40
                    suffix: " f"
                    minimumValue: 1
                    maximumValue: Math.max(1, actionForm.hold)
                    stepPerPixel: 0.5
                    value: actionForm.duration
                    onValueEdited: newValue => actionForm.duration = Math.round(newValue)
                }
            }
            Label {
                objectName: "equationActionHint"
                Layout.fillWidth: true
                text: actionForm.fits
                      ? "hold " + actionForm.hold + "f の中に収まっています。同じ状態の強調は時間を重ねられません。"
                      : "開始 + 長さが hold " + actionForm.hold + "f を超えています。hold の中に収めてください。"
                color: actionForm.fits ? "#9aa2ad" : "#f2c66d"
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }
            Flow {
                Layout.fillWidth: true
                spacing: 6
                ModernDialogButton {
                    objectName: "equationAddAction"
                    visible: root.selectedAction.id === undefined
                    prominent: true
                    text: "強調を追加"
                    onClicked: root.editor.addAction(actionForm.targetId, actionForm.operation,
                                                     actionForm.start, actionForm.duration)
                }
                ModernDialogButton {
                    objectName: "equationUpdateAction"
                    visible: root.selectedAction.id !== undefined
                    prominent: true
                    text: "変更を適用"
                    onClicked: root.editor.updateSelectedAction(actionForm.targetId, actionForm.operation,
                                                                actionForm.start, actionForm.duration)
                }
                ModernDialogButton {
                    objectName: "equationSeekAction"
                    visible: root.selectedAction.id !== undefined
                    text: "再生位置を移動"
                    onClicked: root.editor.seekToSelectedAction()
                }
                ModernDialogButton {
                    objectName: "equationDeleteAction"
                    visible: root.selectedAction.id !== undefined
                    destructive: true
                    text: "削除"
                    onClicked: root.editor.deleteSelectedAction()
                }
                ModernDialogButton {
                    objectName: "equationNewAction"
                    visible: root.selectedAction.id !== undefined
                    text: "新しく追加"
                    onClicked: root.editor.selectAction("")
                }
            }
        }
    }
    Note {
        text: "強調は outline (囲み線) と pulse (拡大と強調色) だけです。位置・拡大・回転・フェードは下のエフェクトで調整できます。"
    }
}

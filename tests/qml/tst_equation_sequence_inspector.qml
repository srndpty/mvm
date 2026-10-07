import QtQuick
import QtQuick.Controls
import QtTest
import "../../apps/mvm" as App

// P3-5: EquationSequenceInspector が表示の値を読み、利用者の意図を editor へ渡すだけであることを
// 偽の editor で確かめる (Project の操作・変換は controller の責務)。
TestCase {
    id: test
    name: "EquationSequenceInspector"
    visible: true
    when: windowShown
    width: 640
    height: 900

    QtObject {
        id: controller
        property bool busy: false
        property bool playing: false
        property int retries: 0
        function retryMathRendering() { retries++; }
    }
    QtObject {
        id: fakeEditor
        property var view: ({})
        property var status: ({})
        property var calls: []
        property var lastArgs: []
        property bool commitResult: true
        property string committed: ""
        function record(name, args) {
            calls = calls.concat([name]);
            lastArgs = args;
        }
        function count(name) {
            let n = 0;
            for (let i = 0; i < calls.length; ++i)
                if (calls[i] === name)
                    ++n;
            return n;
        }
        function selectState(id) { record("selectState", [id]); }
        function selectPart(id) { record("selectPart", [id]); }
        function selectAction(id) { record("selectAction", [id]); }
        function insertState(after) { record("insertState", [after]); return true; }
        function deleteSelectedState() { record("deleteSelectedState", []); return true; }
        function moveSelectedState(delta) { record("moveSelectedState", [delta]); return true; }
        function setHoldFrames(frames) { record("setHoldFrames", [frames]); return true; }
        function setTransitionFrames(id, frames) { record("setTransitionFrames", [id, frames]); return true; }
        function beginSourceEdit(document) { record("beginSourceEdit", [document]); }
        function commitSourceEdit(text) {
            record("commitSourceEdit", [text]);
            committed = text;
            if (commitResult) {
                // 確定した Project の式を view へ反映する (controller と同じ)。
                const next = JSON.parse(JSON.stringify(view));
                next.state.source = text;
                view = next;
            }
            return {ok: commitResult, changed: true, discarded: false};
        }
        function cancelSourceEdit() { record("cancelSourceEdit", []); }
        function addPart(start, end, text, label) { record("addPart", [start, end, text, label]); return true; }
        function rebindSelectedPart(start, end, text) { record("rebindSelectedPart", [start, end, text]); return true; }
        function renameSelectedPart(label) { record("renameSelectedPart", [label]); return true; }
        function deleteSelectedPart() { record("deleteSelectedPart", []); return true; }
        function addCorrespondence(id, from, to) { record("addCorrespondence", [id, from, to]); return true; }
        function removeCorrespondence(id, from, to) { record("removeCorrespondence", [id, from, to]); return true; }
        function addAction(part, op, start, duration) { record("addAction", [part, op, start, duration]); return true; }
        function updateSelectedAction(part, op, start, duration) { record("updateSelectedAction", [part, op, start, duration]); return true; }
        function deleteSelectedAction() { record("deleteSelectedAction", []); return true; }
        function seekToSelectedAction() { record("seekToSelectedAction", []); return true; }
        function refreshStatus() { record("refreshStatus", []); }
    }

    function minimalView(source) {
        return {
            clipId: "clip1", clipName: "数式 sequence", stateCount: 1, fpsText: "60/1 fps",
            lengthText: "300f (5.00 秒)", message: "", messageError: false,
            states: [{id: "s0", index: 0, summary: source, holdFrames: 300, holdText: "300f (5.00 秒)",
                      partCount: 0, actionCount: 0, problem: false, selected: true}],
            state: {id: "s0", index: 0, source: source, holdFrames: 300, holdText: "300f (5.00 秒)",
                    canDelete: false, canMoveUp: false, canMoveDown: false, deleteActions: 0,
                    deleteTransitions: 0, deleteCorrespondence: 0, deleteJoinsNeighbors: false,
                    maximumFrames: 3600000},
            parts: [], selectedPart: ({}), actionTargets: [], transitions: [], actions: [],
            selectedAction: ({})
        };
    }

    Component {
        id: panelComponent
        App.BoundedFlickable {
            id: flick
            property alias inspector: inspector
            width: 320; height: 900; clip: true
            contentWidth: width; contentHeight: inspector.implicitHeight
            flickableDirection: Flickable.VerticalFlick
            App.EquationSequenceInspector {
                id: inspector
                width: parent.width
                mvmController: controller
                editor: fakeEditor
            }
        }
    }

    function init() {
        fakeEditor.calls = [];
        fakeEditor.lastArgs = [];
        fakeEditor.commitResult = true;
        fakeEditor.view = minimalView("α+b");
        fakeEditor.status = {compile: "ready", compileText: "準備完了", renderer: "available",
                         rendererText: "利用可能", artifact: "ready", artifactText: "完了",
                         residency: "static", residencyText: "静止だけ", preview: "static",
                         previewText: "状態 [0] の静止", category: "", headline: "", detail: ""};
    }

    function test_minimal_sequence_has_empty_states() {
        const panel = createTemporaryObject(panelComponent, test);
        verify(findChild(panel, "equationNoTransitions").visible, "変形が無いことを示す");
        verify(findChild(panel, "equationNoParts").visible, "部分式が無いことと作り方を示す");
        verify(findChild(panel, "equationNoActions").visible, "強調が無いことを示す");
        verify(findChild(panel, "equationLastStateNote").visible, "最後の状態は消せないと示す");
        verify(!findChild(panel, "equationDeleteState").enabled, "最後の状態の削除は押せない");
        verify(!findChild(panel, "equationActionForm").visible, "対象の無い強調の入力は出さない");
        compare(findChild(panel, "equationSourceEditor").text, "α+b");
    }

    // 範囲は編集欄の UTF-16 の選択をそのまま渡す (QML で byte に直さない)。
    function test_add_part_passes_utf16_selection() {
        const panel = createTemporaryObject(panelComponent, test);
        const source = findChild(panel, "equationSourceEditor");
        source.select(2, 3);
        mouseClick(findChild(panel, "equationAddPart"));
        compare(fakeEditor.count("addPart"), 1);
        compare(fakeEditor.lastArgs[0], 2);
        compare(fakeEditor.lastArgs[1], 3);
        compare(fakeEditor.lastArgs[2], "α+b");
    }

    // 入力は確定 1 回。Esc は取り消して元の式へ戻す (記録を止めてから戻す)。
    function test_source_session_commit_and_cancel() {
        const panel = createTemporaryObject(panelComponent, test);
        const source = findChild(panel, "equationSourceEditor");
        source.forceActiveFocus();
        verify(fakeEditor.count("beginSourceEdit") === 1, "focus で session を始める");
        verify(fakeEditor.lastArgs[0] !== undefined && fakeEditor.lastArgs[0] !== null,
               "編集欄の textDocument を渡す");
        source.cursorPosition = 0;
        keyClick(Qt.Key_X);
        keyClick(Qt.Key_Y);
        keyClick(Qt.Key_Z);
        compare(fakeEditor.count("commitSourceEdit"), 0, "入力中は確定しない");
        keyClick(Qt.Key_Return, Qt.ControlModifier);
        compare(fakeEditor.count("commitSourceEdit"), 1, "Ctrl+Enter で 1 回だけ確定");
        compare(fakeEditor.committed, "xyzα+b");
        // Esc: 取消。Project の値 (view) の式へ戻す。
        fakeEditor.calls = [];
        source.forceActiveFocus();
        keyClick(Qt.Key_Q);
        keyClick(Qt.Key_Escape);
        compare(fakeEditor.count("cancelSourceEdit") >= 1, true, "Esc で取り消す");
        compare(fakeEditor.count("commitSourceEdit"), 0, "Esc は確定しない");
        compare(source.text, "xyzα+b", "Esc で最後に確定した式へ戻す");
    }

    // 拒否された確定は入力を保持し、理由を示す。
    function test_rejected_commit_keeps_draft() {
        const panel = createTemporaryObject(panelComponent, test);
        const source = findChild(panel, "equationSourceEditor");
        fakeEditor.commitResult = false;
        source.forceActiveFocus();
        source.selectAll();
        keyClick(Qt.Key_Delete);
        keyClick(Qt.Key_Return, Qt.ControlModifier);
        compare(source.text, "", "拒否された入力を保持する");
        verify(findChild(panel, "equationSourceRejection").visible, "拒否を示す");
    }

    // 状態の一覧の Delete は状態も clip も消さない。上下は選択の移動で、並べ替えではない。
    function test_rejected_draft_blocks_state_operations_data() {
        const rows = [];
        for (const explicitCommit of [true, false]) {
            for (const operation of ["equationStateRow_1", "equationInsertBefore",
                                     "equationInsertAfter", "equationDeleteState",
                                     "equationDeleteStateConfirmButton"]) {
                rows.push({tag: operation + (explicitCommit ? "_明示拒否" : "_focus拒否"),
                           explicitCommit: explicitCommit, operation: operation});
            }
        }
        return rows;
    }
    function test_rejected_draft_blocks_state_operations(data) {
        const view = minimalView("x");
        view.state.canDelete = true;
        view.stateCount = 2;
        view.states.push({id: "s1", index: 1, summary: "y", holdFrames: 10,
                          problem: false, selected: false});
        fakeEditor.view = view;
        const panel = createTemporaryObject(panelComponent, test);
        tryVerify(() => findChild(panel, "equationStateRow_1") !== null);
        const source = findChild(panel, "equationSourceEditor");
        if (data.operation === "equationDeleteStateConfirmButton")
            mouseClick(findChild(panel, "equationDeleteState"));
        fakeEditor.commitResult = false;
        source.forceActiveFocus();
        source.text = "拒否された下書き";
        if (data.explicitCommit)
            keyClick(Qt.Key_Return, Qt.ControlModifier);
        const snapshot = JSON.stringify(fakeEditor.view);
        mouseClick(findChild(panel, data.operation));
        compare(source.text, "拒否された下書き", data.operation);
        compare(JSON.stringify(fakeEditor.view), snapshot, data.operation);
        compare(fakeEditor.count("selectState"), 0);
        compare(fakeEditor.count("insertState"), 0);
        compare(fakeEditor.count("deleteSelectedState"), 0);
        verify(source.activeFocus);
    }

    function test_state_list_keys() {
        const view = minimalView("x");
        view.stateCount = 2;
        view.states = [view.states[0], {id: "s1", index: 1, summary: "y", holdFrames: 10,
                                        holdText: "10f", partCount: 0, actionCount: 0,
                                        problem: false, selected: false}];
        view.state.canDelete = true;
        view.state.canMoveDown = true;
        fakeEditor.view = view;
        const panel = createTemporaryObject(panelComponent, test);
        const list = findChild(panel, "equationStateList");
        list.forceActiveFocus();
        keyClick(Qt.Key_Delete);
        compare(fakeEditor.count("deleteSelectedState"), 0, "Delete で状態を消さない");
        keyClick(Qt.Key_Down);
        compare(fakeEditor.count("selectState"), 1, "下で次の状態を選ぶ");
        compare(fakeEditor.lastArgs[0], "s1");
        compare(fakeEditor.count("moveSelectedState"), 0, "矢印で並べ替えない");
        // 削除は確認を経てだけ行う。
        mouseClick(findChild(panel, "equationDeleteState"));
        compare(fakeEditor.count("deleteSelectedState"), 0, "削除ボタンは確認を出すだけ");
        verify(findChild(panel, "equationDeleteStateConfirm").visible, "削除の影響を示す");
        mouseClick(findChild(panel, "equationDeleteStateConfirmButton"));
        compare(fakeEditor.count("deleteSelectedState"), 1, "確認して 1 回削除");
    }

    // 状態の問い合わせの polling は読むだけの refreshStatus しか呼ばない。
    function test_status_polling_is_read_only() {
        const panel = createTemporaryObject(panelComponent, test);
        wait(900);
        const refreshes = fakeEditor.count("refreshStatus");
        verify(refreshes >= 3, "polling が実際に問い合わせる (" + refreshes + ")");
        for (let i = 0; i < fakeEditor.calls.length; ++i)
            verify(fakeEditor.calls[i] === "refreshStatus" || fakeEditor.calls[i] === "cancelSourceEdit",
                   "polling は Project・描画の操作を呼ばない: " + fakeEditor.calls[i]);
    }

    function test_status_dimensions_and_headline() {
        fakeEditor.status = {compile: "ready", compileText: "準備完了", renderer: "available",
                         rendererText: "利用可能", artifact: "ready", artifactText: "完了",
                         residency: "over_budget", residencyText: "preview の memory 上限 (静止で代用)",
                         preview: "static_fallback", previewText: "状態 [0] の静止で代用",
                         category: "memory", headline: "preview の memory 上限のため静止で表示しています",
                         detail: "予算 1 byte"};
        const panel = createTemporaryObject(panelComponent, test);
        compare(findChild(panel, "equationStatusCompile").value, "準備完了");
        compare(findChild(panel, "equationStatusResidency").value, "preview の memory 上限 (静止で代用)");
        verify(findChild(panel, "equationStatusHeadline").visible);
        verify(!findChild(panel, "equationRetryButton").visible, "memory の上限に再試行を出さない");
        verify(!findChild(panel, "equationStatusDetail").visible, "生の詳細は既定で隠す");
    }

    // 修復: 欠落の部分式は「作り直す」、選択範囲をそのまま渡す。
    function test_missing_part_repair_intent() {
        const view = minimalView("a+bc");
        view.parts = [{id: "gone", label: "", displayLabel: "(削除済みの部分式)", expectedText: "",
                       status: "missing_target", statusText: "部分式が見つかりません", repairable: true,
                       exists: false, hasRange: false, rangeStart: -1, rangeEnd: -1,
                       rangeText: "今の式に範囲がありません", actionCount: 1, correspondenceCount: 0,
                       selected: true}];
        view.selectedPart = view.parts[0];
        fakeEditor.view = view;
        const panel = createTemporaryObject(panelComponent, test);
        const rebind = findChild(panel, "equationRebindPart");
        compare(rebind.text, "選択範囲で作り直す");
        verify(!findChild(panel, "equationDeletePart").visible, "欠落は削除の対象ではない");
        findChild(panel, "equationSourceEditor").select(2, 4);
        mouseClick(rebind);
        compare(fakeEditor.count("rebindSelectedPart"), 1);
        compare(fakeEditor.lastArgs[0], 2);
        compare(fakeEditor.lastArgs[1], 4);
    }
}

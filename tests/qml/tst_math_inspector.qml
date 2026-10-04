import QtQuick
import QtQuick.Controls
import QtTest
import "../../apps/mvm" as App

TestCase {
    id: test
    name: "MathInspector"
    visible: true
    when: windowShown
    width: 640
    height: 480
    QtObject {
        id: controller
        property var selectedMathClip: ({clipId: "math1", source: "x=1", fontSize: 96,
            color: "#FFFFFFFF", backgroundColor: "#00000000", state: "ready", log: "", toolchain: "Manim"})
        property bool busy: false
        property bool playing: false
        property int outputHeight: 1080
        property int commits: 0
        property int previews: 0
        property int retries: 0
        property string committed: ""
        property string committedId: ""
        property var lastValues: ({})
        function updateMathClip(id, values) {
            if (values.source !== undefined && !values.source.trim())
                return false;
            commits++; committed = values.source || ""; committedId = id; lastValues = values;
            return true;
        }
        function previewMathClip(id, values) { previews++; return true; }
        function cancelMathPreview() {}
        function retryMathRendering() { retries++; }
    }
    Component {
        id: panelComponent
        App.BoundedFlickable {
            width: 300; height: 200; clip: true
            contentWidth: width; contentHeight: inspector.implicitHeight
            flickableDirection: Flickable.VerticalFlick
            Rectangle { width: parent.width; height: inspector.implicitHeight; color: "#20242a" }
            App.MathClipInspector {
                id: inspector
                width: parent.width
                mvmController: controller
            }
        }
    }
    function init() {
        controller.commits = 0; controller.previews = 0; controller.retries = 0;
        controller.committedId = ""; controller.lastValues = {};
        controller.selectedMathClip = {clipId: "math1", source: "x=1", fontSize: 96,
            color: "#FFFFFFFF", backgroundColor: "#00000000", state: "ready", log: "", toolchain: "Manim",
            unavailableReason: "", canRetry: true, intro: "none", introFrames: 0, introSeconds: 0,
            introMaxSeconds: 5, writeState: "none", writeMessage: ""};
    }
    function test_write_toggle_and_duration() {
        const panel = createTemporaryObject(panelComponent, test, {height: 400});
        const toggle = findChild(panel, "mathWriteToggle");
        const seconds = findChild(panel, "mathWriteSeconds");
        verify(toggle && seconds);
        verify(!toggle.checked);
        verify(!seconds.visible);
        verify(!findChild(panel, "mathWriteState").visible);
        panel.contentY = toggle.mapToItem(panel.contentItem, 0, 0).y;
        wait(50);
        mouseClick(toggle);
        compare(controller.commits, 1);
        compare(controller.lastValues.intro, "write");
        // controller が Project の値を返すまでは、クリックで切れた binding を Project の値へ戻す。
        verify(!toggle.checked);
        controller.selectedMathClip = Object.assign({}, controller.selectedMathClip,
            {intro: "write", introFrames: 60, introSeconds: 1, writeState: "rendering"});
        verify(toggle.checked);
        verify(seconds.visible);
        const state = findChild(panel, "mathWriteState");
        verify(state.visible);
        verify(state.text.indexOf("描画中") >= 0);
        // 直接入力で 0.5 秒にする (ダブルクリックで入力欄へ)。
        panel.contentY = seconds.mapToItem(panel.contentItem, 0, 0).y;
        wait(50);
        mouseDoubleClickSequence(seconds);
        keySequence("ctrl+a");
        keyClick(Qt.Key_0); keyClick(Qt.Key_Period); keyClick(Qt.Key_5);
        keyClick(Qt.Key_Return);
        compare(controller.commits, 2);
        compare(controller.lastValues.introSeconds, 0.5);
        // 上限 (clip の尺) を超える入力は上限に収めて確定する。
        mouseDoubleClickSequence(seconds);
        keySequence("ctrl+a");
        keyClick(Qt.Key_9); keyClick(Qt.Key_9);
        keyClick(Qt.Key_Return);
        compare(controller.commits, 3);
        compare(controller.lastValues.introSeconds, 5);
        controller.selectedMathClip = Object.assign({}, controller.selectedMathClip,
            {writeState: "error", writeMessage: "Write の連番が memory の上限を超えます"});
        verify(state.text.indexOf("エラー") >= 0 && state.text.indexOf("memory") >= 0);
        // 書き出しには使えるが preview の memory に収まらない場合は、完了と理由を両方出す。
        controller.selectedMathClip = Object.assign({}, controller.selectedMathClip,
            {writeState: "ready", writeMessage: "", writePreview: "memory",
             writePreviewMessage: "Write の preview は memory の上限に収まらないため"});
        verify(state.text.indexOf("完了") >= 0 && state.text.indexOf("memory の上限") >= 0);
        controller.selectedMathClip = Object.assign({}, controller.selectedMathClip,
            {writePreview: "loading", writePreviewMessage: ""});
        verify(state.text.indexOf("preview を準備中") >= 0);
        // Write を外す。
        panel.contentY = toggle.mapToItem(panel.contentItem, 0, 0).y;
        wait(50);
        mouseClick(toggle);
        compare(controller.lastValues.intro, "none");
    }
    function test_write_disabled_while_draft_rejected() {
        const panel = createTemporaryObject(panelComponent, test);
        const editor = findChild(panel, "mathSourceEditor");
        editor.forceActiveFocus(); editor.text = "";
        controller.selectedMathClip = Object.assign({}, controller.selectedMathClip, {clipId: "math2", source: "y=4"});
        verify(findChild(panel, "mathDraftRejection").visible);
        verify(!findChild(panel, "mathWriteToggle").enabled);
        keyClick(Qt.Key_Escape);
        verify(findChild(panel, "mathWriteToggle").enabled);
    }
    function test_draft_survives_render_notification_and_cancel() {
        const panel = createTemporaryObject(panelComponent, test);
        verify(panel);
        const editor = findChild(panel, "mathSourceEditor");
        editor.forceActiveFocus();
        editor.text = "x=2";
        tryCompare(controller, "previews", 1, 1200);
        controller.selectedMathClip = Object.assign({}, controller.selectedMathClip, {source: "x=2", state: "stale"});
        compare(editor.text, "x=2");
        keyClick(Qt.Key_Escape);
        compare(editor.text, "x=1");
        compare(controller.commits, 0);
        wait(650);
        compare(controller.previews, 1);
    }
    function test_commit_once_and_retry() {
        const panel = createTemporaryObject(panelComponent, test);
        const editor = findChild(panel, "mathSourceEditor");
        editor.forceActiveFocus(); editor.text = "  \\fracc{a}{b}  ";
        keyClick(Qt.Key_Return, Qt.ControlModifier);
        compare(controller.commits, 1);
        compare(controller.committed.trim(), "\\fracc{a}{b}");
        const retry = findChild(panel, "mathRetryButton");
        panel.contentY = retry.mapToItem(panel.contentItem, 0, 0).y;
        wait(50);
        mouseClick(retry);
        compare(controller.retries, 1);
    }
    function test_selection_commits_old_draft() {
        const panel = createTemporaryObject(panelComponent, test);
        const editor = findChild(panel, "mathSourceEditor");
        editor.forceActiveFocus(); editor.text = "x=3";
        controller.selectedMathClip = Object.assign({}, controller.selectedMathClip, {clipId: "math2", source: "y=4"});
        compare(controller.commits, 1);
        compare(controller.committed, "x=3");
        compare(editor.text, "y=4");
        wait(650);
        compare(controller.previews, 0);
    }
    function test_rejected_selection_data() {
        return [{tag: "空欄を修正して旧 clip に確定", source: "", repair: true},
                {tag: "空白を Esc で明示的に取り消す", source: " \n\t", repair: false}];
    }
    function test_rejected_selection(data) {
        const panel = createTemporaryObject(panelComponent, test);
        const editor = findChild(panel, "mathSourceEditor");
        editor.forceActiveFocus(); editor.text = data.source;
        controller.selectedMathClip = Object.assign({}, controller.selectedMathClip, {clipId: "math2", source: "y=4"});
        compare(controller.commits, 0);
        compare(editor.text, data.source);
        verify(findChild(panel, "mathDraftRejection").visible);
        verify(!findChild(panel, "mathNumberField_fontSize").enabled);
        // 非同期の状態通知とフォーカスの移動でも、拒否した旧入力を上書きしない。
        controller.selectedMathClip = Object.assign({}, controller.selectedMathClip, {state: "rendering"});
        test.forceActiveFocus(); wait(700);
        compare(editor.text, data.source);
        compare(controller.previews, 0);
        editor.forceActiveFocus();
        if (data.repair) {
            editor.text = "x=5";
            keyClick(Qt.Key_Return, Qt.ControlModifier);
            compare(controller.commits, 1);
            compare(controller.committedId, "math1");
            compare(controller.committed, "x=5");
        } else {
            keyClick(Qt.Key_Escape);
            compare(controller.commits, 0);
        }
        compare(editor.text, "y=4");
        verify(!findChild(panel, "mathDraftRejection").visible);
        verify(findChild(panel, "mathNumberField_fontSize").enabled);
    }
    function test_unavailable_guidance_data() {
        return [{tag: "依存不足", reason: "backend", retry: true, guidance: true},
                {tag: "Project lock 未取得", reason: "authority", retry: false, guidance: false},
                {tag: "原因欠落を推測しない", reason: "", retry: false, guidance: false}];
    }
    function test_unavailable_guidance(data) {
        controller.selectedMathClip = Object.assign({}, controller.selectedMathClip,
            {state: "unavailable", unavailableReason: data.reason, canRetry: data.retry,
             message: "Manim / MiKTeX / 他のプロセス（表示文は判定に使わない）"});
        const panel = createTemporaryObject(panelComponent, test);
        compare(findChild(panel, "mathDependencyGuidance").visible, data.guidance);
        const retry = findChild(panel, "mathRetryButton");
        compare(retry.visible, data.retry);
        if (data.retry) {
            panel.contentY = retry.mapToItem(panel.contentItem, 0, 0).y;
            wait(50); mouseClick(retry);
            compare(controller.retries, 1);
        } else {
            compare(controller.retries, 0);
        }
    }
    function test_layout_data() {
        return [{tag: "通常", w: 360, h: 400, state: "ready"},
                {tag: "狭幅・低いパネル・長いエラー", w: 180, h: 100, state: "error"},
                {tag: "利用不可", w: 240, h: 120, state: "unavailable"}];
    }
    function test_layout(data) {
        controller.selectedMathClip = Object.assign({}, controller.selectedMathClip,
            {state: data.state, source: data.state === "ready" ? "ax^2 + bx + c = 0" : "x + ".repeat(100), message: "長い説明です。".repeat(40),
             log: "描画失敗\n".repeat(40), showingPrevious: true,
             unavailableReason: data.state === "unavailable" ? "backend" : "",
             intro: "write", introSeconds: 1.5, introMaxSeconds: 5,
             writeState: data.state === "ready" ? "ready" : "error",
             writeMessage: data.state === "ready" ? "" : "Write の長い説明です。".repeat(20)});
        const panel = createTemporaryObject(panelComponent, test, {width: data.w, height: data.h});
        verify(panel); wait(100);
        grabImage(panel).save("math-inspector-top-" + data.w + ".png");
        verify(panel.contentHeight > panel.height);
        if (data.state === "error") {
            const toggle = findChild(panel, "mathLogToggle");
            panel.contentY = toggle.mapToItem(panel.contentItem, 0, 0).y;
            wait(50);
            mouseClick(toggle);
            verify(findChild(panel, "mathRenderLog").visible);
            wait(100);
        }
        const notes = findChild(panel, "mathInspectorNotes");
        verify(notes.width <= panel.width);
        const seconds = findChild(panel, "mathWriteSeconds");
        verify(seconds.visible && seconds.width <= panel.width);
        verify(findChild(panel, "mathWriteState").width <= panel.width);
        panel.contentY = panel.contentHeight - panel.height;
        wait(50);
        const position = notes.mapToItem(panel, 0, notes.height);
        verify(position.y <= panel.height + 1);
        verify(position.y > 0);
        const rendered = grabImage(panel);
        verify(rendered.width > 0);
        rendered.save("math-inspector-" + data.w + ".png");
    }
}

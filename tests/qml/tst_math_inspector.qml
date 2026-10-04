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
        function updateMathClip(id, values) { commits++; committed = values.source || ""; return true; }
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
        controller.selectedMathClip = {clipId: "math1", source: "x=1", fontSize: 96,
            color: "#FFFFFFFF", backgroundColor: "#00000000", state: "ready", log: "", toolchain: "Manim"};
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
    function test_layout_data() {
        return [{tag: "通常", w: 360, h: 400, state: "ready"},
                {tag: "狭幅・低いパネル・長いエラー", w: 180, h: 100, state: "error"},
                {tag: "利用不可", w: 240, h: 120, state: "unavailable"}];
    }
    function test_layout(data) {
        controller.selectedMathClip = Object.assign({}, controller.selectedMathClip,
            {state: data.state, source: data.state === "ready" ? "ax^2 + bx + c = 0" : "x + ".repeat(100), message: "長い説明です。".repeat(40),
             log: "描画失敗\n".repeat(40), showingPrevious: true});
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

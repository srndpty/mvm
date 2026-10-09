import QtQuick
import QtQuick.Controls
import QtTest
import "../../apps/mvm" as App

TestCase {
    id: test
    name: "GraphInspector"
    visible: true
    when: windowShown
    width: 640
    height: 900
    QtObject {
        id: controller
        property bool busy: false
        property bool playing: false
        property var selectedGraphClip: ({})
        property var calls: []
        property bool accept: true
        property bool renderReady: false
        function editGraphFromUi(authority, id, operation, values) {
            calls = calls.concat([{authority: authority, id: id, operation: operation, values: values}]);
            return {ok: accept, message: accept ? "" : "有限の数値を入力してください"};
        }
        function graphStatusFromUi(id) { return {ready: renderReady, caption: "式の構文が不正です"}; }
    }
    Component {
        id: panel
        App.BoundedFlickable {
            clip: true
            contentWidth: width
            contentHeight: inspector.implicitHeight
            flickableDirection: Flickable.VerticalFlick
            App.GraphClipInspector {
                id: inspector
                width: parent.width
                mvmController: controller
            }
        }
    }
    function init() {
        failOnWarning(/.*/);
        controller.calls = [];
        controller.accept = true;
        controller.renderReady = false;
        controller.selectedGraphClip = {
            clipId: "graphA", generation: "1", revision: "1", xMin: -5, xMax: 5, yMin: -5, yMax: 25,
            showAxes: true, showGrid: true, xLabel: "x", yLabel: "y", draw: true, frames: 3,
            functions: [{id: "f1", expression: "sin(", label: "", color: "#8000FF55", strokeWidth: 3,
                         domainMin: "", domainMax: ""}]
        };
    }
    function test_layout_data() {
        return [{tag: "通常", w: 400, h: 800}, {tag: "狭い", w: 180, h: 800},
                {tag: "低い", w: 400, h: 160}, {tag: "狭く低い", w: 180, h: 160}];
    }
    function test_layout(data) {
        const item = createTemporaryObject(panel, test, {width: data.w, height: data.h});
        verify(item);
        wait(40);
        verify(item.contentHeight > item.height);
        for (let name of ["graphAddFunction", "graphDeleteFunction", "graphMoveFunctionUp",
                          "graphField_expression", "graphField_color", "graphField_frames", "graphPreviewStatus"]) {
            const control = findChild(item, name);
            verify(control, name);
            verify(control.visible, name);
            const point = control.mapToItem(item.contentItem, 0, 0);
            item.contentY = Math.min(Math.max(0, point.y), item.contentHeight - item.height);
            wait(10);
            const visiblePoint = control.mapToItem(item, 0, 0);
            verify(visiblePoint.x >= 0 && visiblePoint.x + control.width <= item.width + 1, name);
            verify(visiblePoint.y >= -1 && visiblePoint.y < item.height, name);
        }
    }
    function test_partialNumber() {
        const item = createTemporaryObject(panel, test, {width: 400, height: 800});
        const field = findChild(item, "graphField_xMin");
        field.forceActiveFocus();
        field.text = "1e";
        compare(controller.calls.length, 0);
        controller.accept = false;
        keyClick(Qt.Key_Return);
        compare(controller.calls.length, 1);
        compare(field.text, "1e");
        compare(controller.calls[0].authority.clipId, "graphA");
        const restored = JSON.parse(JSON.stringify(controller.selectedGraphClip));
        restored.xMin = -9;
        restored.revision = "2";
        controller.selectedGraphClip = restored;
        compare(field.text, "1e");
        keyClick(Qt.Key_Escape);
        compare(field.text, "-9");
    }
    function test_expressionIdentity() {
        const item = createTemporaryObject(panel, test, {width: 400, height: 800});
        const field = findChild(item, "graphField_expression");
        field.forceActiveFocus();
        field.text = "unknown(x)";
        keyClick(Qt.Key_Return);
        compare(controller.calls[0].id, "f1");
        compare(controller.calls[0].values.expression, "unknown(x)");
        compare(findChild(item, "graphField_color").text, "#8000FF55");
    }
    function sampleClip(clipId, functions, extra) {
        const clip = {
            clipId: clipId, generation: "1", revision: "1", xMin: -5, xMax: 5, yMin: -5, yMax: 25,
            showAxes: true, showGrid: true, xLabel: "x", yLabel: "y", draw: true, frames: 3,
            functions: functions
        };
        if (extra) {
            for (let key in extra)
                clip[key] = extra[key];
        }
        return clip;
    }
    function chooseFunction(item, id) {
        const field = findChild(item, "graphField_expression");
        if (field.activeFocus)
            field.focus = false;
        const button = findChild(item, "graphFunction_" + id);
        button.clicked();
        verify(button.text.indexOf("選択中") === 0);
    }
    function assertNoStale(expression, id, clipId) {
        for (let i = 0; i < controller.calls.length; ++i) {
            const call = controller.calls[i];
            if (!call.values || call.values.expression !== expression)
                continue;
            compare(call.id, id);
            compare(call.authority.clipId, clipId);
        }
    }
    function test_sharedControlsKeepCustomBehavior() {
        const item = createTemporaryObject(panel, test, {width: 400, height: 800});
        const field = findChild(item, "graphField_expression");
        const button = findChild(item, "graphAddFunction");
        const box = findChild(item, "graphShowAxes");
        compare(field.implicitHeight, 34);
        compare(field.leftPadding, 10);
        compare(field.rightPadding, 10);
        compare(field.background.radius, 4);
        compare(field.background.color, "#1d2127");
        field.forceActiveFocus();
        compare(field.background.border.color, "#6ca9e6");
        compare(field.background.border.width, 1);
        compare(button.implicitHeight, 31);
        compare(button.background.radius, 4);
        button.forceActiveFocus();
        compare(button.background.border.color, "#9bc8ff");
        compare(button.contentItem.color, "#f0f1f3");
        compare(box.implicitHeight, 30);
        compare(box.spacing, 8);
        compare(box.indicator.width, 18);
        compare(box.indicator.radius, 4);
        box.forceActiveFocus();
        compare(box.indicator.border.color, "#9bc8ff");
        const before = controller.calls.length;
        keyClick(Qt.Key_Space);
        compare(controller.calls.length, before + 1);
        compare(controller.calls[before].operation, "field");
        compare(controller.calls[before].values.showAxes, false);
        button.forceActiveFocus();
        keyClick(Qt.Key_Space);
        compare(controller.calls[before + 1].operation, "add");
        keyClick(Qt.Key_Return);
        compare(controller.calls.length, before + 2);
        field.forceActiveFocus();
        const callsBeforeEscape = controller.calls.length;
        field.text = "1/";
        keyClick(Qt.Key_Escape);
        compare(field.text, "sin(");
        compare(controller.calls.length, callsBeforeEscape);
    }
    function test_draftAcrossFunctions() {
        controller.accept = false;
        controller.selectedGraphClip = sampleClip("graphA", [
            {id: "f1", expression: "x^2", label: "", color: "#FF112233", strokeWidth: 3, domainMin: "", domainMax: ""},
            {id: "f2", expression: "sin(x)", label: "", color: "#FF445566", strokeWidth: 3, domainMin: "", domainMax: ""}
        ]);
        const item = createTemporaryObject(panel, test, {width: 400, height: 800});
        const field = findChild(item, "graphField_expression");
        field.forceActiveFocus();
        field.text = "sin(";
        keyClick(Qt.Key_Return);
        compare(controller.calls.length, 1);
        compare(controller.calls[0].id, "f1");
        compare(controller.calls[0].authority.clipId, "graphA");
        chooseFunction(item, "f2");
        compare(field.text, "sin(x)");
        assertNoStale("sin(", "f1", "graphA");
        chooseFunction(item, "f1");
        compare(field.text, "sin(");
        field.forceActiveFocus();
        keyClick(Qt.Key_Escape);
        compare(field.text, "x^2");
        chooseFunction(item, "f2");
        compare(field.text, "sin(x)");
        field.forceActiveFocus();
        field.text = "2*x";
        controller.accept = true;
        keyClick(Qt.Key_Return);
        const last = controller.calls[controller.calls.length - 1];
        compare(last.id, "f2");
        compare(last.authority.clipId, "graphA");
        compare(last.values.expression, "2*x");
        assertNoStale("sin(", "f1", "graphA");
    }
    function test_draftAcrossClips() {
        controller.accept = false;
        const item = createTemporaryObject(panel, test, {width: 400, height: 800});
        const field = findChild(item, "graphField_xMin");
        field.forceActiveFocus();
        field.text = "1e";
        keyClick(Qt.Key_Return);
        compare(controller.calls.length, 1);
        compare(controller.calls[0].authority.clipId, "graphA");
        compare(controller.calls[0].values.xMin, "1e");
        controller.selectedGraphClip = sampleClip("graphB", [
            {id: "g1", expression: "x", label: "", color: "#FFFFFFFF", strokeWidth: 2, domainMin: "", domainMax: ""}
        ], {xMin: 2, revision: "4"});
        compare(field.text, "2");
        compare(findChild(item, "graphField_expression").text, "x");
        keyClick(Qt.Key_Return);
        compare(controller.calls.length, 1);
        const expression = findChild(item, "graphField_expression");
        expression.forceActiveFocus();
        expression.text = "sin(";
        keyClick(Qt.Key_Return);
        compare(controller.calls[1].authority.clipId, "graphB");
        compare(controller.calls[1].id, "g1");
        compare(controller.calls[1].values.expression, "sin(");
        controller.selectedGraphClip = sampleClip("graphA", [
            {id: "f1", expression: "sin(", label: "", color: "#8000FF55", strokeWidth: 3, domainMin: "", domainMax: ""}
        ]);
        compare(field.text, "1e");
        field.forceActiveFocus();
        keyClick(Qt.Key_Escape);
        compare(field.text, "-5");
        for (let i = 0; i < controller.calls.length; ++i) {
            if (controller.calls[i].authority.clipId === "graphB")
                verify(controller.calls[i].values.xMin === undefined);
        }
    }
    function test_draftAcrossUndoRedo() {
        controller.accept = false;
        controller.selectedGraphClip = sampleClip("graphA", [
            {id: "f1", expression: "x^2", label: "", color: "#FF112233", strokeWidth: 3, domainMin: "", domainMax: ""},
            {id: "f2", expression: "sin(x)", label: "", color: "#FF445566", strokeWidth: 3, domainMin: "", domainMax: ""}
        ]);
        const item = createTemporaryObject(panel, test, {width: 400, height: 800});
        const field = findChild(item, "graphField_expression");
        field.forceActiveFocus();
        field.text = "sin(";
        keyClick(Qt.Key_Return);
        const callsAfterReject = controller.calls.length;
        const undone = sampleClip("graphA", [
            {id: "f1", expression: "x", label: "", color: "#FF112233", strokeWidth: 3, domainMin: "", domainMax: ""},
            {id: "f2", expression: "sin(x)", label: "", color: "#FF445566", strokeWidth: 3, domainMin: "", domainMax: ""}
        ], {revision: "2"});
        controller.selectedGraphClip = undone;
        compare(controller.calls.length, callsAfterReject);
        compare(field.text, "sin(");
        keyClick(Qt.Key_Return);
        compare(controller.calls[controller.calls.length - 1].authority.revision, "1");
        compare(controller.calls[controller.calls.length - 1].id, "f1");
        keyClick(Qt.Key_Escape);
        compare(field.text, "x");
        field.text = "1/";
        keyClick(Qt.Key_Return);
        compare(controller.calls[controller.calls.length - 1].authority.revision, "2");
        compare(controller.calls[controller.calls.length - 1].id, "f1");
        controller.selectedGraphClip = sampleClip("graphA", [
            {id: "f1", expression: "x^2", label: "", color: "#FF112233", strokeWidth: 3, domainMin: "", domainMax: ""},
            {id: "f2", expression: "sin(x)", label: "", color: "#FF445566", strokeWidth: 3, domainMin: "", domainMax: ""}
        ], {revision: "3"});
        compare(field.text, "1/");
        chooseFunction(item, "f2");
        compare(field.text, "sin(x)");
        assertNoStale("1/", "f1", "graphA");
        assertNoStale("sin(", "f1", "graphA");
        chooseFunction(item, "f1");
        compare(field.text, "1/");
        field.forceActiveFocus();
        keyClick(Qt.Key_Escape);
        compare(field.text, "x^2");
    }
    function test_currentStatus() {
        controller.renderReady = true;
        const item = createTemporaryObject(panel, test, {width: 400, height: 800});
        const status = findChild(item, "graphPreviewStatus");
        tryCompare(status, "text", "表示準備完了");
        controller.renderReady = false;
        const next = JSON.parse(JSON.stringify(controller.selectedGraphClip));
        next.revision = "2";
        controller.selectedGraphClip = next;
        verify(status.text !== "表示準備完了");
    }
}

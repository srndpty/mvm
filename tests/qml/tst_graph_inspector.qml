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

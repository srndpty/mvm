pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: root
    objectName: "graphClipInspector"
    required property var mvmController
    readonly property var view: mvmController.selectedGraphClip
    property string selectedFunctionId: ""
    property var wantedFunctions: ({})
    property var drafts: ({})
    readonly property var functions: view.functions || []
    readonly property var selectedFunction: {
        for (let row of functions) {
            if (row.id === selectedFunctionId)
                return row;
        }
        return {};
    }
    property string message: ""
    property var previewStatus: ({})
    readonly property string statusAuthorityKey: (view.clipId || "") + "/" +
                                                 (view.generation || "") + "/" + (view.revision || "")
    onStatusAuthorityKeyChanged: previewStatus = ({})
    spacing: 8
    enabled: !mvmController.busy && !mvmController.playing
    onFunctionsChanged: {
        const wanted = wantedFunctions[view.clipId];
        if (functions.some(row => row.id === wanted))
            selectedFunctionId = wanted;
        else if (!functions.some(row => row.id === selectedFunctionId))
            selectedFunctionId = functions.length ? functions[0].id : "";
    }
    function operate(operation, values) {
        const result = mvmController.editGraphFromUi(view, selectedFunctionId, operation, values);
        message = result.message;
        return result.ok;
    }
    component Heading: Label {
        Layout.fillWidth: true
        font.bold: true
        color: "#e6e8ec"
        font.pixelSize: 12
    }
    // 確定前の入力と authority を欄ごとに保持する。選択変更後の focus loss は旧対象を指す。
    component Field: ColumnLayout {
        id: field
        required property string caption
        required property string fieldKey
        property bool perFunction: false
        readonly property var source: perFunction ? root.selectedFunction : root.view
        readonly property string identity: (root.view.clipId || "") + "/" +
                                           (perFunction ? root.selectedFunctionId : "") + "/" +
                                           (root.view.generation || "") + "/" + fieldKey
        property string loadedIdentity: ""
        property var authority: ({})
        property string functionId: ""
        property string saved: ""
        property bool rejected: false
        Layout.fillWidth: true
        spacing: 3
        function synchronize(force) {
            if (!force && input.activeFocus && loadedIdentity === identity)
                return;
            if (!force && rejected && loadedIdentity === identity)
                return;
            if (loadedIdentity !== "" && loadedIdentity !== identity && input.text !== saved)
                root.drafts[loadedIdentity] = {text: input.text, authority: authority, functionId: functionId};
            loadedIdentity = identity;
            authority = root.view;
            functionId = perFunction ? root.selectedFunctionId : "";
            saved = source[fieldKey] === undefined ? "" : String(source[fieldKey]);
            input.text = saved;
            rejected = false;
            const draft = root.drafts[identity];
            if (draft) {
                input.text = draft.text;
                authority = draft.authority;
                functionId = draft.functionId;
                rejected = true;
            }
        }
        function commit() {
            if (input.text === saved)
                return;
            let values = {};
            values[fieldKey] = input.text;
            const result = root.mvmController.editGraphFromUi(authority, functionId, "field", values);
            root.message = result.message;
            rejected = !result.ok;
            if (result.ok) {
                saved = input.text;
                authority = root.view;
                delete root.drafts[loadedIdentity];
            }
        }
        onIdentityChanged: synchronize()
        onSourceChanged: synchronize()
        Component.onCompleted: synchronize()
        Label {
            Layout.fillWidth: true
            text: field.caption
            color: "#aeb6c2"
            font.pixelSize: 11
            wrapMode: Text.Wrap
        }
        ModernDialogField {
            id: input
            objectName: "graphField_" + field.fieldKey
            Layout.fillWidth: true
            onEditingFinished: field.commit()
            onActiveFocusChanged: {
                if (activeFocus) {
                    field.authority = root.view;
                    field.functionId = field.perFunction ? root.selectedFunctionId : "";
                }
            }
            Keys.onEscapePressed: {
                field.rejected = false;
                delete root.drafts[field.loadedIdentity];
                field.synchronize(true);
                root.message = "";
            }
        }
    }
    Heading { text: "グラフ" }
    Heading { text: "表示範囲" }
    GridLayout {
        Layout.fillWidth: true
        columns: root.width < 250 ? 1 : 2
        Field { caption: "X 最小"; fieldKey: "xMin" }
        Field { caption: "X 最大"; fieldKey: "xMax" }
        Field { caption: "Y 最小"; fieldKey: "yMin" }
        Field { caption: "Y 最大"; fieldKey: "yMax" }
    }
    Heading { text: "軸とラベル（TeX）" }
    ModernDialogCheckBox {
        objectName: "graphShowAxes"
        text: "軸を表示"
        checked: root.view.showAxes || false
        onClicked: root.operate("field", {showAxes: checked})
    }
    ModernDialogCheckBox {
        objectName: "graphShowGrid"
        text: "グリッドを表示"
        checked: root.view.showGrid || false
        onClicked: root.operate("field", {showGrid: checked})
    }
    Field { caption: "X ラベル"; fieldKey: "xLabel" }
    Field { caption: "Y ラベル"; fieldKey: "yLabel" }
    Heading { text: "関数（1〜3 個）" }
    Repeater {
        model: root.functions
        delegate: ModernDialogButton {
            required property var modelData
            required property int index
            Layout.fillWidth: true
            objectName: "graphFunction_" + modelData.id
            text: (root.selectedFunctionId === modelData.id ? "選択中：" : "") +
                  "関数 " + (index + 1) + "  " + modelData.expression
            onClicked: {
                root.wantedFunctions[root.view.clipId] = modelData.id;
                root.selectedFunctionId = modelData.id;
            }
        }
    }
    Flow {
        Layout.fillWidth: true
        spacing: 6
        ModernDialogButton {
            objectName: "graphAddFunction"
            text: "追加"
            enabled: root.functions.length < 3
            onClicked: root.operate("add", {})
        }
        ModernDialogButton {
            objectName: "graphDeleteFunction"
            text: "削除"
            enabled: root.functions.length > 1
            onClicked: root.operate("delete", {})
        }
        ModernDialogButton {
            objectName: "graphMoveFunctionUp"
            text: "前へ"
            enabled: root.functions.findIndex(row => row.id === root.selectedFunctionId) > 0
            onClicked: root.operate("move", {index: root.functions.findIndex(row => row.id === root.selectedFunctionId) - 1})
        }
        ModernDialogButton {
            objectName: "graphMoveFunctionDown"
            text: "後へ"
            enabled: root.functions.findIndex(row => row.id === root.selectedFunctionId) < root.functions.length - 1
            onClicked: root.operate("move", {index: root.functions.findIndex(row => row.id === root.selectedFunctionId) + 1})
        }
    }
    Field { caption: "式（例：x^2、sin(x)、2*x+1）"; fieldKey: "expression"; perFunction: true }
    Field { caption: "表示ラベル（TeX、空欄で非表示）"; fieldKey: "label"; perFunction: true }
    Field { caption: "色（#AARRGGBB、先頭 2 桁が透明度）"; fieldKey: "color"; perFunction: true }
    Field { caption: "線幅（基準 pixel、0 より大きく 64 以下）"; fieldKey: "strokeWidth"; perFunction: true }
    Field { caption: "定義域の最小（空欄で未指定）"; fieldKey: "domainMin"; perFunction: true }
    Field { caption: "定義域の最大（空欄で未指定）"; fieldKey: "domainMax"; perFunction: true }
    Heading { text: "アニメーション" }
    ModernDialogCheckBox {
        objectName: "graphDraw"
        text: "Draw（線を描く）"
        checked: root.view.draw || false
        onClicked: root.operate("field", {draw: checked})
    }
    Field { caption: "Draw frame 数（1〜10000、素材尺以下）"; fieldKey: "frames"; visible: root.view.draw || false }
    Label {
        Layout.fillWidth: true
        text: root.message
        visible: text.length > 0
        color: "#f2c66d"
        wrapMode: Text.Wrap
    }
    Timer {
        interval: 250
        repeat: true
        running: root.visible && !!root.view.clipId
        triggeredOnStart: true
        onTriggered: root.previewStatus = root.mvmController.graphStatusFromUi(root.view.clipId)
    }
    Label {
        objectName: "graphPreviewStatus"
        Layout.fillWidth: true
        color: root.previewStatus.ready ? "#a6d9b0" : "#f2c66d"
        wrapMode: Text.Wrap
        text: root.previewStatus.ready ? "表示準備完了" :
              (root.previewStatus.caption || "現在のグラフの表示を準備しています") +
              (root.previewStatus.message ? "\n" + root.previewStatus.message : "")
    }
}

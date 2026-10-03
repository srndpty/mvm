import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: inspector
    objectName: "keyframeInspector"
    required property var mvmController
    property bool lockAspect: true
    property string selectedChannel: "positionX"
    property var selectedFrames: []
    readonly property string selectionClip: mvmController.keyframeChannels.length ? (mvmController.keyframeChannels[0].clipId || "") : ""
    onSelectionClipChanged: selectedFrames = []
    function selectKey(name, frame, modifiers) {
        let frames = selectedChannel === name ? selectedFrames.slice() : [];
        if (modifiers & Qt.ControlModifier) {
            const at = frames.indexOf(frame);
            if (at >= 0) frames.splice(at, 1); else frames.push(frame);
        } else frames = [frame];
        selectedChannel = name;
        selectedFrames = frames;
    }
    function keepKeyFocus() {
        inspector.forceActiveFocus();
        Qt.callLater(() => inspector.forceActiveFocus());
    }
    function copyKeys(cut) {
        const result = mvmController.copyEffectKeys(selectedChannel, selectedFrames, cut);
        if (result && cut) { selectedFrames = []; keepKeyFocus(); }
        return result;
    }
    function pasteKeys() {
        const result = mvmController.pasteEffectKeys(selectedChannel);
        if (result) keepKeyFocus();
        return result;
    }
    function deleteKeys() {
        if (!mvmController.deleteEffectKeys(selectedChannel, selectedFrames)) return false;
        selectedFrames = [];
        keepKeyFocus();
        return true;
    }
    property var marquee: null
    property var marqueeBefore: []
    property string marqueeChannel: ""
    function beginSelection(area, x, y, modifiers) {
        const name = area.channelName;
        marqueeBefore = selectedFrames.slice();
        marqueeChannel = selectedChannel;
        const base = selectedChannel === name && (modifiers & (Qt.ControlModifier | Qt.ShiftModifier)) ? selectedFrames.slice() : [];
        selectedChannel = name;
        selectedFrames = base;
        marquee = {area: area, name: name, startX: x, startY: y, x: x, y: y, width: 0, height: 0, base: base};
    }
    function updateSelection(area, x, y) {
        const old = marquee;
        if (!old) return;
        const box = {area: old.area, name: old.name, startX: old.startX, startY: old.startY,
                     x: Math.min(old.startX, x), y: Math.min(old.startY, y),
                     width: Math.abs(x - old.startX), height: Math.abs(y - old.startY), base: old.base};
        const frames = old.base.slice();
        for (const key of area.channel.keys) {
            const px = key.frame * area.ppf;
            const py = area.graphArea ? area.graphArea.valueY(key.value) : area.height / 2;
            if (px >= box.x && px <= box.x + box.width && py >= box.y && py <= box.y + box.height && frames.indexOf(key.frame) < 0) frames.push(key.frame);
        }
        selectedFrames = frames;
        marquee = box;
    }
    function cancelSelection() {
        if (!marquee) return;
        selectedChannel = marqueeChannel;
        selectedFrames = marqueeBefore;
        marquee = null;
    }
    Component {
        id: selectionArea
        MouseArea {
            anchors.fill: parent
            enabled: parent.channel ? parent.channel.editable : false
            preventStealing: true
            onPressed: mouse => { inspector.beginSelection(parent, mouse.x, mouse.y, mouse.modifiers); forceActiveFocus(); }
            onPositionChanged: mouse => { if (pressed) inspector.updateSelection(parent, mouse.x, mouse.y); }
            onReleased: inspector.marquee = null
            onCanceled: inspector.cancelSelection()
            Keys.onEscapePressed: inspector.cancelSelection()
            Rectangle {
                visible: inspector.marquee !== null && inspector.marquee.area === parent.parent
                x: inspector.marquee ? inspector.marquee.x : 0
                y: inspector.marquee ? inspector.marquee.y : 0
                width: inspector.marquee ? inspector.marquee.width : 0
                height: inspector.marquee ? inspector.marquee.height : 0
                color: "#304b729a"
                border.color: "#62b6ff"
                border.width: 1
            }
        }
    }
    function navigateKey(direction) {
        const channels = mvmController.keyframeChannels;
        let name = selectedChannel;
        if (!channels.some(c => c.name === name && c.animated)) {
            const fallback = channels.find(c => c.animated);
            if (!fallback) return;
            name = fallback.name;
        }
        mvmController.seekEffectKey(name, direction);
    }
    spacing: 2
    Repeater {
        model: ["positionX", "positionY", "scaleX", "scaleY", "rotation", "cropLeft", "cropRight", "cropTop", "cropBottom", "opacity", "volume"]
        delegate: ColumnLayout {
            id: row
            required property string modelData
            property bool graphOpen: false
            property var channel: {
                const channels = inspector.mvmController.keyframeChannels;
                for (const entry of channels) if (entry.name === modelData) return entry;
                return null;
            }
            Layout.fillWidth: true
            visible: channel !== null
            spacing: 2
            RowLayout {
                Layout.fillWidth: true
                ToolButton {
                    objectName: "animation_" + row.modelData
                    Layout.preferredWidth: 24
                    Layout.preferredHeight: 24
                    padding: 3
                    contentItem: Canvas {
                        property bool active: row.channel ? row.channel.animated : false
                        onActiveChanged: requestPaint()
                        onPaint: {
                            const c = getContext("2d"); c.reset();
                            c.strokeStyle = active ? "#62b6ff" : "#bdc6d2"; c.lineWidth = 1.6;
                            c.beginPath(); c.arc(width / 2, height / 2 + 2, 6, 0, 2 * Math.PI); c.stroke();
                            c.beginPath(); c.moveTo(width / 2 - 3, 2); c.lineTo(width / 2 + 3, 2);
                            c.moveTo(width / 2, 2); c.lineTo(width / 2, 5);
                            c.moveTo(width / 2, height / 2 - 2); c.lineTo(width / 2, height / 2 + 2);
                            c.lineTo(width / 2 + 3, height / 2 + 2); c.stroke();
                        }
                    }
                    background: Rectangle { color: parent.checked ? "#244762" : "#282d35"; radius: 3 }
                    checkable: true
                    checked: row.channel ? row.channel.animated : false
                    enabled: row.channel ? row.channel.editable : false
                    ToolTip.visible: hovered
                    ToolTip.text: "アニメーションを切り替える"
                    onClicked: { inspector.selectedChannel = row.modelData; inspector.mvmController.setEffectAnimation(row.modelData, !row.channel.animated); }
                }
                ToolButton {
                    objectName: "graphToggle_" + row.modelData
                    visible: row.channel && row.channel.animated
                    Layout.preferredWidth: 18
                    Layout.preferredHeight: 24
                    padding: 0
                    contentItem: Item {
                        ChevronIcon {
                            anchors.centerIn: parent
                            width: 9
                            height: 9
                            direction: row.graphOpen ? "down" : "right"
                        }
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: "カーブを表示・編集"
                    onClicked: { inspector.selectedChannel = row.modelData; row.graphOpen = !row.graphOpen; }
                }
                DragNumberField {
                    id: numberField
                    Layout.fillWidth: true
                    objectName: "value_" + row.modelData
                    clickToEdit: true
                    inlineLabelWidth: 65
                    Layout.preferredHeight: 24
                    labelText: row.channel ? row.channel.label : ""
                    value: row.channel ? row.channel.value : 0
                    minimumValue: row.channel ? row.channel.minimum : 0
                    maximumValue: row.channel ? row.channel.maximum : 100
                    suffix: row.modelData === "rotation" ? " °" : " %"
                    enabled: row.channel ? row.channel.editable : false
                    MouseArea {
                        objectName: "channelLabel_" + row.modelData
                        x: 0; y: 0; width: numberField.inlineLabelWidth; height: numberField.height
                        preventStealing: true
                        onClicked: {
                            if (inspector.selectedChannel !== row.modelData) inspector.selectedFrames = [];
                            inspector.selectedChannel = row.modelData;
                            numberField.forceActiveFocus();
                        }
                    }
                    onEditCanceled: inspector.mvmController.cancelEffectPreview()
                    onValueEdited: (value, commit) => {
                        if (inspector.lockAspect && (row.modelData === "scaleX" || row.modelData === "scaleY")) {
                            const other = row.modelData === "scaleX" ? "scaleY" : "scaleX";
                            let otherValue = value;
                            for (const entry of inspector.mvmController.keyframeChannels)
                                if (entry.name === other) otherValue = row.channel.value === 0 ? value : entry.value * value / row.channel.value;
                            const values = {};
                            values[row.modelData] = value;
                            values[other] = otherValue;
                            inspector.mvmController.setEffectValues(values, commit);
                        } else inspector.mvmController.setEffectValue(row.modelData, value, commit);
                    }
                }
                ToolButton {
                    objectName: "previous_" + row.modelData
                    Layout.preferredWidth: 24
                    Layout.preferredHeight: 24
                    padding: 0
                    contentItem: Item {
                        ChevronIcon {
                            anchors.centerIn: parent
                            width: 8
                            height: 10
                            direction: "left"
                            color: parent.parent.enabled ? "#c8cbd1" : "#5b616b"
                        }
                    }
                    enabled: row.channel && row.channel.animated
                    ToolTip.visible: hovered
                    ToolTip.text: "前のキーへ移動"
                    onClicked: inspector.mvmController.seekEffectKey(row.modelData, -1)
                }
                ToolButton {
                    objectName: "key_" + row.modelData
                    Layout.preferredWidth: 24
                    Layout.preferredHeight: 24
                    padding: 0
                    text: row.channel && row.channel.atKey ? "◆" : "◇"
                    enabled: row.channel ? row.channel.editable : false
                    ToolTip.visible: hovered
                    ToolTip.text: "現在位置のキーを追加・削除"
                    onClicked: inspector.mvmController.toggleEffectKey(row.modelData)
                }
                ToolButton {
                    objectName: "next_" + row.modelData
                    Layout.preferredWidth: 24
                    Layout.preferredHeight: 24
                    padding: 0
                    contentItem: Item {
                        ChevronIcon {
                            anchors.centerIn: parent
                            width: 8
                            height: 10
                            direction: "right"
                            color: parent.parent.enabled ? "#c8cbd1" : "#5b616b"
                        }
                    }
                    enabled: row.channel && row.channel.animated
                    ToolTip.visible: hovered
                    ToolTip.text: "次のキーへ移動"
                    onClicked: inspector.mvmController.seekEffectKey(row.modelData, 1)
                }
            }
            Rectangle {
                id: lane
                objectName: "lane_" + row.modelData
                Layout.fillWidth: true
                implicitHeight: 18
                color: "#20262f"
                visible: row.channel && row.channel.animated
                property real pixelsPerFrame: width / Math.max(1, row.channel ? row.channel.duration - 1 : 1)
                MouseArea {
                    objectName: "seekLane_" + row.modelData
                    anchors.fill: parent
                    enabled: row.channel ? row.channel.editable : false
                    preventStealing: true
                    property real pressX: 0
                    property bool scrubbing: false
                    function frameAt(x) {
                        return Math.max(0, Math.min(row.channel.duration - 1, Math.round(x / lane.pixelsPerFrame)));
                    }
                    function finishScrub() {
                        if (scrubbing) inspector.mvmController.endScrub();
                        scrubbing = false;
                    }
                    onPressed: mouse => {
                        if (inspector.selectedChannel !== row.modelData) inspector.selectedFrames = [];
                        inspector.selectedChannel = row.modelData;
                        forceActiveFocus();
                        pressX = mouse.x;
                        scrubbing = false;
                        inspector.mvmController.seekEffectFrame(frameAt(mouse.x));
                    }
                    onPositionChanged: mouse => {
                        if (!pressed) return;
                        if (!scrubbing && Math.abs(mouse.x - pressX) >= 3) {
                            scrubbing = true;
                            inspector.mvmController.beginScrub();
                        }
                        if (scrubbing) inspector.mvmController.seekEffectFrame(frameAt(mouse.x), true);
                    }
                    onReleased: finishScrub()
                    onCanceled: finishScrub()
                    Keys.onEscapePressed: finishScrub()
                }
                Rectangle {
                    x: Math.max(0, Math.min(parent.width - 1, (row.channel ? row.channel.frame : 0) * lane.pixelsPerFrame))
                    width: 1
                    height: parent.height
                    color: "#62b6ff"
                }
                Repeater {
                    model: row.channel ? row.channel.keys.length : 0
                    delegate: Item {
                        id: keyItem
                        objectName: "laneKey_" + row.modelData + "_" + index
                        required property int index
                        property var keyData: row.channel.keys[index]
                        property int originalFrame: 0
                        property int requestedFrame: 0
                        property real originX: 0
                        property bool editCanceled: false
                        x: keyData.frame * lane.pixelsPerFrame - 6
                        width: 12
                        height: lane.height
                        Label {
                            anchors.centerIn: parent
                            text: "◆"
                            color: inspector.selectedChannel === row.modelData && inspector.selectedFrames.indexOf(keyItem.keyData.frame) >= 0 ? "#62b6ff" : "#d1d7df"
                            font.pixelSize: 12
                        }
                        MouseArea {
                            anchors.fill: parent
                            enabled: row.channel.editable
                            preventStealing: true
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onPressed: mouse => {
                                if (mouse.button !== Qt.RightButton || inspector.selectedChannel !== row.modelData || inspector.selectedFrames.indexOf(keyItem.keyData.frame) < 0)
                                    inspector.selectKey(row.modelData, keyItem.keyData.frame, mouse.modifiers);
                                if (mouse.button === Qt.RightButton) { interpolationMenu.popup(); return; }
                                keyItem.editCanceled = false;
                                keyItem.originalFrame = keyItem.keyData.frame;
                                keyItem.requestedFrame = keyItem.originalFrame;
                                keyItem.originX = mapToItem(lane, mouse.x, mouse.y).x;
                                keyItem.forceActiveFocus();
                            }
                            onPositionChanged: mouse => {
                                if (!(pressedButtons & Qt.LeftButton) || keyItem.editCanceled) return;
                                const delta = mapToItem(lane, mouse.x, mouse.y).x - keyItem.originX;
                                const frame = Math.round(keyItem.originalFrame + delta / lane.pixelsPerFrame);
                                if (inspector.mvmController.moveEffectKey(row.modelData, keyItem.originalFrame, frame, false)) {
                                    keyItem.requestedFrame = frame;
                                    inspector.selectedFrames = [frame];
                                }
                            }
                            onReleased: mouse => {
                                if (mouse.button !== Qt.LeftButton || keyItem.editCanceled) return;
                                if (keyItem.requestedFrame === keyItem.originalFrame) { inspector.mvmController.cancelEffectPreview(); return; }
                                if (!inspector.mvmController.moveEffectKey(row.modelData, keyItem.originalFrame, keyItem.requestedFrame, true)) inspector.mvmController.cancelEffectPreview();
                                else inspector.selectedFrames = [keyItem.requestedFrame];
                            }
                            onCanceled: { inspector.selectedFrames = [keyItem.originalFrame]; inspector.mvmController.cancelEffectPreview(); }
                        }
                        Keys.onEscapePressed: { keyItem.editCanceled = true; inspector.selectedFrames = [keyItem.originalFrame]; inspector.mvmController.cancelEffectPreview(); }
                        Menu {
                            id: interpolationMenu
                            MenuItem { text: "キーをコピー"; onTriggered: inspector.copyKeys(false) }
                            MenuItem { text: "キーをカット"; onTriggered: inspector.copyKeys(true) }
                            MenuItem { text: "キーをペースト"; onTriggered: inspector.pasteKeys() }
                            MenuItem { text: "キーを削除"; onTriggered: inspector.deleteKeys() }
                            MenuSeparator {}
                            Repeater {
                                model: ["リニア", "イーズイン", "イーズアウト", "イーズインアウト", "スプライン"]
                                MenuItem {
                                    required property int index
                                    required property string modelData
                                    text: modelData
                                    checkable: true
                                    checked: keyItem.keyData.interpolation === index
                                    onTriggered: inspector.mvmController.setEffectInterpolation(row.modelData, keyItem.keyData.frame, index)
                                }
                            }
                        }
                    }
                }
            }
            Rectangle {
                id: graph
                objectName: "graph_" + row.modelData
                visible: row.graphOpen && row.channel && row.channel.animated
                Layout.fillWidth: true
                implicitHeight: 120
                color: "#191f27"
                clip: true
                property var keys: row.channel ? row.channel.keys : []
                property real low: {
                    if (editing) return fixedLow;
                    let v = keys.map(k => k.value);
                    return v.length ? Math.min.apply(null, v) - Math.max(1, (Math.max.apply(null, v) - Math.min.apply(null, v)) * 0.2) : 0;
                }
                property real high: {
                    if (editing) return fixedHigh;
                    let v = keys.map(k => k.value);
                    return v.length ? Math.max.apply(null, v) + Math.max(1, (Math.max.apply(null, v) - Math.min.apply(null, v)) * 0.2) : 100;
                }
                property bool editing: false
                property real fixedLow: 0
                property real fixedHigh: 100
                property real ppf: width / Math.max(1, row.channel ? row.channel.duration - 1 : 1)
                function valueY(value) { return height - (value - low) * height / (high - low); }
                Canvas {
                    id: curve
                    anchors.fill: parent
                    property var curveKeys: graph.keys
                    property var selection: inspector.selectedFrames
                    onSelectionChanged: requestPaint()
                    onCurveKeysChanged: requestPaint()
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                    onPaint: {
                        const c = getContext("2d"); c.reset();
                        c.strokeStyle = "#303946"; c.lineWidth = 1;
                        for (let n = 1; n < 4; ++n) {
                            c.beginPath(); c.moveTo(0, height * n / 4); c.lineTo(width, height * n / 4); c.stroke();
                        }
                        if (!curveKeys.length) return;
                        c.strokeStyle = "#62b6ff"; c.lineWidth = 1.5; c.beginPath();
                        c.moveTo(0, graph.valueY(curveKeys[0].value));
                        for (const k of curveKeys) {
                            c.lineTo(k.frame * graph.ppf, graph.valueY(k.value));
                            for (const sample of (k.samples || [])) c.lineTo(sample.frame * graph.ppf, graph.valueY(sample.value));
                        }
                        c.lineTo(width, graph.valueY(curveKeys[curveKeys.length - 1].value)); c.stroke();
                        c.strokeStyle = "#47627c"; c.lineWidth = 1;
                        if (inspector.selectedChannel === row.modelData) {
                            for (let i = 0; i + 1 < curveKeys.length; ++i) {
                                const k = curveKeys[i], next = curveKeys[i + 1];
                                if (selection.indexOf(k.frame) < 0) continue;
                                c.beginPath(); c.moveTo(k.frame * graph.ppf, graph.valueY(k.value));
                                c.lineTo((k.frame + (next.frame - k.frame) / 3) * graph.ppf, graph.valueY(k.value + (next.value - k.value) * k.control1)); c.stroke();
                                c.beginPath(); c.moveTo(next.frame * graph.ppf, graph.valueY(next.value));
                                c.lineTo((k.frame + (next.frame - k.frame) * 2 / 3) * graph.ppf, graph.valueY(k.value + (next.value - k.value) * k.control2)); c.stroke();
                            }
                        }
                        c.fillStyle = "#8995a4"; c.font = "10px sans-serif";
                        c.fillText(graph.high.toFixed(1), 5, 11); c.fillText(graph.low.toFixed(1), 5, height - 4);
                    }
                }
                Rectangle {
                    x: Math.max(0, Math.min(graph.width - 1, (row.channel ? row.channel.frame : 0) * graph.ppf))
                    width: 1; height: parent.height; color: "#62b6ff"; opacity: 0.6
                }
                Loader {
                    objectName: "selectionGraph_" + row.modelData
                    anchors.fill: parent
                    property var channel: row.channel
                    property string channelName: row.modelData
                    property real ppf: graph.ppf
                    property var graphArea: graph
                    sourceComponent: selectionArea
                }
                Repeater {
                    model: graph.keys.length
                    delegate: Rectangle {
                        id: node
                        objectName: "graphKey_" + row.modelData + "_" + index
                        required property int index
                        property var key: graph.keys[index]
                        property int originalFrame: 0
                        property int targetFrame: 0
                        property real targetValue: 0
                        property real originalValue: 0
                        property bool canceled: false
                        x: key.frame * graph.ppf - 4
                        y: graph.valueY(key.value) - 4
                        width: 8; height: 8; radius: 4
                        color: inspector.selectedChannel === row.modelData && inspector.selectedFrames.indexOf(key.frame) >= 0 ? "#62b6ff" : "#d1d7df"
                        MouseArea {
                            anchors.fill: parent
                            enabled: row.channel.editable
                            preventStealing: true
                            onPressed: mouse => {
                                inspector.selectKey(row.modelData, node.key.frame, mouse.modifiers);
                                node.originalFrame = node.key.frame; node.targetFrame = node.key.frame; node.targetValue = node.key.value; node.originalValue = node.key.value;
                                graph.fixedLow = graph.low; graph.fixedHigh = graph.high; graph.editing = true;
                                node.canceled = false; node.forceActiveFocus();
                            }
                            onPositionChanged: mouse => {
                                if (!pressed || node.canceled) return;
                                const p = mapToItem(graph, mouse.x, mouse.y);
                                const f = Math.max(0, Math.min(row.channel.duration - 1, Math.round(p.x / graph.ppf)));
                                const v = Math.max(row.channel.minimum, Math.min(row.channel.maximum, graph.low + (graph.height - p.y) / graph.height * (graph.high - graph.low)));
                                if (inspector.mvmController.editEffectKey(row.modelData, node.originalFrame, f, v, false)) {
                                    node.targetFrame = f; node.targetValue = v;
                                    inspector.selectedFrames = [f];
                                }
                            }
                            onReleased: {
                                if (node.targetFrame === node.originalFrame && node.targetValue === node.originalValue) { inspector.mvmController.cancelEffectPreview(); graph.editing = false; return; }
                                if (!node.canceled && inspector.mvmController.editEffectKey(row.modelData, node.originalFrame, node.targetFrame, node.targetValue, true)) inspector.selectedFrames = [node.targetFrame];
                                else inspector.mvmController.cancelEffectPreview();
                                graph.editing = false;
                            }
                            onCanceled: { inspector.selectedFrames = [node.originalFrame]; inspector.mvmController.cancelEffectPreview(); graph.editing = false; }
                        }
                        Keys.onEscapePressed: { canceled = true; inspector.selectedFrames = [originalFrame]; inspector.mvmController.cancelEffectPreview(); graph.editing = false; }
                    }
                }
                Repeater {
                    model: Math.max(0, graph.keys.length - 1)
                    delegate: Item {
                        id: segment
                        anchors.fill: parent
                        required property int index
                        property var first: graph.keys[index]
                        property var next: graph.keys[index + 1]
                        visible: inspector.selectedChannel === row.modelData && inspector.selectedFrames.indexOf(first.frame) >= 0 && first.value !== next.value
                        Repeater {
                            model: 2
                            delegate: Rectangle {
                                id: handle
                                objectName: "curveHandle_" + row.modelData + "_" + segment.index + "_" + index
                                required property int index
                                property real control: index === 0 ? segment.first.control1 : segment.first.control2
                                property real pendingControl: control
                                property bool canceled: false
                                x: (segment.first.frame + (segment.next.frame - segment.first.frame) * (index + 1) / 3) * graph.ppf - 4
                                y: graph.valueY(segment.first.value + (segment.next.value - segment.first.value) * control) - 4
                                width: 8; height: 8; radius: 4
                                color: "#191f27"; border.color: "#62b6ff"; border.width: 2
                                MouseArea {
                                    anchors.fill: parent
                                    enabled: row.channel.editable
                                    preventStealing: true
                                    cursorShape: Qt.SizeVerCursor
                                    onPressed: { handle.canceled = false; handle.pendingControl = handle.control; handle.forceActiveFocus(); }
                                    onPositionChanged: mouse => {
                                        if (!pressed || handle.canceled) return;
                                        const p = mapToItem(graph, mouse.x, mouse.y);
                                        const v = graph.low + (graph.height - p.y) / graph.height * (graph.high - graph.low);
                                        const normalized = Math.max(0, Math.min(1, (v - segment.first.value) / (segment.next.value - segment.first.value)));
                                        const c1 = handle.index === 0 ? normalized : segment.first.control1;
                                        const c2 = handle.index === 1 ? normalized : segment.first.control2;
                                        if (inspector.mvmController.setEffectSpline(row.modelData, segment.first.frame, c1, c2, false)) handle.pendingControl = normalized;
                                    }
                                    onReleased: {
                                        if (!handle.canceled) inspector.mvmController.setEffectSpline(row.modelData, segment.first.frame, handle.index === 0 ? handle.pendingControl : segment.first.control1, handle.index === 1 ? handle.pendingControl : segment.first.control2, true);
                                    }
                                    onCanceled: inspector.mvmController.cancelEffectPreview()
                                }
                                Keys.onEscapePressed: { canceled = true; inspector.mvmController.cancelEffectPreview(); }
                            }
                        }
                    }
                }
            }
        }
    }
    CheckBox {
        text: "縦横比を固定"
        spacing: 6
        indicator: Rectangle {
            x: 0; y: (parent.height - height) / 2
            width: 16; height: 16; radius: 4
            color: parent.checked ? "#357fb6" : "#252b34"
            border.color: parent.checked ? "#62b6ff" : "#667381"
            Canvas {
                anchors.fill: parent
                visible: inspector.lockAspect
                onPaint: {
                    const c = getContext("2d"); c.reset(); c.strokeStyle = "#edf6ff"; c.lineWidth = 1.7;
                    c.beginPath(); c.moveTo(4, 8); c.lineTo(7, 11); c.lineTo(12, 5); c.stroke();
                }
            }
        }
        contentItem: Label { text: parent.text; color: "#d1d7df"; leftPadding: 22; verticalAlignment: Text.AlignVCenter }
        checked: inspector.lockAspect
        visible: inspector.mvmController.keyframeChannels.length > 1
        onToggled: inspector.lockAspect = checked
    }
}

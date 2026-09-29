pragma ComponentBehavior: Bound
import QtQuick
import "PreviewTransform.js" as Transform

// プレビュー上で、選択中の画像・動画を動かす・拡縮する枠 (Photoshop の自由変形に相当)。
//   本体をドラッグ : 移動。出力の端・中央と他の素材の端・中央へ吸着する (Ctrl で吸着しない)
//   8 つのハンドル : 拡縮。反対側を固定する (Ctrl で縦横比を保つ、Alt で中心を固定する)
// 計算は PreviewTransform.js、効果の値への換算は controller (effectsForVisualRect) が行う。
// ドラッグ中は preview だけを更新し、離したときに 1 つの undo として確定する。
Item {
    id: overlay
    required property MvmController mvmController
    // 選択ツールで、文字の編集中でないときだけ操作を受ける。
    property bool active: true
    // 吸着しているガイド線 (出力画素)。文字のドラッグも同じ線をここへ出す。
    property var guides: ({ "xs": [], "ys": [] })

    readonly property real outputWidth: Math.max(1, mvmController.outputWidth)
    readonly property real outputHeight: Math.max(1, mvmController.outputHeight)
    readonly property real toHostX: width / outputWidth
    readonly property real toHostY: height / outputHeight
    // 画面上で 8px 以内なら吸着する。拡縮の最小は出力の 4px。
    readonly property real snapThreshold: Transform.thresholdOutputPx(8, outputWidth, width)
    readonly property real minimumSize: 4

    readonly property string clipId: mvmController.transformClipId
    readonly property var geometry: {
        // 効果の値 (ドラッグ中の preview を含む)・再生位置が変わると stateChanged が来る。
        mvmController.playheadFrame;
        mvmController.effectPositionX;
        return overlay.clipId !== "" ? mvmController.clipVisualGeometry(overlay.clipId) : ({});
    }
    readonly property bool shown: active && geometry.visible === true
    // 回転中はハンドルで拡縮しない (拡縮の軸が画面の軸と一致しないため)。移動はできる。
    readonly property bool resizable: shown && geometry.rotation === 0

    // ドラッグの状態。mode は "move" かハンドル名。
    property string mode: ""
    property string dragClipId: ""
    property var startRect: null
    property var startBounds: null
    property point startPointer
    property point grabOffset
    property var lines: null
    property var lastValues: null

    function pointerAt(item, x, y) {
        const point = item.mapToItem(overlay, x, y);
        return Qt.point(point.x / toHostX, point.y / toHostY);
    }

    function rectOf(geometryMap) {
        return { "x": geometryMap.x, "y": geometryMap.y,
                 "width": geometryMap.width, "height": geometryMap.height };
    }

    function begin(nextMode, id, pointer) {
        const current = mvmController.clipVisualGeometry(id);
        if (current.x === undefined)
            return false;
        mode = nextMode;
        dragClipId = id;
        startRect = rectOf(current);
        startBounds = Transform.rotatedBounds(startRect, current.pivotX, current.pivotY,
                                              current.rotation);
        startPointer = pointer;
        lines = Transform.snapLines(outputWidth, outputHeight, mvmController.previewSnapRects(id));
        lastValues = null;
        return true;
    }

    function apply(rect, guidesX, guidesY) {
        guides = { "xs": guidesX, "ys": guidesY };
        const values = mvmController.effectsForVisualRect(dragClipId, rect.x, rect.y,
                                                          rect.width, rect.height);
        if (values.positionX === undefined)
            return;
        if (mvmController.setEffectValues(values, false))
            lastValues = values;
    }

    // 動かしていなければ (クリックだけなら) undo を積まない。
    function finish(commit) {
        if (mode !== "" && lastValues !== null) {
            if (commit)
                mvmController.setEffectValues(lastValues, true);
            else
                mvmController.cancelEffectPreview();
        }
        mode = "";
        dragClipId = "";
        lastValues = null;
        guides = { "xs": [], "ys": [] };
    }

    // 本体の移動。当たり判定は controller が上の track から行い、画像・動画のときだけ受ける。
    // 文字や何も無い場所は下 (文字の delegate) へ流す。
    MouseArea {
        id: bodyArea
        anchors.fill: parent
        enabled: overlay.active
        acceptedButtons: Qt.LeftButton
        preventStealing: true
        onPressed: mouse => {
            const pointer = overlay.pointerAt(bodyArea, mouse.x, mouse.y);
            const hit = overlay.mvmController.visualClipAt(pointer.x, pointer.y);
            if (hit === "" || overlay.mvmController.clipVisualGeometry(hit).x === undefined) {
                mouse.accepted = false;
                return;
            }
            // selectClip は clip の先頭へ seek するので使わない。選択だけを変える。
            overlay.mvmController.selectTimelineClips([hit]);
            if (!overlay.begin("move", hit, pointer))
                mouse.accepted = false;
        }
        onPositionChanged: mouse => {
            if (overlay.mode !== "move")
                return;
            const pointer = overlay.pointerAt(bodyArea, mouse.x, mouse.y);
            const snapped = Transform.snapMove(overlay.startBounds, pointer.x - overlay.startPointer.x,
                                               pointer.y - overlay.startPointer.y, overlay.lines,
                                               overlay.snapThreshold,
                                               !(mouse.modifiers & Qt.ControlModifier));
            const start = overlay.startRect;
            overlay.apply({ "x": start.x + snapped.dx, "y": start.y + snapped.dy,
                            "width": start.width, "height": start.height },
                          snapped.guidesX, snapped.guidesY);
        }
        onReleased: overlay.finish(true)
        onCanceled: overlay.finish(false)
    }

    // 枠と中心。回転していれば枠も同じだけ回す。
    Item {
        id: frame
        visible: overlay.shown
        x: overlay.shown ? overlay.geometry.x * overlay.toHostX : 0
        y: overlay.shown ? overlay.geometry.y * overlay.toHostY : 0
        width: overlay.shown ? overlay.geometry.width * overlay.toHostX : 0
        height: overlay.shown ? overlay.geometry.height * overlay.toHostY : 0
        transform: Rotation {
            origin.x: overlay.shown ? overlay.geometry.pivotX * overlay.toHostX - frame.x : 0
            origin.y: overlay.shown ? overlay.geometry.pivotY * overlay.toHostY - frame.y : 0
            angle: overlay.shown ? overlay.geometry.rotation : 0
        }

        Rectangle {
            anchors.fill: parent
            color: "transparent"
            border.color: "#4a90e2"
            border.width: 1
        }
        // 中心の目印 (円と十字)。
        Rectangle {
            x: frame.width / 2 - width / 2
            y: frame.height / 2 - height / 2
            width: 12
            height: 12
            radius: 6
            color: "transparent"
            border.color: "#4a90e2"
            border.width: 1
        }
        Rectangle {
            x: frame.width / 2 - 8
            y: frame.height / 2
            width: 16
            height: 1
            color: "#4a90e2"
        }
        Rectangle {
            x: frame.width / 2
            y: frame.height / 2 - 8
            width: 1
            height: 16
            color: "#4a90e2"
        }
    }

    // 8 つのハンドル (四隅と四辺)。
    Repeater {
        model: Transform.handles()
        delegate: Rectangle {
            id: handle
            required property var modelData
            readonly property string name: modelData.name
            visible: overlay.resizable
            x: overlay.resizable ? (overlay.geometry.x + overlay.geometry.width * modelData.fx)
                                   * overlay.toHostX - width / 2 : 0
            y: overlay.resizable ? (overlay.geometry.y + overlay.geometry.height * modelData.fy)
                                   * overlay.toHostY - height / 2 : 0
            width: 9
            height: 9
            radius: 4.5
            color: "white"
            border.color: "#4a90e2"
            border.width: 1

            MouseArea {
                id: handleArea
                // 掴みやすいよう、見た目より少し広く取る。
                anchors.fill: parent
                anchors.margins: -4
                enabled: overlay.resizable
                acceptedButtons: Qt.LeftButton
                preventStealing: true
                cursorShape: handle.name === "tl" || handle.name === "br" ? Qt.SizeFDiagCursor
                             : handle.name === "tr" || handle.name === "bl" ? Qt.SizeBDiagCursor
                             : handle.name === "l" || handle.name === "r" ? Qt.SizeHorCursor
                             : Qt.SizeVerCursor
                onPressed: mouse => {
                    const pointer = overlay.pointerAt(handleArea, mouse.x, mouse.y);
                    if (!overlay.begin(handle.name, overlay.clipId, pointer)) {
                        mouse.accepted = false;
                        return;
                    }
                    // ハンドルの中心からずれた位置を掴んでも、辺が指へ跳ばないようにする。
                    const start = overlay.startRect;
                    overlay.grabOffset = Qt.point(
                        start.x + start.width * handle.modelData.fx - pointer.x,
                        start.y + start.height * handle.modelData.fy - pointer.y);
                }
                onPositionChanged: mouse => {
                    if (overlay.mode !== handle.name)
                        return;
                    const pointer = overlay.pointerAt(handleArea, mouse.x, mouse.y);
                    const options = {
                        "keepAspect": (mouse.modifiers & Qt.ControlModifier) !== 0,
                        "fromCenter": (mouse.modifiers & Qt.AltModifier) !== 0
                    };
                    const target = { "x": pointer.x + overlay.grabOffset.x,
                                     "y": pointer.y + overlay.grabOffset.y };
                    const resized = Transform.resizeRect(overlay.startRect, handle.name, target,
                                                         options, overlay.minimumSize);
                    const snapped = Transform.snapResize(overlay.startRect, resized, handle.name,
                                                         overlay.lines, overlay.snapThreshold,
                                                         options, overlay.minimumSize);
                    overlay.apply(snapped.rect, snapped.guidesX, snapped.guidesY);
                }
                onReleased: overlay.finish(true)
                onCanceled: overlay.finish(false)
            }
        }
    }

    // 吸着しているガイド線。
    Repeater {
        model: overlay.guides.xs
        delegate: Rectangle {
            required property real modelData
            x: modelData * overlay.toHostX
            y: 0
            width: 1
            height: overlay.height
            color: "#ff3fd0"
        }
    }
    Repeater {
        model: overlay.guides.ys
        delegate: Rectangle {
            required property real modelData
            x: 0
            y: modelData * overlay.toHostY
            width: overlay.width
            height: 1
            color: "#ff3fd0"
        }
    }
}

pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "TransitionEditorMath.js" as SpanMath

// 選択中のトランジション (クロスディゾルブ / クロスフェード / 数式の変形) のエフェクトコントロール。
// 長さと配置を数値欄・配置の選択・ミニタイムラインのドラッグで変える。
// ドラッグ中は表示だけを追従させ (Project は変えない)、離したときに setTransitionSpan を
// 1 回だけ呼ぶ (Undo 1 回分)。素材 frame に乗らない長さは controller が最も近い長さへ吸着させる。
// ミニタイムラインの A / B はリップルトリム、cut 線はローリング編集 (Premiere と同じ)。
// 再生ヘッドは timeline と同じ playheadFrame で、ルーラーを押すとそこへ scrub する。
ColumnLayout {
    id: root
    objectName: "transitionInspector"
    required property MvmController mvmController
    readonly property var transition: root.mvmController.selectedTransition
    readonly property bool hasTransition: root.transition.transitionId !== undefined
    readonly property bool editable: root.hasTransition && !root.mvmController.busy
    readonly property bool isMathTransform: root.transition.kind === "math_transform"
    property bool transformLogExpanded: false

    readonly property int cut: root.transition.cut ?? 0
    readonly property int committedBefore: root.transition.framesBeforeCut ?? 0
    readonly property int committedAfter: root.transition.framesAfterCut ?? 0
    readonly property int maxBefore: root.transition.maxBefore ?? 0
    readonly property int maxAfter: root.transition.maxAfter ?? 0

    // ドラッグ中の仮の値。dragging でなければ確定済みの値を出す。
    property bool dragging: false
    property int ghostBefore: 0
    property int ghostAfter: 0
    readonly property int shownBefore: root.dragging ? root.ghostBefore : root.committedBefore
    readonly property int shownAfter: root.dragging ? root.ghostAfter : root.committedAfter
    readonly property string alignment: SpanMath.alignmentOf(root.shownBefore, root.shownAfter)

    // ミニタイムラインに出す範囲。cut を中央にし、確定済みの長さの 2 倍を左右に取る。
    // ドラッグ中に倍率が変わると掴んだ位置がずれるので、ゴーストでは変えない。
    readonly property int viewRadius: Math.max(root.committedBefore, root.committedAfter, 5) * 2
    readonly property int viewStart: root.cut - root.viewRadius
    readonly property int viewEnd: root.cut + root.viewRadius

    // A / B / cut 線のドラッグ中の編集 (SpanMath.edgeEditFor の gesture) と、確定と同じ規則
    // (clampEdgeDrag) で止めた移動量。表示だけを動かし、離したときに 1 回だけ確定する。
    property string edgeGesture: ""
    property int edgeDelta: 0
    readonly property var edgeEdit: SpanMath.edgeEditFor(root.edgeGesture)
    function shownShift(key) {
        return root.edgeEdit ? root.edgeEdit[key] * root.edgeDelta : 0;
    }
    // cut 線とトランジションは B の先頭に付けて描く (A のリップル・ローリングでは A の終端と同じ)。
    readonly property int shownCut: root.cut + root.shownShift("incomingStartShift")
    readonly property int shownOutgoingEnd: root.cut + root.shownShift("outgoingEndShift")
    readonly property int shownIncomingEnd: (root.transition.incomingEnd ?? 0)
                                            + root.shownShift("incomingEndShift")

    // 長さの入力単位。既定は秒で、フレームへ切り替えられる。秒は timeline の fps で frame に丸める。
    property bool frameUnit: false
    readonly property int fpsNum: root.mvmController.timelineFpsNum
    readonly property int fpsDen: root.mvmController.timelineFpsDen
    function secondsOf(frames) {
        return SpanMath.framesToSeconds(frames, root.fpsNum, root.fpsDen);
    }

    spacing: 6

    function showGhost(span) {
        root.ghostBefore = span.before;
        root.ghostAfter = span.after;
        root.dragging = true;
    }

    // keepTotal: 長さ・配置・本体のドラッグは総尺を保って吸着させる。片側の端のドラッグは false
    // (動かした側だけを吸着させる)。
    function commitSpan(span, keepTotal) {
        root.dragging = false;
        if (span.before === root.committedBefore && span.after === root.committedAfter)
            return;
        root.mvmController.setTransitionSpan(span.before, span.after, keepTotal);
    }

    function edgeClipId(edit) {
        return edit.side === "outgoing" ? root.transition.outgoingClipId
                                        : root.transition.incomingClipId;
    }

    function previewEdgeEdit(gesture, frames) {
        const edit = SpanMath.edgeEditFor(gesture);
        root.edgeDelta = root.mvmController.clampEdgeDrag(root.edgeClipId(edit), edit.edge, edit.tool,
                                                          frames, true);
        root.edgeGesture = gesture;
    }

    function cancelEdgeEdit() {
        root.edgeGesture = "";
        root.edgeDelta = 0;
    }

    // controller の呼び出しで selectedTransition が変わるので、値は先に取り出す。
    function commitEdgeEdit() {
        const edit = root.edgeEdit;
        const delta = root.edgeDelta;
        const clipId = edit ? root.edgeClipId(edit) : "";
        root.cancelEdgeEdit();
        if (!edit || delta === 0)
            return;
        if (edit.tool === "rolling")
            root.mvmController.rollClipEdge(clipId, edit.edge, delta, true);
        else
            root.mvmController.rippleTrimClip(clipId, edit.edge, delta, true);
    }

    Label {
        Layout.fillWidth: true
        text: root.isMathTransform ? "数式の変形"
                                   : root.transition.trackKind === "audio" ? "クロスフェード"
                                                                          : "クロスディゾルブ"
        color: "#e6e8ec"
        font.bold: true
        font.pixelSize: 12
    }

    // 数式の変形: disk の描画 (書き出しが使う) の状態と、preview 用の mask を memory に置けたかを
    // 分けて示す。使えない間の preview は前後の式を cut で切り替える。
    Label {
        objectName: "mathTransformState"
        Layout.fillWidth: true
        visible: root.isMathTransform
        text: (({checking: "変形: 準備中", rendering: "変形: 描画中（cut で表示）", ready: "変形: 完了",
                 error: "変形: エラー（cut で表示）", unavailable: "変形: 利用不可（cut で表示）"})
               [root.transition.transformState] || "変形: 準備中")
              + (root.transition.transformPreview === "loading" ? "（preview を準備中）" : "")
        color: root.transition.transformState === "error"
               || root.transition.transformState === "unavailable"
               || root.transition.transformPreview === "memory" ? "#f2c66d" : "#a8d5a2"
        wrapMode: Text.Wrap
    }
    // 描画の失敗・backend の不在の理由。
    Label {
        objectName: "mathTransformMessage"
        Layout.fillWidth: true
        visible: root.isMathTransform && text.length > 0
        text: root.transition.transformMessage || ""
        color: "#f2c66d"
        wrapMode: Text.Wrap
    }
    // 描画は済んでいるが、preview の memory の上限で cut で表示している (書き出しには影響しない)。
    Label {
        objectName: "mathTransformPreviewMemory"
        Layout.fillWidth: true
        visible: root.isMathTransform && root.transition.transformPreview === "memory"
        text: root.transition.transformPreviewMessage || ""
        color: "#f2c66d"
        wrapMode: Text.Wrap
    }
    MathDependencyGuidance {
        objectName: "mathTransformDependencyGuidance"
        visible: root.isMathTransform && root.transition.transformUnavailableReason === "backend"
    }
    Flow {
        Layout.fillWidth: true
        visible: root.isMathTransform
        spacing: 6
        ModernDialogButton {
            objectName: "mathTransformRetryButton"
            visible: root.transition.transformCanRetry === true
            text: "再試行"
            onClicked: root.mvmController.retryMathRendering()
        }
        ModernDialogButton {
            objectName: "mathTransformLogToggle"
            visible: (root.transition.transformLog || "").length > 0
            text: root.transformLogExpanded ? "ログを閉じる" : "ログを表示"
            onClicked: root.transformLogExpanded = !root.transformLogExpanded
        }
    }
    ModernDialogTextArea {
        objectName: "mathTransformLog"
        Layout.fillWidth: true
        Layout.preferredHeight: implicitHeight
        visible: root.isMathTransform && root.transformLogExpanded && text.length > 0
        text: root.transition.transformLog || ""
        readOnly: true
        font.family: "Consolas"
    }

    GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: 6
        rowSpacing: 4
        enabled: root.editable

        DragNumberField {
            id: durationField
            objectName: "transitionDurationField"
            Layout.fillWidth: true
            labelText: "長さ"
            readonly property int frames: root.shownBefore + root.shownAfter
            readonly property int maxFrames: Math.max(1, root.maxBefore + root.maxAfter)
            // 単位は横のボタンが示すので、欄には数字だけを出す。
            decimals: root.frameUnit ? 0 : 2
            value: root.frameUnit ? durationField.frames : root.secondsOf(durationField.frames)
            minimumValue: root.frameUnit ? 1 : root.secondsOf(1)
            maximumValue: root.frameUnit ? durationField.maxFrames : root.secondsOf(durationField.maxFrames)
            stepPerPixel: root.frameUnit ? 0.25 : 0.005
            // 1 回のクリックで直接入力にする (ドラッグせずに離したとき)。
            clickToEdit: true
            onValueEdited: (newValue, commit) => {
                const frames = root.frameUnit
                    ? Math.round(newValue)
                    : SpanMath.secondsToFrames(newValue, root.fpsNum, root.fpsDen);
                const span = SpanMath.spanForDuration(
                    frames, SpanMath.alignmentOf(root.committedBefore, root.committedAfter),
                    root.committedBefore, root.committedAfter, root.maxBefore, root.maxAfter);
                if (commit)
                    root.commitSpan(span, true);
                else
                    root.showGhost(span);
            }
            onEditCanceled: root.dragging = false
        }

        // 単位の切り替え。押すと秒 ⇔ フレーム。
        Button {
            objectName: "transitionDurationUnit"
            Layout.alignment: Qt.AlignBottom
            Layout.preferredHeight: 26
            flat: true
            text: root.frameUnit ? "フレーム" : "秒"
            font.pixelSize: 11
            onClicked: root.frameUnit = !root.frameUnit
        }

        // もう一方の単位と timecode を並べて出す。
        Label {
            Layout.columnSpan: 2
            text: (root.frameUnit
                   ? root.secondsOf(durationField.frames).toFixed(2) + " 秒"
                   : durationField.frames + " f")
                  + "  ·  " + (root.transition.durationText ?? "")
            color: "#9aa2ad"
            font.pixelSize: 10
        }

        Label {
            Layout.columnSpan: 2
            text: "配置"
            color: "#9aa2ad"
            font.pixelSize: 10
        }

        ComboBox {
            id: alignmentBox
            objectName: "transitionAlignmentBox"
            Layout.columnSpan: 2
            Layout.fillWidth: true
            textRole: "text"
            valueRole: "value"
            // カスタムは長さをドラッグで決めたときの表示専用。選んでも何もしない。
            model: [
                { "value": "center", "text": "中央" },
                { "value": "start", "text": "cut で開始" },
                { "value": "end", "text": "cut で終了" },
                { "value": "custom", "text": "カスタム" }
            ]
            currentIndex: alignmentBox.indexOfValue(root.alignment)
            onActivated: index => {
                const chosen = alignmentBox.valueAt(index);
                // 選ぶと currentIndex の binding が外れるので、表示を確定値へ結び直す。
                alignmentBox.currentIndex = Qt.binding(() => alignmentBox.indexOfValue(root.alignment));
                if (chosen === "custom")
                    return;
                root.commitSpan(SpanMath.spanForAlignment(chosen, root.committedBefore,
                                                          root.committedAfter, root.maxBefore,
                                                          root.maxAfter), true);
            }
        }
    }

    // --- ミニタイムライン ---
    // 上から ルーラー / A (outgoing) / トランジション / B (incoming)。
    // A は cut の後ろへ、B は cut の前へ素材を延ばして使う。使っている延長を斜線で、
    // 延ばせる上限までを薄く描く。
    Item {
        id: lane
        objectName: "transitionMiniTimeline"
        Layout.fillWidth: true
        Layout.topMargin: 4
        implicitHeight: 104

        readonly property real rulerHeight: 24
        readonly property real rowHeight: 22
        readonly property real rowGap: 4
        readonly property real rowAY: lane.rulerHeight + lane.rowGap
        readonly property real rowTY: lane.rowAY + lane.rowHeight + lane.rowGap
        readonly property real rowBY: lane.rowTY + lane.rowHeight + lane.rowGap
        readonly property real framesPerPixel: (root.viewEnd - root.viewStart) / Math.max(1, lane.width)

        function xAt(frame) {
            return (frame - root.viewStart) / Math.max(1, root.viewEnd - root.viewStart) * lane.width;
        }
        function frameAt(x) {
            return Math.round(root.viewStart + x * lane.framesPerPixel);
        }
        // [from, to) を範囲に収めた x と幅。範囲外なら幅 0。
        function spanX(from, to) {
            const left = lane.xAt(Math.max(from, root.viewStart));
            const right = lane.xAt(Math.min(to, root.viewEnd));
            return { "x": left, "width": Math.max(0, right - left) };
        }

        Rectangle {
            anchors.fill: parent
            color: "#15181d"
            border.color: "#343840"
            radius: 3
        }

        // ルーラー (と、clip の無い所) を押すとそこへ scrub する (timeline のルーラーと同じ)。
        MouseArea {
            id: scrubArea
            anchors.fill: parent
            enabled: root.hasTransition && !root.mvmController.busy
            onPressed: mouse => {
                root.mvmController.beginScrub();
                root.mvmController.scrubToFrame(Math.max(0, lane.frameAt(mouse.x)));
            }
            onPositionChanged: mouse => {
                if (pressed)
                    root.mvmController.scrubToFrame(Math.max(0, lane.frameAt(mouse.x)));
            }
            onReleased: root.mvmController.endScrub()
            onCanceled: root.mvmController.endScrub()
        }

        // ルーラー。目盛りは frame 単位まで細かくなり、文字の目盛りに timecode を書く (Premiere と同じ)。
        Rectangle {
            objectName: "transitionMiniRuler"
            x: 0
            y: 0
            width: lane.width
            height: lane.rulerHeight
            color: "#20242b"
            radius: 3
            clip: true

            Repeater {
                model: root.hasTransition
                       ? SpanMath.rulerTicks(root.viewStart, root.viewEnd, lane.width,
                                             root.fpsNum / Math.max(1, root.fpsDen), 96)
                       : []
                Item {
                    id: tick
                    required property var modelData
                    x: lane.xAt(tick.modelData.frame)
                    width: 1
                    height: lane.rulerHeight

                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: 1
                        height: tick.modelData.major ? 9 : 4
                        color: tick.modelData.major ? "#8a919c" : "#5a616c"
                    }
                    Label {
                        visible: tick.modelData.major
                        x: 3
                        y: 1
                        text: root.mvmController.frameTimecode(tick.modelData.frame)
                        color: "#aab1ba"
                        font.pixelSize: 10
                    }
                }
            }
        }

        // A: cut までの clip と、cut の後ろへ延ばした素材。
        TransitionClipBar {
            objectName: "transitionMiniClipA"
            lane: lane
            inspector: root
            gesture: "rippleA"
            y: lane.rowAY
            clipFrom: root.transition.outgoingStart ?? 0
            clipTo: root.shownOutgoingEnd
            usedFrom: root.shownOutgoingEnd
            usedTo: root.shownOutgoingEnd + root.shownAfter
            availableFrom: root.shownOutgoingEnd + root.shownAfter
            availableTo: root.shownOutgoingEnd + root.maxAfter
            title: "A  " + (root.transition.outgoingName ?? "")
        }

        // B: cut からの clip と、cut の前へ延ばした素材。
        TransitionClipBar {
            objectName: "transitionMiniClipB"
            lane: lane
            inspector: root
            gesture: "rippleB"
            y: lane.rowBY
            clipFrom: root.shownCut
            clipTo: root.shownIncomingEnd
            usedFrom: root.shownCut - root.shownBefore
            usedTo: root.shownCut
            availableFrom: root.shownCut - root.maxBefore
            availableTo: root.shownCut - root.shownBefore
            title: "B  " + (root.transition.incomingName ?? "")
        }

        // cut 線を掴むとローリング編集。トランジションの行はトランジションのドラッグを優先する
        // (下で重ねる)。
        EdgeEditArea {
            objectName: "transitionMiniCutLine"
            lane: lane
            inspector: root
            gesture: "roll"
            x: lane.xAt(root.shownCut) - 4
            y: lane.rowAY
            width: 9
            height: lane.rowBY + lane.rowHeight - lane.rowAY
            cursorShape: Qt.SplitHCursor
        }

        // トランジション本体。左端・右端・本体をドラッグして長さと位置を変える。
        Rectangle {
            id: transitionBar
            objectName: "transitionMiniBar"
            readonly property var span: lane.spanX(root.shownCut - root.shownBefore,
                                                   root.shownCut + root.shownAfter)
            x: transitionBar.span.x
            y: lane.rowTY
            width: Math.max(2, transitionBar.span.width)
            height: lane.rowHeight
            radius: 2
            color: "#c0e0b040"
            border.color: "#ffe08a"

            // 左下から右上への斜線 (timeline のトランジションと同じ)。
            Canvas {
                anchors.fill: parent
                onPaint: {
                    const context = getContext("2d");
                    context.reset();
                    context.strokeStyle = "#fff3cf";
                    context.lineWidth = 1;
                    context.beginPath();
                    context.moveTo(0, height);
                    context.lineTo(width, 0);
                    context.stroke();
                }
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
            }

            TransitionDragHandle {
                lane: lane
                inspector: root
                handle: "body"
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                cursorShape: Qt.SizeAllCursor
            }
            TransitionDragHandle {
                lane: lane
                inspector: root
                handle: "left"
                x: -3
                width: 9
                height: parent.height
                cursorShape: Qt.SizeHorCursor
            }
            TransitionDragHandle {
                lane: lane
                inspector: root
                handle: "right"
                x: parent.width - 6
                width: 9
                height: parent.height
                cursorShape: Qt.SizeHorCursor
            }
        }

        // cut の位置。
        Rectangle {
            readonly property real cutX: lane.xAt(root.shownCut)
            x: cutX
            y: lane.rowAY
            width: 1
            height: lane.rowBY + lane.rowHeight - lane.rowAY
            color: "#8a919c"
        }

        // 再生ヘッド。timeline と同じ playheadFrame を指す。範囲外なら端に寄せず出さない。
        Item {
            objectName: "transitionMiniPlayhead"
            readonly property int frame: root.mvmController.playheadFrame
            visible: root.hasTransition && frame >= root.viewStart && frame <= root.viewEnd
            x: lane.xAt(frame)
            width: 1
            height: lane.height

            Rectangle {
                x: -4
                width: 9
                height: 8
                radius: 2
                color: "#4aa3ff"
            }
            Rectangle {
                width: 1
                height: parent.height
                color: "#4aa3ff"
            }
        }
    }

    // A / B の 1 行。clip の本体と、延ばして使っている素材 (斜線)、延ばせる上限 (薄い帯)。
    // inline component は外側の id を参照できないので、lane を property で受け取る。
    // 行を掴んでドラッグすると gesture の編集 (A / B のリップルトリム) になる。
    component TransitionClipBar: Item {
        id: bar
        required property var lane
        required property var inspector
        required property string gesture
        property int clipFrom: 0
        property int clipTo: 0
        property int usedFrom: 0
        property int usedTo: 0
        property int availableFrom: 0
        property int availableTo: 0
        property string title
        width: bar.lane.width
        height: bar.lane.rowHeight

        Rectangle {
            readonly property var span: bar.lane.spanX(bar.availableFrom, bar.availableTo)
            x: span.x
            width: span.width
            height: parent.height
            color: "#1f2a36"
        }
        Rectangle {
            id: usedBar
            readonly property var span: bar.lane.spanX(bar.usedFrom, bar.usedTo)
            x: span.x
            width: span.width
            height: parent.height
            color: "#2a4a6a"
            Canvas {
                anchors.fill: parent
                onPaint: {
                    const context = getContext("2d");
                    context.reset();
                    context.strokeStyle = "#5f8fbf";
                    context.lineWidth = 1;
                    context.beginPath();
                    for (let offset = -height; offset < width; offset += 6) {
                        context.moveTo(offset, height);
                        context.lineTo(offset + height, 0);
                    }
                    context.stroke();
                }
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
            }
        }
        Rectangle {
            readonly property var span: bar.lane.spanX(bar.clipFrom, bar.clipTo)
            x: span.x
            width: span.width
            height: parent.height
            radius: 2
            color: "#315f86"
            border.color: "#65a8dc"
            clip: true
            Label {
                anchors.fill: parent
                anchors.leftMargin: 5
                anchors.rightMargin: 5
                verticalAlignment: Text.AlignVCenter
                text: bar.title
                color: "white"
                font.pixelSize: 11
                elide: Text.ElideRight
            }
        }
        EdgeEditArea {
            anchors.fill: parent
            lane: bar.lane
            inspector: bar.inspector
            gesture: bar.gesture
        }
    }

    // ドラッグの起点を lane の座標で覚える (掴んだ矩形自体が動くので自分の座標は使えない)。
    component TransitionDragHandle: MouseArea {
        id: handleArea
        required property string handle
        required property var lane
        required property var inspector
        property real pressLaneX: 0
        enabled: handleArea.inspector.editable
        preventStealing: true
        onPressed: mouse => {
            const ui = handleArea.inspector;
            handleArea.pressLaneX = handleArea.mapToItem(handleArea.lane, mouse.x, 0).x;
            ui.showGhost({ "before": ui.committedBefore, "after": ui.committedAfter });
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return;
            const ui = handleArea.inspector;
            const laneX = handleArea.mapToItem(handleArea.lane, mouse.x, 0).x;
            const deltaFrames = (laneX - handleArea.pressLaneX) * handleArea.lane.framesPerPixel;
            ui.showGhost(SpanMath.spanForDrag(handleArea.handle, deltaFrames, ui.committedBefore,
                                              ui.committedAfter, ui.maxBefore, ui.maxAfter));
        }
        onReleased: {
            const ui = handleArea.inspector;
            ui.commitSpan({ "before": ui.ghostBefore, "after": ui.ghostAfter },
                          handleArea.handle === "body");
        }
        onCanceled: handleArea.inspector.dragging = false
    }

    // A / B / cut 線のドラッグ。起点は lane の座標で覚え、確定と同じ規則で止めた量を表示する。
    component EdgeEditArea: MouseArea {
        id: edgeArea
        required property string gesture
        required property var lane
        required property var inspector
        property real pressLaneX: 0
        enabled: edgeArea.inspector.editable
        preventStealing: true
        cursorShape: Qt.SizeHorCursor
        onPressed: mouse => {
            edgeArea.pressLaneX = edgeArea.mapToItem(edgeArea.lane, mouse.x, 0).x;
            edgeArea.inspector.previewEdgeEdit(edgeArea.gesture, 0);
        }
        onPositionChanged: mouse => {
            if (!edgeArea.pressed)
                return;
            const laneX = edgeArea.mapToItem(edgeArea.lane, mouse.x, 0).x;
            edgeArea.inspector.previewEdgeEdit(
                edgeArea.gesture,
                Math.round((laneX - edgeArea.pressLaneX) * edgeArea.lane.framesPerPixel));
        }
        onReleased: edgeArea.inspector.commitEdgeEdit()
        onCanceled: edgeArea.inspector.cancelEdgeEdit()
    }
}

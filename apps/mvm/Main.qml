pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Shapes
import "TimelineGestures.js" as Gestures
import "PreviewTransform.js" as Transform
import "TrackEyePaint.js" as EyePaint

ApplicationWindow {
    id: root
    required property MvmController mvmController
    required property WaveformCache waveformCache
    width: 1440
    height: 900
    minimumWidth: 980
    minimumHeight: 640
    // PreviewSurface自体はこのQMLのscene graphへ宣言済み。C++から動的追加しない。
    visible: true
    title: "mvm" + (root.mvmController.dirty ? " *" : "") + " — " + root.mvmController.projectPath
    color: "#15171b"

    Action {
        id: openProjectAction
        text: "プロジェクトを開く"
        shortcut: "Ctrl+O"
        enabled: !root.mvmController.busy
        onTriggered: root.requestProjectAction("open")
    }
    Action {
        id: closeProjectAction
        text: "プロジェクトを閉じる"
        shortcut: "Ctrl+Shift+W"
        enabled: !root.mvmController.busy
        onTriggered: root.requestProjectAction("close")
    }
    Action {
        id: saveProjectAction
        text: "保存"
        shortcut: "Ctrl+S"
        enabled: !root.mvmController.busy && root.mvmController.dirty
        onTriggered: root.mvmController.saveProject()
    }
    Action {
        id: saveProjectAsAction
        text: "名前を付けて保存"
        shortcut: "Ctrl+Shift+S"
        enabled: !root.mvmController.busy
        onTriggered: saveProjectDialog.open()
    }
    Action {
        id: exportMediaAction
        text: "メディアを書き出し"
        shortcut: "Ctrl+M"
        enabled: root.mvmController.canExport
        onTriggered: exportDialog.open()
    }
    Action {
        id: undoAction
        text: "元に戻す"
        shortcut: "Ctrl+Z"
        enabled: root.mvmController.canUndo
        onTriggered: root.mvmController.undoLastEdit()
    }
    Action {
        id: redoAction
        text: "やり直し"
        shortcut: "Ctrl+Shift+Z"
        enabled: root.mvmController.canRedo
        onTriggered: root.mvmController.redoLastEdit()
    }
    Action {
        id: copyClipsAction
        text: "クリップをコピー"
        shortcut: "Ctrl+C"
        enabled: !root.mvmController.busy && !root.keyboardFocusTakesKeys
        onTriggered: root.mvmController.copySelectedClips()
    }
    Action {
        id: cutClipsAction
        text: "クリップをカット"
        shortcut: "Ctrl+X"
        enabled: !root.mvmController.busy && !root.keyboardFocusTakesKeys
        onTriggered: root.mvmController.cutSelectedClips()
    }
    Action {
        id: pasteClipsAction
        text: "クリップをペースト"
        shortcut: "Ctrl+V"
        enabled: !root.mvmController.busy && !root.keyboardFocusTakesKeys
        onTriggered: root.mvmController.pasteClips()
    }
    Action {
        id: duplicateClipsAction
        text: "クリップを複製"
        shortcut: "Ctrl+D"
        enabled: !root.mvmController.busy && !root.keyboardFocusTakesKeys
        onTriggered: root.mvmController.duplicateSelectedClips()
    }
    Action {
        id: selectAllClipsAction
        text: "すべてを選択"
        shortcut: "Ctrl+A"
        enabled: !root.mvmController.busy && !root.keyboardFocusTakesKeys
        onTriggered: root.mvmController.selectAllClips()
    }
    Action {
        id: splitAtPlayheadAction
        text: "編集点を追加"
        shortcut: "Ctrl+K"
        enabled: !root.mvmController.busy && !root.keyboardFocusTakesKeys
        onTriggered: root.mvmController.splitSelectionAtPlayhead()
    }
    Action {
        id: splitAllTracksAction
        text: "編集点をすべてのトラックに追加"
        shortcut: "Ctrl+Shift+K"
        enabled: !root.mvmController.busy && !root.keyboardFocusTakesKeys
        onTriggered: root.mvmController.splitClipAt("", root.mvmController.playheadFrame, true, true)
    }
    // Premiere は ] で上げるが、ここでは [ で上げる (キーボードで [ が上にあり、上げる操作として
    // 直感的なため。利用者の判断で意図的に逆にしている)。
    Action {
        id: volumeUpAction
        text: "クリップの音量を上げる"
        shortcut: "["
        enabled: !root.mvmController.busy && !root.keyboardFocusTakesKeys
        onTriggered: root.mvmController.stepSelectedClipVolume(1)
    }
    Action {
        id: volumeDownAction
        text: "クリップの音量を下げる"
        shortcut: "]"
        enabled: !root.mvmController.busy && !root.keyboardFocusTakesKeys
        onTriggered: root.mvmController.stepSelectedClipVolume(-1)
    }
    Action {
        id: toggleClipEnabledAction
        text: "有効/無効を切り換え"
        shortcut: "Shift+E"
        enabled: !root.mvmController.busy && !root.keyboardFocusTakesKeys
        onTriggered: root.mvmController.toggleSelectedClipsEnabled()
    }
    Action {
        id: defaultTransitionAction
        text: "デフォルトのトランジションを適用"
        shortcut: "Shift+D"
        enabled: !root.mvmController.busy && !root.keyboardFocusTakesKeys
        onTriggered: root.mvmController.applyDefaultTransition()
    }
    Action {
        id: speedDurationAction
        text: "速度・デュレーション..."
        shortcut: "Ctrl+R"
        enabled: !root.mvmController.busy && root.mvmController.currentClipIndex >= 0
                 && !root.keyboardFocusTakesKeys
        onTriggered: root.openSpeedDurationDialog("")
    }

    function openSpeedDurationDialog(clipId) {
        const state = root.mvmController.clipSpeedDurationState(clipId);
        if (!state.clipId)
            return;
        speedDurationDialog.clipId = state.clipId;
        speedDurationDialog.still = state.still;
        speedDurationDialog.lastInput = state.still ? "duration" : "speed";
        speedDurationDialog.syncing = true;
        speedField.value = state.speedPercent;
        durationField.text = state.durationText;
        preservePitchBox.checked = state.preservePitch;
        rippleBox.checked = false;
        speedDurationDialog.syncing = false;
        speedDurationDialog.open();
    }
    Action {
        id: addMarkerAction
        text: "マーカーを追加"
        shortcut: "M"
        enabled: !root.mvmController.busy && !root.keyboardFocusTakesKeys
        onTriggered: root.mvmController.addTimelineMarker()
    }
    Action {
        id: nextMarkerAction
        text: "次のマーカーへ移動"
        shortcut: "Shift+M"
        enabled: !root.mvmController.busy && !root.keyboardFocusTakesKeys
        onTriggered: root.mvmController.jumpToMarker(1)
    }
    Action {
        id: previousMarkerAction
        text: "前のマーカーへ移動"
        shortcut: "Ctrl+Shift+M"
        enabled: !root.mvmController.busy && !root.keyboardFocusTakesKeys
        onTriggered: root.mvmController.jumpToMarker(-1)
    }
    Action {
        id: markInAction
        text: "インをマーク"
        shortcut: "I"
        enabled: !root.mvmController.busy && !root.keyboardFocusTakesKeys
        onTriggered: root.mvmController.markIn()
    }
    Action {
        id: markOutAction
        text: "アウトをマーク"
        shortcut: "O"
        enabled: !root.mvmController.busy && !root.keyboardFocusTakesKeys
        onTriggered: root.mvmController.markOut()
    }
    Action {
        id: jumpInAction
        text: "インへ移動"
        shortcut: "Shift+I"
        enabled: !root.mvmController.busy && root.mvmController.inFrame >= 0
                 && !root.keyboardFocusTakesKeys
        onTriggered: root.mvmController.jumpToIn()
    }
    Action {
        id: jumpOutAction
        text: "アウトへ移動"
        shortcut: "Shift+O"
        enabled: !root.mvmController.busy && root.mvmController.outFrame >= 0
                 && !root.keyboardFocusTakesKeys
        onTriggered: root.mvmController.jumpToOut()
    }
    Action {
        id: clearInOutAction
        text: "インとアウトを消去"
        shortcut: "Alt+X"
        enabled: !root.mvmController.busy && !root.keyboardFocusTakesKeys
        onTriggered: root.mvmController.clearInOut()
    }
    // Windows で一般的な Ctrl+Y も同じやり直しにする。Action は shortcut を 1 つしか持てない。
    Shortcut {
        sequence: "Ctrl+Y"
        enabled: redoAction.enabled
        onActivated: redoAction.trigger()
    }

    menuBar: MenuBar {
        height: 33
        padding: 3
        spacing: 2
        background: Rectangle {
            color: "#25272b"
        }
        delegate: MenuBarItem {
            id: barItem
            implicitHeight: 27
            leftPadding: 11
            rightPadding: 11
            font.pixelSize: 12
            contentItem: Label {
                text: barItem.text
                color: barItem.highlighted ? "#ffffff" : "#d4d7dc"
                font: barItem.font
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                color: barItem.highlighted ? "#414750" : "transparent"
                radius: 4
            }
        }
        CompactMenu {
            title: "ファイル"
            CompactMenuItem {
                text: "新規プロジェクト"
                enabled: !root.mvmController.busy
                onTriggered: root.requestProjectAction("new")
            }
            CompactMenuItem {
                action: openProjectAction
            }
            CompactMenuItem {
                action: closeProjectAction
            }
            CompactMenuSeparator {}
            CompactMenuItem {
                action: saveProjectAction
            }
            CompactMenuItem {
                action: saveProjectAsAction
            }
            CompactMenuSeparator {}
            CompactMenuItem {
                text: "メディアを追加"
                enabled: !root.mvmController.busy
                onTriggered: mediaDialog.open()
            }
            CompactMenuItem {
                text: "Manim clip"
                visible: !root.mvmController.hasManimAsset
                enabled: root.mvmController.previewReady && !root.mvmController.busy
                onTriggered: scriptDialog.open()
            }
            CompactMenuSeparator {}
            CompactMenuItem {
                action: exportMediaAction
            }
        }
        CompactMenu {
            title: "編集"
            CompactMenuItem {
                action: undoAction
            }
            CompactMenuItem {
                action: redoAction
            }
            CompactMenuSeparator {}
            CompactMenuItem { action: copyClipsAction }
            CompactMenuItem { action: cutClipsAction }
            CompactMenuItem { action: pasteClipsAction }
            CompactMenuItem { action: duplicateClipsAction }
            CompactMenuItem { action: speedDurationAction }
            CompactMenuSeparator {}
            CompactMenuItem { action: splitAtPlayheadAction }
            CompactMenuItem { action: splitAllTracksAction }
            CompactMenuItem { action: selectAllClipsAction }
            CompactMenuSeparator {}
            CompactMenuItem { action: volumeUpAction }
            CompactMenuItem { action: volumeDownAction }
            CompactMenuItem { action: toggleClipEnabledAction }
            CompactMenuItem { action: defaultTransitionAction }
            CompactMenuSeparator {}
            // 実行は Shortcut "Delete" が担う。ここは表示だけで sequence を持たせない (二重発火を防ぐ)。
            CompactMenuItem {
                text: (root.mvmController.selectedTransitionId !== "" ? "トランジションを削除"
                                                                      : "クリップを削除") + "\tDelete"
                enabled: root.mvmController.canDeleteSelection
                onTriggered: root.mvmController.deleteSelection()
            }
        }
        CompactMenu {
            title: "再生"
            // 実行は Shortcut "Space" が担う。ここは表示だけで sequence を持たせない (二重発火を防ぐ)。
            CompactMenuItem {
                text: (root.mvmController.playing ? "一時停止" : "再生") + "\tSpace"
                enabled: !root.mvmController.busy
                         && (root.mvmController.playing || root.mvmController.canPlay)
                onTriggered: {
                    if (root.mvmController.playing)
                        root.mvmController.pauseTimeline();
                    else
                        root.mvmController.playTimeline();
                }
            }
            CompactMenuSeparator {}
            CompactMenuItem { action: addMarkerAction }
            CompactMenuItem { action: nextMarkerAction }
            CompactMenuItem { action: previousMarkerAction }
            CompactMenuItem { action: markInAction }
            CompactMenuItem { action: markOutAction }
            CompactMenuItem { action: jumpInAction }
            CompactMenuItem { action: jumpOutAction }
            CompactMenuItem { action: clearInOutAction }
            CompactMenuSeparator {}
            CompactMenuItem {
                text: "左へシャトル\tJ"
                enabled: !root.mvmController.busy && root.mvmController.clipCount > 0
                onTriggered: root.mvmController.shuttleLeft()
            }
            CompactMenuItem {
                text: "停止\tK"
                enabled: root.mvmController.playing
                onTriggered: root.mvmController.pauseTimeline()
            }
            CompactMenuItem {
                text: "右へシャトル\tL"
                enabled: !root.mvmController.busy && root.mvmController.clipCount > 0
                onTriggered: root.mvmController.shuttleRight()
            }
            CompactMenuSeparator {}
            CompactMenuItem {
                text: "1フレーム前へ\t←"
                enabled: !root.mvmController.busy && root.mvmController.clipCount > 0
                onTriggered: root.mvmController.stepTimelineFrames(-1)
            }
            CompactMenuItem {
                text: "1フレーム先へ\t→"
                enabled: !root.mvmController.busy && root.mvmController.clipCount > 0
                onTriggered: root.mvmController.stepTimelineFrames(1)
            }
            CompactMenuItem {
                text: "5フレーム前へ\tShift+←"
                enabled: !root.mvmController.busy && root.mvmController.clipCount > 0
                onTriggered: root.mvmController.stepTimelineFrames(-5)
            }
            CompactMenuItem {
                text: "5フレーム先へ\tShift+→"
                enabled: !root.mvmController.busy && root.mvmController.clipCount > 0
                onTriggered: root.mvmController.stepTimelineFrames(5)
            }
            CompactMenuSeparator {}
            CompactMenuItem {
                text: "前の編集点へ\t↑"
                enabled: !root.mvmController.busy && root.mvmController.clipCount > 0
                onTriggered: root.mvmController.jumpToEditPoint(-1)
            }
            CompactMenuItem {
                text: "次の編集点へ\t↓"
                enabled: !root.mvmController.busy && root.mvmController.clipCount > 0
                onTriggered: root.mvmController.jumpToEditPoint(1)
            }
        }
        CompactMenu {
            title: "プロジェクト"
            CompactMenuItem {
                text: "プロジェクト設定"
                enabled: !root.mvmController.busy
                onTriggered: root.openProjectSettingsDialog()
            }
        }
    }

    property url selectedManimScript
    property url pendingExportFile
    property bool closeConfirmed: false
    property string pendingProjectAction: ""
    // 終了/New/Openの保存が外部変更で止まったときだけ立てる。上書きかSave Asの成功で再開する。
    property bool pendingSaveContinuation: false
    // 左上パネルのタブ。0: エフェクトコントロール / 1: プロジェクト
    property int leftPanelTab: 0
    property real leftPanelWidth: 340
    // タイムラインの現在のツール。取りうる値は TimelineToolPanel.tools の tool。
    property string timelineTool: "select"
    property string editingTextClipId: ""
    property bool textEditing: false
    property string draggingTextClipId: ""
    // 映像のある frame では文字を preview engine が track 順に合成する。
    // 編集中・ドラッグ中の文字だけはここで重ねるので、engine の合成から外させる。
    readonly property string textOverlayClipId: root.textEditing ? root.editingTextClipId
                                                                 : root.draggingTextClipId
    onTextOverlayClipIdChanged: root.mvmController.setTextOverlayClip(root.textOverlayClipId)
    property real textEditorX: 0
    property real textEditorY: 0

    function finishTextEditing(save) {
        if (!root.textEditing)
            return;
        const content = textEditor.text;
        const clipId = root.editingTextClipId;
        root.textEditing = false;
        root.editingTextClipId = "";
        // 非表示になった editor に focus が残ると、V や T などの単キー操作が
        // keyboardFocusTakesKeys に止められ続ける。editor が focus を持つとき
        // (Esc / Ctrl+Enter) だけ window へ戻し、他の control へ移った focus は奪わない。
        if (textEditor.activeFocus)
            root.contentItem.forceActiveFocus();
        if (!save || content.trim().length === 0)
            return;
        if (clipId.length > 0)
            root.mvmController.updateTextClip(clipId, {content: content});
        else
            root.mvmController.createTextClip(content,
                Math.round(root.textEditorX * root.mvmController.outputWidth / previewHost.width),
                Math.round(root.textEditorY * root.mvmController.outputHeight / previewHost.height));
    }
    // 文字入力・選択肢・popup (dialog / menu) に focus がある間は、window 全体の
    // 単一キーと矢印の shortcut にキーを奪わせない。個々の編集状態 (名前変更中など) を
    // 並べず、focus を持つ control の種類だけで決める。
    readonly property bool keyboardFocusTakesKeys: {
        const item = root.activeFocusItem;
        if (item instanceof TextInput || item instanceof TextEdit || item instanceof ComboBox
                || item instanceof SpinBox)
            return true;
        for (let ancestor = item; ancestor; ancestor = ancestor.parent) {
            if (ancestor === root.Overlay.overlay)
                return true;
        }
        return false;
    }
    readonly property bool timelineShortcutsEnabled: !root.mvmController.busy
                                                     && root.mvmController.clipCount > 0
                                                     && !root.keyboardFocusTakesKeys
    readonly property string projectFileName: {
        const parts = root.mvmController.projectPath.split(/[\\/]/);
        return parts[parts.length - 1];
    }

    Component.onCompleted: {
        if (root.mvmController.recoveryAvailable || root.mvmController.recoveryCorrupt
                || root.mvmController.recoveryForeign)
            recoveryDialog.open();
    }

    onClosing: close => {
        if (!closeConfirmed && root.mvmController.dirty) {
            close.accepted = false;
            pendingProjectAction = "close";
            unsavedChangesDialog.open();
        }
    }

    function performProjectAction(action) {
        if (action === "close") {
            closeConfirmed = true;
            close();
        } else if (action === "new") {
            newProjectDialog.open();
        } else if (action === "open") {
            openProjectDialog.open();
        }
    }

    function requestProjectAction(action) {
        if (root.mvmController.dirty) {
            pendingProjectAction = action;
            unsavedChangesDialog.open();
            return;
        }
        performProjectAction(action);
    }

    function continuePendingProjectAction() {
        const action = pendingProjectAction;
        pendingProjectAction = "";
        performProjectAction(action);
    }

    function noteExternalSaveDuringPendingAction() {
        pendingSaveContinuation = pendingProjectAction !== "";
    }

    function abandonExternalSaveContinuation() {
        pendingSaveContinuation = false;
    }

    // 直接のCtrl+Sではcontinuationが空なので、dialogを閉じるだけで終わる。
    function completeExternalSave(saved) {
        if (!saved)
            return;
        externalSaveDialog.close();
        if (!pendingSaveContinuation)
            return;
        pendingSaveContinuation = false;
        unsavedChangesDialog.close();
        continuePendingProjectAction();
    }

    // インスペクタの拡大率 X/Y を連動させるか。UI の状態であり Project には保存しない。
    property bool lockEffectScaleAspect: true

    // 外部ファイルのドロップ位置がプロジェクトパネル上か。そこへ落とした素材は
    // timeline へは置かず、bin へ読み込むだけにする。
    function isOverProjectPanel(x, y) {
        if (root.leftPanelTab !== 1)
            return false;
        const point = projectPanel.mapFromItem(videoDropArea, x, y);
        return projectPanel.contains(point);
    }

    // item 上の点がタイムラインのどこへのドロップになるか。タイムラインの外なら null。
    //   トラックの行           -> その track
    //   最上段 video より上     -> 新しい video track (index == videoCount)
    //   最下段 audio より下     -> 新しい audio track (index == audioCount)
    function timelineDropTarget(item, x, y) {
        const visible = timelineFlick.mapFromItem(item, x, y);
        if (!timelineFlick.contains(visible))
            return null;
        const point = timelineContent.mapFromItem(item, x, y);
        const frame = timelinePanel.frameAtContentX(point.x);
        if (point.y < timelinePanel.tracksTop)
            return { "kind": "video", "index": timelinePanel.videoCount, "frame": frame };
        const track = timelinePanel.trackAtY(point.y);
        if (!track)
            return { "kind": "audio", "index": timelinePanel.audioCount, "frame": frame };
        return { "kind": track.kind, "index": track.index, "frame": frame };
    }

    // 拡大率の入力。縦横比を固定しているときは、もう一方も同じ比で変えて 1 つの変更にする。
    function setEffectScale(key, value, commit) {
        if (!root.lockEffectScaleAspect)
            return root.mvmController.setEffectValue(key, value, commit);
        const x = root.mvmController.effectScaleX;
        const y = root.mvmController.effectScaleY;
        const clamp = v => Math.max(1, Math.min(1000, v));
        const values = key === "scaleX" ? { "scaleX": value, "scaleY": clamp(y * value / x) }
                                        : { "scaleX": clamp(x * value / y), "scaleY": value };
        return root.mvmController.setEffectValues(values, commit);
    }

    function isLocalFileUrl(url) {
        // 対応形式は拡張子では決めない。drop 後に controller が MLT で内容を検査し、
        // 映像 stream・有限尺・FPS を確認できた素材だけを timeline へ追加する。
        return /^file:/i.test(url.toString());
    }

    function openProjectSettingsDialog() {
        projectWidthField.text = root.mvmController.outputWidth.toString();
        projectHeightField.text = root.mvmController.outputHeight.toString();
        projectFpsBox.syncFromController();
        projectSettingsDialog.open();
    }

    function confirmProjectSettingsFromClip(clipId) {
        const settings = root.mvmController.projectSettingsForClip(clipId);
        matchClipSettingsDialog.validSettings = settings.valid === true;
        matchClipSettingsDialog.changesSettings = settings.changes === true;
        matchClipSettingsDialog.clipName = settings.clipName || "";
        matchClipSettingsDialog.sourceText = settings.sourceText || "";
        matchClipSettingsDialog.errorText = settings.error || "";
        matchClipSettingsDialog.targetWidth = settings.width || 0;
        matchClipSettingsDialog.targetHeight = settings.height || 0;
        matchClipSettingsDialog.targetFpsNum = settings.fpsNum || 0;
        matchClipSettingsDialog.targetFpsDen = settings.fpsDen || 1;
        matchClipSettingsDialog.open();
    }

    // mvm は drop されたファイルを移動も複製もせず、元の場所のまま参照する。
    // drag source へ返す action は常に CopyAction に固定する。source が提示した
    // action (Shift+drag の MoveAction など) をそのまま受理すると、source 側が
    // 「移動が成功した」として元ファイルを後始末しうる。
    DropArea {
        id: videoDropArea
        property bool acceptingVideoDrag: false
        anchors.fill: parent
        z: 2000

        function copyAllowed(drag) {
            return (drag.supportedActions & Qt.CopyAction) !== 0;
        }

        onEntered: drag => {
            acceptingVideoDrag = false;
            drag.accepted = false;
            if (root.mvmController.busy || !drag.hasUrls || !copyAllowed(drag))
                return;
            for (let index = 0; index < drag.urls.length; ++index) {
                if (root.isLocalFileUrl(drag.urls[index])) {
                    acceptingVideoDrag = true;
                    drag.accept(Qt.CopyAction);
                    return;
                }
            }
        }
        onPositionChanged: drag => {
            // move のたびに action を確定し直す。Qt は move event ごとに proposed action へ戻す。
            if (acceptingVideoDrag)
                drag.accept(Qt.CopyAction);
            projectPanel.externalDropHover = acceptingVideoDrag
                                             && root.isOverProjectPanel(drag.x, drag.y);
            timelinePanel.dropTarget = acceptingVideoDrag
                                       ? root.timelineDropTarget(videoDropArea, drag.x, drag.y)
                                       : null;
        }
        onExited: {
            acceptingVideoDrag = false;
            projectPanel.externalDropHover = false;
            timelinePanel.dropTarget = null;
        }
        // 素材は必ずプロジェクトパネルへ登録する (パネルが素材の唯一の出どころ)。
        // タイムライン上へ落とした場合は、登録したうえでその位置へ置く。
        onDropped: drop => {
            acceptingVideoDrag = false;
            projectPanel.externalDropHover = false;
            timelinePanel.dropTarget = null;
            if (!copyAllowed(drop)) {
                drop.accepted = false;
                return;
            }
            const target = root.timelineDropTarget(videoDropArea, drop.x, drop.y);
            const urls = [];
            for (let index = 0; index < drop.urls.length; ++index) {
                // 動画・音声・画像は内容で判定する (拡張子は見ない)。
                if (root.isLocalFileUrl(drop.urls[index]))
                    urls.push(drop.urls[index]);
            }
            if (urls.length === 0)
                return;
            if (target)
                root.mvmController.addMediaFilesToTimelineAt(urls, target.kind, target.index,
                                                             target.frame);
            else
                projectPanel.importUrls(urls);
            drop.accept(Qt.CopyAction);
        }
    }

    Rectangle {
        anchors.fill: parent
        z: 1999
        visible: videoDropArea.acceptingVideoDrag
        color: "#99151920"
        border.width: 3
        border.color: "#64a8e8"

        Label {
            anchors.centerIn: parent
            text: timelinePanel.dropTarget ? "タイムラインへ配置" : "プロジェクトへ読み込み"
            color: "white"
            font.pixelSize: 20
            font.bold: true
        }
    }

    Shortcut {
        sequence: "Delete"
        enabled: root.mvmController.canDeleteSelection && !root.keyboardFocusTakesKeys
        onActivated: root.mvmController.deleteSelection()
    }
    Shortcut {
        sequence: "Space"
        autoRepeat: false
        enabled: !root.mvmController.busy && (root.mvmController.playing || root.mvmController.canPlay)
                 && !root.keyboardFocusTakesKeys
        onActivated: {
            if (root.mvmController.playing)
                root.mvmController.pauseTimeline();
            else
                root.mvmController.playTimeline();
        }
    }
    Shortcut {
        sequence: "J"
        autoRepeat: false
        enabled: root.timelineShortcutsEnabled
        onActivated: root.mvmController.shuttleLeft()
    }
    Shortcut {
        sequence: "K"
        autoRepeat: false
        enabled: root.mvmController.playing && !root.keyboardFocusTakesKeys
        onActivated: root.mvmController.pauseTimeline()
    }
    Shortcut {
        sequence: "L"
        autoRepeat: false
        enabled: root.timelineShortcutsEnabled
        onActivated: root.mvmController.shuttleRight()
    }
    Shortcut {
        sequence: "Left"
        enabled: root.timelineShortcutsEnabled
        onActivated: root.mvmController.stepTimelineFrames(-1)
    }
    Shortcut {
        sequence: "Right"
        enabled: root.timelineShortcutsEnabled
        onActivated: root.mvmController.stepTimelineFrames(1)
    }
    Shortcut {
        sequence: "Shift+Left"
        enabled: root.timelineShortcutsEnabled
        onActivated: root.mvmController.stepTimelineFrames(-5)
    }
    Shortcut {
        sequence: "Shift+Right"
        enabled: root.timelineShortcutsEnabled
        onActivated: root.mvmController.stepTimelineFrames(5)
    }
    Shortcut {
        sequence: "Up"
        enabled: root.timelineShortcutsEnabled
        onActivated: root.mvmController.jumpToEditPoint(-1)
    }
    Shortcut {
        sequence: "Down"
        enabled: root.timelineShortcutsEnabled
        onActivated: root.mvmController.jumpToEditPoint(1)
    }
    // タイムラインツールの切り替え (V / A / Shift+A / C / B / N / Y / U / H / Z)。
    // キーと有効/無効は TimelineToolPanel.tools だけが決める。
    Instantiator {
        model: timelineToolPanel.tools
        delegate: Shortcut {
            required property var modelData
            sequence: modelData.key
            autoRepeat: false
            enabled: modelData.available && !root.keyboardFocusTakesKeys
            onActivated: timelineToolPanel.requestTool(modelData.tool)
        }
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        // --- 作業状態 -----------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            BusyIndicator {
                running: root.mvmController.busy
                visible: running
                implicitWidth: 22
                implicitHeight: 22
            }
            Label {
                Layout.fillWidth: true
                text: root.mvmController.statusText
                color: "#e6e8ec"
                elide: Text.ElideRight
            }
        }

        // --- 上段: 左にインスペクタ、中央にプレビュー、右にメーター -------
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: root.height * 0.42
            Layout.minimumHeight: 220
            // 左パネルとプレビューの隙間は下の境界ハンドルが兼ねる。
            spacing: 0

            Frame {
                id: leftPanel
                Layout.preferredWidth: root.leftPanelWidth
                Layout.fillHeight: true
                padding: 8

                background: Rectangle {
                    color: "#1b1f25"
                    border.color: "#343840"
                    radius: 4
                }

                contentItem: ColumnLayout {
                    spacing: 6

                    // premiere と同じく、同じ領域をタブで切り替える。
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 14

                        Repeater {
                            model: ["エフェクトコントロール", "プロジェクト: " + root.projectFileName]
                            Label {
                                required property int index
                                required property string modelData
                                Layout.maximumWidth: index === 1 ? leftPanel.availableWidth * 0.55 : -1
                                text: modelData
                                color: root.leftPanelTab === index ? "#e6e8ec" : "#8a919c"
                                font.bold: root.leftPanelTab === index
                                elide: Text.ElideMiddle
                                bottomPadding: 4

                                Rectangle {
                                    anchors.bottom: parent.bottom
                                    width: parent.width
                                    height: 2
                                    visible: root.leftPanelTab === parent.index
                                    color: "#64a8e8"
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.leftPanelTab = parent.index
                                }
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.topMargin: -6
                        implicitHeight: 1
                        color: "#343840"
                    }

                    StackLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        currentIndex: root.leftPanelTab

                        // トランジションを選んでいる間は、clip の項目の代わりにトランジションの
                        // 長さと配置を出す (Premiere のエフェクトコントロールと同じ領域)。
                        Item {
                            TransitionInspector {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                visible: root.mvmController.selectedTransitionId !== ""
                                mvmController: root.mvmController
                            }

                            // 項目が増えるとパネルの高さを超え、下の再生時間の表示に重なっていた。
                            // 縦にスクロールさせ、はみ出した分は切り取る。端では跳ね返らずにそのまま止める。
                            Flickable {
                                id: effectControlsScroll
                                anchors.fill: parent
                                visible: root.mvmController.selectedTransitionId === ""
                                clip: true
                                contentWidth: width
                                contentHeight: effectControlsColumn.implicitHeight
                                flickableDirection: Flickable.VerticalFlick
                                boundsBehavior: Flickable.StopAtBounds
                                boundsMovement: Flickable.StopAtBounds
                                ScrollBar.vertical: ScrollBar {
                                    id: effectControlsScrollBar
                                    policy: ScrollBar.AsNeeded
                                }

                                ColumnLayout {
                                    id: effectControlsColumn
                                    // scrollbar が出ている間は、その幅だけ項目を狭めて重ならないようにする。
                                    width: effectControlsScroll.width
                                           - (effectControlsScrollBar.visible ? effectControlsScrollBar.width : 0)
                                    spacing: 6

                                    // 文字 clip の名前は本文の先頭なので、本文の欄と重複する。出さない。
                                    Label {
                                        Layout.fillWidth: true
                                        visible: root.mvmController.selectedTextClip.clipId === undefined
                                        text: root.mvmController.currentClipIndex >= 0
                                              ? root.mvmController.currentClipName
                                              : "クリップ未選択"
                                        color: "#9aa2ad"
                                        font.pixelSize: 11
                                        elide: Text.ElideMiddle
                                    }

                                    TextClipInspector {
                                        Layout.fillWidth: true
                                        visible: root.mvmController.selectedTextClip.clipId !== undefined
                                        mvmController: root.mvmController
                                    }

                                    GridLayout {
                                        id: inspectorGrid
                                        Layout.fillWidth: true
                                        visible: root.mvmController.selectedTextClip.clipId === undefined
                                        columns: 2
                                        columnSpacing: 6
                                        rowSpacing: 4
                                        enabled: root.mvmController.currentClipIndex >= 0 && !root.mvmController.busy
                                                 && !root.mvmController.playing

                                        DragNumberField {
                                            Layout.fillWidth: true
                                            labelText: "位置 X"
                                            suffix: " %"
                                            value: root.mvmController.effectPositionX
                                            minimumValue: -1000
                                            maximumValue: 1000
                                            stepPerPixel: 0.5
                                            onEditCanceled: root.mvmController.cancelEffectPreview()
                                            onValueEdited: (newValue, commit) => root.mvmController.setEffectValue("positionX", newValue, commit)
                                        }
                                        DragNumberField {
                                            Layout.fillWidth: true
                                            labelText: "位置 Y"
                                            suffix: " %"
                                            value: root.mvmController.effectPositionY
                                            minimumValue: -1000
                                            maximumValue: 1000
                                            stepPerPixel: 0.5
                                            onEditCanceled: root.mvmController.cancelEffectPreview()
                                            onValueEdited: (newValue, commit) => root.mvmController.setEffectValue("positionY", newValue, commit)
                                        }
                                        DragNumberField {
                                            Layout.fillWidth: true
                                            labelText: "拡大率 X"
                                            suffix: " %"
                                            value: root.mvmController.effectScaleX
                                            minimumValue: 1
                                            maximumValue: 1000
                                            stepPerPixel: 0.5
                                            onEditCanceled: root.mvmController.cancelEffectPreview()
                                            onValueEdited: (newValue, commit) => root.setEffectScale("scaleX", newValue, commit)
                                        }
                                        DragNumberField {
                                            Layout.fillWidth: true
                                            labelText: "拡大率 Y"
                                            suffix: " %"
                                            value: root.mvmController.effectScaleY
                                            minimumValue: 1
                                            maximumValue: 1000
                                            stepPerPixel: 0.5
                                            onEditCanceled: root.mvmController.cancelEffectPreview()
                                            onValueEdited: (newValue, commit) => root.setEffectScale("scaleY", newValue, commit)
                                        }
                                        CheckBox {
                                            Layout.columnSpan: 2
                                            text: "縦横比を固定"
                                            checked: root.lockEffectScaleAspect
                                            font.pixelSize: 11
                                            onToggled: root.lockEffectScaleAspect = checked
                                        }
                                        DragNumberField {
                                            Layout.fillWidth: true
                                            labelText: "回転"
                                            suffix: " °"
                                            value: root.mvmController.effectRotation
                                            minimumValue: -360
                                            maximumValue: 360
                                            stepPerPixel: 0.5
                                            onEditCanceled: root.mvmController.cancelEffectPreview()
                                            onValueEdited: (newValue, commit) => root.mvmController.setEffectValue("rotation", newValue, commit)
                                        }
                                        DragNumberField {
                                            Layout.fillWidth: true
                                            labelText: "不透明度"
                                            suffix: " %"
                                            value: root.mvmController.effectOpacity
                                            minimumValue: 0
                                            maximumValue: 100
                                            stepPerPixel: 0.3
                                            onEditCanceled: root.mvmController.cancelEffectPreview()
                                            onValueEdited: (newValue, commit) => root.mvmController.setEffectValue("opacity", newValue, commit)
                                        }
                                        Item { Layout.fillWidth: true; implicitHeight: 1 }

                                        DragNumberField {
                                            Layout.fillWidth: true
                                            labelText: "Crop 左"
                                            suffix: " %"
                                            value: root.mvmController.effectCropLeft
                                            minimumValue: 0
                                            maximumValue: 99
                                            stepPerPixel: 0.2
                                            onEditCanceled: root.mvmController.cancelEffectPreview()
                                            onValueEdited: (newValue, commit) => root.mvmController.setEffectValue("cropLeft", newValue, commit)
                                        }
                                        DragNumberField {
                                            Layout.fillWidth: true
                                            labelText: "Crop 右"
                                            suffix: " %"
                                            value: root.mvmController.effectCropRight
                                            minimumValue: 0
                                            maximumValue: 99
                                            stepPerPixel: 0.2
                                            onEditCanceled: root.mvmController.cancelEffectPreview()
                                            onValueEdited: (newValue, commit) => root.mvmController.setEffectValue("cropRight", newValue, commit)
                                        }
                                        DragNumberField {
                                            Layout.fillWidth: true
                                            labelText: "Crop 上"
                                            suffix: " %"
                                            value: root.mvmController.effectCropTop
                                            minimumValue: 0
                                            maximumValue: 99
                                            stepPerPixel: 0.2
                                            onEditCanceled: root.mvmController.cancelEffectPreview()
                                            onValueEdited: (newValue, commit) => root.mvmController.setEffectValue("cropTop", newValue, commit)
                                        }
                                        DragNumberField {
                                            Layout.fillWidth: true
                                            labelText: "Crop 下"
                                            suffix: " %"
                                            value: root.mvmController.effectCropBottom
                                            minimumValue: 0
                                            maximumValue: 99
                                            stepPerPixel: 0.2
                                            onEditCanceled: root.mvmController.cancelEffectPreview()
                                            onValueEdited: (newValue, commit) => root.mvmController.setEffectValue("cropBottom", newValue, commit)
                                        }
                                        DragNumberField {
                                            Layout.fillWidth: true
                                            labelText: "フェードイン (素材f)"
                                            value: root.mvmController.effectFadeIn
                                            minimumValue: 0
                                            maximumValue: 1000000
                                            stepPerPixel: 1
                                            onEditCanceled: root.mvmController.cancelEffectPreview()
                                            onValueEdited: (newValue, commit) => root.mvmController.setEffectValue("fadeIn", newValue, commit)
                                        }
                                        DragNumberField {
                                            Layout.fillWidth: true
                                            labelText: "フェードアウト (素材f)"
                                            value: root.mvmController.effectFadeOut
                                            minimumValue: 0
                                            maximumValue: 1000000
                                            stepPerPixel: 1
                                            onEditCanceled: root.mvmController.cancelEffectPreview()
                                            onValueEdited: (newValue, commit) => root.mvmController.setEffectValue("fadeOut", newValue, commit)
                                        }
                                    }

                                    Frame {
                                        Layout.fillWidth: true
                                        visible: root.mvmController.hasManimAsset
                                        padding: 6

                                        contentItem: ColumnLayout {
                                            spacing: 4
                                            Label {
                                                Layout.fillWidth: true
                                                text: "Manim: " + root.mvmController.manimSceneName
                                                color: "#e6e8ec"
                                                elide: Text.ElideRight
                                                font.pixelSize: 11
                                            }
                                            Label {
                                                text: root.mvmController.manimStateText
                                                color: root.mvmController.manimStateText === "SourceChanged" ? "#f2c66d" : "#a8d5a2"
                                                font.pixelSize: 11
                                            }
                                            RowLayout {
                                                Button {
                                                    text: root.mvmController.busy ? "生成中…" : "再生成"
                                                    enabled: !root.mvmController.busy
                                                    onClicked: root.mvmController.regenerateManimClip()
                                                }
                                                Button {
                                                    text: "timelineへ"
                                                    visible: !root.mvmController.hasManimTimelineClip
                                                    enabled: visible && !root.mvmController.busy
                                                    onClicked: root.mvmController.addManimToTimeline()
                                                }
                                            }
                                        }
                                    }

                                    Item { Layout.fillHeight: true }
                                }
                            }
                        }

                        ProjectPanel {
                            id: projectPanel
                            objectName: "projectPanel"
                            mvmController: root.mvmController
                        }
                    }
                }
            }

            // 左パネルの幅を変える境界。列の多いプロジェクトパネルのために広げられるようにする。
            Item {
                Layout.preferredWidth: 8
                Layout.fillHeight: true

                Rectangle {
                    anchors.centerIn: parent
                    width: 2
                    height: parent.height
                    color: splitterMouse.containsMouse || splitterMouse.pressed ? "#64a8e8" : "transparent"
                }

                MouseArea {
                    id: splitterMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.SplitHCursor
                    property real pressX: 0
                    property real pressWidth: 0
                    onPressed: mouse => {
                        pressX = mapToItem(null, mouse.x, 0).x;
                        pressWidth = root.leftPanelWidth;
                    }
                    onPositionChanged: mouse => {
                        if (!pressed)
                            return;
                        const delta = mapToItem(null, mouse.x, 0).x - pressX;
                        root.leftPanelWidth = Math.max(240, Math.min(root.width * 0.5, pressWidth + delta));
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#08090b"
                border.color: "#343840"

                Item {
                    id: previewArea
                    anchors.fill: parent

                    // タイムラインと同じく、押したらキーボードの宛先をここへ移す (文字の入力中は除く)。
                    PressFocus {
                        enabled: !root.textEditing
                    }

                    Item {
                        id: previewHost
                        objectName: "previewHost"
                        anchors.centerIn: parent
                        readonly property real outputAspect: Math.max(1, root.mvmController.outputWidth)
                                                             / Math.max(1, root.mvmController.outputHeight)
                        width: Math.min(parent.width, parent.height * outputAspect)
                        height: width / outputAspect

                        PreviewSurface {
                            id: previewSurface
                            objectName: "previewSurface"
                            anchors.fill: parent
                            // 映像の無い frame (画像・文字だけ、音声だけ) も engine が提示する。
                            // 隠すと render されず、seek が完了しない。
                        }

                        Repeater {
                            model: root.mvmController.timelineModel
                            delegate: Item {
                                id: textLayer
                                required property int index
                                required property string clipId
                                required property string clipKind
                                required property int trackIndex
                                required property var timelineStartFrame
                                required property var timelineDurationFrames
                                property real dragOffsetX: 0
                                property real dragOffsetY: 0
                                readonly property bool selectedText:
                                    root.mvmController.selectedTextClip.clipId === textLayer.clipId
                                // 出力画素の描画範囲。Project が変わるたびに (stateChanged) 取り直す。
                                readonly property rect bounds: {
                                    root.mvmController.selectedTextClip;
                                    // 見えない文字の画像を作らせない (clip ごとに 1 画面分の raster を描く)。
                                    return textLayer.clipKind === "text" && textLayer.visible
                                           ? root.mvmController.textClipBounds(textLayer.clipId)
                                           : Qt.rect(0, 0, 0, 0);
                                }
                                // bounds を previewHost 上の矩形へ写したもの。
                                readonly property rect hostBounds: Qt.rect(
                                    textLayer.bounds.x * textLayer.width / root.mvmController.outputWidth,
                                    textLayer.bounds.y * textLayer.height / root.mvmController.outputHeight,
                                    textLayer.bounds.width * textLayer.width / root.mvmController.outputWidth,
                                    textLayer.bounds.height * textLayer.height / root.mvmController.outputHeight)
                                anchors.fill: previewHost
                                z: trackIndex + 1
                                transform: Translate {
                                    x: textLayer.dragOffsetX
                                    y: textLayer.dragOffsetY
                                }
                                visible: clipKind === "text"
                                         && (!root.textEditing || root.editingTextClipId !== clipId)
                                         && root.mvmController.playheadFrame >= timelineStartFrame
                                         && root.mvmController.textClipVisible(index)

                                // ドラッグ・編集中の文字だけをここで描く。
                                // それ以外は engine が track 順に合成済みなので透明にする。
                                Image {
                                    anchors.fill: parent
                                    // textPreviewSerial は数値のドラッグ中の描き直しで増える。
                                    source: textLayer.clipKind === "text"
                                            ? (root.mvmController.textPreviewSerial,
                                               root.mvmController.textRasterUrl(textLayer.index))
                                            : ""
                                    cache: false
                                    fillMode: Image.Stretch
                                    // UI が描くときも、書き出しと同じ opacity (値・key・fade) を掛ける。
                                    opacity: (root.textOverlayClipId === textLayer.clipId ? 1 : 0)
                                             * (root.mvmController.playheadFrame,
                                                root.mvmController.textClipOpacity(textLayer.index))
                                }

                                // 選択中の文字の範囲 (Premiere の選択枠に相当)。掴める範囲と同じ。
                                Rectangle {
                                    visible: textLayer.selectedText && root.timelineTool === "select"
                                             && textLayer.bounds.width > 0
                                    x: textLayer.hostBounds.x - 1
                                    y: textLayer.hostBounds.y - 1
                                    width: textLayer.hostBounds.width + 2
                                    height: textLayer.hostBounds.height + 2
                                    color: "transparent"
                                    border.color: "#4a90e2"
                                    border.width: 1
                                }

                                // 文字の範囲だけを掴める。重なった文字は textClipAt が上の track を返す。
                                MouseArea {
                                    x: textLayer.hostBounds.x
                                    y: textLayer.hostBounds.y
                                    width: textLayer.hostBounds.width
                                    height: textLayer.hostBounds.height
                                    enabled: root.timelineTool === "select" && textLayer.visible
                                    // 選択ツールで動かしていることが分かるよう、通常の矢印のままにする。
                                    cursorShape: Qt.ArrowCursor
                                    property bool draggingText: false
                                    property real startX: 0
                                    property real startY: 0
                                    property int originalX: 0
                                    property int originalY: 0
                                    // 吸着先の線 (出力画素)。押した時点の他の素材で決める。
                                    property var snapLines: null
                                    function textAt(mouseX, mouseY) {
                                        const p = mapToItem(textLayer, mouseX, mouseY);
                                        const pixelX = Math.floor(p.x * root.mvmController.outputWidth / textLayer.width);
                                        const pixelY = Math.floor(p.y * root.mvmController.outputHeight / textLayer.height);
                                        return root.mvmController.textClipAt(pixelX, pixelY);
                                    }
                                    onPositionChanged: mouse => {
                                        if (!draggingText)
                                            return;
                                        // 画像・動画と同じく、端・中央を出力と他の素材へ吸着させる
                                        // (Ctrl で吸着しない)。計算は出力画素で行う。
                                        const current = mapToItem(previewHost, mouse.x, mouse.y);
                                        const scaleX = root.mvmController.outputWidth / textLayer.width;
                                        const scaleY = root.mvmController.outputHeight / textLayer.height;
                                        const snapped = Transform.snapMove(
                                            textLayer.bounds, (current.x - startX) * scaleX,
                                            (current.y - startY) * scaleY, snapLines,
                                            previewTransform.snapThreshold,
                                            !(mouse.modifiers & Qt.ControlModifier));
                                        textLayer.dragOffsetX = snapped.dx / scaleX;
                                        textLayer.dragOffsetY = snapped.dy / scaleY;
                                        previewTransform.guides = { "xs": snapped.guidesX,
                                                                    "ys": snapped.guidesY };
                                    }
                                    onPressed: mouse => {
                                        if (textAt(mouse.x, mouse.y) !== textLayer.clipId) {
                                            mouse.accepted = false;
                                            return;
                                        }
                                        const data = root.mvmController.textClipData(textLayer.clipId);
                                        originalX = data.x;
                                        originalY = data.y;
                                        // 移動量は動かない previewHost の座標で測る。この
                                        // MouseArea は Translate で文字と一緒に動くので、
                                        // 自身の座標で測ると移動量が打ち消し合い半分ほどになる。
                                        const start = mapToItem(previewHost, mouse.x, mouse.y);
                                        startX = start.x;
                                        startY = start.y;
                                        snapLines = Transform.snapLines(
                                            root.mvmController.outputWidth,
                                            root.mvmController.outputHeight,
                                            root.mvmController.previewSnapRects(textLayer.clipId));
                                        draggingText = true;
                                        root.draggingTextClipId = textLayer.clipId;
                                        // selectClip は clip の先頭へ seek するので使わない。
                                        // 再生位置は動かさず、選択だけを変える。
                                        root.mvmController.selectTimelineClips([textLayer.clipId]);
                                    }
                                    onReleased: {
                                        if (!draggingText)
                                            return;
                                        draggingText = false;
                                        const moved = textLayer.dragOffsetX !== 0 || textLayer.dragOffsetY !== 0;
                                        const x = Math.max(0, Math.min(root.mvmController.outputWidth - 1,
                                            Math.round(originalX + textLayer.dragOffsetX
                                                       * root.mvmController.outputWidth / textLayer.width)));
                                        const y = Math.max(0, Math.min(root.mvmController.outputHeight - 1,
                                            Math.round(originalY + textLayer.dragOffsetY
                                                       * root.mvmController.outputHeight / textLayer.height)));
                                        textLayer.dragOffsetX = 0;
                                        textLayer.dragOffsetY = 0;
                                        // 新しい位置を確定してから engine の合成へ戻す。
                                        // クリックだけ (移動なし) なら選択だけにして Undo を積まない。
                                        if (moved)
                                            root.mvmController.updateTextClip(textLayer.clipId, {x: x, y: y});
                                        root.draggingTextClipId = "";
                                        previewTransform.guides = { "xs": [], "ys": [] };
                                    }
                                    onCanceled: {
                                        draggingText = false;
                                        textLayer.dragOffsetX = 0;
                                        textLayer.dragOffsetY = 0;
                                        root.draggingTextClipId = "";
                                        previewTransform.guides = { "xs": [], "ys": [] };
                                    }
                                }
                            }
                        }

                        PreviewTransformOverlay {
                            id: previewTransform
                            anchors.fill: parent
                            z: 50
                            mvmController: root.mvmController
                            active: root.timelineTool === "select" && !root.mvmController.busy
                                    && !root.textEditing
                        }

                        MouseArea {
                            anchors.fill: parent
                            z: 8
                            // 無効でも cursorShape は効くので、文字ツール以外では出さない。
                            // 出したままだと選択ツールでも preview 上が文字カーソルになる。
                            visible: root.timelineTool === "text"
                            enabled: root.timelineTool === "text" && !root.mvmController.busy
                            cursorShape: Qt.IBeamCursor
                            onClicked: mouse => {
                                if (root.textEditing)
                                    root.finishTextEditing(true);
                                const pixelX = Math.floor(mouse.x * root.mvmController.outputWidth / width);
                                const pixelY = Math.floor(mouse.y * root.mvmController.outputHeight / height);
                                const hit = root.mvmController.textClipAt(pixelX, pixelY);
                                const data = hit.length > 0 ? root.mvmController.textClipData(hit) : null;
                                root.editingTextClipId = hit;
                                root.textEditorX = data ? data.x * width / root.mvmController.outputWidth : mouse.x;
                                root.textEditorY = data ? data.y * height / root.mvmController.outputHeight : mouse.y;
                                textEditor.text = data ? data.content : "";
                                root.textEditing = true;
                                textEditor.forceActiveFocus();
                            }
                        }

                        Rectangle {
                            id: textEditorFrame
                            z: 9
                            visible: root.textEditing
                            x: root.textEditorX
                            y: root.textEditorY
                            width: Math.max(80, previewHost.width - x)
                            height: Math.max(40, textEditor.contentHeight + 10)
                            color: "#66000000"
                            border.color: "#75baff"
                            TextEdit {
                                id: textEditor
                                anchors.fill: parent
                                anchors.margins: 5
                                color: "white"
                                font.family: root.editingTextClipId.length > 0
                                             ? (root.mvmController.textClipData(root.editingTextClipId).fontFamily || "Meiryo")
                                             : "Meiryo"
                                font.pixelSize: Math.max(8, 64 * previewHost.height /
                                                         root.mvmController.outputHeight)
                                wrapMode: TextEdit.NoWrap
                                selectByMouse: true
                                Keys.onEscapePressed: root.finishTextEditing(false)
                                Keys.onPressed: event => {
                                    if ((event.modifiers & Qt.ControlModifier)
                                            && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
                                        root.finishTextEditing(true);
                                        event.accepted = true;
                                    }
                                }
                                onActiveFocusChanged: {
                                    if (!activeFocus && root.textEditing)
                                        root.finishTextEditing(true);
                                }
                            }
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.leftMargin: 8
                Layout.preferredWidth: 110
                Layout.minimumWidth: 110
                Layout.maximumWidth: 110
                Layout.fillHeight: true
                spacing: 4
                AudioMeter {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    dbLeft: root.mvmController.audioMeterDbLeft
                    dbRight: root.mvmController.audioMeterDbRight
                }
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: "音量 " + Math.round(root.mvmController.masterVolume * 100) + "%"
                    color: "#9aa2ad"
                    font.pixelSize: 10
                }
                Slider {
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    value: root.mvmController.masterVolume
                    onMoved: root.mvmController.masterVolume = value
                }
            }
        }

        // --- トランスポート -------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: root.mvmController.currentTimeText
                color: "#e6e8ec"
                font.bold: true
                font.family: "Consolas"
                font.pixelSize: 16
            }
            Label {
                text: root.mvmController.timelineFpsText
                      + (root.mvmController.frameRateMeasured ? "" : " (未計測)")
                      + "  |  zoom " + Math.round(timelinePanel.pixelsPerFrame * 100) + "%"
                color: root.mvmController.frameRateMeasured ? "#9aa2ad" : "#f2c66d"
                ToolTip.visible: !root.mvmController.frameRateMeasured && hovered
                ToolTip.text: "このframe rateのpreviewは実測していません"

                HoverHandler { id: fpsHover }
                property bool hovered: fpsHover.hovered
            }
            Item { Layout.fillWidth: true }
            Label {
                text: {
                    const info = timelineToolPanel.toolInfo(root.timelineTool);
                    return (info ? info.name + ": " + info.hint + "  |  " : "")
                           + "Alt+操作: リンクの片方だけ / Space: 再生・一時停止 / Alt+ホイール: ズーム";
                }
                color: "#6f7681"
                font.pixelSize: 11
                elide: Text.ElideLeft
                Layout.maximumWidth: timelinePanel.width * 0.75
            }
        }

        // --- タイムライン ---------------------------------------------------
        Rectangle {
            id: timelinePanel
            objectName: "timelinePanel"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 200
            Layout.preferredHeight: root.height * 0.48
            radius: 5
            color: "#20242a"
            border.color: "#3c424c"

            // タイムラインを押したらキーボードの宛先をここへ移す。プロジェクトパネルに
            // フォーカスが残ったままだと、Delete が素材の削除になる。最前面の MouseArea で
            // 受けると hover とカーソルが下の clip・ruler へ届かないので、PressFocus (window の
            // press を先に見る) にする。
            PressFocus {}

            // 離散段階で管理し、下限へ到達した後も逆方向のwheelを確実に受理する。
            readonly property var zoomLevels: [0.005, 0.01, 0.02, 0.05, 0.1, 0.2,
                                                0.35, 0.5, 0.75, 1.0, 1.5, 2, 3, 4,
                                                6, 8, 12, 16, 24]
            property int zoomIndex: 10
            property int observedTimelineFrames: root.mvmController.navigationTimelineFrames
            onObservedTimelineFramesChanged: {
                zoomIndex = Math.max(minimumZoomIndex,
                                     Math.min(zoomLevels.length - 1, zoomIndex));
            }
            // 通常の最小倍率でも全体が収まらない長いtimelineだけ、70%幅へ収める
            // 特別なfit倍率を使う。短いtimelineで最大倍率側へ固定しない。
            readonly property real requestedFitPixelsPerFrame:
                root.mvmController.totalTimelineFrames > 0 && timelineFlick.width > 0
                ? timelineFlick.width * 0.7 / root.mvmController.totalTimelineFrames
                : zoomLevels[0]
            readonly property real fitPixelsPerFrame:
                Math.min(zoomLevels[0], requestedFitPixelsPerFrame)
            readonly property int minimumZoomIndex: 0
            readonly property real pixelsPerFrame:
                zoomIndex === minimumZoomIndex ? fitPixelsPerFrame : zoomLevels[zoomIndex]
            property string activeDragLinkGroup: ""
            property string activeDragClipId: ""
            property bool activeDragDuplicate: false
            property bool activeDragMoved: false
            property real activeDragOffsetX: 0
            property string activeDragTrackKind: ""
            // clip 移動で吸着した frame (吸着の目印を描く)。吸着していなければ -1。
            property real snapGuideFrame: -1
            // 吸着させる距離 (px)。Ctrl を押している間は吸着しない (プレビューの枠と同じ)。
            readonly property real snapThresholdPixels: 8
            property real activeDragOffsetY: 0
            property real selectionStartX: 0
            property real selectionStartY: 0
            property real selectionCurrentX: 0
            property real selectionCurrentY: 0
            property bool selecting: false
            readonly property string tool: root.timelineTool
            // リンク相手にも適用する編集の途中経過。操作中の clip が自分の見かけの変化を
            // ここへ出し、同じ link group の相手 clip が読んで同じだけ動いて見せる。
            // Alt を押しながらの操作 (片方だけ) では linkedEditGroup を空にする。
            property string linkedEditGroup: ""
            property string linkedEditClipId: ""
            property real linkedLeftDelta: 0
            property real linkedRightDelta: 0
            property int linkedSlideFrames: 0
            // レート調整の drag 中の表示。{clipId: {startDelta, endDelta, speed}}。
            // リンク相手は同じ速度で自分の尺になるので、共有の linkedLeft/RightDelta では
            // 表せない。確定と同じ計算 (mvmController.previewRateStretch) の結果をそのまま使う。
            property var ratePreviewClips: ({})
            property int linkedSlipDelta: 0
            // ローリング / スライドで一緒に動く隣の clip の編集点。{kind, index, frame, side}。
            // side="start" はその frame から始まる clip の左端、"end" はその frame で終わる
            // clip の右端が adjacentEditDelta だけ動いて見える。確定は Project 側が行い、
            // ここは drag 中の表示だけを受け持つ。
            property var adjacentEditPoints: []
            property int adjacentEditDelta: 0
            // clip の端のハンドルを使うツール。それ以外のツールでは端も clip 本体として扱う。
            readonly property bool edgeToolActive: tool === "select" || tool === "ripple"
                                                   || tool === "rolling" || tool === "rate"
            readonly property bool trackSelectToolActive: tool === "trackForward"
                                                          || tool === "trackBackward"
            // timeline の表示だけを変えるツール。clip や ruler への操作を受けない。
            readonly property bool viewToolActive: tool === "hand" || tool === "zoom"
            // 端を掴むときのカーソル (TrimCursor)。premiere と同じく、選択ツールの trim は赤、
            // リップルは黄のブラケット。ブラケットは端の種類 (左端 "[" / 右端 "]")、矢印は pointer の
            // ある側を向く: 右端の内側 <-]・外側 [->、左端の内側 [->・外側 <-]。どちらの側でも
            // 同じ端を動かす。ローリングとレート調整は挙動が違うので従来の形のまま。
            readonly property string edgeCursorMode: tool === "rolling" ? "split"
                                                     : tool === "rate" ? "sizeHor" : "trim"
            readonly property color edgeCursorColor: tool === "ripple" ? "#f2c94c" : "#e8413c"
            readonly property color edgeHandleColor: tool === "ripple" ? "#e8c15a"
                                                     : tool === "rolling" ? "#e27d6a"
                                                     : tool === "rate" ? "#b99af0" : "#85c4ee"
            // 素材ドラッグ中のドロップ先 ({kind, index, frame})。null ならタイムライン外。
            property var dropTarget: null
            readonly property real toolPanelWidth: 34
            readonly property real labelWidth: 96
            readonly property real rulerHeight: 26
            readonly property real trackHeight: 54
            // 最上段の video track (Vn) の上に置く「+V」の行。track と一緒に scroll する。
            readonly property real addVideoRowHeight: 28
            // track 領域の上端 (content 座標)。ruler と「+V」の行の下。
            readonly property real tracksTop: rulerHeight + addVideoRowHeight
            readonly property int videoCount: root.mvmController.videoTrackCount
            readonly property int audioCount: root.mvmController.audioTrackCount
            readonly property int rowCount: videoCount + audioCount
            readonly property real tracksHeight: rowCount * trackHeight
            // ルーラーの目盛り間隔。ズームに応じて 1/2/5/10/30/60 秒から選ぶ。
            readonly property int tickSeconds: {
                const nominalFps = Math.max(1, Math.round(root.mvmController.timelineFpsNum / root.mvmController.timelineFpsDen));
                const candidates = [1, 2, 5, 10, 30, 60, 300, 900, 3600];
                for (let index = 0; index < candidates.length; ++index) {
                    if (candidates[index] * nominalFps * pixelsPerFrame >= 70)
                        return candidates[index];
                }
                return candidates[candidates.length - 1];
            }

            // video は index が大きいほど上。audio は video の下へ順に並べる。
            function rowIndexFor(kind, index) {
                return kind === "video" ? (videoCount - 1 - index) : (videoCount + index);
            }
            function rowY(kind, index) {
                return tracksTop + rowIndexFor(kind, index) * trackHeight;
            }
            // トラック領域内の y からトラックを引く。範囲外は null。
            function trackAtY(y) {
                const row = Math.floor((y - tracksTop) / trackHeight);
                if (row < 0 || row >= rowCount)
                    return null;
                if (row < videoCount)
                    return { "kind": "video", "index": videoCount - 1 - row };
                return { "kind": "audio", "index": row - videoCount };
            }
            // drag中の中心Yを、同じ種別の最寄りtrackへ必ずsnapする。
            function trackForDrag(kind, trackAreaY) {
                let row = Math.floor(trackAreaY / trackHeight);
                if (kind === "video") {
                    row = Math.max(0, Math.min(videoCount - 1, row));
                    return { "kind": "video", "index": videoCount - 1 - row };
                }
                row = Math.max(videoCount, Math.min(rowCount - 1, row));
                return { "kind": "audio", "index": row - videoCount };
            }
            function frameAtContentX(contentX) {
                return Math.max(0, Math.round(contentX / pixelsPerFrame));
            }
            function setZoom(direction, anchorItemX) {
                const nextIndex = Math.max(minimumZoomIndex, Math.min(zoomLevels.length - 1,
                                                       zoomIndex + direction));
                if (nextIndex === zoomIndex)
                    return;
                // カーソル下のフレームを固定したままズームする。
                const anchorFrame = (timelineFlick.contentX + anchorItemX) / pixelsPerFrame;
                zoomIndex = nextIndex;
                const nextContentWidth = Math.max(
                    timelineFlick.width,
                    root.mvmController.navigationTimelineFrames * pixelsPerFrame + 240);
                const nextMaxContentX = Math.max(0, nextContentWidth - timelineFlick.width);
                const desiredContentX = anchorFrame * pixelsPerFrame - anchorItemX;
                timelineFlick.contentX = Math.max(
                    0, Math.min(nextMaxContentX, desiredContentX));
            }

            // 再生ヘッドが表示幅の端へ近づいた時だけ、約70%幅ずつページを送る。
            function pageForPlayback() {
                if (!root.mvmController.playing || timelineFlick.width <= 0 ||
                    root.mvmController.totalTimelineFrames <= 0)
                    return;
                const headX = root.mvmController.playheadFrame * pixelsPerFrame;
                const leftEdge = timelineFlick.contentX;
                const viewportWidth = timelineFlick.width;
                const maxContentX = Math.max(0, timelineFlick.contentWidth - viewportWidth);
                if (root.mvmController.shuttleRate < 0) {
                    if (leftEdge <= 0 || headX > leftEdge + viewportWidth * 0.15)
                        return;
                    timelineFlick.contentX = Math.max(0, headX - viewportWidth * 0.85);
                } else {
                    if (leftEdge >= maxContentX || headX < leftEdge + viewportWidth * 0.85)
                        return;
                    timelineFlick.contentX = Math.min(maxContentX,
                                                       headX - viewportWidth * 0.15);
                }
            }

            // QQuickWindowのevent filterがFlickableより先にAlt/Ctrl wheelを捕捉し、
            // modifierを判定済みの専用入口へ渡す。
            function handleNativeAltWheel(wheelDelta, localX) {
                if (wheelDelta === 0)
                    return;
                setZoom(wheelDelta > 0 ? 1 : -1,
                        Math.max(0, localX - timelineFlick.x));
            }
            // track 上で frame に side の端を持つ clip があるか。
            function hasEditPoint(spans, kind, index, frame, side) {
                for (const span of spans) {
                    if (span.trackKind === kind && span.trackIndex === index
                            && (side === "start" ? span.start : span.end) === frame)
                        return true;
                }
                return false;
            }
            // source (とリンク相手) が動かす編集点を集める。mode は "rolling" / "slide"。
            // ローリングでリンク相手が編集点を持たなければ (L / J カット)、Project と同じく
            // 相手は動かさない。そのとき false を返す。
            function beginAdjacentPreview(sourceClipId, mode, edge, linked) {
                const spans = root.mvmController.timelineModel.clipSpans();
                const source = spans.find(span => span.clipId === sourceClipId);
                if (!source)
                    return false;
                const moving = [source];
                if (linked && source.linkGroupId !== "") {
                    for (const span of spans) {
                        if (span !== source && span.linkGroupId === source.linkGroupId)
                            moving.push(span);
                    }
                }
                const points = [];
                let partnerMoves = true;
                for (const span of moving) {
                    if (mode === "rolling") {
                        const frame = edge === "right" ? span.end : span.start;
                        const side = edge === "right" ? "start" : "end";
                        if (span !== source && !hasEditPoint(spans, span.trackKind, span.trackIndex, frame, side)) {
                            partnerMoves = false;
                            continue;
                        }
                        points.push({ "kind": span.trackKind, "index": span.trackIndex,
                                      "frame": frame, "side": side });
                    } else {
                        points.push({ "kind": span.trackKind, "index": span.trackIndex,
                                      "frame": span.start, "side": "end" });
                        points.push({ "kind": span.trackKind, "index": span.trackIndex,
                                      "frame": span.end, "side": "start" });
                    }
                }
                adjacentEditDelta = 0;
                adjacentEditPoints = points;
                return partnerMoves;
            }
            // 隣接 clip の見かけの伸縮。start / end はその clip の timeline 上の端。
            function adjacentDeltaFor(kind, index, start, end, side) {
                for (const point of adjacentEditPoints) {
                    if (point.side === side && point.kind === kind && point.index === index
                            && point.frame === (side === "start" ? start : end))
                        return adjacentEditDelta;
                }
                return 0;
            }

            // トラックの選択ツール。Shift なら押した track だけ、そうでなければ全 track。
            function selectFromFrame(frame, modifiers, trackKind, trackIndex) {
                const singleTrack = (modifiers & Qt.ShiftModifier) !== 0;
                root.mvmController.selectClipsFromFrame(
                    frame, tool === "trackForward" ? "forward" : "backward",
                    singleTrack ? trackKind : "", trackIndex);
            }

            // --- ツールパネル (左端) ---
            TimelineToolPanel {
                id: timelineToolPanel
                x: 0
                y: 0
                width: timelinePanel.toolPanelWidth
                height: parent.height
                currentTool: root.timelineTool
                onToolRequested: tool => root.timelineTool = tool
            }
            function handleNativeCtrlWheel(wheelDelta) {
                timelineFlick.contentY = Math.max(
                    0,
                    Math.min(timelineFlick.contentHeight - timelineFlick.height,
                             timelineFlick.contentY - wheelDelta));
            }
            function scrollTimelineHorizontally(wheelDelta) {
                timelineFlick.contentX = Math.max(
                    0,
                    Math.min(timelineFlick.contentWidth - timelineFlick.width,
                             timelineFlick.contentX - wheelDelta));
            }
            function handleNativeShiftWheel(wheelDelta) {
                scrollTimelineHorizontally(wheelDelta);
            }
            function handleNativePlainWheel(wheelDelta) {
                const maxY = Math.max(0, timelineFlick.contentHeight - timelineFlick.height);
                if (maxY > 0) {
                    timelineFlick.contentY = Math.max(
                        0, Math.min(maxY, timelineFlick.contentY - wheelDelta));
                    return;
                }
                scrollTimelineHorizontally(wheelDelta);
            }

            // --- トラックヘッダ (左端) ---
            Item {
                id: headerColumn
                x: timelinePanel.toolPanelWidth
                y: 0
                width: timelinePanel.labelWidth
                height: parent.height
                clip: true

                // 目玉のドラッグ塗り。押した track の表示を反転し、ドラッグで通った video track を
                // 同じ値にする (Photoshop のレイヤーの目玉)。通った track は先に見た目だけ変え、
                // 離したときにまとめて確定する (1 回の undo。途中で model を作り直すと押した
                // delegate が消えてドラッグが切れるため、途中では確定しない)。
                property bool eyePainting: false
                property bool eyePaintMuted: false
                property var eyePaintIndices: []
                property int eyePaintLastIndex: -1

                function beginEyePaint(index, muted) {
                    eyePaintMuted = EyePaint.paintMutedFor(muted);
                    eyePaintIndices = [index];
                    eyePaintLastIndex = index;
                    eyePainting = true;
                }
                function continueEyePaint(contentY) {
                    if (!eyePainting)
                        return;
                    const index = EyePaint.videoIndexAtY(contentY, timelinePanel.tracksTop,
                                                         timelinePanel.trackHeight,
                                                         timelinePanel.videoCount);
                    if (index < 0 || index === eyePaintLastIndex)
                        return;
                    eyePaintIndices = EyePaint.addPassed(eyePaintIndices, eyePaintLastIndex, index);
                    eyePaintLastIndex = index;
                }
                function finishEyePaint() {
                    if (!eyePainting)
                        return;
                    const indices = eyePaintIndices;
                    const muted = eyePaintMuted;
                    cancelEyePaint();
                    root.mvmController.setTracksMuted("video", indices, muted);
                }
                function cancelEyePaint() {
                    eyePainting = false;
                    eyePaintIndices = [];
                    eyePaintLastIndex = -1;
                }

                // M / S の小さなトグル。押した時点で確定する。
                component TrackToggle: Rectangle {
                    id: toggle
                    required property string label
                    required property bool active
                    required property color activeColor
                    property string tip: ""
                    signal toggled()

                    width: 20
                    height: 20
                    radius: 3
                    color: active ? activeColor : (toggleArea.containsMouse ? "#3a414c" : "#2b3038")
                    border.color: "#4a515c"
                    opacity: enabled ? 1 : 0.5

                    Label {
                        anchors.centerIn: parent
                        text: toggle.label
                        color: toggle.active ? "#15181c" : "#c9ccd2"
                        font.pixelSize: 11
                        font.bold: true
                    }
                    MouseArea {
                        id: toggleArea
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton
                        onClicked: toggle.toggled()
                    }
                    ToolTip.visible: toggleArea.containsMouse && tip !== ""
                    ToolTip.text: tip
                }

                component TrackHeader: Rectangle {
                    id: header
                    required property string headerKind
                    required property int headerIndex
                    required property string headerName
                    required property bool headerMuted
                    required property bool headerSolo
                    required property bool headerOutputEnabled
                    readonly property bool video: headerKind === "video"
                    readonly property bool shownMuted: EyePaint.displayedMuted(
                        headerIndex, headerMuted, headerColumn.eyePainting,
                        headerColumn.eyePaintIndices, headerColumn.eyePaintMuted)

                    width: timelinePanel.labelWidth
                    height: timelinePanel.trackHeight
                    y: timelinePanel.rowY(headerKind, headerIndex) - timelineFlick.contentY
                    color: video ? "#252a31" : "#232a2a"
                    border.color: "#3c424c"

                    // video: 表示/非表示の目玉。押してから上下へドラッグすると、通った track も
                    // 押した track と同じ表示状態にする。
                    Item {
                        id: eyeButton
                        objectName: "trackEye_" + header.headerKind + "_" + header.headerIndex
                        visible: header.video
                        x: 4
                        anchors.verticalCenter: parent.verticalCenter
                        width: 20
                        height: 20
                        opacity: eyeArea.enabled ? 1 : 0.5

                        Rectangle {
                            anchors.fill: parent
                            radius: 3
                            color: eyeArea.containsMouse && !headerColumn.eyePainting ? "#3a414c" : "transparent"
                        }
                        TrackEyeIcon {
                            objectName: "trackEyeIcon_" + header.headerKind + "_" + header.headerIndex
                            anchors.centerIn: parent
                            width: 16
                            height: 16
                            hidden: header.shownMuted
                            color: header.shownMuted ? "#4f9cf0" : "#c9ccd2"
                        }
                        MouseArea {
                            id: eyeArea
                            anchors.fill: parent
                            enabled: header.video && !root.mvmController.busy
                            hoverEnabled: true
                            preventStealing: true
                            acceptedButtons: Qt.LeftButton
                            onPressed: headerColumn.beginEyePaint(header.headerIndex, header.headerMuted)
                            onPositionChanged: mouse => {
                                if (!pressed)
                                    return;
                                const point = mapToItem(headerColumn, mouse.x, mouse.y);
                                headerColumn.continueEyePaint(point.y + timelineFlick.contentY);
                            }
                            onReleased: headerColumn.finishEyePaint()
                            onCanceled: headerColumn.cancelEyePaint()
                        }
                        ToolTip.visible: eyeArea.containsMouse && !headerColumn.eyePainting
                        ToolTip.text: header.headerMuted ? "トラックを表示 (ドラッグで連続切り替え)"
                                                         : "トラックを非表示 (ドラッグで連続切り替え)"
                    }

                    // audio: ミュートとソロ。
                    Row {
                        id: audioToggles
                        visible: !header.video
                        x: 4
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2

                        TrackToggle {
                            objectName: "trackMute_" + header.headerKind + "_" + header.headerIndex
                            label: "M"
                            active: header.headerMuted
                            activeColor: "#5fb878"
                            enabled: !root.mvmController.busy
                            tip: header.headerMuted ? "ミュート解除" : "ミュート"
                            onToggled: root.mvmController.setTrackMuted(header.headerKind, header.headerIndex,
                                                                        !header.headerMuted)
                        }
                        TrackToggle {
                            objectName: "trackSolo_" + header.headerKind + "_" + header.headerIndex
                            label: "S"
                            active: header.headerSolo
                            activeColor: "#e8c15a"
                            enabled: !root.mvmController.busy
                            tip: header.headerSolo ? "ソロ解除" : "ソロ (このトラックだけを鳴らす)"
                            onToggled: root.mvmController.setTrackSolo(header.headerKind, header.headerIndex,
                                                                       !header.headerSolo)
                        }
                    }

                    Label {
                        anchors.left: header.video ? eyeButton.right : audioToggles.right
                        anchors.leftMargin: 5
                        anchors.verticalCenter: parent.verticalCenter
                        text: header.headerName
                        // 非表示・ミュート・他 track のソロで出力されない track は暗くする。
                        color: header.headerOutputEnabled && !header.shownMuted ? "#c9ccd2" : "#8b8f96"
                        font.bold: true
                    }

                    Button {
                        anchors.right: parent.right
                        anchors.rightMargin: 3
                        anchors.verticalCenter: parent.verticalCenter
                        implicitWidth: 18
                        implicitHeight: 18
                        text: "×"
                        flat: true
                        // 再生中に track index が変わると preview の対応が崩れる。
                        enabled: !root.mvmController.busy && !root.mvmController.playing
                        ToolTip.visible: hovered
                        ToolTip.text: "このトラックを削除"
                        onClicked: root.mvmController.removeTrack(header.headerKind, header.headerIndex)
                    }
                }

                Repeater {
                    model: root.mvmController.videoTrackModel
                    delegate: TrackHeader {
                        required property string trackName
                        required property bool trackMuted
                        required property bool trackSolo
                        required property bool trackOutputEnabled
                        required property int trackIndex
                        headerKind: "video"
                        headerIndex: trackIndex
                        headerName: trackName
                        headerMuted: trackMuted
                        headerSolo: trackSolo
                        headerOutputEnabled: trackOutputEnabled
                    }
                }
                Repeater {
                    model: root.mvmController.audioTrackModel
                    delegate: TrackHeader {
                        required property string trackName
                        required property bool trackMuted
                        required property bool trackSolo
                        required property bool trackOutputEnabled
                        required property int trackIndex
                        headerKind: "audio"
                        headerIndex: trackIndex
                        headerName: trackName
                        headerMuted: trackMuted
                        headerSolo: trackSolo
                        headerOutputEnabled: trackOutputEnabled
                    }
                }

                Rectangle {
                    y: timelinePanel.tracksTop + timelinePanel.videoCount * timelinePanel.trackHeight
                       - timelineFlick.contentY - 2
                    width: parent.width
                    height: 4
                    color: "#59636f"
                    z: 10
                }

                // video は最上段 (Vn) の上に足すので、追加ボタンを Vn の上の専用行に置く。
                Button {
                    y: timelinePanel.rulerHeight - timelineFlick.contentY
                       + (timelinePanel.addVideoRowHeight - height) / 2
                    x: 3
                    implicitHeight: 22
                    implicitWidth: 42
                    text: "+V"
                    enabled: !root.mvmController.busy
                    ToolTip.visible: hovered
                    ToolTip.text: "video トラックを追加"
                    onClicked: root.mvmController.addTrack("video")
                }

                // audio は最下段の下に足すので、追加ボタンも最下段の下に置く。
                Button {
                    y: timelinePanel.tracksTop + timelinePanel.tracksHeight
                       - timelineFlick.contentY + 4
                    x: 3
                    implicitHeight: 22
                    implicitWidth: 42
                    text: "+A"
                    enabled: !root.mvmController.busy
                    ToolTip.visible: hovered
                    ToolTip.text: "audio トラックを追加"
                    onClicked: root.mvmController.addTrack("audio")
                }

                // track headerがscrollしてもruler領域へ描画されないよう覆う。
                Rectangle {
                    x: 0
                    y: 0
                    width: parent.width
                    height: timelinePanel.rulerHeight
                    color: "#191c21"
                    z: 100
                }
            }

            // --- スクロール領域 ---
            Flickable {
                id: timelineFlick
                x: timelinePanel.toolPanelWidth + timelinePanel.labelWidth
                y: 0
                width: parent.width - x - 4
                height: parent.height - 4
                clip: true
                contentWidth: Math.max(width, root.mvmController.navigationTimelineFrames * timelinePanel.pixelsPerFrame + 240)
                contentHeight: Math.max(height, timelinePanel.tracksTop
                                        + timelinePanel.tracksHeight + 34)
                boundsBehavior: Flickable.StopAtBounds
                flickableDirection: Flickable.HorizontalAndVerticalFlick
                interactive: false

                ScrollBar.horizontal: ScrollBar {
                    id: timelineHorizontalScrollBar
                    policy: ScrollBar.AlwaysOn
                    interactive: true
                    active: true
                    height: 12
                    minimumSize: 0.08
                }
                ScrollBar.vertical: ScrollBar {
                    id: timelineVerticalScrollBar
                    policy: ScrollBar.AsNeeded
                    interactive: true
                    // Flickable自体はclip操作との競合を避けるためinteractive=false。
                    // その場合もfade-outさせず、overflow中は必ず操作可能にする。
                    active: timelineFlick.contentHeight > timelineFlick.height
                    width: 12
                    minimumSize: 0.08
                }

                Item {
                    id: timelineContent
                    width: timelineFlick.contentWidth
                    // sticky rulerを最下部までscrollしてもcontentの内側に保つ。
                    height: timelineFlick.contentHeight

                    // --- ルーラー ---
                    Rectangle {
                        id: ruler
                        x: 0
                        // 縦scrollから独立させ、常に上端へ固定する。
                        y: timelineFlick.contentY
                        width: parent.width
                        height: timelinePanel.rulerHeight
                        color: "#191c21"
                        // 後から宣言されるtrack、clip、選択矩形より常に前面へ置く。
                        z: 100

                        Repeater {
                            model: {
                                const nominalFps = Math.max(1, Math.round(root.mvmController.timelineFpsNum / root.mvmController.timelineFpsDen));
                                const framesPerTick = timelinePanel.tickSeconds * nominalFps;
                                return Math.ceil(timelineContent.width / (framesPerTick * timelinePanel.pixelsPerFrame)) + 1;
                            }

                            Item {
                                id: rulerTick
                                required property int index
                                readonly property int nominalFps: Math.max(1, Math.round(root.mvmController.timelineFpsNum / root.mvmController.timelineFpsDen))
                                x: index * timelinePanel.tickSeconds * nominalFps * timelinePanel.pixelsPerFrame
                                width: 1
                                height: ruler.height

                                Rectangle {
                                    anchors.bottom: parent.bottom
                                    width: 1
                                    height: 10
                                    color: "#77808c"
                                }
                                Label {
                                    x: 4
                                    y: 1
                                    text: {
                                        const seconds = rulerTick.index * timelinePanel.tickSeconds;
                                        const minutes = Math.floor(seconds / 60);
                                        const rest = seconds % 60;
                                        return minutes + ":" + (rest < 10 ? "0" : "") + rest;
                                    }
                                    color: "#aab1ba"
                                    font.pixelSize: 10
                                }
                            }
                        }

                        Rectangle {
                            visible: root.mvmController.inFrame >= 0
                                     && root.mvmController.outFrame > root.mvmController.inFrame
                            x: root.mvmController.inFrame * timelinePanel.pixelsPerFrame
                            width: Math.max(1, (root.mvmController.outFrame
                                                - root.mvmController.inFrame)
                                               * timelinePanel.pixelsPerFrame)
                            height: 4
                            anchors.bottom: parent.bottom
                            color: "#56a5e8"
                        }
                        Repeater {
                            model: root.mvmController.timelineMarkers
                            Rectangle {
                                required property var modelData
                                x: modelData * timelinePanel.pixelsPerFrame - 3
                                y: 2
                                width: 7
                                height: 12
                                color: "#f3bd54"
                                radius: 2
                            }
                        }
                        Rectangle {
                            visible: root.mvmController.inFrame >= 0
                            x: root.mvmController.inFrame * timelinePanel.pixelsPerFrame - 2
                            y: 0
                            width: 4
                            height: parent.height
                            color: "#56a5e8"
                        }
                        Rectangle {
                            visible: root.mvmController.outFrame >= 0
                            x: root.mvmController.outFrame * timelinePanel.pixelsPerFrame - 2
                            y: 0
                            width: 4
                            height: parent.height
                            color: "#56a5e8"
                        }

                        // ルーラー上はクリックでもドラッグでもスクラブできる。
                        MouseArea {
                            id: rulerArea
                            anchors.fill: parent
                            enabled: !root.mvmController.busy
                                     && root.mvmController.navigationTimelineFrames > 0
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            cursorShape: Qt.SizeHorCursor
                            preventStealing: true
                            property var menuMarkerFrame: -1
                            onPressed: mouse => {
                                if (mouse.button === Qt.RightButton) {
                                    menuMarkerFrame = Gestures.markerNearRulerX(
                                        mouse.x, timelinePanel.pixelsPerFrame,
                                        root.mvmController.timelineMarkers, 9);
                                    rulerMarkMenu.popup();
                                    return;
                                }
                                root.mvmController.beginScrub();
                                root.mvmController.scrubToFrame(timelinePanel.frameAtContentX(mouse.x));
                            }
                            onPositionChanged: mouse => {
                                if (pressedButtons & Qt.LeftButton)
                                    root.mvmController.scrubToFrame(timelinePanel.frameAtContentX(mouse.x));
                            }
                            onReleased: mouse => {
                                if (mouse.button === Qt.LeftButton)
                                    root.mvmController.endScrub();
                            }
                            onCanceled: root.mvmController.endScrub()

                            CompactMenu {
                                id: rulerMarkMenu
                                CompactMenuItem {
                                    text: "マーカーを削除"
                                    visible: rulerArea.menuMarkerFrame >= 0
                                    height: visible ? implicitHeight : 0
                                    onTriggered: root.mvmController.deleteTimelineMarker(
                                                     rulerArea.menuMarkerFrame)
                                }
                                CompactMenuItem {
                                    text: "インを消去"
                                    enabled: root.mvmController.inFrame >= 0
                                    onTriggered: root.mvmController.clearIn()
                                }
                                CompactMenuItem {
                                    text: "アウトを消去"
                                    enabled: root.mvmController.outFrame >= 0
                                    onTriggered: root.mvmController.clearOut()
                                }
                                CompactMenuItem {
                                    text: "イン・アウトを消去"
                                    enabled: root.mvmController.inFrame >= 0
                                             || root.mvmController.outFrame >= 0
                                    onTriggered: root.mvmController.clearInOut()
                                }
                            }
                        }
                    }

                    // --- 素材ドロップ ---
                    // プロジェクトパネルの素材を受ける。外部ファイルは window 全体の videoDropArea が受ける。
                    DropArea {
                        id: mediaBinDropArea
                        anchors.fill: parent
                        keys: ["mvm-media-bin"]
                        onPositionChanged: drag => {
                            timelinePanel.dropTarget = root.timelineDropTarget(mediaBinDropArea,
                                                                               drag.x, drag.y);
                            projectPanel.dragLabelOverride = timelinePanel.dropTarget
                                                             ? "タイムラインへ追加" : "";
                        }
                        onExited: {
                            timelinePanel.dropTarget = null;
                            projectPanel.dragLabelOverride = "";
                        }
                        onDropped: drop => {
                            const target = root.timelineDropTarget(mediaBinDropArea, drop.x, drop.y);
                            const ids = projectPanel.dragIds.slice();
                            timelinePanel.dropTarget = null;
                            if (!target || ids.length === 0)
                                return;
                            drop.accept();
                            // 配置は timeline model を作り直す。drag の後始末が終わってから行う。
                            Qt.callLater(() => root.mvmController.addMediaItemsToTimelineAt(
                                             ids, target.kind, target.index, target.frame));
                        }
                    }
                    Item {
                        id: dropTargetHint
                        anchors.fill: parent
                        z: 90
                        visible: timelinePanel.dropTarget !== null

                        Rectangle {
                            readonly property var target: timelinePanel.dropTarget
                            readonly property int rowCount: !target ? 0
                                                            : target.kind === "video"
                                                              ? timelinePanel.videoCount
                                                              : timelinePanel.audioCount
                            x: 0
                            width: parent.width
                            y: !target ? 0
                               : target.index < rowCount
                                 ? timelinePanel.rowY(target.kind, target.index)
                                 : target.kind === "video"
                                   ? timelinePanel.tracksTop - timelinePanel.addVideoRowHeight
                                   : timelinePanel.tracksTop + timelinePanel.tracksHeight
                            height: target && target.kind === "video" && target.index >= rowCount
                                    ? timelinePanel.addVideoRowHeight : timelinePanel.trackHeight
                            color: "#3364a8e8"
                            border.color: "#64a8e8"
                            border.width: 1
                        }
                        Rectangle {
                            x: timelinePanel.dropTarget
                               ? timelinePanel.dropTarget.frame * timelinePanel.pixelsPerFrame : 0
                            y: timelinePanel.tracksTop - timelinePanel.addVideoRowHeight
                            width: 2
                            height: timelinePanel.tracksHeight + timelinePanel.addVideoRowHeight
                                    + timelinePanel.trackHeight
                            color: "#64a8e8"
                        }
                    }

                    // --- トラック背景 ---
                    Item {
                        id: trackArea
                        x: 0
                        y: timelinePanel.tracksTop
                        width: parent.width
                        height: timelinePanel.tracksHeight

                        Rectangle {
                            anchors.fill: parent
                            color: "#191c21"
                        }

                        // 空白から始めた左ドラッグは、触れたclipをすべて選ぶ。
                        // clip delegateは後に描画されるため、clip上のpressはそちらが受ける。
                        MouseArea {
                            id: rectangleSelectionArea
                            anchors.fill: parent
                            enabled: !root.mvmController.busy
                            acceptedButtons: Qt.LeftButton
                            preventStealing: true
                            onPressed: mouse => {
                                const frame = Math.max(0, Math.floor(mouse.x / timelinePanel.pixelsPerFrame));
                                if (timelinePanel.trackSelectToolActive) {
                                    const track = timelinePanel.trackAtY(mouse.y + timelinePanel.tracksTop);
                                    if (track)
                                        timelinePanel.selectFromFrame(frame, mouse.modifiers,
                                                                      track.kind, track.index);
                                    return;
                                }
                                if (timelinePanel.tool === "razor") {
                                    // 空白でも Shift なら、その位置に掛かる全 track の clip を切る。
                                    if ((mouse.modifiers & Qt.ShiftModifier) !== 0)
                                        root.mvmController.splitClipAt("", Math.round(mouse.x / timelinePanel.pixelsPerFrame), true,
                                                                       Gestures.linkedFor(mouse.modifiers));
                                    return;
                                }
                                timelinePanel.selectionStartX = mouse.x;
                                timelinePanel.selectionStartY = mouse.y;
                                timelinePanel.selectionCurrentX = mouse.x;
                                timelinePanel.selectionCurrentY = mouse.y;
                                timelinePanel.selecting = true;
                            }
                            onPositionChanged: mouse => {
                                timelinePanel.selectionCurrentX = mouse.x;
                                timelinePanel.selectionCurrentY = mouse.y;
                            }
                            onReleased: {
                                if (!timelinePanel.selecting)
                                    return;
                                const left = Math.min(timelinePanel.selectionStartX,
                                                      timelinePanel.selectionCurrentX);
                                const right = Math.max(timelinePanel.selectionStartX,
                                                       timelinePanel.selectionCurrentX);
                                const top = Math.min(timelinePanel.selectionStartY,
                                                     timelinePanel.selectionCurrentY);
                                const bottom = Math.max(timelinePanel.selectionStartY,
                                                        timelinePanel.selectionCurrentY);
                                const selectedIds = [];
                                for (let index = 0; index < timelineClips.count; ++index) {
                                    const item = timelineClips.itemAt(index);
                                    if (item && item.x <= right && item.x + item.width >= left
                                            && item.y <= bottom && item.y + item.height >= top)
                                        selectedIds.push(root.mvmController.timelineModel.clipIdAt(index));
                                }
                                timelinePanel.selecting = false;
                                root.mvmController.selectTimelineClips(selectedIds);
                            }
                            onCanceled: timelinePanel.selecting = false
                        }

                        Repeater {
                            model: timelinePanel.rowCount
                            Rectangle {
                                required property int index
                                y: index * timelinePanel.trackHeight
                                width: parent.width
                                height: timelinePanel.trackHeight
                                color: index < timelinePanel.videoCount
                                       ? (index % 2 === 0 ? "#252a31" : "#20252b")
                                       : "#1e2626"
                                border.color: "#343a43"
                            }
                        }

                        // video/audio の境界を通常の track 罫線より太く示す。
                        Rectangle {
                            y: timelinePanel.videoCount * timelinePanel.trackHeight - 2
                            width: parent.width
                            height: 4
                            color: "#59636f"
                            z: 10
                        }

                        // クリップの無い場所での右クリック。リップル削除を出す。
                        MouseArea {
                            id: emptyContextArea
                            anchors.fill: parent
                            acceptedButtons: Qt.RightButton
                            property string menuTrackKind: "video"
                            property int menuTrackIndex: 0
                            property int menuFrame: 0

                            onPressed: mouse => {
                                const track = timelinePanel.trackAtY(mouse.y + timelinePanel.tracksTop);
                                if (!track)
                                    return;
                                // hit test はclipの半開区間に合わせてfloorする。
                                const frame = Math.max(0, Math.floor(mouse.x / timelinePanel.pixelsPerFrame));
                                if (root.mvmController.hasClipAt(track.kind, track.index, frame))
                                    return;
                                menuTrackKind = track.kind;
                                menuTrackIndex = track.index;
                                menuFrame = frame;
                                gapMenu.popup();
                            }

                            CompactMenu {
                                id: gapMenu
                                CompactMenuItem {
                                    text: "リップル削除（空白を詰める）"
                                    enabled: root.mvmController.hasGapAt(emptyContextArea.menuTrackKind,
                                                                    emptyContextArea.menuTrackIndex,
                                                                    emptyContextArea.menuFrame)
                                    onTriggered: root.mvmController.rippleDeleteGap(emptyContextArea.menuTrackKind,
                                                                               emptyContextArea.menuTrackIndex,
                                                                               emptyContextArea.menuFrame)
                                }
                            }
                        }

                        // --- クリップ ---
                        Repeater {
                            id: timelineClips
                            model: root.mvmController.timelineModel

                            delegate: Rectangle {
                                id: clipItem
                                objectName: "timelineClip_" + clipId
                                required property int index
                                required property string clipId
                                required property string displayName
                                required property string clipKind
                                required property real timelineStartFrame
                                required property real timelineDurationFrames
                                required property real sourceInFrame
                                required property real sourceOutFrame
                                required property real sourceFpsNum
                                required property real sourceFpsDen
                                required property real speed
                                required property bool frameHold
                                required property bool previewSupported
                                required property string trackKind
                                required property int trackIndex
                                required property bool linked
                                required property string linkGroupId
                                required property bool selected
                                required property string mediaPath
                                required property var automationKeys
                                required property real automationBase
                                required property bool clipEnabled

                                property var previewKeys: null
                                property var penState: null
                                property int penFrame: 0
                                property real penValue: 100
                                readonly property real automationMaximum: clipKind === "audio" ? 200 : 100
                                // 線の描画・マウス位置→値・当たり判定が共有する座標系 (TimelineGestures.js)。
                                // 線は上下 4px を空けて描き、当たり判定は線の上下 12px、キーの左右 8px。
                                readonly property var penGeometry: ({
                                    "pixelsPerFrame": timelinePanel.pixelsPerFrame,
                                    "maximum": automationMaximum,
                                    "width": width,
                                    "height": height,
                                    "inset": 4,
                                    "keyPixels": 8,
                                    "linePixels": 12
                                })
                                // ペンで指しているキー (clip 先頭からの frame)。-1 は無し。
                                property int penHoverKeyFrame: -1
                                readonly property var shownKeys: previewKeys || automationKeys
                                function automationY(value) {
                                    return Gestures.penY(clipItem.penGeometry, value);
                                }
                                // 自動化の線の頂点 (clip 内の座標)。ペンの当たり判定と同じ頂点を使う。
                                readonly property var automationPoints:
                                    Gestures.penLinePoints(shownKeys, automationBase, penGeometry)
                                        .map(point => Qt.point(point.x, point.y))

                                function penFrameAt(x) {
                                    return Math.max(0, Math.min(clipItem.timelineDurationFrames - 1,
                                                                Math.round(x / timelinePanel.pixelsPerFrame)));
                                }
                                function penValueAt(y) {
                                    return Gestures.penValueAt(clipItem.penGeometry, y);
                                }

                                property real leftPreviewDelta: 0
                                property real rightPreviewDelta: 0
                                property point bodyPressPoint: Qt.point(0, 0)
                                property real bodyDragOffsetX: 0
                                property real bodyDragOffsetY: 0
                                property real rawBodyDragOffsetX: 0
                                // press 時点で一緒に動く clip 群の端 (controller.timelineDragBounds)。
                                property var bodyDragBounds: ({})
                                // press 時点の吸着の候補 (Gestures.dragSnapFrames)。
                                property var bodySnap: null
                                property bool bodyMoved: false
                                property bool bodyAdditiveSelection: false
                                property string dragTrackKind: trackKind
                                property int dragTrackIndex: trackIndex
                                // 本体を押したときのツール操作。"move" / "trackSelect" / "razor"
                                // / "slip" / "slide"。押していない間は空。
                                property string bodyGesture: ""
                                // slip / slide のドラッグ量 (project frame)。
                                property int toolDragFrames: 0
                                // slip で素材の端に止めた後の in の移動量 (素材 frame)。
                                // controller が確定時と同じ規則で求めた値を表示に使う。
                                property int slipSourceDelta: 0
                                // レーザーツールの切断位置の表示。clip 内の x、無ければ -1。
                                property real razorHoverX: -1
                                // 押した時点で Alt が無ければ、リンク相手にも同じ編集を適用する。
                                property bool editLinked: true
                                // clip 本体の press で決めた操作 (Gestures.bodyPress の戻り値)。
                                property var gestureState: null
                                readonly property bool linkedEditSource:
                                    timelinePanel.linkedEditClipId === clipId
                                // リンク相手の clip を操作中。相手の途中経過をそのまま見せる。
                                readonly property bool linkedEditPartner:
                                    linkGroupId !== "" && linkGroupId === timelinePanel.linkedEditGroup
                                    && !linkedEditSource
                                // レート調整の drag 中なら、この clip の表示 (無ければ undefined)。
                                readonly property var ratePreview: timelinePanel.ratePreviewClips[clipId]
                                readonly property real shownLeftDelta:
                                    ratePreview !== undefined ? ratePreview.startDelta
                                    : (linkedEditPartner ? timelinePanel.linkedLeftDelta : leftPreviewDelta)
                                    + timelinePanel.adjacentDeltaFor(trackKind, trackIndex, timelineStartFrame,
                                                     timelineStartFrame + timelineDurationFrames, "start")
                                readonly property real shownRightDelta:
                                    ratePreview !== undefined ? ratePreview.endDelta
                                    : (linkedEditPartner ? timelinePanel.linkedRightDelta : rightPreviewDelta)
                                    + timelinePanel.adjacentDeltaFor(trackKind, trackIndex, timelineStartFrame,
                                                     timelineStartFrame + timelineDurationFrames, "end")
                                // レート調整の drag 中の速度。確定と同じ Project の計算から受け取る。
                                readonly property real shownSpeed:
                                    ratePreview !== undefined ? ratePreview.speed : speed
                                readonly property int shownSlideFrames:
                                    bodyGesture === "slide" ? toolDragFrames
                                    : (linkedEditPartner ? timelinePanel.linkedSlideFrames : 0)
                                readonly property int shownSlipDelta:
                                    bodyGesture === "slip" ? slipSourceDelta
                                    : (linkedEditPartner ? timelinePanel.linkedSlipDelta : 0)
                                // trim/drag中の見かけの横ずれ。波形の可視範囲計算にも使う。
                                readonly property real renderOffsetX:
                                    shownLeftDelta * timelinePanel.pixelsPerFrame
                                    + shownSlideFrames * timelinePanel.pixelsPerFrame
                                    + (timelinePanel.activeDragDuplicate ? 0 : (bodyMoved
                                       ? bodyDragOffsetX
                                       : ((selected
                                           || (timelinePanel.activeDragLinkGroup !== ""
                                               && clipItem.linkGroupId === timelinePanel.activeDragLinkGroup))
                                          && clipId !== timelinePanel.activeDragClipId
                                          ? timelinePanel.activeDragOffsetX : 0)))

                                x: timelineStartFrame * timelinePanel.pixelsPerFrame
                                y: timelinePanel.rowY(trackKind, trackIndex) - timelinePanel.tracksTop + 3
                                width: Math.max(2, (timelineDurationFrames - shownLeftDelta + shownRightDelta) * timelinePanel.pixelsPerFrame)
                                height: timelinePanel.trackHeight - 6
                                radius: 3
                                color: selected
                                       ? "#315f86"
                                       : (trackKind === "audio" ? "#2b3a33" : "#2b3038")
                                border.color: previewSupported ? "#65a8dc" : "#c88b4a"
                                // 無効にした clip は timeline に残したまま暗くする (Shift+E)。
                                opacity: clipEnabled ? 1 : 0.4
                                z: bodyMoved ? 20 : 1
                                transform: Translate {
                                    x: clipItem.renderOffsetX
                                    y: timelinePanel.activeDragDuplicate ? 0 : (clipItem.bodyMoved
                                       ? clipItem.bodyDragOffsetY
                                       : (clipItem.selected
                                          && clipItem.trackKind === timelinePanel.activeDragTrackKind
                                          && clipItem.clipId !== timelinePanel.activeDragClipId
                                          ? timelinePanel.activeDragOffsetY : 0))
                                }

                                Rectangle {
                                    visible: timelinePanel.activeDragDuplicate && timelinePanel.activeDragMoved
                                             && clipItem.selected
                                    x: timelinePanel.activeDragOffsetX
                                    y: clipItem.trackKind === timelinePanel.activeDragTrackKind
                                       ? timelinePanel.activeDragOffsetY : 0
                                    width: clipItem.width
                                    height: clipItem.height
                                    radius: clipItem.radius
                                    color: clipItem.color
                                    border.color: clipItem.border.color
                                    opacity: 0.55
                                    z: 40
                                    Label {
                                        anchors.fill: parent
                                        anchors.leftMargin: 12
                                        anchors.rightMargin: 12
                                        anchors.topMargin: 5
                                        text: clipItem.displayName
                                        color: "white"
                                        font.bold: true
                                        font.pixelSize: 12
                                        elide: Text.ElideMiddle
                                    }
                                }

                                TapHandler {
                                    acceptedButtons: Qt.RightButton
                                    onTapped: clipMenu.popup()
                                }

                                CompactMenu {
                                    id: clipMenu
                                    CompactMenuItem {
                                        text: "速度・デュレーション...\tCtrl+R"
                                        enabled: !root.mvmController.busy
                                        onTriggered: root.openSpeedDurationDialog(clipItem.clipId)
                                    }
                                    CompactMenuItem {
                                        text: "フレーム保持を挿入"
                                        // canInsertFrameHold は Project を複製して試すので、
                                        // 再生中に全 clip で評価しないようメニューを開いている間だけ見る。
                                        enabled: clipMenu.visible
                                                 && root.mvmController.playheadFrame > clipItem.timelineStartFrame
                                                 && root.mvmController.playheadFrame
                                                    < clipItem.timelineStartFrame + clipItem.timelineDurationFrames
                                                 && root.mvmController.canInsertFrameHold(clipItem.clipId)
                                        onTriggered: root.mvmController.insertFrameHoldAtPlayhead(clipItem.clipId)
                                    }
                                    CompactMenuSeparator {}
                                    CompactMenuItem {
                                        text: "プロジェクト設定をこの素材に合わせる"
                                        enabled: clipItem.clipKind !== "audio"
                                                 && !root.mvmController.busy
                                        onTriggered: root.confirmProjectSettingsFromClip(
                                                         clipItem.clipId)
                                    }
                                    CompactMenuSeparator {}
                                    CompactMenuItem {
                                        text: clipItem.clipEnabled ? "無効にする\tShift+E" : "有効にする\tShift+E"
                                        enabled: !root.mvmController.busy
                                        onTriggered: root.mvmController.toggleTimelineClipEnabled(clipItem.clipId)
                                    }
                                    CompactMenuItem {
                                        text: "削除"
                                        onTriggered: root.mvmController.deleteTimelineClip(clipItem.clipId)
                                    }
                                    CompactMenuItem {
                                        text: "リンクを解除"
                                        enabled: clipItem.linked
                                        onTriggered: root.mvmController.unlinkTimelineClip(clipItem.clipId)
                                    }
                                }

                                // premiere と同様に channel ごとに 1 行 (mono 1 行、stereo 2 行)。
                                // 長い clip を高倍率で見ても巨大な texture を作らないよう、
                                // viewport と重なる範囲だけに置いて、その左端の素材時刻を渡す。
                                WaveformView {
                                    id: clipWaveform
                                    readonly property real clipContentX:
                                        trackArea.x + clipItem.x + clipItem.renderOffsetX
                                    readonly property real visibleLeft:
                                        Math.max(0, timelineFlick.contentX - clipContentX)
                                    readonly property real visibleRight:
                                        Math.min(clipItem.width,
                                                 timelineFlick.contentX + timelineFlick.width - clipContentX)
                                    readonly property real timelineSecondsPerFrame:
                                        root.mvmController.timelineFpsDen / Math.max(1, root.mvmController.timelineFpsNum)
                                    visible: clipItem.clipKind === "audio" && visibleRight > visibleLeft
                                    cache: root.waveformCache
                                    mediaPath: clipItem.clipKind === "audio" ? clipItem.mediaPath : ""
                                    x: visibleLeft
                                    y: 2
                                    width: Math.max(0, Math.ceil(visibleRight - visibleLeft))
                                    height: clipItem.height - 4
                                    // clip 左端 = 素材の sourceInFrame (trim preview 中は leftPreviewDelta 分ずれる)。
                                    // timeline の 1 秒は素材の shownSpeed 秒。レート調整の drag は素材の in を
                                    // 動かさないのでずらさず、縮尺だけを drag に合わせて変える。
                                    startSeconds: (clipItem.sourceInFrame + clipItem.shownSlipDelta)
                                                  * clipItem.sourceFpsDen
                                                  / Math.max(1, clipItem.sourceFpsNum)
                                                  + ((timelinePanel.tool === "rate" ? 0 : clipItem.shownLeftDelta)
                                                     + visibleLeft / timelinePanel.pixelsPerFrame)
                                                    * timelineSecondsPerFrame * clipItem.shownSpeed
                                    secondsPerPixel: timelineSecondsPerFrame * clipItem.shownSpeed
                                                     / timelinePanel.pixelsPerFrame
                                    color: clipItem.selected ? "#a9d6ff" : "#7fd49a"
                                }

                                Column {
                                    anchors.fill: parent
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 12
                                    anchors.topMargin: 5
                                    spacing: 2

                                    Label {
                                        width: parent.width
                                            text: clipItem.displayName
                                        color: "white"
                                        font.bold: true
                                        font.pixelSize: 12
                                        elide: Text.ElideMiddle
                                    }
                                    Label {
                                        width: parent.width
                                        // audio clip は波形を優先し、尺の表示を重ねない。
                                            visible: clipItem.clipKind !== "audio"
                                            text: (clipItem.frameHold ? "保持  |  " : "")
                                                + (clipItem.shownSpeed !== 1
                                                   ? (Math.round(clipItem.shownSpeed * 10000) / 100) + "%  |  " : "")
                                                + (clipItem.clipKind === "audio"
                                                   ? Math.round(clipItem.timelineDurationFrames) + "f"
                                                   : clipItem.sourceFpsNum + "/" + clipItem.sourceFpsDen + " fps  |  " + Math.round(clipItem.timelineDurationFrames) + "f")
                                            color: clipItem.previewSupported ? "#b8c1cc" : "#f0b870"
                                        font.pixelSize: 10
                                        elide: Text.ElideRight
                                    }
                                }

                                // 操作中の途中経過をリンク相手へ見せる。
                                Binding {
                                    target: timelinePanel
                                    property: "linkedLeftDelta"
                                    value: clipItem.leftPreviewDelta
                                    when: clipItem.linkedEditSource
                                }
                                Binding {
                                    target: timelinePanel
                                    property: "linkedRightDelta"
                                    value: clipItem.rightPreviewDelta
                                    when: clipItem.linkedEditSource
                                }
                                Binding {
                                    target: timelinePanel
                                    property: "linkedSlideFrames"
                                    value: clipItem.bodyGesture === "slide" ? clipItem.toolDragFrames : 0
                                    when: clipItem.linkedEditSource
                                }
                                Binding {
                                    target: timelinePanel
                                    property: "linkedSlipDelta"
                                    value: clipItem.bodyGesture === "slip" ? clipItem.slipSourceDelta : 0
                                    when: clipItem.linkedEditSource
                                }

                                // リンク相手へ適用するかを決め、相手の追従を始める。
                                function beginLinkedEdit(linked) {
                                    clipItem.editLinked = linked;
                                    timelinePanel.linkedEditGroup = clipItem.editLinked ? clipItem.linkGroupId : "";
                                    timelinePanel.linkedEditClipId = clipItem.clipId;
                                }
                                // controller 呼び出しより前に必ず呼ぶ (呼び出しは delegate を破棄し得る)。
                                // リンク相手と隣の clip の途中表示をまとめて片付ける。
                                function endLinkedEdit() {
                                    timelinePanel.ratePreviewClips = ({});
                                    timelinePanel.linkedEditGroup = "";
                                    timelinePanel.linkedEditClipId = "";
                                    timelinePanel.adjacentEditPoints = [];
                                    timelinePanel.adjacentEditDelta = 0;
                                }
                                // 端のドラッグを始める。ローリングは隣の clip の追従を表示する。
                                function beginEdgeDrag(edge, modifiers) {
                                    clipItem.beginLinkedEdit(Gestures.linkedFor(modifiers));
                                    if (timelinePanel.tool === "rolling"
                                            && !timelinePanel.beginAdjacentPreview(clipItem.clipId, "rolling", edge,
                                                                                   clipItem.editLinked))
                                        timelinePanel.linkedEditGroup = "";
                                }

                                // 端のドラッグを現在のツールの編集として確定する。
                                // controller 呼び出しは delegate を破棄し得るので、値は先に取り出す。
                                function commitEdgeDrag(edge, delta) {
                                    const id = clipItem.clipId;
                                    const action = Gestures.edgeRelease(timelinePanel.tool, edge, delta,
                                                                        clipItem.editLinked);
                                    clipItem.endLinkedEdit();
                                    if (action.action === "selectEdit")
                                        root.mvmController.selectEditPoint(id, action.edge);
                                    else if (action.action === "rippleTrim")
                                        root.mvmController.rippleTrimClip(id, action.edge, action.delta, action.linked);
                                    else if (action.action === "roll")
                                        root.mvmController.rollClipEdge(id, action.edge, action.delta, action.linked);
                                    else if (action.action === "rateStretch")
                                        root.mvmController.rateStretchClip(id, action.edge, action.delta, action.linked);
                                    else if (action.action === "trim")
                                        root.mvmController.trimClip(id, action.edge, action.delta, action.linked);
                                }

                                // レーザーツールの切断位置。frame 境界へ寄せて表示する。
                                Rectangle {
                                    visible: timelinePanel.tool === "razor" && clipItem.razorHoverX >= 0
                                    x: Math.round(clipItem.razorHoverX / timelinePanel.pixelsPerFrame)
                                       * timelinePanel.pixelsPerFrame
                                    width: 1
                                    height: parent.height
                                    color: "white"
                                    z: 40
                                }

                                // スリップ中は clip を動かさず、素材のずらし量だけを示す。
                                Rectangle {
                                    visible: clipItem.bodyGesture === "slip" && clipItem.slipSourceDelta !== 0
                                    anchors.centerIn: parent
                                    width: slipLabel.implicitWidth + 12
                                    height: slipLabel.implicitHeight + 4
                                    radius: 3
                                    color: "#cc15171b"
                                    z: 40
                                    Label {
                                        id: slipLabel
                                        anchors.centerIn: parent
                                        text: "イン/アウト " + (clipItem.slipSourceDelta > 0 ? "+" : "")
                                              + clipItem.slipSourceDelta + "f"
                                        color: "white"
                                        font.pixelSize: 11
                                    }
                                }

                                // Canvas は clip の小数 px 位置と HiDPI で texture が拡大・補間され、
                                // 急な傾きの線がぼやける。線は Shape (CurveRenderer) でベクタ描画する。
                                Shape {
                                    id: automationShape
                                    anchors.fill: parent
                                    z: 30
                                    preferredRendererType: Shape.CurveRenderer
                                    // 波形や clip の色と同系色にしない。暗い縁取りで明るい波形の上でも読める。
                                    ShapePath {
                                        strokeColor: Qt.rgba(0, 0, 0, 0.65)
                                        strokeWidth: 4
                                        fillColor: "transparent"
                                        joinStyle: ShapePath.RoundJoin
                                        capStyle: ShapePath.FlatCap
                                        PathPolyline { path: clipItem.automationPoints }
                                    }
                                    ShapePath {
                                        strokeColor: "#ffd84d"
                                        strokeWidth: 2
                                        fillColor: "transparent"
                                        joinStyle: ShapePath.RoundJoin
                                        capStyle: ShapePath.FlatCap
                                        PathPolyline { path: clipItem.automationPoints }
                                    }
                                }

                                // キーは塗りつぶした青い四角。ドラッグ中・ポイント中のキーは白く大きくする。
                                Repeater {
                                    model: clipItem.shownKeys
                                    delegate: Rectangle {
                                        required property var modelData
                                        readonly property bool active:
                                            modelData.frame === (clipItem.previewKeys ? clipItem.penFrame
                                                                                      : clipItem.penHoverKeyFrame)
                                        readonly property int half: active ? 5 : 4
                                        x: Math.round(modelData.frame * timelinePanel.pixelsPerFrame) - half
                                        y: Math.round(clipItem.automationY(modelData.value)) - half
                                        z: 31
                                        width: half * 2
                                        height: half * 2
                                        color: active ? "#ffffff" : "#3d8bff"
                                        border.width: 1
                                        border.color: active ? "#3d8bff" : "#0b1a33"
                                        antialiasing: false
                                    }
                                }

                                MouseArea {
                                    id: bodyArea
                                    anchors.fill: parent
                                    anchors.leftMargin: timelinePanel.edgeToolActive ? 9 : 0
                                    anchors.rightMargin: timelinePanel.edgeToolActive ? 9 : 0
                                    enabled: !root.mvmController.busy && !timelinePanel.viewToolActive
                                    acceptedButtons: Qt.LeftButton
                                    preventStealing: true
                                    hoverEnabled: timelinePanel.tool === "razor" || timelinePanel.tool === "pen"
                                    cursorShape: timelinePanel.tool === "razor" ? Qt.IBeamCursor
                                                 : timelinePanel.tool === "pen" ? Qt.CrossCursor
                                                 : (timelinePanel.tool === "slip" || timelinePanel.tool === "slide"
                                                    ? Qt.SizeHorCursor : Qt.ArrowCursor)
                                    onExited: {
                                        clipItem.razorHoverX = -1;
                                        clipItem.penHoverKeyFrame = -1;
                                    }
                                    onPressed: mouse => {
                                        mouse.accepted = true;
                                        const pressPoint = bodyArea.mapToItem(clipItem, mouse.x, mouse.y);
                                        const pressFrame = Math.round(clipItem.timelineStartFrame
                                                                      + pressPoint.x / timelinePanel.pixelsPerFrame);
                                        clipItem.bodyPressPoint = mapToItem(trackArea, mouse.x, mouse.y);
                                        const tool = timelinePanel.tool;
                                        if (tool === "pen") {
                                            const localFrame = clipItem.penFrameAt(pressPoint.x);
                                            clipItem.penState = Gestures.penPress(
                                                clipItem.automationKeys, clipItem.automationBase,
                                                localFrame, pressPoint.x, pressPoint.y,
                                                clipItem.penGeometry, mouse.modifiers);
                                            const penGesture = clipItem.penState.gesture;
                                            if (penGesture !== "pen") {
                                                const deletedFrame = clipItem.penState.frame;
                                                clipItem.penState = null;
                                                clipItem.penHoverKeyFrame = -1;
                                                // controller 呼び出しは delegate を破棄し得るので最後に呼ぶ。
                                                if (penGesture === "deleteKey")
                                                    root.mvmController.deleteClipKey(clipItem.clipId,
                                                                                     deletedFrame);
                                                else if (penGesture === "select")
                                                    root.mvmController.selectTimelineClip(
                                                        clipItem.clipId, true);
                                                return;
                                            }
                                            clipItem.penFrame = clipItem.penState.frame;
                                            clipItem.penValue = Gestures.penSnapValue(clipItem.penState.value,
                                                                                      mouse.modifiers);
                                            const candidate = root.mvmController.previewClipKey(
                                                clipItem.clipId, clipItem.penState.originalFrame,
                                                clipItem.penFrame, clipItem.penValue);
                                            if (candidate.success) {
                                                clipItem.previewKeys = candidate.keys;
                                                clipItem.penFrame = candidate.frame;
                                            }
                                            clipItem.bodyGesture = "pen";
                                            return;
                                        }
                                        // 操作の種類と release で使う値 (分割位置など) はここで確定する。
                                        clipItem.gestureState = Gestures.bodyPress(tool, mouse.modifiers, pressFrame);
                                        clipItem.bodyGesture = clipItem.gestureState.gesture;
                                        clipItem.beginLinkedEdit(clipItem.gestureState.linked);
                                        if (tool === "razor")
                                            return;
                                        if (tool === "slip" || tool === "slide") {
                                            clipItem.toolDragFrames = 0;
                                            clipItem.slipSourceDelta = 0;
                                            if (tool === "slide")
                                                timelinePanel.beginAdjacentPreview(clipItem.clipId, "slide", "",
                                                                                   clipItem.editLinked);
                                            if (!clipItem.selected || !clipItem.editLinked)
                                                root.mvmController.selectTimelineClip(clipItem.clipId,
                                                                                      clipItem.editLinked);
                                            // スリップ中は preview に新しいイン点の frame を出す。
                                            if (tool === "slip")
                                                root.mvmController.beginSlipPreview(clipItem.clipId,
                                                                                    clipItem.editLinked);
                                            return;
                                        }
                                        clipItem.bodyMoved = false;
                                        clipItem.bodyAdditiveSelection = clipItem.gestureState.additive;
                                        clipItem.bodyDragOffsetX = 0;
                                        clipItem.bodyDragOffsetY = 0;
                                        clipItem.rawBodyDragOffsetX = 0;
                                        clipItem.dragTrackKind = clipItem.trackKind;
                                        clipItem.dragTrackIndex = clipItem.trackIndex;
                                        if (clipItem.bodyGesture === "trackSelect") {
                                            // 選んだ clip 群はそのままドラッグで移動できる。
                                            timelinePanel.selectFromFrame(pressFrame, mouse.modifiers,
                                                                          clipItem.trackKind, clipItem.trackIndex);
                                        } else if (!clipItem.bodyAdditiveSelection
                                                   && !clipItem.selected) {
                                            // Alt+クリックはリンク相手を外して、この clip だけを選ぶ。
                                            root.mvmController.selectTimelineClip(clipItem.clipId, clipItem.editLinked);
                                        }
                                        // 選択を確定した後で、一緒に動く群の端を取る。
                                        clipItem.bodyDragBounds = root.mvmController.timelineDragBounds(clipItem.clipId);
                                        clipItem.bodySnap = Gestures.dragSnapFrames(
                                            root.mvmController.timelineModel.clipSpans(),
                                            clipItem.bodyDragBounds.clipIds || [],
                                            root.mvmController.playheadFrame);
                                        timelinePanel.activeDragLinkGroup = clipItem.editLinked ? clipItem.linkGroupId : "";
                                        timelinePanel.activeDragClipId = clipItem.clipId;
                                        timelinePanel.activeDragDuplicate = clipItem.gestureState.duplicate;
                                        timelinePanel.activeDragMoved = false;
                                        timelinePanel.activeDragOffsetX = 0;
                                        timelinePanel.activeDragTrackKind = clipItem.trackKind;
                                        timelinePanel.activeDragOffsetY = 0;
                                    }
                                    onPositionChanged: mouse => {
                                        if (pressed && clipItem.bodyGesture === "pen") {
                                            const point = bodyArea.mapToItem(clipItem, mouse.x, mouse.y);
                                            const dragValue = Gestures.penSnapValue(
                                                clipItem.penValueAt(point.y + clipItem.penState.grabOffsetY),
                                                mouse.modifiers);
                                            const candidate = root.mvmController.previewClipKey(
                                                clipItem.clipId, clipItem.penState.originalFrame,
                                                clipItem.penFrameAt(point.x + clipItem.penState.grabOffsetX),
                                                dragValue);
                                            if (candidate.success) {
                                                clipItem.previewKeys = candidate.keys;
                                                clipItem.penFrame = candidate.frame;
                                                clipItem.penValue = dragValue;
                                            }
                                            return;
                                        }
                                        if (!pressed && timelinePanel.tool === "pen") {
                                            const point = bodyArea.mapToItem(clipItem, mouse.x, mouse.y);
                                            const hovered = Gestures.penNearestKey(
                                                clipItem.automationKeys, clipItem.penGeometry, point.x,
                                                clipItem.penValueAt(point.y));
                                            clipItem.penHoverKeyFrame = hovered ? hovered.frame : -1;
                                            return;
                                        }
                                        if (!pressed || clipItem.bodyGesture === "razor") {
                                            clipItem.razorHoverX = bodyArea.mapToItem(clipItem, mouse.x, mouse.y).x;
                                            return;
                                        }
                                        const now = mapToItem(trackArea, mouse.x, mouse.y);
                                        if (clipItem.bodyGesture === "slip" || clipItem.bodyGesture === "slide") {
                                            const frames = Math.round((now.x - clipItem.bodyPressPoint.x)
                                                                      / timelinePanel.pixelsPerFrame);
                                            // slide で clip を 0 frame より左へは描かない。
                                            // slide は確定時と同じ規則 (前後の clip の素材の端など) で止めて見せる。
                                            clipItem.toolDragFrames = clipItem.bodyGesture === "slide"
                                                                      ? root.mvmController.clampSlideDrag(
                                                                            clipItem.clipId, frames,
                                                                            clipItem.editLinked)
                                                                      : frames;
                                            if (clipItem.bodyGesture === "slide")
                                                timelinePanel.adjacentEditDelta = clipItem.toolDragFrames;
                                            if (clipItem.bodyGesture === "slip")
                                                clipItem.slipSourceDelta = root.mvmController.previewSlip(frames);
                                            return;
                                        }
                                        clipItem.rawBodyDragOffsetX = now.x - clipItem.bodyPressPoint.x;
                                        // track移動時の小さな横ぶれは無視する。
                                        const intendedOffset = Math.abs(clipItem.rawBodyDragOffsetX) < 12
                                                                   ? 0 : clipItem.rawBodyDragOffsetX;
                                        // リンク・複数選択の群全体が 0 frame と既存 track に収まる量で
                                        // 止める。確定にも同じ量を渡すので、見えている位置のまま置かれる。
                                        const boundedOffset = Gestures.groupDragOffsetX(
                                            intendedOffset, timelinePanel.pixelsPerFrame,
                                            clipItem.bodyDragBounds);
                                        // 他の clip の端・再生ヘッドの近くでは端を吸着させる。吸着した
                                        // 量も同じ規則で丸め直す (0 frame より左へは出さない)。
                                        const edgeSnap = clipItem.bodySnap !== null
                                                        && (mouse.modifiers & Qt.ControlModifier) === 0
                                                        ? Gestures.snapDragOffsetX(
                                                              boundedOffset, timelinePanel.pixelsPerFrame,
                                                              clipItem.bodySnap,
                                                              timelinePanel.snapThresholdPixels)
                                                        : { "offsetX": boundedOffset, "frame": -1 };
                                        clipItem.bodyDragOffsetX = Gestures.groupDragOffsetX(
                                            edgeSnap.offsetX, timelinePanel.pixelsPerFrame,
                                            clipItem.bodyDragBounds);
                                        timelinePanel.snapGuideFrame =
                                            clipItem.bodyDragOffsetX === edgeSnap.offsetX ? edgeSnap.frame : -1;
                                        timelinePanel.activeDragOffsetX = clipItem.bodyDragOffsetX;
                                        const rawCenterY = clipItem.y
                                                           + (now.y - clipItem.bodyPressPoint.y)
                                                           + clipItem.height / 2;
                                        const snapped = timelinePanel.trackForDrag(clipItem.trackKind,
                                                                                    rawCenterY);
                                        snapped.index = Gestures.groupDragTrackIndex(
                                            snapped.kind, clipItem.trackIndex, snapped.index,
                                            snapped.kind === "video" ? timelinePanel.videoCount
                                                                     : timelinePanel.audioCount,
                                            clipItem.bodyDragBounds);
                                        clipItem.dragTrackKind = snapped.kind;
                                        clipItem.dragTrackIndex = snapped.index;
                                        clipItem.bodyDragOffsetY = timelinePanel.rowY(snapped.kind,
                                                                                     snapped.index)
                                                                   - timelinePanel.rowY(clipItem.trackKind,
                                                                                        clipItem.trackIndex);
                                        timelinePanel.activeDragOffsetY = clipItem.bodyDragOffsetY;
                                        if (Math.abs(clipItem.rawBodyDragOffsetX) > 5
                                                || snapped.index !== clipItem.trackIndex)
                                            clipItem.bodyMoved = true;
                                        timelinePanel.activeDragMoved = clipItem.bodyMoved;
                                    }
                                    onReleased: mouse => {
                                        if (timelinePanel.tool === "pen" && clipItem.penState === null)
                                            return;
                                        if (clipItem.bodyGesture === "pen") {
                                            // Shift を押しただけでマウスを動かしていなくても吸着させる。
                                            const action = Gestures.penRelease(
                                                clipItem.penState, clipItem.penFrame,
                                                Gestures.penSnapValue(clipItem.penValue, mouse.modifiers));
                                            const id = clipItem.clipId;
                                            clipItem.previewKeys = null;
                                            clipItem.penState = null;
                                            clipItem.bodyGesture = "";
                                            root.mvmController.commitClipKey(id, action.originalFrame,
                                                                              action.frame, action.value);
                                            return;
                                        }
                                        const gesture = clipItem.bodyGesture;
                                        const releasedClipId = clipItem.clipId;
                                        const destinationKind = clipItem.dragTrackKind;
                                        const destinationIndex = clipItem.dragTrackIndex;
                                        const releasePoint = bodyArea.mapToItem(clipItem, mouse.x, mouse.y);
                                        const releaseFrame = Math.round(clipItem.timelineStartFrame
                                                                        + releasePoint.x / timelinePanel.pixelsPerFrame);
                                        const movedToFrame = Math.max(
                                            0, Math.round((clipItem.timelineStartFrame * timelinePanel.pixelsPerFrame
                                                           + clipItem.bodyDragOffsetX) / timelinePanel.pixelsPerFrame));
                                        const action = Gestures.bodyRelease(clipItem.gestureState, clipItem.bodyMoved,
                                                                            movedToFrame, releaseFrame,
                                                                            clipItem.toolDragFrames);

                                        // controller呼び出しはmodelを同期更新し、このdelegateを破棄し得る。
                                        // delegateが生きている間にdrag状態をすべて片付ける。
                                        clipItem.bodyGesture = "";
                                        clipItem.gestureState = null;
                                        clipItem.toolDragFrames = 0;
                                        clipItem.slipSourceDelta = 0;
                                        clipItem.endLinkedEdit();
                                        clipItem.bodyDragOffsetX = 0;
                                        clipItem.bodyDragOffsetY = 0;
                                        clipItem.rawBodyDragOffsetX = 0;
                                        clipItem.bodyMoved = false;
                                        clipItem.bodyAdditiveSelection = false;
                                        timelinePanel.activeDragLinkGroup = "";
                                        timelinePanel.activeDragClipId = "";
                                        timelinePanel.activeDragDuplicate = false;
                                        timelinePanel.activeDragMoved = false;
                                        timelinePanel.activeDragOffsetX = 0;
                                        timelinePanel.activeDragTrackKind = "";
                                        timelinePanel.activeDragOffsetY = 0;
                                        timelinePanel.snapGuideFrame = -1;
                                        clipItem.bodySnap = null;

                                        // slip の preview は確定の有無によらず通常の表示へ戻す。
                                        if (gesture === "slip")
                                            root.mvmController.endSlipPreview();
                                        switch (action.action) {
                                        case "split":
                                            root.mvmController.splitClipAt(releasedClipId, action.frame,
                                                                           action.allTracks, action.linked);
                                            break;
                                        case "slip":
                                            root.mvmController.slipClip(releasedClipId, action.delta, action.linked);
                                            break;
                                        case "slide":
                                            root.mvmController.slideClip(releasedClipId, action.delta, action.linked);
                                            break;
                                        case "move":
                                            root.mvmController.moveTimelineClip(
                                                releasedClipId, destinationKind, destinationIndex,
                                                action.frame, action.linked);
                                            break;
                                        case "duplicate":
                                            root.mvmController.duplicateTimelineClipsAt(
                                                releasedClipId, destinationKind, destinationIndex,
                                                action.frame);
                                            break;
                                        case "toggle":
                                            root.mvmController.toggleTimelineClipSelection(
                                                releasedClipId);
                                            break;
                                        case "select":
                                            root.mvmController.selectTimelineClip(releasedClipId,
                                                                                  action.linked);
                                            break;
                                        }
                                    }
                                    onCanceled: {
                                        clipItem.previewKeys = null;
                                        clipItem.penState = null;
                                        if (clipItem.bodyGesture === "slip")
                                            root.mvmController.endSlipPreview();
                                        clipItem.bodyGesture = "";
                                        clipItem.gestureState = null;
                                        clipItem.toolDragFrames = 0;
                                        clipItem.slipSourceDelta = 0;
                                        clipItem.endLinkedEdit();
                                        clipItem.razorHoverX = -1;
                                        clipItem.bodyDragOffsetX = 0;
                                        clipItem.bodyDragOffsetY = 0;
                                        clipItem.rawBodyDragOffsetX = 0;
                                        clipItem.bodyMoved = false;
                                        clipItem.bodyAdditiveSelection = false;
                                        clipItem.dragTrackKind = clipItem.trackKind;
                                        clipItem.dragTrackIndex = clipItem.trackIndex;
                                        timelinePanel.activeDragLinkGroup = "";
                                        timelinePanel.activeDragClipId = "";
                                        timelinePanel.activeDragDuplicate = false;
                                        timelinePanel.activeDragMoved = false;
                                        timelinePanel.activeDragOffsetX = 0;
                                        timelinePanel.activeDragTrackKind = "";
                                        timelinePanel.activeDragOffsetY = 0;
                                    }
                                }

                                // 端のハンドル。選択=trim、リップル=後ろを詰める、ローリング=隣と境界を共有。
                                Rectangle {
                                    visible: timelinePanel.edgeToolActive
                                    width: 8
                                    height: parent.height
                                    anchors.left: parent.left
                                    color: timelinePanel.edgeHandleColor
                                    radius: 2
                                    z: 30

                                    MouseArea {
                                        property real pressContentX: 0
                                        property int dragDelta: 0
                                        // 端の線をまたいで内側 8px (見えているハンドル) と外側 8px を掴める。どちらも同じ
                                        // 端を動かし、カーソルだけが pointer のある側を示す (内側 [-> / 外側 <-])。
                                        x: -8
                                        width: 16
                                        height: parent.height
                                        enabled: !root.mvmController.busy
                                        // timeline の clip では cursorShape も hover も window から届かない。
                                        // TrimCursor が mouse の位置を自分で見て、この帯にある間と押している
                                        // 間だけ application のカーソルを出す。
                                        TrimCursor {
                                            anchors.fill: parent
                                            edge: "in"
                                            mode: timelinePanel.edgeCursorMode
                                            color: timelinePanel.edgeCursorColor
                                            held: parent.pressed
                                        }
                                        onPressed: mouse => {
                                            pressContentX = mapToItem(timelineContent, mouse.x, mouse.y).x;
                                            dragDelta = 0;
                                            clipItem.beginEdgeDrag("left", mouse.modifiers);
                                        }
                                        onPositionChanged: mouse => {
                                            const now = mapToItem(timelineContent, mouse.x, mouse.y).x;
                                            const requested = Math.round((now - pressContentX) / timelinePanel.pixelsPerFrame);
                                            // レート調整は clip ごとの結果 (リンク相手を含む) を確定と同じ計算で受け取る。
                                            if (timelinePanel.tool === "rate") {
                                                const preview = root.mvmController.previewRateStretch(
                                                    clipItem.clipId, "left", requested, clipItem.editLinked);
                                                dragDelta = preview.delta;
                                                timelinePanel.ratePreviewClips = preview.clips;
                                                return;
                                            }
                                            // 素材の端や最小尺を越える分は、確定時と同じ規則で止めて見せる。
                                            dragDelta = root.mvmController.clampEdgeDrag(
                                                clipItem.clipId, "left", timelinePanel.tool, requested,
                                                clipItem.editLinked);
                                            // リップルの left 端は clip の開始位置を保ち、右端側が伸び縮みする。
                                            if (timelinePanel.tool === "ripple")
                                                clipItem.rightPreviewDelta = -dragDelta;
                                            else
                                                clipItem.leftPreviewDelta = dragDelta;
                                            timelinePanel.adjacentEditDelta = dragDelta;
                                        }
                                        onReleased: {
                                            const delta = dragDelta;
                                            dragDelta = 0;
                                            clipItem.leftPreviewDelta = 0;
                                            clipItem.rightPreviewDelta = 0;
                                            clipItem.commitEdgeDrag("left", delta);
                                        }
                                        onCanceled: {
                                            dragDelta = 0;
                                            clipItem.leftPreviewDelta = 0;
                                            clipItem.rightPreviewDelta = 0;
                                            clipItem.endLinkedEdit();
                                        }
                                    }
                                }

                                Rectangle {
                                    visible: timelinePanel.edgeToolActive
                                    width: 8
                                    height: parent.height
                                    anchors.right: parent.right
                                    color: timelinePanel.edgeHandleColor
                                    radius: 2
                                    z: 30

                                    MouseArea {
                                        property real pressContentX: 0
                                        // 内側 8px (見えているハンドル) と外側 8px。内側 <-] / 外側 [->。
                                        x: 0
                                        width: 16
                                        height: parent.height
                                        enabled: !root.mvmController.busy
                                        TrimCursor {
                                            anchors.fill: parent
                                            edge: "out"
                                            mode: timelinePanel.edgeCursorMode
                                            color: timelinePanel.edgeCursorColor
                                            held: parent.pressed
                                        }
                                        onPressed: mouse => {
                                            pressContentX = mapToItem(timelineContent, mouse.x, mouse.y).x;
                                            clipItem.beginEdgeDrag("right", mouse.modifiers);
                                        }
                                        onPositionChanged: mouse => {
                                            const now = mapToItem(timelineContent, mouse.x, mouse.y).x;
                                            const requested = Math.round((now - pressContentX) / timelinePanel.pixelsPerFrame);
                                            if (timelinePanel.tool === "rate") {
                                                const preview = root.mvmController.previewRateStretch(
                                                    clipItem.clipId, "right", requested, clipItem.editLinked);
                                                clipItem.rightPreviewDelta = preview.delta;
                                                timelinePanel.ratePreviewClips = preview.clips;
                                                return;
                                            }
                                            clipItem.rightPreviewDelta = root.mvmController.clampEdgeDrag(
                                                clipItem.clipId, "right", timelinePanel.tool, requested,
                                                clipItem.editLinked);
                                            timelinePanel.adjacentEditDelta = clipItem.rightPreviewDelta;
                                        }
                                        onReleased: {
                                            const delta = clipItem.rightPreviewDelta;
                                            clipItem.rightPreviewDelta = 0;
                                            clipItem.commitEdgeDrag("right", delta);
                                        }
                                        onCanceled: {
                                            clipItem.rightPreviewDelta = 0;
                                            clipItem.endLinkedEdit();
                                        }
                                    }
                                }
                            }
                        }

                        // --- トランジション ---
                        // cut の前後の区間に重ねて描く。押すと選択し、Delete で消せる。
                        Repeater {
                            model: root.mvmController.timelineTransitions

                            delegate: Rectangle {
                                id: transitionItem
                                required property var modelData
                                readonly property bool selected:
                                    modelData.transitionId === root.mvmController.selectedTransitionId
                                objectName: "timelineTransition_" + modelData.transitionId
                                x: modelData.start * timelinePanel.pixelsPerFrame
                                // Premiere と同じく clip の中央に低い帯で描く。上下に残した clip の部分で
                                // cut の端を掴んで trim できる (離れればトランジションは消える)。
                                y: timelinePanel.rowY(modelData.trackKind, modelData.trackIndex)
                                   - timelinePanel.tracksTop + 3
                                   + Math.round((timelinePanel.trackHeight - 6 - height) / 2)
                                width: Math.max(4, (modelData.end - modelData.start)
                                                   * timelinePanel.pixelsPerFrame)
                                height: Math.round((timelinePanel.trackHeight - 6) * 0.55)
                                radius: 2
                                color: selected ? "#c0e0b040" : "#80c89a3c"
                                border.color: selected ? "#ffe08a" : "#d8b35a"
                                border.width: selected ? 2 : 1
                                z: 35

                                // 左下から右上への斜線 (Premiere のトランジションの表示)。
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
                                Label {
                                    anchors.centerIn: parent
                                    visible: parent.width > 70
                                    text: transitionItem.modelData.trackKind === "audio"
                                          ? "クロスフェード" : "クロスディゾルブ"
                                    color: "white"
                                    font.pixelSize: 11
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    enabled: !root.mvmController.busy
                                    acceptedButtons: Qt.LeftButton
                                    onClicked: {
                                        // 選んだトランジションはエフェクトコントロールで編集する。
                                        if (root.mvmController.selectTransition(
                                                transitionItem.modelData.transitionId))
                                            root.leftPanelTab = 0;
                                    }
                                }
                            }
                        }

                        // clip 移動の吸着の目印。吸着した frame に縦線を描く。
                        Rectangle {
                            visible: timelinePanel.snapGuideFrame >= 0 && timelinePanel.activeDragMoved
                            x: timelinePanel.snapGuideFrame * timelinePanel.pixelsPerFrame
                            width: 1
                            height: parent.height
                            color: "#ffe08a"
                            z: 95
                        }

                        // 選択中の編集点。cut の位置に括弧を描く。
                        Rectangle {
                            readonly property var point: root.mvmController.selectedEditPoint
                            visible: point.frame !== undefined
                            x: (point.frame !== undefined ? point.frame : 0)
                               * timelinePanel.pixelsPerFrame - 3
                            y: point.frame !== undefined
                               ? timelinePanel.rowY(point.trackKind, point.trackIndex)
                                 - timelinePanel.tracksTop + 1
                               : 0
                            width: 6
                            height: timelinePanel.trackHeight - 2
                            color: "transparent"
                            border.color: "#ffe08a"
                            border.width: 2
                            z: 36
                        }

                        Rectangle {
                            visible: timelinePanel.selecting
                            x: Math.min(timelinePanel.selectionStartX,
                                        timelinePanel.selectionCurrentX)
                            y: Math.min(timelinePanel.selectionStartY,
                                        timelinePanel.selectionCurrentY)
                            width: Math.abs(timelinePanel.selectionCurrentX
                                            - timelinePanel.selectionStartX)
                            height: Math.abs(timelinePanel.selectionCurrentY
                                             - timelinePanel.selectionStartY)
                            color: "#334f78a8"
                            border.color: "#9bc8ff"
                            border.width: 1
                            z: 90
                        }
                    }

                    // --- 再生ヘッド ---
                    Rectangle {
                        id: playhead
                        x: root.mvmController.playheadFrame * timelinePanel.pixelsPerFrame - 1
                        y: 0
                        width: 2
                        height: timelinePanel.tracksTop + timelinePanel.tracksHeight
                        color: "#f15b5b"
                        z: 200

                        Rectangle {
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: 12
                            height: 9
                            color: "#f15b5b"
                        }

                        MouseArea {
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: 0
                            width: 16
                            // ペンは再生ヘッドの真下のキーも操作する。track 上では clip へ通し、
                            // ruler の部分だけで再生ヘッドを掴む。
                            height: timelinePanel.tool === "pen" ? timelinePanel.rulerHeight
                                                                 : parent.height
                            enabled: !root.mvmController.busy && root.mvmController.clipCount > 0
                            cursorShape: Qt.SizeHorCursor
                            preventStealing: true
                            onPressed: root.mvmController.beginScrub()
                            onPositionChanged: mouse => {
                                const point = mapToItem(timelineContent, mouse.x, mouse.y);
                                root.mvmController.scrubToFrame(timelinePanel.frameAtContentX(point.x));
                            }
                            onReleased: root.mvmController.endScrub()
                            onCanceled: root.mvmController.endScrub()
                        }
                    }

                    Label {
                        // トラック行に重ねない。行の下の空き領域へ置く。
                        visible: root.mvmController.clipCount === 0
                        x: 24
                        y: timelinePanel.tracksTop + timelinePanel.tracksHeight + 12
                        text: "クリップがありません。「動画を追加」から始めてください"
                        color: "#858b95"
                    }
                }
            }

            // ハンド / ズームツール。表示だけを動かし、clip や ruler へは操作を渡さない。
            // scrollbar は覆わず、常に操作できるようにする。
            MouseArea {
                id: viewToolArea
                property real pressX: 0
                property real pressY: 0
                property real pressContentX: 0
                property real pressContentY: 0
                x: timelineFlick.x
                y: timelineFlick.y
                width: timelineFlick.width - timelineVerticalScrollBar.width
                height: timelineFlick.height - timelineHorizontalScrollBar.height
                enabled: timelinePanel.viewToolActive
                visible: enabled
                acceptedButtons: Qt.LeftButton
                preventStealing: true
                cursorShape: timelinePanel.tool === "hand"
                             ? (pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor)
                             : Qt.CrossCursor
                onPressed: mouse => {
                    pressX = mouse.x;
                    pressY = mouse.y;
                    pressContentX = timelineFlick.contentX;
                    pressContentY = timelineFlick.contentY;
                }
                onPositionChanged: mouse => {
                    if (timelinePanel.tool !== "hand")
                        return;
                    timelineFlick.contentX = Math.max(
                        0, Math.min(timelineFlick.contentWidth - timelineFlick.width,
                                    pressContentX - (mouse.x - pressX)));
                    timelineFlick.contentY = Math.max(
                        0, Math.min(timelineFlick.contentHeight - timelineFlick.height,
                                    pressContentY - (mouse.y - pressY)));
                }
                onClicked: mouse => {
                    if (timelinePanel.tool === "zoom")
                        timelinePanel.setZoom((mouse.modifiers & Qt.AltModifier) !== 0 ? -1 : 1,
                                              mouse.x);
                }
            }

        }
    }

    // --- ダイアログ --------------------------------------------------------
    ModernDialog {
        id: speedDurationDialog
        anchors.centerIn: parent
        width: Math.min(root.width - 40, 440)
        modal: true
        title: "速度・デュレーション"
        property string clipId: ""
        property bool still: false
        property bool syncing: false
        property string lastInput: "speed"

        function updatePair(input) {
            if (syncing)
                return;
            lastInput = input;
            const preview = root.mvmController.previewClipSpeedDuration(
                                clipId, input, speedField.value, durationField.text,
                                preservePitchBox.checked, rippleBox.checked);
            if (preview.error)
                return;
            syncing = true;
            if (input === "speed")
                durationField.text = preview.durationText;
            else
                speedField.value = preview.speedPercent;
            syncing = false;
        }

        function currentInput() {
            return still ? "duration" : lastInput;
        }

        // 適用の入口。リップルしない変更が後続 clip と重なるなら、上書きの確認を挟む。
        function requestApply() {
            // 速度の直接入力が数値として読めなければ、古い値で適用せずに止める (入力欄は開いたまま)。
            if (!speedField.commitEditing()) {
                errorText = "速度を数値で入力してください";
                return;
            }
            errorText = "";
            if (!rippleBox.checked
                    && root.mvmController.clipSpeedDurationNeedsOverwrite(
                        clipId, currentInput(), speedField.value, durationField.text)) {
                speedOverwriteDialog.open();
                return;
            }
            apply(false);
        }

        function apply(overwrite) {
            if (root.mvmController.applyClipSpeedDuration(
                        clipId, currentInput(), speedField.value, durationField.text,
                        preservePitchBox.checked, rippleBox.checked, overwrite)) {
                close();
                return;
            }
            // status bar はダイアログの陰になるので、失敗理由をダイアログ内に出す。
            errorText = root.mvmController.statusText;
        }

        property string errorText: ""
        onOpened: errorText = ""

        contentItem: ColumnLayout {
            spacing: 10
            DragNumberField {
                id: speedField
                objectName: "speedDurationSpeedField"
                Layout.fillWidth: true
                clickToEdit: true
                labelText: "速度"
                minimumValue: 10
                maximumValue: 1000
                decimals: 2
                suffix: "%"
                enabled: !speedDurationDialog.still
                onValueEdited: (newValue, commit) => {
                    speedField.value = newValue;
                    speedDurationDialog.updatePair("speed");
                }
            }
            Label { text: "デュレーション" }
            ModernDialogField {
                id: durationField
                Layout.fillWidth: true
                placeholderText: "00:00:05:00"
                onTextEdited: speedDurationDialog.updatePair("duration")
            }
            CheckBox {
                id: preservePitchBox
                text: "オーディオのピッチを維持"
                // 等速では伸縮しないので保存時に落とされる。押せても効かない状態にしない。
                enabled: !speedDurationDialog.still && Math.abs(speedField.value - 100) > 1e-9
            }
            CheckBox {
                id: rippleBox
                text: "リップル編集 (後続クリップをシフト)"
            }
            Label {
                Layout.fillWidth: true
                visible: speedDurationDialog.errorText !== ""
                text: speedDurationDialog.errorText
                wrapMode: Text.Wrap
                color: "#f0b870"
            }
        }
        footer: ModernDialogFooter {
            ModernDialogButton {
                text: "キャンセル"
                onClicked: speedDurationDialog.close()
            }
            ModernDialogButton {
                text: "適用"
                prominent: true
                onClicked: speedDurationDialog.requestApply()
            }
        }
    }

    // 速度・尺の変更で延びた先の clip と重なるとき、上書きしてよいかを確かめる。
    ModernDialog {
        id: speedOverwriteDialog
        anchors.centerIn: parent
        width: Math.min(root.width - 40, 420)
        modal: true
        title: "後続のクリップを上書き"
        contentItem: Label {
            text: "変更後のクリップが後ろのクリップと重なります。\n"
                  + "重なった部分を上書き (後ろのクリップを削除または短縮) してよいですか？"
            wrapMode: Text.Wrap
            color: "#e6e8ec"
        }
        footer: ModernDialogFooter {
            ModernDialogButton {
                text: "キャンセル"
                onClicked: speedOverwriteDialog.close()
            }
            ModernDialogButton {
                text: "上書きする"
                prominent: true
                onClicked: {
                    speedOverwriteDialog.close();
                    speedDurationDialog.apply(true);
                }
            }
        }
    }

    ModernDialog {
        id: projectSettingsDialog
        anchors.centerIn: parent
        width: Math.min(root.width - 40, 520)
        modal: true
        title: "プロジェクト設定"

        contentItem: ColumnLayout {
            spacing: 12

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: 12
                rowSpacing: 8

                Label { text: "幅" }
                ModernDialogField {
                    id: projectWidthField
                    Layout.fillWidth: true
                    validator: IntValidator { bottom: 2; top: 16384 }
                    inputMethodHints: Qt.ImhDigitsOnly
                    placeholderText: "1920"
                }
                Label { text: "高さ" }
                ModernDialogField {
                    id: projectHeightField
                    Layout.fillWidth: true
                    validator: IntValidator { bottom: 2; top: 16384 }
                    inputMethodHints: Qt.ImhDigitsOnly
                    placeholderText: "1080"
                }
                Label { text: "フレームレート" }
                ModernDialogComboBox {
                    id: projectFpsBox
                    Layout.fillWidth: true
                    textRole: "label"
                    model: root.mvmController.supportedFrameRates
                    function syncFromController() {
                        for (let index = 0; index < count; ++index) {
                            const entry = root.mvmController.supportedFrameRates[index];
                            if (entry.num === root.mvmController.timelineFpsNum
                                    && entry.den === root.mvmController.timelineFpsDen) {
                                currentIndex = index;
                                return;
                            }
                        }
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                visible: root.mvmController.clipCount > 0
                text: "fpsを変更すると、既存クリップの開始位置を秒位置が保たれるよう換算します。素材のin/outは変更しません。"
                color: "#f0c36a"
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                text: "幅と高さはMP4（yuv420p）で扱える2〜16384の偶数を指定してください。"
                color: "#aeb4bf"
                wrapMode: Text.Wrap
            }
        }

        footer: ModernDialogFooter {
            ModernDialogButton {
                text: "キャンセル"
                onClicked: projectSettingsDialog.close()
            }
            ModernDialogButton {
                text: "適用"
                prominent: true
                enabled: projectWidthField.acceptableInput
                         && projectHeightField.acceptableInput
                         && Number(projectWidthField.text) % 2 === 0
                         && Number(projectHeightField.text) % 2 === 0
                         && projectFpsBox.currentIndex >= 0
                onClicked: {
                    const fps = root.mvmController.supportedFrameRates[projectFpsBox.currentIndex];
                    if (root.mvmController.setProjectVideoSettings(
                                Number(projectWidthField.text), Number(projectHeightField.text),
                                fps.num, fps.den))
                        projectSettingsDialog.close();
                }
            }
        }
    }

    ModernDialog {
        id: matchClipSettingsDialog
        property bool validSettings: false
        property bool changesSettings: false
        property string clipName: ""
        property string sourceText: ""
        property string errorText: ""
        property int targetWidth: 0
        property int targetHeight: 0
        property int targetFpsNum: 0
        property int targetFpsDen: 1
        anchors.centerIn: parent
        width: Math.min(root.width - 40, 580)
        modal: true
        title: "プロジェクト設定を素材に合わせる"

        contentItem: ColumnLayout {
            spacing: 10

            Label {
                Layout.fillWidth: true
                visible: matchClipSettingsDialog.validSettings
                text: "素材: " + matchClipSettingsDialog.clipName + "\n"
                      + matchClipSettingsDialog.sourceText + "\n\n"
                      + "現在: " + root.mvmController.outputWidth + "×" + root.mvmController.outputHeight
                      + " / " + root.mvmController.timelineFpsText + "\n"
                      + "変更後: " + matchClipSettingsDialog.targetWidth + "×"
                      + matchClipSettingsDialog.targetHeight + " / "
                      + (matchClipSettingsDialog.targetFpsNum
                         / matchClipSettingsDialog.targetFpsDen).toFixed(
                             matchClipSettingsDialog.targetFpsDen === 1 ? 0 : 2) + " fps"
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: matchClipSettingsDialog.validSettings
                         && root.mvmController.clipCount > 0
                text: "既存クリップの開始位置は秒位置を維持して換算します。素材のin/outは変更しません。続行しますか？"
                color: "#f0c36a"
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: !matchClipSettingsDialog.validSettings
                text: matchClipSettingsDialog.errorText
                color: "#ef8b8b"
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: matchClipSettingsDialog.validSettings
                         && !matchClipSettingsDialog.changesSettings
                text: "プロジェクト設定はすでにこの素材と一致しています。"
                color: "#aeb4bf"
                wrapMode: Text.Wrap
            }
        }

        footer: ModernDialogFooter {
            ModernDialogButton {
                text: "キャンセル"
                onClicked: matchClipSettingsDialog.close()
            }
            ModernDialogButton {
                text: "変更する"
                prominent: true
                enabled: matchClipSettingsDialog.validSettings
                         && matchClipSettingsDialog.changesSettings
                onClicked: {
                    if (root.mvmController.setProjectVideoSettings(
                                matchClipSettingsDialog.targetWidth,
                                matchClipSettingsDialog.targetHeight,
                                matchClipSettingsDialog.targetFpsNum,
                                matchClipSettingsDialog.targetFpsDen))
                        matchClipSettingsDialog.close();
                }
            }
        }
    }

    ModernDialog {
        id: exportProgressDialog
        anchors.centerIn: parent
        width: Math.min(root.width - 40, 460)
        modal: true
        visible: root.mvmController.exporting
        closePolicy: Popup.NoAutoClose
        title: "動画を書き出しています"

        contentItem: ColumnLayout {
            spacing: 12

            Label {
                Layout.fillWidth: true
                text: root.mvmController.exportProgressText
                horizontalAlignment: Text.AlignHCenter
            }
            ModernDialogProgressBar {
                Layout.fillWidth: true
                from: 0
                to: 1
                value: root.mvmController.exportProgress
                indeterminate: root.mvmController.exportProgressText === "準備しています…"
            }
        }
        footer: ModernDialogFooter {
            ModernDialogButton {
                text: root.mvmController.exportCancelling
                      ? "キャンセル中…" : "キャンセル"
                enabled: !root.mvmController.exportCancelling
                onClicked: root.mvmController.cancelTimelineExport()
            }
        }
    }

    ModernDialog {
        id: exportFailureDialog
        property string message: ""
        anchors.centerIn: parent
        width: Math.min(root.width - 40, 560)
        modal: true
        title: "書き出しに失敗しました"

        contentItem: Label {
            width: exportFailureDialog.availableWidth
            text: exportFailureDialog.message
            wrapMode: Text.Wrap
        }
        footer: ModernDialogFooter {
            ModernDialogButton {
                text: "OK"
                prominent: true
                onClicked: exportFailureDialog.close()
            }
        }
    }

    ModernDialog {
        id: exportSettingsDialog
        property string inputSpecText: ""
        property string outputSpecText: ""
        property string comparisonWarningText: ""
        readonly property var qualityOptions: [
            { key: "high", label: "高品質", detail: "CRF 18・容量大" },
            { key: "standard", label: "標準", detail: "CRF 23" },
            { key: "compact", label: "容量優先", detail: "CRF 28・画質低下" }
        ]
        anchors.centerIn: parent
        width: Math.min(root.width - 40, 640)
        modal: true
        title: "書き出し設定"
        onOpened: qualityCombo.currentIndex = 1

        contentItem: ColumnLayout {
            spacing: 12

            Label {
                Layout.fillWidth: true
                text: exportSettingsDialog.inputSpecText
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                text: exportSettingsDialog.outputSpecText
                wrapMode: Text.Wrap
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: comparisonWarningLabel.implicitHeight + 20
                visible: exportSettingsDialog.comparisonWarningText.length > 0
                color: "#3a3020"
                border.color: "#b58a43"
                radius: 4

                Label {
                    id: comparisonWarningLabel
                    anchors.fill: parent
                    anchors.margins: 10
                    text: exportSettingsDialog.comparisonWarningText
                    color: "#f0c36a"
                    wrapMode: Text.Wrap
                }
            }
            Label {
                text: "エンコード品質"
                font.bold: true
            }
            ModernDialogComboBox {
                id: qualityCombo
                Layout.fillWidth: true
                model: exportSettingsDialog.qualityOptions
                textRole: "label"
                delegate: ModernDialogOption {
                    required property int index
                    required property var modelData
                    width: qualityCombo.width - 8
                    text: modelData.label + "（" + modelData.detail + "）"
                    highlighted: qualityCombo.highlightedIndex === index
                }
                contentItem: Label {
                    leftPadding: 12
                    rightPadding: qualityCombo.indicator.width + 12
                    text: {
                        const option = exportSettingsDialog.qualityOptions[qualityCombo.currentIndex];
                        return option ? option.label + "（" + option.detail + "）" : "";
                    }
                    color: "#f0f1f3"
                    verticalAlignment: Text.AlignVCenter
                }
            }
            Label {
                Layout.fillWidth: true
                text: "品質は圧縮率だけを変更します。出力の解像度とfpsはプロジェクト設定のままです。"
                color: "#aeb4bf"
                wrapMode: Text.Wrap
            }
        }

        footer: ModernDialogFooter {
            ModernDialogButton {
                text: "キャンセル"
                onClicked: exportSettingsDialog.close()
            }
            ModernDialogButton {
                text: "書き出す"
                prominent: true
                onClicked: {
                    const option = exportSettingsDialog.qualityOptions[qualityCombo.currentIndex];
                    if (option && root.mvmController.exportTimelineWithQuality(
                                root.pendingExportFile, option.key))
                        exportSettingsDialog.close();
                }
            }
        }
    }

    Connections {
        target: root.mvmController
        function onStateChanged() {
            timelinePanel.pageForPlayback();
        }
        function onExportFailed(message) {
            exportFailureDialog.message = message;
            exportFailureDialog.open();
        }
        function onRecoveryDetected() {
            recoveryDialog.open();
        }
        function onExternalCanonicalChangeOnSave() {
            root.noteExternalSaveDuringPendingAction();
            externalSaveDialog.open();
        }
    }

    FileDialog {
        id: mediaDialog
        title: "メディアファイルを選択"
        nameFilters: root.mvmController.mediaFileNameFilters
        onAccepted: root.mvmController.addMediaFileToTimeline(selectedFile)
    }

    FileDialog {
        id: exportDialog
        title: "書き出し先を指定"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "mp4"
        nameFilters: ["MP4 (*.mp4)"]
        onAccepted: {
            root.pendingExportFile = selectedFile;
            const summary = root.mvmController.exportSettingsSummary();
            exportSettingsDialog.inputSpecText = summary.inputText;
            exportSettingsDialog.outputSpecText = summary.outputText;
            exportSettingsDialog.comparisonWarningText = summary.warningText;
            exportSettingsDialog.open();
        }
    }

    FileDialog {
        id: newProjectDialog
        title: "新規プロジェクトの保存先"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "mvm"
        nameFilters: ["mvm プロジェクト (*.mvm)"]
        onAccepted: root.mvmController.newProject(selectedFile)
    }

    FileDialog {
        id: openProjectDialog
        title: "プロジェクトを開く"
        nameFilters: ["mvm プロジェクト (*.mvm)", "すべて (*)"]
        onAccepted: root.mvmController.openProject(selectedFile)
    }

    FileDialog {
        id: saveProjectDialog
        title: "名前を付けて保存"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "mvm"
        nameFilters: ["mvm プロジェクト (*.mvm)"]
        onAccepted: root.completeExternalSave(root.mvmController.saveProjectAs(selectedFile))
        onRejected: root.abandonExternalSaveContinuation()
    }

    ModernDialog {
        id: recoveryDialog
        anchors.centerIn: parent
        width: Math.min(root.width - 40, 620)
        modal: true
        closePolicy: Popup.NoAutoClose
        title: root.mvmController.recoveryCorrupt
               ? "自動復旧データを読めません"
               : (root.mvmController.recoveryForeign
                  ? "別のProjectの自動復旧データです"
                  : (root.mvmController.recoveryCanonicalChanged
                     ? "Project fileが外部で変更されています"
                     : "自動保存された編集があります"))

        contentItem: Label {
            text: root.mvmController.recoveryCorrupt
                  ? "自動復旧データが壊れているため、最後に保存したProjectを開きました。復旧fileは残しています。\n"
                    + root.mvmController.recoveryProjectPath
                  : (root.mvmController.recoveryForeign
                     ? "この自動復旧データは、今開いているProjectのものではありません。fileは残しています。\n"
                       + root.mvmController.recoveryProjectPath
                     : (root.mvmController.recoveryCanonicalChanged
                     ? "自動保存のあとでProject fileの内容が変わっています。復元すると、その変更は明示保存するまでfileへ書き込まれません。\n"
                       + root.mvmController.recoveryProjectPath
                     : "前回、正常に保存されなかった編集が見つかりました。\n"
                       + root.mvmController.recoveryProjectPath
                       + "\n\n自動保存された編集を復元しますか？"))
            color: "#f0f1f3"
            wrapMode: Text.Wrap
        }

        footer: ModernDialogFooter {
            ModernDialogButton {
                visible: !root.mvmController.recoveryCorrupt && !root.mvmController.recoveryForeign
                text: "復元する"
                prominent: true
                onClicked: {
                    if (root.mvmController.restoreRecovery())
                        recoveryDialog.close();
                }
            }
            ModernDialogButton {
                visible: !root.mvmController.recoveryCorrupt && !root.mvmController.recoveryForeign
                text: root.mvmController.recoveryCanonicalChanged ? "現在のProjectを開く" : "最後の保存状態を使う"
                destructive: true
                onClicked: {
                    if (root.mvmController.discardRecovery())
                        recoveryDialog.close();
                }
            }
            ModernDialogButton {
                visible: root.mvmController.recoveryCanonicalChanged && !root.mvmController.recoveryCorrupt
                      && !root.mvmController.recoveryForeign
                text: "キャンセル"
                onClicked: {
                    if (root.mvmController.dismissRecovery())
                        recoveryDialog.close();
                }
            }
            ModernDialogButton {
                visible: root.mvmController.recoveryCorrupt || root.mvmController.recoveryForeign
                text: "OK"
                prominent: true
                onClicked: {
                    if (root.mvmController.dismissRecovery())
                        recoveryDialog.close();
                }
            }
        }
    }

    ModernDialog {
        id: externalSaveDialog
        anchors.centerIn: parent
        width: Math.min(root.width - 40, 620)
        modal: true
        closePolicy: Popup.NoAutoClose
        title: "Project fileが外部で変更されています"

        contentItem: Label {
            text: "このProjectを開いたあとで、fileの内容が変わっています。このまま保存すると、その変更を上書きします。"
            color: "#f0f1f3"
            wrapMode: Text.Wrap
        }

        footer: ModernDialogFooter {
            ModernDialogButton {
                text: "上書きする"
                destructive: true
                onClicked: root.completeExternalSave(
                               root.mvmController.saveProjectOverwritingExternalChange())
            }
            ModernDialogButton {
                text: "名前を付けて保存"
                onClicked: {
                    externalSaveDialog.close();
                    saveProjectDialog.open();
                }
            }
            ModernDialogButton {
                text: "キャンセル"
                onClicked: {
                    root.abandonExternalSaveContinuation();
                    externalSaveDialog.close();
                }
            }
        }
    }

    ModernDialog {
        id: unsavedChangesDialog
        anchors.centerIn: parent
        width: Math.min(root.width - 40, 420)
        modal: true
        closePolicy: Popup.NoAutoClose
        title: "未保存の変更"

        contentItem: Label {
            text: "プロジェクトへの変更を保存しますか？"
            color: "#f0f1f3"
            wrapMode: Text.Wrap
        }

        footer: ModernDialogFooter {
            ModernDialogButton {
                text: "保存"
                prominent: true
                onClicked: {
                    if (root.mvmController.saveProject()) {
                        unsavedChangesDialog.close();
                        root.continuePendingProjectAction();
                    }
                }
            }
            ModernDialogButton {
                text: "保存しない"
                destructive: true
                onClicked: {
                    if (root.mvmController.discardUnsavedChanges()) {
                        unsavedChangesDialog.close();
                        root.continuePendingProjectAction();
                    }
                }
            }
            ModernDialogButton {
                text: "キャンセル"
                onClicked: {
                    root.pendingProjectAction = "";
                    root.pendingSaveContinuation = false;
                    unsavedChangesDialog.close();
                }
            }
        }
    }

    FileDialog {
        id: scriptDialog
        title: "Manim Python scriptを選択"
        nameFilters: ["Python script (*.py)"]
        onAccepted: {
            root.selectedManimScript = selectedFile;
            sceneField.text = "";
            generationDialog.open();
            sceneField.forceActiveFocus();
        }
    }

    ModernDialog {
        id: generationDialog
        anchors.centerIn: parent
        width: Math.min(root.width - 40, 660)
        modal: true
        closePolicy: Popup.CloseOnEscape
        title: "Add Manim Clip"

        contentItem: ColumnLayout {
            spacing: 12

            Label {
                text: "Script"
                font.bold: true
            }
            ModernDialogField {
                Layout.fillWidth: true
                readOnly: true
                text: root.selectedManimScript.toString().replace(/^file:\/\//, "")
            }
            Label {
                text: "Scene class"
                font.bold: true
            }
            ModernDialogField {
                id: sceneField
                Layout.fillWidth: true
                placeholderText: "MvmM0Scene"
                enabled: !root.mvmController.busy
                onAccepted: generateButton.clicked()
            }
        }
        footer: ModernDialogFooter {
            ModernDialogButton {
                text: "Cancel"
                enabled: !root.mvmController.busy
                onClicked: generationDialog.close()
            }
            ModernDialogButton {
                id: generateButton
                text: root.mvmController.busy ? "Generating…" : "Generate"
                prominent: true
                enabled: !root.mvmController.busy && sceneField.text.trim().length > 0
                onClicked: {
                    if (root.mvmController.generateManimClip(root.selectedManimScript, sceneField.text))
                        generationDialog.close();
                }
            }
        }
    }
}

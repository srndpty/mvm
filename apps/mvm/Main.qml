import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import mvm.preview 1.0

ApplicationWindow {
    id: root
    width: 1440
    height: 900
    minimumWidth: 980
    minimumHeight: 640
    // PreviewSurface自体はこのQMLのscene graphへ宣言済み。C++から動的追加しない。
    visible: true
    title: "mvm" + (mvmController.dirty ? " *" : "") + " — " + mvmController.projectPath
    color: "#15171b"

    property url selectedManimScript
    property url pendingExportFile
    property bool closeConfirmed: false

    onClosing: close => {
        if (!closeConfirmed && mvmController.dirty) {
            close.accepted = false;
            unsavedChangesDialog.open();
        }
    }

    function isLocalFileUrl(url) {
        // 対応形式は拡張子では決めない。drop 後に controller が MLT で内容を検査し、
        // 映像 stream・有限尺・FPS を確認できた素材だけを timeline へ追加する。
        return /^file:/i.test(url.toString());
    }

    function openProjectSettingsDialog() {
        projectWidthField.text = mvmController.outputWidth.toString();
        projectHeightField.text = mvmController.outputHeight.toString();
        projectFpsBox.syncFromController();
        projectSettingsDialog.open();
    }

    function confirmProjectSettingsFromClip(clipId) {
        const settings = mvmController.projectSettingsForClip(clipId);
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

    DropArea {
        id: videoDropArea
        property bool acceptingVideoDrag: false
        anchors.fill: parent
        z: 2000

        onEntered: drag => {
            acceptingVideoDrag = false;
            drag.accepted = false;
            if (mvmController.busy || !drag.hasUrls)
                return;
            for (let index = 0; index < drag.urls.length; ++index) {
                if (root.isLocalFileUrl(drag.urls[index])) {
                    acceptingVideoDrag = true;
                    drag.accepted = true;
                    return;
                }
            }
        }
        onExited: acceptingVideoDrag = false
        onDropped: drop => {
            acceptingVideoDrag = false;
            let accepted = false;
            for (let index = 0; index < drop.urls.length; ++index) {
                const url = drop.urls[index];
                if (!root.isLocalFileUrl(url))
                    continue;
                accepted = true;
                mvmController.addVideoClip(url);
            }
            if (accepted)
                drop.acceptProposedAction();
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
            text: "メディアファイルをドロップして検査・追加"
            color: "white"
            font.pixelSize: 20
            font.bold: true
        }
    }

    Shortcut {
        sequence: "Delete"
        enabled: !mvmController.busy && mvmController.currentClipIndex >= 0
        onActivated: mvmController.deleteCurrentClip()
    }
    Shortcut {
        sequence: "Space"
        autoRepeat: false
        enabled: !mvmController.busy && (mvmController.playing || mvmController.canPlay)
        onActivated: {
            if (mvmController.playing)
                mvmController.pauseTimeline();
            else
                mvmController.playTimeline();
        }
    }
    Shortcut {
        sequence: "Ctrl+Z"
        enabled: mvmController.canUndo
        onActivated: mvmController.undoLastEdit()
    }
    Shortcut {
        sequence: "Ctrl+S"
        enabled: !mvmController.busy && mvmController.dirty
        onActivated: mvmController.saveProject()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        // --- ツールバー ---------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Button {
                text: "新規"
                enabled: !mvmController.busy
                onClicked: newProjectDialog.open()
            }
            Button {
                text: "開く"
                enabled: !mvmController.busy
                onClicked: openProjectDialog.open()
            }
            Button {
                text: "保存"
                enabled: !mvmController.busy && mvmController.dirty
                onClicked: mvmController.saveProject()
            }
            Button {
                text: "名前を付けて保存"
                enabled: !mvmController.busy
                onClicked: saveProjectDialog.open()
            }
            ToolSeparator {}
            Button {
                text: "動画を追加"
                enabled: !mvmController.busy
                onClicked: videoDialog.open()
            }
            Button {
                text: "音声を追加"
                enabled: !mvmController.busy && mvmController.audioTrackCount > 0
                onClicked: audioDialog.open()
            }
            Button {
                text: "Manim clip"
                visible: !mvmController.hasManimAsset
                enabled: mvmController.previewReady && !mvmController.busy
                onClicked: scriptDialog.open()
            }
            Button {
                text: "書き出し"
                enabled: mvmController.canExport
                onClicked: exportDialog.open()
            }
            ToolSeparator {}
            Button {
                text: "プロジェクト設定: " + mvmController.outputWidth + "×"
                      + mvmController.outputHeight + " / " + mvmController.timelineFpsText
                enabled: !mvmController.busy
                onClicked: root.openProjectSettingsDialog()
            }
            BusyIndicator {
                running: mvmController.busy
                visible: running
                implicitWidth: 22
                implicitHeight: 22
            }
            Label {
                Layout.fillWidth: true
                text: mvmController.statusText
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
            spacing: 8

            Frame {
                Layout.preferredWidth: 268
                Layout.fillHeight: true
                padding: 8

                background: Rectangle {
                    color: "#1b1f25"
                    border.color: "#343840"
                    radius: 4
                }

                contentItem: ColumnLayout {
                    spacing: 6

                    Label {
                        text: "エフェクトコントロール"
                        color: "#e6e8ec"
                        font.bold: true
                    }
                    Label {
                        Layout.fillWidth: true
                        text: mvmController.currentClipIndex >= 0
                              ? mvmController.currentClipName
                              : "クリップ未選択"
                        color: "#9aa2ad"
                        font.pixelSize: 11
                        elide: Text.ElideMiddle
                    }

                    GridLayout {
                        id: inspectorGrid
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 6
                        rowSpacing: 4
                        enabled: mvmController.currentClipIndex >= 0 && !mvmController.busy
                                 && !mvmController.playing

                        DragNumberField {
                            Layout.fillWidth: true
                            labelText: "位置 X"
                            suffix: " %"
                            value: mvmController.effectPositionX
                            minimumValue: -1000
                            maximumValue: 1000
                            stepPerPixel: 0.5
                            onEditCanceled: mvmController.cancelEffectPreview()
                            onValueEdited: (newValue, commit) => mvmController.setEffectValue("positionX", newValue, commit)
                        }
                        DragNumberField {
                            Layout.fillWidth: true
                            labelText: "位置 Y"
                            suffix: " %"
                            value: mvmController.effectPositionY
                            minimumValue: -1000
                            maximumValue: 1000
                            stepPerPixel: 0.5
                            onEditCanceled: mvmController.cancelEffectPreview()
                            onValueEdited: (newValue, commit) => mvmController.setEffectValue("positionY", newValue, commit)
                        }
                        DragNumberField {
                            Layout.fillWidth: true
                            labelText: "拡大率"
                            suffix: " %"
                            value: mvmController.effectScale
                            minimumValue: 1
                            maximumValue: 1000
                            stepPerPixel: 0.5
                            onEditCanceled: mvmController.cancelEffectPreview()
                            onValueEdited: (newValue, commit) => mvmController.setEffectValue("scale", newValue, commit)
                        }
                        DragNumberField {
                            Layout.fillWidth: true
                            labelText: "回転"
                            suffix: " °"
                            value: mvmController.effectRotation
                            minimumValue: -360
                            maximumValue: 360
                            stepPerPixel: 0.5
                            onEditCanceled: mvmController.cancelEffectPreview()
                            onValueEdited: (newValue, commit) => mvmController.setEffectValue("rotation", newValue, commit)
                        }
                        DragNumberField {
                            Layout.fillWidth: true
                            labelText: "不透明度"
                            suffix: " %"
                            value: mvmController.effectOpacity
                            minimumValue: 0
                            maximumValue: 100
                            stepPerPixel: 0.3
                            onEditCanceled: mvmController.cancelEffectPreview()
                            onValueEdited: (newValue, commit) => mvmController.setEffectValue("opacity", newValue, commit)
                        }
                        Item { Layout.fillWidth: true; implicitHeight: 1 }

                        DragNumberField {
                            Layout.fillWidth: true
                            labelText: "Crop 左"
                            suffix: " %"
                            value: mvmController.effectCropLeft
                            minimumValue: 0
                            maximumValue: 99
                            stepPerPixel: 0.2
                            onEditCanceled: mvmController.cancelEffectPreview()
                            onValueEdited: (newValue, commit) => mvmController.setEffectValue("cropLeft", newValue, commit)
                        }
                        DragNumberField {
                            Layout.fillWidth: true
                            labelText: "Crop 右"
                            suffix: " %"
                            value: mvmController.effectCropRight
                            minimumValue: 0
                            maximumValue: 99
                            stepPerPixel: 0.2
                            onEditCanceled: mvmController.cancelEffectPreview()
                            onValueEdited: (newValue, commit) => mvmController.setEffectValue("cropRight", newValue, commit)
                        }
                        DragNumberField {
                            Layout.fillWidth: true
                            labelText: "Crop 上"
                            suffix: " %"
                            value: mvmController.effectCropTop
                            minimumValue: 0
                            maximumValue: 99
                            stepPerPixel: 0.2
                            onEditCanceled: mvmController.cancelEffectPreview()
                            onValueEdited: (newValue, commit) => mvmController.setEffectValue("cropTop", newValue, commit)
                        }
                        DragNumberField {
                            Layout.fillWidth: true
                            labelText: "Crop 下"
                            suffix: " %"
                            value: mvmController.effectCropBottom
                            minimumValue: 0
                            maximumValue: 99
                            stepPerPixel: 0.2
                            onEditCanceled: mvmController.cancelEffectPreview()
                            onValueEdited: (newValue, commit) => mvmController.setEffectValue("cropBottom", newValue, commit)
                        }
                        DragNumberField {
                            Layout.fillWidth: true
                            labelText: "フェードイン (素材f)"
                            value: mvmController.effectFadeIn
                            minimumValue: 0
                            maximumValue: 1000000
                            stepPerPixel: 1
                            onEditCanceled: mvmController.cancelEffectPreview()
                            onValueEdited: (newValue, commit) => mvmController.setEffectValue("fadeIn", newValue, commit)
                        }
                        DragNumberField {
                            Layout.fillWidth: true
                            labelText: "フェードアウト (素材f)"
                            value: mvmController.effectFadeOut
                            minimumValue: 0
                            maximumValue: 1000000
                            stepPerPixel: 1
                            onEditCanceled: mvmController.cancelEffectPreview()
                            onValueEdited: (newValue, commit) => mvmController.setEffectValue("fadeOut", newValue, commit)
                        }
                    }

                    Frame {
                        Layout.fillWidth: true
                        visible: mvmController.hasManimAsset
                        padding: 6

                        contentItem: ColumnLayout {
                            spacing: 4
                            Label {
                                Layout.fillWidth: true
                                text: "Manim: " + mvmController.manimSceneName
                                color: "#e6e8ec"
                                elide: Text.ElideRight
                                font.pixelSize: 11
                            }
                            Label {
                                text: mvmController.manimStateText
                                color: mvmController.manimStateText === "SourceChanged" ? "#f2c66d" : "#a8d5a2"
                                font.pixelSize: 11
                            }
                            RowLayout {
                                Button {
                                    text: mvmController.busy ? "生成中…" : "再生成"
                                    enabled: !mvmController.busy
                                    onClicked: mvmController.regenerateManimClip()
                                }
                                Button {
                                    text: "timelineへ"
                                    visible: !mvmController.hasManimTimelineClip
                                    enabled: visible && !mvmController.busy
                                    onClicked: mvmController.addManimToTimeline()
                                }
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }
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

                    Item {
                        id: previewHost
                        objectName: "previewHost"
                        anchors.centerIn: parent
                        readonly property real outputAspect: Math.max(1, mvmController.outputWidth)
                                                             / Math.max(1, mvmController.outputHeight)
                        width: Math.min(parent.width, parent.height * outputAspect)
                        height: width / outputAspect

                        PreviewSurface {
                            id: previewSurface
                            objectName: "previewSurface"
                            anchors.fill: parent
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.preferredWidth: 110
                Layout.minimumWidth: 110
                Layout.maximumWidth: 110
                Layout.fillHeight: true
                spacing: 4
                AudioMeter {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    dbLeft: mvmController.audioMeterDbLeft
                    dbRight: mvmController.audioMeterDbRight
                }
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: "音量 " + Math.round(mvmController.masterVolume * 100) + "%"
                    color: "#9aa2ad"
                    font.pixelSize: 10
                }
                Slider {
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    value: mvmController.masterVolume
                    onMoved: mvmController.masterVolume = value
                }
            }
        }

        // --- トランスポート -------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Button {
                text: mvmController.playing ? "一時停止" : "再生"
                enabled: mvmController.playing || mvmController.canPlay
                onClicked: {
                    if (mvmController.playing)
                        mvmController.pauseTimeline();
                    else
                        mvmController.playTimeline();
                }
            }
            Label {
                text: mvmController.currentTimeText
                color: "#e6e8ec"
                font.bold: true
                font.family: "Consolas"
                font.pixelSize: 16
            }
            Label {
                text: mvmController.timelineFpsText
                      + (mvmController.frameRateMeasured ? "" : " (未計測)")
                      + "  |  zoom " + Math.round(timelinePanel.pixelsPerFrame * 100) + "%"
                color: mvmController.frameRateMeasured ? "#9aa2ad" : "#f2c66d"
                ToolTip.visible: !mvmController.frameRateMeasured && hovered
                ToolTip.text: "このframe rateのpreviewは実測していません"

                HoverHandler { id: fpsHover }
                property bool hovered: fpsHover.hovered
            }
            Item { Layout.fillWidth: true }
            Label {
                text: "ドラッグ: 矩形選択 / Shift+クリック: 複数選択 / Space: 再生・一時停止 / Alt+ホイール: ズーム"
                color: "#6f7681"
                font.pixelSize: 11
            }
            Button {
                text: "クリップ削除"
                enabled: !mvmController.busy && mvmController.currentClipIndex >= 0
                onClicked: mvmController.deleteCurrentClip()
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

            // 離散段階で管理し、下限へ到達した後も逆方向のwheelを確実に受理する。
            readonly property var zoomLevels: [0.005, 0.01, 0.02, 0.05, 0.1, 0.2,
                                                0.35, 0.5, 0.75, 1.0, 1.5, 2, 3, 4,
                                                6, 8, 12, 16, 24]
            property int zoomIndex: 10
            // 最大zoom-outではclip全体をviewportの70%幅へ収める。離散倍率へ
            // 切り上げると全体が収まらないため、下限の段階だけ正確なfit倍率を使う。
            readonly property real fitPixelsPerFrame:
                mvmController.totalTimelineFrames > 0 && timelineFlick.width > 0
                ? timelineFlick.width * 0.7 / mvmController.totalTimelineFrames
                : zoomLevels[0]
            readonly property int minimumZoomIndex: {
                for (let index = 0; index < zoomLevels.length; ++index) {
                    if (zoomLevels[index] >= fitPixelsPerFrame)
                        return index;
                }
                return zoomLevels.length - 1;
            }
            readonly property real pixelsPerFrame:
                zoomIndex === minimumZoomIndex ? fitPixelsPerFrame : zoomLevels[zoomIndex]
            onMinimumZoomIndexChanged: {
                if (zoomIndex < minimumZoomIndex)
                    zoomIndex = minimumZoomIndex;
            }
            property string activeDragLinkGroup: ""
            property string activeDragClipId: ""
            property real activeDragOffsetX: 0
            property string activeDragTrackKind: ""
            property real activeDragOffsetY: 0
            property real selectionStartX: 0
            property real selectionStartY: 0
            property real selectionCurrentX: 0
            property real selectionCurrentY: 0
            property bool selecting: false
            readonly property real labelWidth: 96
            readonly property real rulerHeight: 26
            readonly property real trackHeight: 54
            readonly property int videoCount: mvmController.videoTrackCount
            readonly property int audioCount: mvmController.audioTrackCount
            readonly property int rowCount: videoCount + audioCount
            readonly property real tracksHeight: rowCount * trackHeight
            // ルーラーの目盛り間隔。ズームに応じて 1/2/5/10/30/60 秒から選ぶ。
            readonly property int tickSeconds: {
                const nominalFps = Math.max(1, Math.round(mvmController.timelineFpsNum / mvmController.timelineFpsDen));
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
                return rulerHeight + rowIndexFor(kind, index) * trackHeight;
            }
            // トラック領域内の y からトラックを引く。範囲外は null。
            function trackAtY(y) {
                const row = Math.floor((y - rulerHeight) / trackHeight);
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
                    mvmController.totalTimelineFrames * pixelsPerFrame + 240);
                const nextMaxContentX = Math.max(0, nextContentWidth - timelineFlick.width);
                const desiredContentX = anchorFrame * pixelsPerFrame - anchorItemX;
                timelineFlick.contentX = Math.max(
                    0, Math.min(nextMaxContentX, desiredContentX));
            }

            // QQuickWindowのevent filterがFlickableより先にAlt/Ctrl wheelを捕捉し、
            // modifierを判定済みの専用入口へ渡す。
            function handleNativeAltWheel(wheelDelta, localX) {
                if (wheelDelta === 0)
                    return;
                setZoom(wheelDelta > 0 ? 1 : -1,
                        Math.max(0, localX - labelWidth));
            }
            function handleNativeCtrlWheel(wheelDelta) {
                timelineFlick.contentY = Math.max(
                    0,
                    Math.min(timelineFlick.contentHeight - timelineFlick.height,
                             timelineFlick.contentY - wheelDelta));
            }
            function handleNativeShiftWheel(wheelDelta) {
                timelineFlick.contentX = Math.max(
                    0,
                    Math.min(timelineFlick.contentWidth - timelineFlick.width,
                             timelineFlick.contentX - wheelDelta * 5));
            }
            function handleNativePlainWheel(wheelDelta) {
                const maxY = Math.max(0, timelineFlick.contentHeight - timelineFlick.height);
                if (maxY > 0) {
                    timelineFlick.contentY = Math.max(
                        0, Math.min(maxY, timelineFlick.contentY - wheelDelta));
                    return;
                }
                timelineFlick.contentX = Math.max(
                    0, Math.min(timelineFlick.contentWidth - timelineFlick.width,
                                timelineFlick.contentX - wheelDelta));
            }

            // --- トラックヘッダ (左端) ---
            Item {
                id: headerColumn
                x: 0
                y: 0
                width: timelinePanel.labelWidth
                height: parent.height
                clip: true

                component TrackHeader: Rectangle {
                    id: header
                    required property string headerKind
                    required property int headerIndex
                    required property string headerName
                    required property bool headerMuted

                    width: timelinePanel.labelWidth
                    height: timelinePanel.trackHeight
                    y: timelinePanel.rowY(headerKind, headerIndex) - timelineFlick.contentY
                    color: headerKind === "video" ? "#252a31" : "#232a2a"
                    border.color: "#3c424c"

                    // ミュートボタンはトラックの左側に置く。
                    Button {
                        id: muteButton
                        x: 4
                        anchors.verticalCenter: parent.verticalCenter
                        implicitWidth: 24
                        implicitHeight: 22
                        text: "M"
                        checkable: true
                        checked: header.headerMuted
                        enabled: !mvmController.busy
                        ToolTip.visible: hovered
                        ToolTip.text: header.headerMuted ? "ミュート解除" : "ミュート"
                        onClicked: {
                            if (!mvmController.setTrackMuted(header.headerKind, header.headerIndex, checked))
                                checked = header.headerMuted;
                        }
                        background: Rectangle {
                            radius: 3
                            color: header.headerMuted ? "#c05a5a" : (muteButton.hovered ? "#3a414c" : "#2b3038")
                            border.color: "#4a515c"
                        }
                        contentItem: Label {
                            text: muteButton.text
                            color: "white"
                            font.pixelSize: 11
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }

                    Label {
                        anchors.left: muteButton.right
                        anchors.leftMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        text: header.headerName
                        color: header.headerMuted ? "#8b8f96" : "#c9ccd2"
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
                        enabled: !mvmController.busy && !mvmController.playing
                        ToolTip.visible: hovered
                        ToolTip.text: "このトラックを削除"
                        onClicked: mvmController.removeTrack(header.headerKind, header.headerIndex)
                    }
                }

                Repeater {
                    model: mvmController.videoTrackModel
                    delegate: TrackHeader {
                        required property string trackName
                        required property bool trackMuted
                        required property int trackIndex
                        headerKind: "video"
                        headerIndex: trackIndex
                        headerName: trackName
                        headerMuted: trackMuted
                    }
                }
                Repeater {
                    model: mvmController.audioTrackModel
                    delegate: TrackHeader {
                        required property string trackName
                        required property bool trackMuted
                        required property int trackIndex
                        headerKind: "audio"
                        headerIndex: trackIndex
                        headerName: trackName
                        headerMuted: trackMuted
                    }
                }

                Rectangle {
                    y: timelinePanel.rulerHeight + timelinePanel.videoCount * timelinePanel.trackHeight
                       - timelineFlick.contentY - 2
                    width: parent.width
                    height: 4
                    color: "#59636f"
                    z: 10
                }

                Row {
                    y: timelinePanel.rulerHeight + timelinePanel.tracksHeight
                       - timelineFlick.contentY + 4
                    x: 3
                    spacing: 4
                    Button {
                        implicitHeight: 22
                        implicitWidth: 42
                        text: "+V"
                        enabled: !mvmController.busy
                        ToolTip.visible: hovered
                        ToolTip.text: "video トラックを追加"
                        onClicked: mvmController.addTrack("video")
                    }
                    Button {
                        implicitHeight: 22
                        implicitWidth: 42
                        text: "+A"
                        enabled: !mvmController.busy
                        ToolTip.visible: hovered
                        ToolTip.text: "audio トラックを追加"
                        onClicked: mvmController.addTrack("audio")
                    }
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
                x: timelinePanel.labelWidth
                y: 0
                width: parent.width - x - 4
                height: parent.height - 4
                clip: true
                contentWidth: Math.max(width, mvmController.totalTimelineFrames * timelinePanel.pixelsPerFrame + 240)
                contentHeight: Math.max(height, timelinePanel.rulerHeight
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
                                const nominalFps = Math.max(1, Math.round(mvmController.timelineFpsNum / mvmController.timelineFpsDen));
                                const framesPerTick = timelinePanel.tickSeconds * nominalFps;
                                return Math.ceil(timelineContent.width / (framesPerTick * timelinePanel.pixelsPerFrame)) + 1;
                            }

                            Item {
                                required property int index
                                readonly property int nominalFps: Math.max(1, Math.round(mvmController.timelineFpsNum / mvmController.timelineFpsDen))
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
                                        const seconds = index * timelinePanel.tickSeconds;
                                        const minutes = Math.floor(seconds / 60);
                                        const rest = seconds % 60;
                                        return minutes + ":" + (rest < 10 ? "0" : "") + rest;
                                    }
                                    color: "#aab1ba"
                                    font.pixelSize: 10
                                }
                            }
                        }

                        // ルーラー上はクリックでもドラッグでもスクラブできる。
                        MouseArea {
                            anchors.fill: parent
                            enabled: !mvmController.busy && mvmController.clipCount > 0
                            cursorShape: Qt.SizeHorCursor
                            preventStealing: true
                            onPressed: mouse => {
                                mvmController.beginScrub();
                                mvmController.scrubToFrame(timelinePanel.frameAtContentX(mouse.x));
                            }
                            onPositionChanged: mouse => {
                                if (pressed)
                                    mvmController.scrubToFrame(timelinePanel.frameAtContentX(mouse.x));
                            }
                            onReleased: mvmController.endScrub()
                            onCanceled: mvmController.endScrub()
                        }
                    }

                    // --- トラック背景 ---
                    Item {
                        id: trackArea
                        x: 0
                        y: timelinePanel.rulerHeight
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
                            enabled: !mvmController.busy
                            acceptedButtons: Qt.LeftButton
                            preventStealing: true
                            onPressed: mouse => {
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
                                        selectedIds.push(item.clipId);
                                }
                                timelinePanel.selecting = false;
                                mvmController.selectTimelineClips(selectedIds);
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
                                const track = timelinePanel.trackAtY(mouse.y + timelinePanel.rulerHeight);
                                if (!track)
                                    return;
                                // hit test はclipの半開区間に合わせてfloorする。
                                const frame = Math.max(0, Math.floor(mouse.x / timelinePanel.pixelsPerFrame));
                                if (mvmController.hasClipAt(track.kind, track.index, frame))
                                    return;
                                menuTrackKind = track.kind;
                                menuTrackIndex = track.index;
                                menuFrame = frame;
                                gapMenu.popup();
                            }

                            Menu {
                                id: gapMenu
                                MenuItem {
                                    text: "リップル削除（空白を詰める）"
                                    enabled: mvmController.hasGapAt(emptyContextArea.menuTrackKind,
                                                                    emptyContextArea.menuTrackIndex,
                                                                    emptyContextArea.menuFrame)
                                    onTriggered: mvmController.rippleDeleteGap(emptyContextArea.menuTrackKind,
                                                                               emptyContextArea.menuTrackIndex,
                                                                               emptyContextArea.menuFrame)
                                }
                            }
                        }

                        // --- クリップ ---
                        Repeater {
                            id: timelineClips
                            model: mvmController.timelineModel

                            delegate: Rectangle {
                                id: clipItem
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
                                required property bool previewSupported
                                required property string trackKind
                                required property int trackIndex
                                required property bool linked
                                required property string linkGroupId
                                required property bool selected

                                property real leftPreviewDelta: 0
                                property real rightPreviewDelta: 0
                                property point bodyPressPoint: Qt.point(0, 0)
                                property real bodyDragOffsetX: 0
                                property real bodyDragOffsetY: 0
                                property real rawBodyDragOffsetX: 0
                                property bool bodyMoved: false
                                property bool bodyAdditiveSelection: false
                                property string dragTrackKind: trackKind
                                property int dragTrackIndex: trackIndex

                                x: timelineStartFrame * timelinePanel.pixelsPerFrame
                                y: timelinePanel.rowY(trackKind, trackIndex) - timelinePanel.rulerHeight + 3
                                width: Math.max(2, (timelineDurationFrames - leftPreviewDelta + rightPreviewDelta) * timelinePanel.pixelsPerFrame)
                                height: timelinePanel.trackHeight - 6
                                radius: 3
                                color: selected
                                       ? "#315f86"
                                       : (trackKind === "audio" ? "#2b3a33" : "#2b3038")
                                border.color: previewSupported ? "#65a8dc" : "#c88b4a"
                                z: bodyMoved ? 20 : 1
                                transform: Translate {
                                    x: clipItem.leftPreviewDelta * timelinePanel.pixelsPerFrame
                                       + (clipItem.bodyMoved
                                          ? clipItem.bodyDragOffsetX
                                          : ((clipItem.selected
                                              || (timelinePanel.activeDragLinkGroup !== ""
                                                  && clipItem.linkGroupId === timelinePanel.activeDragLinkGroup))
                                             && clipItem.clipId !== timelinePanel.activeDragClipId
                                             ? timelinePanel.activeDragOffsetX : 0))
                                    y: clipItem.bodyMoved
                                       ? clipItem.bodyDragOffsetY
                                       : (clipItem.selected
                                          && clipItem.trackKind === timelinePanel.activeDragTrackKind
                                          && clipItem.clipId !== timelinePanel.activeDragClipId
                                          ? timelinePanel.activeDragOffsetY : 0)
                                }

                                TapHandler {
                                    acceptedButtons: Qt.RightButton
                                    onTapped: clipMenu.popup()
                                }

                                Menu {
                                    id: clipMenu
                                    MenuItem {
                                        text: "プロジェクト設定をこの素材に合わせる"
                                        enabled: clipItem.clipKind !== "audio"
                                                 && !mvmController.busy
                                        onTriggered: root.confirmProjectSettingsFromClip(
                                                         clipItem.clipId)
                                    }
                                    MenuSeparator {}
                                    MenuItem {
                                        text: "削除"
                                        onTriggered: mvmController.deleteTimelineClip(clipItem.clipId)
                                    }
                                    MenuItem {
                                        text: "リンクを解除"
                                        enabled: clipItem.linked
                                        onTriggered: mvmController.unlinkTimelineClip(clipItem.clipId)
                                    }
                                }

                                Column {
                                    anchors.fill: parent
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 12
                                    anchors.topMargin: 5
                                    spacing: 2

                                    Label {
                                        width: parent.width
                                        text: displayName
                                        color: "white"
                                        font.bold: true
                                        font.pixelSize: 12
                                        elide: Text.ElideMiddle
                                    }
                                    Label {
                                        width: parent.width
                                        text: clipKind === "audio"
                                              ? Math.round(timelineDurationFrames) + "f"
                                              : sourceFpsNum + "/" + sourceFpsDen + " fps  |  " + Math.round(timelineDurationFrames) + "f"
                                        color: previewSupported ? "#b8c1cc" : "#f0b870"
                                        font.pixelSize: 10
                                        elide: Text.ElideRight
                                    }
                                }

                                MouseArea {
                                    id: bodyArea
                                    anchors.fill: parent
                                    anchors.leftMargin: 9
                                    anchors.rightMargin: 9
                                    enabled: !mvmController.busy
                                    acceptedButtons: Qt.LeftButton
                                    preventStealing: true
                                    onPressed: mouse => {
                                        mouse.accepted = true;
                                        clipItem.bodyMoved = false;
                                        clipItem.bodyAdditiveSelection =
                                                (mouse.modifiers & Qt.ShiftModifier) !== 0;
                                        clipItem.bodyDragOffsetX = 0;
                                        clipItem.bodyDragOffsetY = 0;
                                        clipItem.rawBodyDragOffsetX = 0;
                                        clipItem.dragTrackKind = clipItem.trackKind;
                                        clipItem.dragTrackIndex = clipItem.trackIndex;
                                        clipItem.bodyPressPoint = mapToItem(trackArea, mouse.x, mouse.y);
                                        if (!clipItem.bodyAdditiveSelection && !clipItem.selected) {
                                            const point = bodyArea.mapToItem(clipItem, mouse.x, mouse.y);
                                            const frame = Math.round(clipItem.timelineStartFrame
                                                                     + point.x / timelinePanel.pixelsPerFrame);
                                            mvmController.selectTimelineClip(clipItem.clipId, frame);
                                        }
                                        timelinePanel.activeDragLinkGroup = clipItem.linkGroupId;
                                        timelinePanel.activeDragClipId = clipItem.clipId;
                                        timelinePanel.activeDragOffsetX = 0;
                                        timelinePanel.activeDragTrackKind = clipItem.trackKind;
                                        timelinePanel.activeDragOffsetY = 0;
                                    }
                                    onPositionChanged: mouse => {
                                        const now = mapToItem(trackArea, mouse.x, mouse.y);
                                        clipItem.rawBodyDragOffsetX = now.x - clipItem.bodyPressPoint.x;
                                        // track移動時の小さな横ぶれは無視する。
                                        const intendedOffset = Math.abs(clipItem.rawBodyDragOffsetX) < 12
                                                                   ? 0 : clipItem.rawBodyDragOffsetX;
                                        // drag中もanchorを0秒より左へ描画しない。リンク・複数選択の
                                        // 最左端はProject側が同じdeltaで最終スナップする。
                                        clipItem.bodyDragOffsetX = Math.max(
                                            -clipItem.timelineStartFrame * timelinePanel.pixelsPerFrame,
                                            intendedOffset);
                                        timelinePanel.activeDragOffsetX = clipItem.bodyDragOffsetX;
                                        const rawCenterY = clipItem.y
                                                           + (now.y - clipItem.bodyPressPoint.y)
                                                           + clipItem.height / 2;
                                        const snapped = timelinePanel.trackForDrag(clipItem.trackKind,
                                                                                    rawCenterY);
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
                                    }
                                    onReleased: mouse => {
                                        const moved = clipItem.bodyMoved;
                                        const additiveSelection = clipItem.bodyAdditiveSelection;
                                        const releasedClipId = clipItem.clipId;
                                        const destinationKind = clipItem.dragTrackKind;
                                        const destinationIndex = clipItem.dragTrackIndex;
                                        let targetFrame = clipItem.timelineStartFrame;
                                        if (clipItem.bodyMoved) {
                                            const candidateX = clipItem.timelineStartFrame * timelinePanel.pixelsPerFrame + clipItem.bodyDragOffsetX;
                                            targetFrame = Math.max(
                                                0, Math.round(candidateX / timelinePanel.pixelsPerFrame));
                                        } else {
                                            const point = bodyArea.mapToItem(clipItem, mouse.x, mouse.y);
                                            targetFrame = Math.round(
                                                clipItem.timelineStartFrame
                                                + point.x / timelinePanel.pixelsPerFrame);
                                        }

                                        // controller呼び出しはmodelを同期更新し、このdelegateを破棄し得る。
                                        // delegateが生きている間にdrag状態をすべて片付ける。
                                        clipItem.bodyDragOffsetX = 0;
                                        clipItem.bodyDragOffsetY = 0;
                                        clipItem.rawBodyDragOffsetX = 0;
                                        clipItem.bodyMoved = false;
                                        clipItem.bodyAdditiveSelection = false;
                                        timelinePanel.activeDragLinkGroup = "";
                                        timelinePanel.activeDragClipId = "";
                                        timelinePanel.activeDragOffsetX = 0;
                                        timelinePanel.activeDragTrackKind = "";
                                        timelinePanel.activeDragOffsetY = 0;

                                        if (moved) {
                                            mvmController.moveTimelineClip(
                                                releasedClipId, destinationKind, destinationIndex,
                                                targetFrame);
                                        } else if (additiveSelection) {
                                            mvmController.toggleTimelineClipSelection(
                                                releasedClipId, targetFrame);
                                        } else {
                                            mvmController.selectTimelineClip(releasedClipId,
                                                                             targetFrame);
                                        }
                                    }
                                    onCanceled: {
                                        clipItem.bodyDragOffsetX = 0;
                                        clipItem.bodyDragOffsetY = 0;
                                        clipItem.rawBodyDragOffsetX = 0;
                                        clipItem.bodyMoved = false;
                                        clipItem.bodyAdditiveSelection = false;
                                        clipItem.dragTrackKind = clipItem.trackKind;
                                        clipItem.dragTrackIndex = clipItem.trackIndex;
                                        timelinePanel.activeDragLinkGroup = "";
                                        timelinePanel.activeDragClipId = "";
                                        timelinePanel.activeDragOffsetX = 0;
                                        timelinePanel.activeDragTrackKind = "";
                                        timelinePanel.activeDragOffsetY = 0;
                                    }
                                }

                                Rectangle {
                                    width: 8
                                    height: parent.height
                                    anchors.left: parent.left
                                    color: "#85c4ee"
                                    radius: 2
                                    z: 30

                                    MouseArea {
                                        property real pressContentX: 0
                                        anchors.fill: parent
                                        cursorShape: Qt.SizeHorCursor
                                        enabled: !mvmController.busy
                                        onPressed: mouse => {
                                            pressContentX = mapToItem(timelineContent, mouse.x, mouse.y).x;
                                        }
                                        onPositionChanged: mouse => {
                                            const now = mapToItem(timelineContent, mouse.x, mouse.y).x;
                                            clipItem.leftPreviewDelta = Math.round((now - pressContentX) / timelinePanel.pixelsPerFrame);
                                        }
                                        onReleased: {
                                            const delta = clipItem.leftPreviewDelta;
                                            clipItem.leftPreviewDelta = 0;
                                            if (delta !== 0)
                                                mvmController.trimClip(clipItem.clipId, "left", delta);
                                        }
                                        onCanceled: clipItem.leftPreviewDelta = 0
                                    }
                                }

                                Rectangle {
                                    width: 8
                                    height: parent.height
                                    anchors.right: parent.right
                                    color: "#85c4ee"
                                    radius: 2
                                    z: 30

                                    MouseArea {
                                        property real pressContentX: 0
                                        anchors.fill: parent
                                        cursorShape: Qt.SizeHorCursor
                                        enabled: !mvmController.busy
                                        onPressed: mouse => {
                                            pressContentX = mapToItem(timelineContent, mouse.x, mouse.y).x;
                                        }
                                        onPositionChanged: mouse => {
                                            const now = mapToItem(timelineContent, mouse.x, mouse.y).x;
                                            clipItem.rightPreviewDelta = Math.round((now - pressContentX) / timelinePanel.pixelsPerFrame);
                                        }
                                        onReleased: {
                                            const delta = clipItem.rightPreviewDelta;
                                            clipItem.rightPreviewDelta = 0;
                                            if (delta !== 0)
                                                mvmController.trimClip(clipItem.clipId, "right", delta);
                                        }
                                        onCanceled: clipItem.rightPreviewDelta = 0
                                    }
                                }
                            }
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
                        x: mvmController.playheadFrame * timelinePanel.pixelsPerFrame - 1
                        y: 0
                        width: 2
                        height: timelinePanel.rulerHeight + timelinePanel.tracksHeight
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
                            height: parent.height
                            enabled: !mvmController.busy && mvmController.clipCount > 0
                            cursorShape: Qt.SizeHorCursor
                            preventStealing: true
                            onPressed: mvmController.beginScrub()
                            onPositionChanged: mouse => {
                                const point = mapToItem(timelineContent, mouse.x, mouse.y);
                                mvmController.scrubToFrame(timelinePanel.frameAtContentX(point.x));
                            }
                            onReleased: mvmController.endScrub()
                            onCanceled: mvmController.endScrub()
                        }
                    }

                    Label {
                        // トラック行に重ねない。行の下の空き領域へ置く。
                        visible: mvmController.clipCount === 0
                        x: 24
                        y: timelinePanel.rulerHeight + timelinePanel.tracksHeight + 12
                        text: "クリップがありません。「動画を追加」から始めてください"
                        color: "#858b95"
                    }
                }
            }

        }
    }

    // --- ダイアログ --------------------------------------------------------
    Dialog {
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
                TextField {
                    id: projectWidthField
                    Layout.fillWidth: true
                    validator: IntValidator { bottom: 2; top: 16384 }
                    inputMethodHints: Qt.ImhDigitsOnly
                    placeholderText: "1920"
                }
                Label { text: "高さ" }
                TextField {
                    id: projectHeightField
                    Layout.fillWidth: true
                    validator: IntValidator { bottom: 2; top: 16384 }
                    inputMethodHints: Qt.ImhDigitsOnly
                    placeholderText: "1080"
                }
                Label { text: "フレームレート" }
                ComboBox {
                    id: projectFpsBox
                    Layout.fillWidth: true
                    textRole: "label"
                    model: mvmController.supportedFrameRates
                    function syncFromController() {
                        for (let index = 0; index < count; ++index) {
                            const entry = mvmController.supportedFrameRates[index];
                            if (entry.num === mvmController.timelineFpsNum
                                    && entry.den === mvmController.timelineFpsDen) {
                                currentIndex = index;
                                return;
                            }
                        }
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                visible: mvmController.clipCount > 0
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

        footer: DialogButtonBox {
            Button {
                text: "キャンセル"
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
                onClicked: projectSettingsDialog.close()
            }
            Button {
                text: "適用"
                enabled: projectWidthField.acceptableInput
                         && projectHeightField.acceptableInput
                         && Number(projectWidthField.text) % 2 === 0
                         && Number(projectHeightField.text) % 2 === 0
                         && projectFpsBox.currentIndex >= 0
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
                onClicked: {
                    const fps = mvmController.supportedFrameRates[projectFpsBox.currentIndex];
                    if (mvmController.setProjectVideoSettings(
                                Number(projectWidthField.text), Number(projectHeightField.text),
                                fps.num, fps.den))
                        projectSettingsDialog.close();
                }
            }
        }
    }

    Dialog {
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
                      + "現在: " + mvmController.outputWidth + "×" + mvmController.outputHeight
                      + " / " + mvmController.timelineFpsText + "\n"
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
                         && mvmController.clipCount > 0
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

        footer: DialogButtonBox {
            Button {
                text: "キャンセル"
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
                onClicked: matchClipSettingsDialog.close()
            }
            Button {
                text: "変更する"
                enabled: matchClipSettingsDialog.validSettings
                         && matchClipSettingsDialog.changesSettings
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
                onClicked: {
                    if (mvmController.setProjectVideoSettings(
                                matchClipSettingsDialog.targetWidth,
                                matchClipSettingsDialog.targetHeight,
                                matchClipSettingsDialog.targetFpsNum,
                                matchClipSettingsDialog.targetFpsDen))
                        matchClipSettingsDialog.close();
                }
            }
        }
    }

    Dialog {
        id: exportProgressDialog
        anchors.centerIn: parent
        width: Math.min(root.width - 40, 460)
        modal: true
        visible: mvmController.exporting
        closePolicy: Popup.NoAutoClose
        title: "動画を書き出しています"

        contentItem: ColumnLayout {
            spacing: 12

            Label {
                Layout.fillWidth: true
                text: mvmController.exportProgressText
                horizontalAlignment: Text.AlignHCenter
            }
            ProgressBar {
                Layout.fillWidth: true
                from: 0
                to: 1
                value: mvmController.exportProgress
                indeterminate: mvmController.exportProgressText === "準備しています…"
            }
            Button {
                Layout.alignment: Qt.AlignHCenter
                text: mvmController.exportCancelling
                      ? "キャンセル中…" : "キャンセル"
                enabled: !mvmController.exportCancelling
                onClicked: mvmController.cancelTimelineExport()
            }
        }
    }

    Dialog {
        id: exportFailureDialog
        property string message: ""
        anchors.centerIn: parent
        width: Math.min(root.width - 40, 560)
        modal: true
        title: "書き出しに失敗しました"
        standardButtons: Dialog.Ok

        contentItem: Label {
            width: exportFailureDialog.availableWidth
            text: exportFailureDialog.message
            wrapMode: Text.Wrap
        }
    }

    Dialog {
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
            ComboBox {
                id: qualityCombo
                Layout.fillWidth: true
                model: exportSettingsDialog.qualityOptions
                textRole: "label"
                delegate: ItemDelegate {
                    width: qualityCombo.width
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

        footer: DialogButtonBox {
            Button {
                text: "キャンセル"
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
                onClicked: exportSettingsDialog.close()
            }
            Button {
                text: "書き出す"
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
                onClicked: {
                    const option = exportSettingsDialog.qualityOptions[qualityCombo.currentIndex];
                    if (option && mvmController.exportTimelineWithQuality(
                                root.pendingExportFile, option.key))
                        exportSettingsDialog.close();
                }
            }
        }
    }

    Connections {
        target: mvmController
        function onExportFailed(message) {
            exportFailureDialog.message = message;
            exportFailureDialog.open();
        }
    }

    FileDialog {
        id: videoDialog
        title: "動画ファイルを選択"
        nameFilters: ["動画 (*.mp4 *.mov *.mkv *.ts)", "すべて (*)"]
        onAccepted: mvmController.addVideoClip(selectedFile)
    }

    FileDialog {
        id: audioDialog
        title: "音声ファイルを選択"
        nameFilters: ["音声 (*.wav *.mp3 *.m4a *.aac *.flac)", "すべて (*)"]
        onAccepted: mvmController.addAudioClip(selectedFile)
    }

    FileDialog {
        id: exportDialog
        title: "書き出し先を指定"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "mp4"
        nameFilters: ["MP4 (*.mp4)"]
        onAccepted: {
            root.pendingExportFile = selectedFile;
            const summary = mvmController.exportSettingsSummary();
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
        onAccepted: mvmController.newProject(selectedFile)
    }

    FileDialog {
        id: openProjectDialog
        title: "プロジェクトを開く"
        nameFilters: ["mvm プロジェクト (*.mvm)", "すべて (*)"]
        onAccepted: mvmController.openProject(selectedFile)
    }

    FileDialog {
        id: saveProjectDialog
        title: "名前を付けて保存"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "mvm"
        nameFilters: ["mvm プロジェクト (*.mvm)"]
        onAccepted: mvmController.saveProjectAs(selectedFile)
    }

    Dialog {
        id: unsavedChangesDialog
        anchors.centerIn: parent
        modal: true
        closePolicy: Popup.NoAutoClose
        title: "未保存の変更"

        contentItem: Label {
            text: "プロジェクトへの変更を保存してから終了しますか？"
            color: "white"
            wrapMode: Text.Wrap
        }

        footer: DialogButtonBox {
            Button {
                text: "保存"
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
                onClicked: {
                    if (mvmController.saveProject()) {
                        unsavedChangesDialog.close();
                        root.closeConfirmed = true;
                        root.close();
                    }
                }
            }
            Button {
                text: "保存しない"
                DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
                onClicked: {
                    if (mvmController.discardUnsavedChanges()) {
                        unsavedChangesDialog.close();
                        root.closeConfirmed = true;
                        root.close();
                    }
                }
            }
            Button {
                text: "キャンセル"
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
                onClicked: unsavedChangesDialog.close()
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

    Dialog {
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
            TextField {
                Layout.fillWidth: true
                readOnly: true
                text: root.selectedManimScript.toString().replace(/^file:\/\//, "")
            }
            Label {
                text: "Scene class"
                font.bold: true
            }
            TextField {
                id: sceneField
                Layout.fillWidth: true
                placeholderText: "MvmM0Scene"
                enabled: !mvmController.busy
                onAccepted: generateButton.clicked()
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight

                Button {
                    text: "Cancel"
                    enabled: !mvmController.busy
                    onClicked: generationDialog.close()
                }
                Button {
                    id: generateButton
                    text: mvmController.busy ? "Generating…" : "Generate"
                    enabled: !mvmController.busy && sceneField.text.trim().length > 0
                    onClicked: {
                        if (mvmController.generateManimClip(root.selectedManimScript, sceneField.text))
                            generationDialog.close();
                    }
                }
            }
        }
    }
}

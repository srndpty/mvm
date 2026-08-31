import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 1180
    height: 760
    minimumWidth: 760
    minimumHeight: 520
    visible: true
    title: "mvm — Manim MVP"
    color: "#15171b"

    property url selectedManimScript
    readonly property real pixelsPerFrame: 1.5
    property bool playheadDragging: false
    property int dragPlayheadFrame: 0

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Button {
                text: "Add Video"
                enabled: !mvmController.busy
                onClicked: videoDialog.open()
            }
            Button {
                text: "Add Manim Clip"
                visible: !mvmController.hasManimAsset
                enabled: mvmController.previewReady && !mvmController.busy
                onClicked: scriptDialog.open()
            }
            Button {
                text: "Export"
                enabled: mvmController.canExport
                onClicked: exportDialog.open()
            }
            BusyIndicator {
                running: mvmController.busy
                visible: running
                implicitWidth: 26
                implicitHeight: 26
            }
            Label {
                Layout.fillWidth: true
                text: mvmController.statusText
                color: "#e6e8ec"
                elide: Text.ElideRight
            }
        }

        Frame {
            visible: mvmController.hasManimAsset
            Layout.fillWidth: true

            contentItem: RowLayout {
                spacing: 14

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    Label {
                        Layout.fillWidth: true
                        text: "Script: " + mvmController.manimScriptPath
                        color: "#e6e8ec"
                        elide: Text.ElideMiddle
                    }
                    Label {
                        Layout.fillWidth: true
                        text: "Scene: " + mvmController.manimSceneName
                        color: "#e6e8ec"
                        elide: Text.ElideRight
                    }
                    Label {
                        text: "State: " + mvmController.manimStateText
                        color: mvmController.manimStateText === "SourceChanged"
                               ? "#f2c66d" : "#a8d5a2"
                        font.bold: true
                    }
                }

                Button {
                    text: mvmController.busy ? "Generating…" : "Regenerate"
                    visible: mvmController.hasManimAsset
                    enabled: visible && !mvmController.busy
                    onClicked: mvmController.regenerateManimClip()
                }
                Button {
                    text: "Add Manim to Timeline"
                    visible: !mvmController.hasManimTimelineClip
                    enabled: visible && !mvmController.busy
                    onClicked: mvmController.addManimToTimeline()
                }
            }
        }

        Frame {
            Layout.fillWidth: true
            Layout.fillHeight: true
            padding: 0

            background: Rectangle {
                color: "#08090b"
                border.color: "#343840"
            }

            Item {
                id: previewHost
                objectName: "previewHost"
                anchors.fill: parent
            }
        }

        RowLayout {
            Layout.fillWidth: true

            Button {
                text: mvmController.playing ? "Pause" : "Play"
                enabled: mvmController.playing || mvmController.canPlay
                onClicked: {
                    if (mvmController.playing)
                        mvmController.pauseTimeline()
                    else
                        mvmController.playTimeline()
                }
            }
            Label {
                Layout.fillWidth: true
                text: "Timeline  60 fps  |  " + mvmController.currentTimeText
                color: "#c9ccd2"
                font.bold: true
            }
            Button {
                text: "Delete"
                enabled: !mvmController.busy && mvmController.currentClipIndex >= 0
                onClicked: mvmController.deleteCurrentClip()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 142
            radius: 5
            color: "#20242a"
            border.color: "#3c424c"

            Flickable {
                id: timelineFlick
                anchors.fill: parent
                anchors.margins: 6
                clip: true
                contentWidth: Math.max(width, mvmController.totalTimelineFrames
                                               * root.pixelsPerFrame + 24)
                contentHeight: height
                boundsBehavior: Flickable.StopAtBounds

                Item {
                    id: timelineContent
                    width: timelineFlick.contentWidth
                    height: timelineFlick.height

                    Rectangle {
                        id: ruler
                        x: 0
                        y: 0
                        width: parent.width
                        height: 34
                        color: "#191c21"

                        Repeater {
                            model: Math.ceil(mvmController.totalTimelineFrames / 60) + 1

                            Item {
                                required property int index
                                x: index * 60 * root.pixelsPerFrame
                                width: 1
                                height: ruler.height

                                Rectangle {
                                    anchors.bottom: parent.bottom
                                    width: 1
                                    height: 12
                                    color: "#77808c"
                                }
                                Label {
                                    x: 4
                                    y: 1
                                    text: index + "s"
                                    color: "#aab1ba"
                                    font.pixelSize: 11
                                }
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: !mvmController.busy && mvmController.clipCount > 0
                            onClicked: mouse => {
                                const frame = Math.round(mouse.x / root.pixelsPerFrame)
                                mvmController.seekTimelineFrame(frame)
                            }
                        }
                    }

                    ListView {
                        id: timelineList
                        x: 0
                        y: ruler.height + 4
                        width: parent.width
                        height: parent.height - y
                        orientation: ListView.Horizontal
                        interactive: false
                        spacing: 0
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

                            property real leftPreviewDelta: 0
                            property real rightPreviewDelta: 0
                            property real bodyPressContentX: 0
                            property real bodyDragOffset: 0
                            property bool bodyMoved: false

                            width: Math.max(1, (timelineDurationFrames - leftPreviewDelta
                                               + rightPreviewDelta) * root.pixelsPerFrame)
                            height: timelineList.height - 8
                            radius: 3
                            color: index === mvmController.currentClipIndex ? "#315f86" : "#2b3038"
                            border.color: previewSupported ? "#65a8dc" : "#c88b4a"
                            z: bodyMoved ? 20 : 1
                            transform: Translate {
                                x: clipItem.leftPreviewDelta * root.pixelsPerFrame
                                   + clipItem.bodyDragOffset
                            }

                            Column {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                anchors.topMargin: 8
                                spacing: 3

                                Label {
                                    width: parent.width
                                    text: displayName
                                    color: "white"
                                    font.bold: true
                                    elide: Text.ElideMiddle
                                }
                                Label {
                                    width: parent.width
                                    text: sourceFpsNum + "/" + sourceFpsDen + " fps  |  "
                                          + Math.round(timelineDurationFrames) + "f"
                                    color: previewSupported ? "#b8c1cc" : "#f0b870"
                                    elide: Text.ElideRight
                                }
                            }

                            MouseArea {
                                id: bodyArea
                                anchors.fill: parent
                                anchors.leftMargin: 9
                                anchors.rightMargin: 9
                                enabled: !mvmController.busy
                                onPressed: mouse => {
                                    clipItem.bodyMoved = false
                                    clipItem.bodyDragOffset = 0
                                    clipItem.bodyPressContentX = mapToItem(timelineContent,
                                                                           mouse.x, mouse.y).x
                                }
                                onPositionChanged: mouse => {
                                    const now = mapToItem(timelineContent, mouse.x, mouse.y).x
                                    clipItem.bodyDragOffset = now - clipItem.bodyPressContentX
                                    if (Math.abs(clipItem.bodyDragOffset) > 5)
                                        clipItem.bodyMoved = true
                                }
                                onReleased: mouse => {
                                    const center = clipItem.timelineStartFrame
                                                   * root.pixelsPerFrame
                                                   + clipItem.bodyDragOffset
                                                   + clipItem.width / 2
                                    if (clipItem.bodyMoved) {
                                        let destination = timelineList.indexAt(center,
                                                                               timelineList.height / 2)
                                        if (destination < 0)
                                            destination = center < 0 ? 0
                                                                     : mvmController.clipCount - 1
                                        clipItem.bodyDragOffset = 0
                                        mvmController.reorderClip(clipItem.clipId, destination)
                                    } else {
                                        clipItem.bodyDragOffset = 0
                                        const point = bodyArea.mapToItem(clipItem,
                                                                         mouse.x, mouse.y)
                                        const frame = Math.round((clipItem.timelineStartFrame
                                                                  + point.x
                                                                    / root.pixelsPerFrame))
                                        mvmController.seekTimelineFrame(frame)
                                    }
                                }
                                onCanceled: {
                                    clipItem.bodyDragOffset = 0
                                    clipItem.bodyMoved = false
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
                                        pressContentX = mapToItem(timelineContent, mouse.x, mouse.y).x
                                    }
                                    onPositionChanged: mouse => {
                                        const now = mapToItem(timelineContent, mouse.x, mouse.y).x
                                        leftPreviewDelta = Math.round((now - pressContentX)
                                                                      / root.pixelsPerFrame)
                                    }
                                    onReleased: {
                                        const delta = leftPreviewDelta
                                        leftPreviewDelta = 0
                                        if (delta !== 0)
                                            mvmController.trimClip(clipId, "left", delta)
                                    }
                                    onCanceled: leftPreviewDelta = 0
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
                                        pressContentX = mapToItem(timelineContent, mouse.x, mouse.y).x
                                    }
                                    onPositionChanged: mouse => {
                                        const now = mapToItem(timelineContent, mouse.x, mouse.y).x
                                        rightPreviewDelta = Math.round((now - pressContentX)
                                                                       / root.pixelsPerFrame)
                                    }
                                    onReleased: {
                                        const delta = rightPreviewDelta
                                        rightPreviewDelta = 0
                                        if (delta !== 0)
                                            mvmController.trimClip(clipId, "right", delta)
                                    }
                                    onCanceled: rightPreviewDelta = 0
                                }
                            }
                        }
                    }

                    Rectangle {
                        id: playhead
                        x: (root.playheadDragging ? root.dragPlayheadFrame
                                                  : mvmController.playheadFrame)
                           * root.pixelsPerFrame - 1
                        y: 0
                        width: 2
                        height: parent.height
                        color: "#f15b5b"
                        z: 100

                        Rectangle {
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: 12
                            height: 9
                            color: "#f15b5b"
                        }

                        MouseArea {
                            anchors.centerIn: parent
                            width: 16
                            height: parent.height
                            enabled: !mvmController.busy && mvmController.clipCount > 0
                            cursorShape: Qt.SizeHorCursor
                            onPressed: {
                                root.dragPlayheadFrame = mvmController.playheadFrame
                                root.playheadDragging = true
                            }
                            onPositionChanged: mouse => {
                                const point = mapToItem(timelineContent, mouse.x, mouse.y)
                                root.dragPlayheadFrame = Math.max(0,
                                    Math.min(mvmController.totalTimelineFrames - 1,
                                             Math.round(point.x / root.pixelsPerFrame)))
                            }
                            onReleased: {
                                root.playheadDragging = false
                                mvmController.seekTimelineFrame(root.dragPlayheadFrame)
                            }
                            onCanceled: root.playheadDragging = false
                        }
                    }

                    Label {
                        visible: mvmController.clipCount === 0
                        anchors.centerIn: parent
                        text: "No clip"
                        color: "#858b95"
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            text: "Project: " + mvmController.projectPath
            color: "#858b95"
            elide: Text.ElideMiddle
        }
    }

    FileDialog {
        id: videoDialog
        title: "動画ファイルを選択"
        nameFilters: ["動画 (*.mp4 *.mov *.mkv *.ts)", "すべて (*)"]
        onAccepted: mvmController.addVideoClip(selectedFile)
    }

    FileDialog {
        id: exportDialog
        title: "書き出し先を指定"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "mp4"
        nameFilters: ["MP4 (*.mp4)"]
        onAccepted: mvmController.exportTimeline(selectedFile)
    }

    FileDialog {
        id: scriptDialog
        title: "Manim Python scriptを選択"
        nameFilters: ["Python script (*.py)"]
        onAccepted: {
            root.selectedManimScript = selectedFile
            sceneField.text = ""
            generationDialog.open()
            sceneField.forceActiveFocus()
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
                        if (mvmController.generateManimClip(root.selectedManimScript,
                                                           sceneField.text))
                            generationDialog.close()
                    }
                }
            }
        }
    }
}

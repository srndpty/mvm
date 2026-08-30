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

            Label {
                Layout.fillWidth: true
                text: "Timeline (左から順に書き出されます)"
                color: "#c9ccd2"
                font.bold: true
            }
            Button {
                text: "Move Left"
                enabled: !mvmController.busy && mvmController.currentClipIndex > 0
                onClicked: mvmController.moveCurrentClipLeft()
            }
            Button {
                text: "Move Right"
                enabled: !mvmController.busy
                         && mvmController.currentClipIndex >= 0
                         && mvmController.currentClipIndex < mvmController.clipCount - 1
                onClicked: mvmController.moveCurrentClipRight()
            }
            Button {
                text: "Delete"
                enabled: !mvmController.busy && mvmController.currentClipIndex >= 0
                onClicked: mvmController.deleteCurrentClip()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 92
            radius: 5
            color: "#20242a"
            border.color: "#3c424c"

            // 並び順がそのまま再生順。M5 はボタンによる移動・削除だけを持つ。
            Row {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8

                Repeater {
                    model: mvmController.clipNames

                    Rectangle {
                        required property int index
                        required property string modelData

                        height: parent.height
                        width: Math.max(150, Math.min(320,
                                   (parent.width - (mvmController.clipCount - 1) * 8)
                                   / Math.max(1, mvmController.clipCount)))
                        radius: 4
                        color: index === mvmController.currentClipIndex ? "#315f86" : "#2b3038"
                        border.color: index === mvmController.currentClipIndex
                                      ? "#65a8dc" : "#454b56"

                        Column {
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 4

                            Label {
                                width: parent.width
                                text: (index + 1) + ". " + modelData
                                color: "white"
                                font.bold: true
                                elide: Text.ElideMiddle
                            }
                            Label {
                                width: parent.width
                                text: index === mvmController.currentClipIndex
                                      ? "Preview中" : "クリックでPreview"
                                color: "#a9b1bd"
                                elide: Text.ElideRight
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: !mvmController.busy
                            onClicked: mvmController.selectClip(index)
                        }
                    }
                }
            }

            Label {
                visible: mvmController.clipCount === 0
                anchors.centerIn: parent
                text: "No clip"
                color: "#858b95"
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

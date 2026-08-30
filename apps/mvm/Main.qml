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
                text: "Add Manim Clip"
                enabled: mvmController.previewReady && !mvmController.busy
                onClicked: scriptDialog.open()
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

        Label {
            text: "Timeline"
            color: "#c9ccd2"
            font.bold: true
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 92
            radius: 5
            color: "#20242a"
            border.color: "#3c424c"

            Rectangle {
                visible: mvmController.hasCurrentClip
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.margins: 10
                width: Math.min(parent.width - 20, 520)
                radius: 4
                color: "#315f86"
                border.color: "#65a8dc"

                Column {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 5

                    Label {
                        width: parent.width
                        text: mvmController.currentClipName
                        color: "white"
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    Label {
                        width: parent.width
                        text: "Video · " + mvmController.currentClipPath
                        color: "#d8edf9"
                        elide: Text.ElideMiddle
                    }
                }
            }

            Label {
                visible: !mvmController.hasCurrentClip
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

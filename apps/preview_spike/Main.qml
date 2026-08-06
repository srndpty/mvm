// mvm Phase 1 / P1 - preview_spike の UI
//
// **製品 UI ではない。** P1 の判定に必要な操作と表示だけを置く。
// タイムライン・トラック・エフェクト・音声は無い。

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import mvm.preview 1.0

ApplicationWindow {
    id: root
    width: 1280
    height: 800
    visible: true
    title: "mvm preview_spike (Phase 1 / P1)"

    // 計測モードでは操作させない。人の操作が測定値に混ざる。
    readonly property bool interactive: !measureMode

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        PreviewSurface {
            id: surface
            objectName: "previewSurface"
            Layout.fillWidth: true
            Layout.fillHeight: true
            backgroundColor: "#101014"
            linearFilter: filterCheck.checked
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            spacing: 8

            Button {
                text: "Open"
                enabled: root.interactive
                onClicked: fileDialog.open()
            }
            Button {
                text: spike.playing ? "Pause" : "Play"
                enabled: root.interactive
                onClicked: spike.playing ? spike.pause() : spike.play()
            }
            Button {
                text: "Step"
                enabled: root.interactive
                onClicked: spike.stepForward()
            }
            CheckBox {
                id: filterCheck
                text: "linear filter"
                checked: true
            }
            Label { text: "seek" }
            Slider {
                id: seekSlider
                Layout.fillWidth: true
                from: 0
                to: Math.max(1, spike.frameCount - 1)
                stepSize: 1
                enabled: root.interactive && spike.frameCount > 0
                onMoved: spike.seekTo(Math.round(value))
            }
        }

        GridLayout {
            columns: 4
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            Layout.bottomMargin: 8
            columnSpacing: 16

            Label { text: "requested frame" }
            Label { text: spike.requestedFrame; font.bold: true }
            Label { text: "displayed frame" }
            Label { text: spike.displayedFrame; font.bold: true }

            Label { text: "fps (表示)" }
            Label { text: spike.fps.toFixed(1); font.bold: true }
            Label { text: "frames" }
            Label { text: spike.frameCount }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            text: spike.counterText
            font.family: "Consolas"
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            text: spike.deviceText
            font.family: "Consolas"
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            Layout.bottomMargin: 8
            text: spike.statusText
            wrapMode: Text.WordWrap
        }
    }

    FileDialog {
        id: fileDialog
        title: "素材を開く"
        nameFilters: ["動画 (*.mp4 *.mov *.mkv *.ts)", "すべて (*)"]
        onAccepted: spike.openMedia(selectedFile)
    }
}

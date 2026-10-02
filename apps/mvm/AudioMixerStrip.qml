pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    required property var mvmController
    property bool master: false
    property int trackIndex: -1
    property string trackName: ""
    property string mixerName: ""
    property bool muted: false
    property bool solo: false
    property real gainDb: 0
    property real pan: 0
    property bool compact: false
    property real meterLeft: -96
    property real meterRight: -96
    property bool meterClipped: false
    readonly property real currentGain: master ? (mvmController.masterVolume <= 0 ? -96 : 20 * Math.log10(mvmController.masterVolume)) : gainDb
    implicitWidth: compact ? 88 : 111
    implicitHeight: 330
    color: compact ? "transparent" : master ? "#242931" : "#1b1f25"
    border.color: compact ? "transparent" : "#343b45"
    function editGain(value, commit) {
        if (master) mvmController.masterVolume = value <= -96 ? 0 : Math.pow(10, value / 20);
        else mvmController.setAudioTrackMix(trackIndex, value, pan, commit);
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: root.compact ? 0 : 5
        spacing: 3
        Item {
            visible: !root.compact
            Layout.fillWidth: true
            Layout.preferredHeight: 86
            AudioPanKnob {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: !root.master
                value: root.pan
                onValueEdited: (value, commit) => root.mvmController.setAudioTrackMix(root.trackIndex, root.gainDb, value, commit)
                onEditCanceled: root.mvmController.cancelAudioTrackMix(root.trackIndex)
            }
            Label { anchors.centerIn: parent; visible: root.master; text: "マスター"; color: "#c3cbd5"; font.pixelSize: 11 }
        }
        RowLayout {
            visible: !root.compact
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredHeight: 24
            Button {
                visible: !root.master
                text: "M"; implicitWidth: 26; implicitHeight: 23
                highlighted: root.muted
                onClicked: root.mvmController.setTrackMuted("audio", root.trackIndex, !root.muted)
            }
            Button {
                visible: !root.master
                text: "S"; implicitWidth: 26; implicitHeight: 23
                highlighted: root.solo
                onClicked: root.mvmController.setTrackSolo("audio", root.trackIndex, !root.solo)
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 75
            spacing: 4
            AudioFader {
                Layout.preferredWidth: 43
                Layout.fillHeight: true
                value: root.currentGain
                onValueEdited: (value, commit) => root.editGain(value, commit)
                onEditCanceled: if (!root.master) root.mvmController.cancelAudioTrackMix(root.trackIndex)
            }
            AudioMeter {
                id: meter
                Layout.fillHeight: true
                Layout.fillWidth: true
                showScale: !root.compact
                dbLeft: root.master ? root.mvmController.audioMeterDbLeft : root.meterLeft
                dbRight: root.master ? root.mvmController.audioMeterDbRight : root.meterRight
                clipped: root.master ? root.mvmController.audioMeterClipped : root.meterClipped
                onClipCleared: {
                    if (root.master) root.mvmController.clearMasterAudioClip();
                    else { root.mvmController.clearAudioTrackClip(root.trackIndex); root.meterClipped = false; }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 0
            AudioValueField {
                Layout.preferredWidth: 43
                value: root.currentGain
                onValueEdited: value => root.editGain(value, true)
            }
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: meter.dbText(Math.max(meter.shownLeft, meter.shownRight))
                color: "#a1aab6"; font.pixelSize: 10
            }
        }
        TextField {
            objectName: root.master ? "audioMasterName" : "audioMixerName" + root.trackIndex
            visible: !root.compact
            Layout.fillWidth: true
            Layout.preferredHeight: 24
            text: root.master ? "マスター" : root.mixerName
            readOnly: root.master
            selectByMouse: true
            property bool nameEdited: false
            onTextEdited: nameEdited = true
            font.pixelSize: 10
            horizontalAlignment: Text.AlignHCenter
            padding: 3
            onEditingFinished: {
                if (!root.master && nameEdited) root.mvmController.setAudioMixerName(root.trackIndex, text);
                nameEdited = false;
                text = Qt.binding(() => root.master ? "マスター" : root.mixerName);
            }
            Keys.onEscapePressed: event => {
                nameEdited = false;
                text = Qt.binding(() => root.master ? "マスター" : root.mixerName);
                focus = false; event.accepted = true;
            }
        }
        Label { visible: !root.compact; Layout.alignment: Qt.AlignHCenter; text: root.master ? "マスター" : root.trackName; color: "#8a929d"; font.pixelSize: 9 }
    }
    Timer {
        interval: 50
        running: !root.master && root.visible
        repeat: true
        onTriggered: {
            const reading = root.mvmController.audioTrackMeter(root.trackIndex);
            root.meterLeft = reading.left;
            root.meterRight = reading.right;
            root.meterClipped = reading.clipped;
        }
    }
}

pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

ScrollView {
    id: root
    required property var mvmController
    implicitHeight: 295
    clip: true
    contentWidth: strips.width
    contentHeight: strips.height
    ScrollBar.vertical.policy: ScrollBar.AsNeeded
    WheelHandler {
        target: null
        onWheel: event => {
            const maximum = Math.max(0, root.contentWidth - root.availableWidth);
            if (maximum <= 0) {
                event.accepted = false;
                return;
            }
            const pixels = event.pixelDelta;
            const angles = event.angleDelta;
            const delta = pixels.x !== 0 ? pixels.x : pixels.y !== 0 ? pixels.y
                          : (angles.x !== 0 ? angles.x : angles.y) / 120 * 40;
            root.contentItem.contentX = Math.max(0, Math.min(maximum, root.contentItem.contentX - delta));
            event.accepted = true;
        }
    }
    Row {
        id: strips
        spacing: 1
        height: Math.max(295, root.availableHeight)
        Repeater {
            model: root.mvmController.audioTrackModel
            AudioMixerStrip {
                required trackIndex
                required trackName
                required mixerName
                required property real mixerGainDb
                required property real mixerPan
                required property bool trackMuted
                required property bool trackSolo
                objectName: "audioMixerTrack" + trackIndex
                height: strips.height
                mvmController: root.mvmController
                gainDb: mixerGainDb
                pan: mixerPan
                muted: trackMuted
                solo: trackSolo
            }
        }
        AudioMixerStrip {
            objectName: "audioMixerMaster"
            mvmController: root.mvmController
            master: true
            height: strips.height
        }
    }
}

import QtQuick
import QtQuick.Controls

ProgressBar {
    id: progress
    implicitHeight: 7

    background: Rectangle {
        implicitHeight: 7
        color: "#1d2127"
        radius: 4
    }
    contentItem: Item {
        clip: true
        Rectangle {
            id: fill
            property real phase: -0.35
            x: progress.indeterminate ? phase * parent.width : 0
            width: progress.indeterminate ? parent.width * 0.35
                                          : parent.width * progress.position
            height: parent.height
            color: "#5a9cda"
            radius: 4
            NumberAnimation on phase {
                from: -0.35
                to: 1
                duration: 1400
                loops: Animation.Infinite
                running: progress.indeterminate
            }
        }
    }
}

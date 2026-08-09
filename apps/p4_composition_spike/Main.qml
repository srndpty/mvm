import QtQuick
import QtQuick.Window
import mvm.compositor 1.0

Window {
    width: 1920
    height: 1080
    visible: true
    color: "black"
    title: "mvm P4 dynamic composition spike"
    CompositorSurface {
        objectName: "compositorSurface"
        anchors.fill: parent
    }
}

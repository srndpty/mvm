import QtQuick
import QtQuick.Window
import mvm.compositor 1.0

Window {
    width: 1920
    height: 1080
    // main()がcontroller.attach()でdiagnostic configを確定させるまでrender threadを
    // 起動させない。visible:trueのままだとengine.load()中にinitialize()が走り、
    // attach()前のconfigでrendererが構成されるrunが混ざる。
    visible: false
    color: "black"
    title: "mvm P2-C2 compositor spike"
    CompositorSurface {
        objectName: "compositorSurface"
        anchors.fill: parent
    }
}

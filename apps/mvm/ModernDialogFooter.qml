import QtQuick

Item {
    id: footer
    implicitHeight: 58
    default property alias actions: buttons.data

    Rectangle {
        anchors.fill: parent
        color: "#25272b"
        radius: 8
    }
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 1
        color: "#41454b"
    }
    Row {
        id: buttons
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8
    }
}

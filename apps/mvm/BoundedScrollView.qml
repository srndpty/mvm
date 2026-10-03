import QtQuick
import QtQuick.Controls

ScrollView {
    id: view
    clip: true
    Binding {
        target: view.contentItem
        property: "boundsBehavior"
        value: Flickable.StopAtBounds
        when: view.contentItem instanceof Flickable
    }
    Binding {
        target: view.contentItem
        property: "boundsMovement"
        value: Flickable.StopAtBounds
        when: view.contentItem instanceof Flickable
    }
}

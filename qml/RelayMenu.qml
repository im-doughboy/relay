import QtQuick
import QtQuick.Controls.Basic

// Right-click menu in Relay's style. Add RelayMenuItem { text; onTriggered } children.
Menu {
    id: menu
    padding: 6
    implicitWidth: 260

    background: Rectangle {
        implicitWidth: 260
        radius: Theme.radius
        color: Theme.panelSolid
        border.width: 1
        border.color: Theme.border
    }

    delegate: RelayMenuItem {}
}

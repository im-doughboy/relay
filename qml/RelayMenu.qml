import QtQuick
import QtQuick.Controls.Basic

// Right-click menu in Relay's style. Add MenuItem { text; onTriggered } children.
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

    delegate: MenuItem {
        id: item
        implicitHeight: 40
        contentItem: Text {
            leftPadding: 12
            text: item.text
            color: item.enabled ? Theme.text : Theme.textMuted
            font.pixelSize: Theme.fontBody - 1
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: 6
            color: item.highlighted ? Theme.focus : "transparent"
        }
    }
}

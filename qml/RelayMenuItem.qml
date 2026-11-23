import QtQuick
import QtQuick.Controls.Basic

// An item of a RelayMenu (right-click menus), in Relay's style.
MenuItem {
    id: item
    implicitHeight: visible ? 40 : 0
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

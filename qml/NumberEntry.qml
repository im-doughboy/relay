import QtQuick
import Relay.Core

// Big channel number in the corner while the user types digits.
Rectangle {
    anchors.top: parent.top
    anchors.right: parent.right
    anchors.margins: 36
    width: Math.max(140, digits.implicitWidth + 48)
    height: 88
    radius: Theme.radius
    color: Theme.panel
    border.width: 1
    border.color: Theme.accent
    visible: Player.pendingNumber.length > 0

    Text {
        id: digits
        anchors.centerIn: parent
        text: Player.pendingNumber
        color: Theme.accent
        font.pixelSize: Theme.fontDisplay
        font.weight: Font.DemiBold
        font.letterSpacing: 2
    }
}

import QtQuick
import Relay.Core

// While casting: what's on, and where, with pause and stop. Sits at the top
// of the picture; Relay's own picture shows a casting screen underneath.
Item {
    id: root
    signal stopRequested()
    signal pickRequested()
    visible: Cast.active
    anchors.fill: parent

    // Instead of the picture.
    Rectangle {
        anchors.fill: parent
        color: Theme.background
        // Clicks stay here (the picture underneath isn't playing).
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => { w.accepted = false } }
        Column {
            anchors.centerIn: parent
            spacing: 14
            width: Math.min(parent.width - 80, 640)
            Icon {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 64; height: 64
                path: Icons.cast
                color: Theme.accent
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: Cast.state === "connecting" ? qsTr("Starting on %1…").arg(Cast.deviceName) : qsTr("Playing on %1").arg(Cast.deviceName)
                color: Theme.text
                font.pixelSize: Theme.fontLarge + 4
                font.weight: Font.DemiBold
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: Cast.title
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                visible: Cast.state === "connecting"
                text: qsTr("Live channels take a few seconds to start on a Chromecast.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 12
                topPadding: 10
                BarButton {
                    icon: Cast.state === "paused" ? Icons.play : Icons.pause; iconFilled: true
                    label: Cast.state === "paused" ? qsTr("Play") : qsTr("Pause"); shortcut: "Space"
                    enabled: Cast.state === "playing" || Cast.state === "paused"
                    onClicked: Cast.togglePause()
                }
                BarButton {
                    icon: Icons.cast
                    label: qsTr("Cast somewhere else"); shortcut: Keymap.label("cast")
                    onClicked: root.pickRequested()
                }
                BarButton {
                    icon: Icons.close
                    accent: true
                    label: qsTr("Stop casting"); shortcut: "Esc"
                    onClicked: root.stopRequested()
                }
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                visible: Cast.live
                text: qsTr("↑ ↓ and the channel list change the channel on the TV")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
            }
        }
    }
}

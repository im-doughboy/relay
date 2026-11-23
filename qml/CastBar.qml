import QtQuick
import Relay.Core

// While casting: what's on, and where, with pause and stop. Sits at the top
// of the picture; Relay's own picture shows a casting screen underneath.
Item {
    id: root
    signal stopRequested()
    signal pickRequested()
    signal nextRequested()
    function clock(secs) {
        secs = Math.max(0, Math.round(secs))
        const h = Math.floor(secs / 3600), m = Math.floor(secs % 3600 / 60), s = secs % 60
        return (h > 0 ? h + ":" + String(m).padStart(2, "0") : String(m)) + ":" + String(s).padStart(2, "0")
    }
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
                text: Cast.deviceId.startsWith("cc:") ? qsTr("Live channels take a few seconds to start on a Chromecast.")
                                                      : qsTr("The TV may take a few seconds to start.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
            }
            // A movie or episode: how far in, and skipping (click the bar to jump).
            Column {
                width: parent.width
                spacing: 6
                visible: !Cast.live && Cast.duration > 0
                Item {
                    width: parent.width
                    height: 18
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                        height: 6
                        radius: 3
                        color: Theme.track
                        Rectangle {
                            width: parent.width * Math.min(1, Cast.position / Math.max(1, Cast.duration))
                            height: parent.height
                            radius: 3
                            color: Theme.accent
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: (m) => Cast.seek(Cast.duration * m.x / width)
                    }
                }
                Item {
                    width: parent.width
                    height: posText.implicitHeight
                    Text { id: posText; text: root.clock(Cast.position); color: Theme.textSecondary; font.pixelSize: Theme.fontSmall }
                    Text { anchors.right: parent.right; text: "-" + root.clock(Math.max(0, Cast.duration - Cast.position)); color: Theme.textSecondary; font.pixelSize: Theme.fontSmall }
                }
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 12
                topPadding: 10
                BarButton {
                    visible: !Cast.live && Cast.duration > 0
                    icon: Icons.rewind; iconFilled: true
                    label: qsTr("Back 10 seconds"); shortcut: "←"; showLabel: false
                    onClicked: Cast.seekBy(-10)
                }
                BarButton {
                    icon: Cast.state === "paused" ? Icons.play : Icons.pause; iconFilled: true
                    label: Cast.state === "paused" ? qsTr("Play") : qsTr("Pause"); shortcut: "Space"
                    enabled: Cast.state === "playing" || Cast.state === "paused"
                    onClicked: Cast.togglePause()
                }
                BarButton {
                    visible: !Cast.live && Cast.duration > 0
                    icon: Icons.forward; iconFilled: true
                    label: qsTr("Ahead 30 seconds"); shortcut: "→"; showLabel: false
                    onClicked: Cast.seekBy(30)
                }
                BarButton {
                    visible: !Cast.live && (Vod.queue.length > 0 || Player.vod.kind === "episode")
                    icon: Icons.skipNext; iconFilled: true
                    label: Vod.queue.length > 0 ? qsTr("Play the next in the queue") : qsTr("Next episode"); shortcut: "N"; showLabel: false
                    onClicked: root.nextRequested()
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
            // Up next (the queue).
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                visible: !Cast.live && Vod.queue.length > 0
                text: Vod.queue.length > 0
                      ? qsTr("Up next: %1").arg(Vod.queue[0].title + (Vod.queue[0].kind === "episode" ? " · " + Vod.queue[0].subtitle : ""))
                        + (Vod.queue.length > 1 ? "  " + qsTr("(+%1 more)").arg(Vod.queue.length - 1) : "")
                      : ""
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall + 1
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                visible: !Cast.live && Cast.duration > 0
                text: qsTr("← → skip back and ahead   ·   ↑ ↓ a minute   ·   Space pause   ·   Menu: browse while it plays")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
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

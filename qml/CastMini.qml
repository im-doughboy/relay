import QtQuick
import Relay.Core

// While casting and something else is open (Movies & series, the guide,
// Recordings…): a small panel in the corner, so you can browse, add to the
// queue or pick something else, and still pause, skip or stop the TV.
// Play / pause on a remote or the phone works from anywhere too.
Rectangle {
    id: root
    property bool shown: false
    signal stopRequested()
    signal backRequested()          // to the casting screen
    signal nextRequested()

    visible: Cast.active && shown
    width: Math.min(parent.width - 32, 520)
    height: column.implicitHeight + 24
    anchors.right: parent.right
    anchors.bottom: parent.bottom
    anchors.margins: 18
    radius: 14
    color: Theme.panelSolid
    border.width: 1
    border.color: Theme.border
    function clock(secs) {
        secs = Math.max(0, Math.round(secs))
        const h = Math.floor(secs / 3600), m = Math.floor(secs % 3600 / 60), s = secs % 60
        return (h > 0 ? h + ":" + String(m).padStart(2, "0") : String(m)) + ":" + String(s).padStart(2, "0")
    }
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = true }

    Column {
        id: column
        x: 14; y: 12
        width: parent.width - 28
        spacing: 8
        Row {
            width: parent.width
            spacing: 10
            Icon { width: 22; height: 22; path: Icons.cast; color: Theme.accent; anchors.verticalCenter: parent.verticalCenter }
            Column {
                width: parent.width - 32
                Text {
                    width: parent.width
                    text: Cast.title
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    text: (Cast.state === "paused" ? qsTr("Paused on %1") : qsTr("Playing on %1")).arg(Cast.deviceName)
                          + (Vod.queue.length ? "  ·  " + qsTr("%n up next", "", Vod.queue.length) : "")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                    elide: Text.ElideRight
                }
            }
        }
        Item {
            visible: !Cast.live && Cast.duration > 0
            width: parent.width
            height: 14
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width; height: 4; radius: 2
                color: Theme.track
                Rectangle { width: parent.width * Math.min(1, Cast.position / Math.max(1, Cast.duration)); height: parent.height; radius: 2; color: Theme.accent }
            }
            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: (m) => Cast.seek(Cast.duration * m.x / width) }
        }
        Row {
            spacing: 6
            Text {
                visible: !Cast.live && Cast.duration > 0
                anchors.verticalCenter: parent.verticalCenter
                width: 96
                text: root.clock(Cast.position) + " / " + root.clock(Cast.duration)
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
            }
            BarButton { visible: !Cast.live && Cast.duration > 0; icon: Icons.rewind; iconFilled: true; label: qsTr("Back 10 seconds"); showLabel: false; onClicked: Cast.seekBy(-10) }
            BarButton { icon: Cast.state === "paused" ? Icons.play : Icons.pause; iconFilled: true; label: qsTr("Pause / play"); showLabel: false; onClicked: Cast.togglePause() }
            BarButton { visible: !Cast.live && Cast.duration > 0; icon: Icons.forward; iconFilled: true; label: qsTr("Ahead 30 seconds"); showLabel: false; onClicked: Cast.seekBy(30) }
            BarButton {
                visible: !Cast.live && (Vod.queue.length > 0 || Player.vod.kind === "episode")
                icon: Icons.skipNext; iconFilled: true
                label: Vod.queue.length > 0 ? qsTr("Play the next in the queue") : qsTr("Next episode"); showLabel: false
                onClicked: root.nextRequested()
            }
            BarButton { icon: Icons.cast; label: qsTr("Casting screen"); showLabel: false; onClicked: root.backRequested() }
            BarButton { icon: Icons.close; label: qsTr("Stop casting"); showLabel: false; onClicked: root.stopRequested() }
        }
    }
}

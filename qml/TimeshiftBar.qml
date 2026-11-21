import QtQuick
import Relay.Core

// Live TV's player controls (pause and rewind live TV). They appear along the
// bottom when the mouse moves over the picture, or for a moment after a skip
// with the keys: back / forward 10 s, pause, a timeline of what's been kept
// (click or drag to jump; the right end is live) and Live. While paused or
// behind live with the controls hidden, a small reminder stays at the top left.
Item {
    id: root
    property var video
    // Mouse moved over the picture recently (set by Main).
    property bool mouseActive: false
    // Nothing full-screen is open on top of live TV (set by Main).
    property bool allowed: true
    // Room to leave at the bottom (the info bar, when it's up).
    property real bottomInset: 0
    signal message(string text)

    readonly property bool canRewind: Settings.timeshiftMb > 0
    readonly property bool behind: video.liveBehind > 0.5
    readonly property bool timeshifting: !Player.vodActive && video.playing && (video.paused || behind)
    readonly property bool shown: allowed && !Player.vodActive && Player.hasChannel
                                  && (mouseActive || flashTimer.running || hover.hovered || scrub.pressed)
    readonly property real panelHeight: panel.height

    anchors.fill: parent

    function flash() { flashTimer.restart() }
    function skip(secs) {
        if (!canRewind) { message(qsTr("Turn on “Pause live TV” in Settings to rewind")); return }
        if (secs < 0 && video.liveBuffer < 1) { message(qsTr("Nothing saved to rewind yet")); flash(); return }
        if (secs > 0 && !behind) { message(qsTr("Already live")); flash(); return }
        video.seekLive(secs)
        flash()
    }
    function togglePause() { video.togglePause(); flash() }
    function goLive() { video.goLive(); message(qsTr("Live")); flash() }
    function clock(secs) {
        secs = Math.max(0, Math.round(secs))
        const h = Math.floor(secs / 3600), m = Math.floor(secs % 3600 / 60), s = secs % 60
        return (h > 0 ? h + ":" + String(m).padStart(2, "0") : String(m)) + ":" + String(s).padStart(2, "0")
    }
    Timer { id: flashTimer; interval: 3500 }

    // ---- Reminder while not live (controls hidden) -------------------------------
    Rectangle {
        visible: root.allowed && root.timeshifting && !root.shown
        x: 24; y: 24
        height: 40
        width: pillRow.implicitWidth + 28
        radius: 20
        color: "#CC000000"
        border.width: 1
        border.color: Theme.border
        Row {
            id: pillRow
            anchors.centerIn: parent
            spacing: 10
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                width: 16; height: 16
                path: root.video.paused ? Icons.pause : Icons.play
                filled: true
                color: Theme.accent
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.behind ? qsTr("%1 behind live").arg(root.clock(root.video.liveBehind)) : qsTr("Paused")
                color: Theme.text
                font.pixelSize: Theme.fontSmall + 1
                font.weight: Font.Medium
            }
        }
        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.flash() }
    }

    // ---- Controls ------------------------------------------------------------------
    Item {
        id: panel
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.bottomInset
        height: 128
        opacity: root.shown ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: Theme.animNormal } }
        Behavior on anchors.bottomMargin { NumberAnimation { duration: Theme.animNormal; easing.type: Easing.OutCubic } }

        Rectangle {
            anchors.fill: parent
            visible: root.bottomInset === 0
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#00000000" }
                GradientStop { position: 0.5; color: "#B0000000" }
                GradientStop { position: 1.0; color: "#E0000000" }
            }
        }
        HoverHandler { id: hover }
        // Clicks on the bar stay on the bar (the picture underneath would open
        // the channel list); the wheel still reaches the picture (volume).
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: (w) => { w.accepted = false }
        }

        // Timeline of what's been kept; the right end is live.
        Item {
            id: track
            x: 40
            width: parent.width - 80
            y: 26
            height: 24
            enabled: root.canRewind
            readonly property real total: root.video.liveBuffer + root.video.liveBehind
            readonly property real fraction: total > 1 ? root.video.liveBuffer / total : 1
            readonly property real dragFraction: Math.max(0, Math.min(1, scrub.mouseX / width))
            readonly property real shownFraction: scrub.pressed ? dragFraction : fraction
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: scrub.containsMouse || scrub.pressed ? 8 : 5
                radius: height / 2
                color: Theme.track
                Rectangle {
                    width: parent.width * track.shownFraction
                    height: parent.height
                    radius: height / 2
                    color: Theme.accent
                }
            }
            Rectangle {   // the playing point
                x: track.width * track.shownFraction - width / 2
                anchors.verticalCenter: parent.verticalCenter
                width: scrub.pressed ? 20 : 16
                height: width
                radius: width / 2
                color: Theme.text
                visible: root.canRewind
            }
            Rectangle {   // where a click / drag lands, as time behind live
                visible: (scrub.containsMouse || scrub.pressed) && track.total > 1
                x: Math.max(0, Math.min(track.width - width, scrub.mouseX - width / 2))
                y: -36
                width: tipText.implicitWidth + 18
                height: 28
                radius: 5
                color: Theme.panelSolid
                border.width: 1
                border.color: Theme.border
                Text {
                    id: tipText
                    anchors.centerIn: parent
                    readonly property real offset: (1 - track.dragFraction) * track.total
                    text: offset < 2 ? qsTr("Live") : "−" + root.clock(offset)
                    color: Theme.text
                    font.pixelSize: Theme.fontSmall
                }
            }
            MouseArea {
                id: scrub
                anchors.fill: parent
                anchors.topMargin: -10
                anchors.bottomMargin: -10
                hoverEnabled: true
                cursorShape: root.canRewind ? Qt.PointingHandCursor : Qt.ArrowCursor
                preventStealing: true
                onReleased: {
                    if (track.total <= 1) return
                    const target = track.total * track.dragFraction
                    const delta = target - root.video.liveBuffer
                    if (track.dragFraction > 0.995) root.goLive()
                    else if (Math.abs(delta) >= 1) { root.video.seekLive(delta); root.flash() }
                }
            }
        }

        // Status, buttons, Live.
        Item {
            x: 40
            width: parent.width - 80
            y: 64
            height: 48

            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 10; height: 10; radius: 5
                    color: root.timeshifting ? Theme.textMuted : "#FF4D4D"
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.behind ? qsTr("%1 behind live").arg(root.clock(root.video.liveBehind))
                          : root.video.paused ? qsTr("Paused") : qsTr("Live")
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.Medium
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: root.canRewind && track.total > 1
                    text: qsTr("· %1 saved").arg(root.clock(track.total))
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall + 1
                }
            }

            Row {
                anchors.centerIn: parent
                spacing: 12
                BarButton {
                    anchors.verticalCenter: parent.verticalCenter
                    icon: Icons.rewind; iconFilled: true
                    label: qsTr("Back 10 seconds"); shortcut: "Shift+←"; showLabel: false
                    enabled: root.canRewind
                    onClicked: root.skip(-10)
                }
                BarButton {
                    anchors.verticalCenter: parent.verticalCenter
                    size: 52
                    accent: true
                    icon: root.video.paused ? Icons.play : Icons.pause; iconFilled: true
                    label: root.video.paused ? qsTr("Play") : qsTr("Pause"); shortcut: "Space"; showLabel: false
                    onClicked: root.togglePause()
                }
                BarButton {
                    anchors.verticalCenter: parent.verticalCenter
                    icon: Icons.forward; iconFilled: true
                    label: qsTr("Forward 10 seconds"); shortcut: "Shift+→"; showLabel: false
                    enabled: root.canRewind && root.behind
                    onClicked: root.skip(10)
                }
            }

            BarButton {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                icon: Icons.toLive; iconFilled: true
                label: qsTr("Live"); shortcut: "End"
                accent: root.timeshifting
                enabled: root.timeshifting
                onClicked: root.goLive()
            }
        }
    }
}

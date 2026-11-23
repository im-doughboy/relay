import QtQuick
import QtQuick.Controls.Basic
import Relay.Core

// Playback controls for movies, episodes and catch-up: title, seek bar,
// times and audio/subtitle pickers. Shows on any key or mouse move, hides
// after a few seconds unless paused.
//
// Catch-up streams usually can't seek, so a catch-up seek asks the server
// for the archive again from the new time (after a short pause, so several
// presses add up to one request).
Item {
    id: root
    property var video
    property bool shown: false
    property string seekHint      // "+1 min 20 s" while seeking
    property int seekTotal: 0
    signal nextRequested()
    signal liveRequested()
    signal backRequested()
    signal optionsRequested()
    signal castRequested()
    signal multiViewRequested()
    signal sourceRequested()

    readonly property bool catchup: Player.vod.kind === "catchup"
    property real pendingPos: -1          // catch-up seek waiting to be sent
    // Position and length in seconds; for catch-up, within the show.
    readonly property real dur: catchup ? (Player.vod.programmeStop - Player.vod.programmeStart) / 1000
                                        : video.duration
    readonly property real pos: pendingPos >= 0 ? pendingPos
                              : catchup ? (Player.vod.offset || 0) - (Player.vod.start || 0) + video.position
                              : video.position
    // Catch-up can't go past what has aired.
    readonly property real maxPos: catchup ? Math.min(dur, (Date.now() - Player.vod.programmeStart) / 1000 - 30) : dur

    anchors.fill: parent
    visible: Player.vodActive

    function show() { shown = true; hideTimer.restart() }
    function hide() { shown = false }
    function toggle() { shown ? hide() : show() }
    function seekTo(target) {
        if (!(dur > 0)) return
        if (catchup) {
            pendingPos = Math.max(0, Math.min(target, maxPos - 5))
            catchupSeek.restart()
        } else {
            video.seek(target, false)
        }
        show()
    }
    function seekBy(secs) {
        if (!(dur > 0)) return
        if (catchup) seekTo(pos + secs)
        else video.seek(secs, true)
        seekTotal = hintTimer.running ? seekTotal + secs : secs
        seekHint = (seekTotal >= 0 ? "+" : "−") + root.amount(Math.abs(seekTotal))
        hintTimer.restart()
        show()
    }
    function amount(secs) {
        const m = Math.floor(secs / 60), s = secs % 60
        if (!m) return qsTr("%1 s").arg(s)
        return s ? qsTr("%1 min %2 s").arg(m).arg(s) : qsTr("%1 min").arg(m)
    }
    function clock(secs) {
        secs = Math.max(0, Math.round(secs || 0))
        const h = Math.floor(secs / 3600), m = Math.floor(secs % 3600 / 60), s = secs % 60
        return (h > 0 ? h + ":" + String(m).padStart(2, "0") : String(m)) + ":" + String(s).padStart(2, "0")
    }
    // Next audio track, or next subtitle track (subtitles include "off").
    function cycleTrack(type) {
        const list = video.tracks(type)
        if (type === "audio" && list.length < 2) return list.length ? qsTr("Only one audio track") : qsTr("No audio tracks")
        if (type === "sub" && !list.length) return qsTr("No subtitles")
        let cur = list.findIndex(t => t.selected)
        if (type === "sub") {
            // off → 0 → 1 → … → off
            const next = cur + 1
            if (next >= list.length) { video.setTrack("sub", -1); return qsTr("Subtitles off") }
            video.setTrack("sub", list[next].id)
            return qsTr("Subtitles: %1").arg(list[next].label)
        }
        const next = list[(cur + 1) % list.length]
        video.setTrack("audio", next.id)
        return qsTr("Audio: %1").arg(next.label)
    }
    function openTrackMenu(type, anchor) {
        trackMenu.type = type
        trackMenu.items = video.tracks(type)
        trackMenu.popup(anchor, 0, -trackMenu.implicitHeight - 8)
    }

    Timer { id: hideTimer; interval: 4000; onTriggered: if (!root.video.paused && !trackMenu.opened && !panelHover.hovered) root.shown = false }
    Timer { id: hintTimer; interval: 900; onTriggered: root.seekHint = "" }
    Timer { id: catchupSeek; interval: 700; onTriggered: Player.seekCatchup(root.pendingPos) }
    Connections {
        target: root.video
        function onPausedChanged() { if (root.video.paused) root.show(); else hideTimer.restart() }
        function onPlaybackStarted() { if (!catchupSeek.running) root.pendingPos = -1 }
        function onPlaybackError() { root.pendingPos = -1 }
    }
    Connections {
        target: Player
        function onVodChanged() { if (Player.vodActive) root.show(); else root.pendingPos = -1 }
    }

    // Big pause / seek feedback in the middle.
    Rectangle {
        anchors.centerIn: parent
        visible: Player.vodActive && (root.video.paused || root.seekHint.length > 0) && !root.video.buffering
        width: Math.max(96, centerText.implicitWidth + 48)
        height: 96
        radius: 48
        color: "#A0000000"
        Text {
            id: centerText
            anchors.centerIn: parent
            text: root.seekHint.length ? root.seekHint : "❚❚"
            color: Theme.text
            font.pixelSize: root.seekHint.length ? Theme.fontTitle : 34
            font.weight: Font.DemiBold
        }
    }

    // Stop and go back (same as Esc).
    CloseButton {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 24
        visible: Player.vodActive && panel.visible
        opacity: panel.opacity
        label: root.catchup ? qsTr("Back to live") : qsTr("Stop")
        onClicked: root.backRequested()
    }

    Item {
        id: panel
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 200
        opacity: root.shown ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: Theme.animNormal } }

        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#00000000" }
                GradientStop { position: 0.55; color: "#C0000000" }
                GradientStop { position: 1.0; color: "#E6000000" }
            }
        }
        HoverHandler {
            id: panelHover
            onHoveredChanged: if (!hovered) hideTimer.restart()
        }
        MouseArea {   // keep the panel up while the pointer is on it
            anchors.fill: parent
            hoverEnabled: true
            onPositionChanged: root.show()
            acceptedButtons: Qt.NoButton
        }

        Column {
            x: 40
            width: parent.width - 80
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 24
            spacing: 10

            Row {
                width: parent.width
                spacing: 14
                Text {
                    id: titleText
                    width: Math.min(implicitWidth, parent.width - buttonsRow.width - 30)
                    anchors.bottom: parent.bottom
                    text: Player.vod.title || ""
                    color: Theme.text
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width - titleText.width - buttonsRow.width - 30
                    anchors.baseline: titleText.baseline
                    text: Player.vod.subtitle || ""
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontLarge
                    elide: Text.ElideRight
                }
            }

            // Seek bar
            Item {
                id: bar
                width: parent.width
                height: 22
                readonly property real fraction: root.dur > 0 ? Math.max(0, Math.min(1, root.pos / root.dur)) : 0
                // Catch-up: the part of the show that hasn't aired yet.
                readonly property real aired: root.catchup && root.dur > 0 ? Math.max(0, Math.min(1, root.maxPos / root.dur)) : 1
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    height: barMouse.containsMouse ? 8 : 5
                    radius: height / 2
                    color: Theme.track
                    Rectangle {   // not aired yet
                        x: parent.width * bar.aired
                        width: parent.width - x
                        height: parent.height
                        radius: height / 2
                        color: "#20FFFFFF"
                        visible: bar.aired < 1
                    }
                    Rectangle {
                        width: parent.width * bar.fraction
                        height: parent.height
                        radius: height / 2
                        color: Theme.accent
                    }
                }
                Rectangle {   // knob
                    x: bar.width * bar.fraction - width / 2
                    anchors.verticalCenter: parent.verticalCenter
                    width: 16; height: 16; radius: 8
                    color: Theme.text
                    visible: root.dur > 0
                }
                Rectangle {   // hover time
                    visible: barMouse.containsMouse && root.dur > 0
                    x: Math.max(0, Math.min(bar.width - width, barMouse.mouseX - width / 2))
                    y: -34
                    width: hoverText.implicitWidth + 16
                    height: 26
                    radius: 4
                    color: Theme.panelSolid
                    Text {
                        id: hoverText
                        anchors.centerIn: parent
                        text: root.clock(root.dur * Math.max(0, Math.min(1, barMouse.mouseX / bar.width)))
                        color: Theme.text
                        font.pixelSize: Theme.fontSmall
                    }
                }
                MouseArea {
                    id: barMouse
                    anchors.fill: parent
                    anchors.topMargin: -8
                    anchors.bottomMargin: -8
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onPositionChanged: root.show()
                    onClicked: (m) => {
                        root.seekTo(root.dur * Math.max(0, Math.min(1, m.x / bar.width)))
                    }
                }
            }

            Item {
                width: parent.width
                height: 40
                id: controlsLine
                // Narrow window: the play controls move next to the time, and
                // the buttons use short names, rather than overlap.
                readonly property real centredX: (width - transport.implicitWidth) / 2
                readonly property bool crowded: centredX < timeText.implicitWidth + 16
                                                || centredX + transport.implicitWidth > width - buttonsRow.width - 16
                readonly property bool shortNames: timeText.implicitWidth + 16 + transport.implicitWidth + 16 + buttonsRow.fullWidth > width
                Text {
                    id: timeText
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.clock(root.pos) + "  /  " + (root.dur > 0 ? root.clock(root.dur) : "–:––")
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                }
                // Back / pause / forward (hover for the keys).
                Row {
                    id: transport
                    anchors.verticalCenter: parent.verticalCenter
                    x: controlsLine.crowded ? timeText.implicitWidth + 16 : controlsLine.centredX
                    spacing: 12
                    BarButton {
                        anchors.verticalCenter: parent.verticalCenter
                        icon: Icons.rewind; iconFilled: true
                        label: qsTr("Back 10 seconds"); shortcut: "←"; showLabel: false
                        onClicked: { root.seekBy(-10); root.show() }
                    }
                    BarButton {
                        anchors.verticalCenter: parent.verticalCenter
                        size: 52
                        accent: true
                        icon: root.video.paused ? Icons.play : Icons.pause; iconFilled: true
                        label: root.video.paused ? qsTr("Play") : qsTr("Pause"); shortcut: "Space"; showLabel: false
                        onClicked: { root.video.togglePause(); root.show() }
                    }
                    BarButton {
                        anchors.verticalCenter: parent.verticalCenter
                        icon: Icons.forward; iconFilled: true
                        label: qsTr("Forward 10 seconds"); shortcut: "→"; showLabel: false
                        onClicked: { root.seekBy(10); root.show() }
                    }
                }
                Row {
                    id: buttonsRow
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8
                    // Width with the long names (the short ones are used when it doesn't fit).
                    readonly property real fullWidth: {
                        let w = multiViewButton.width + castButton.width + 2 * spacing
                        for (let i = 0; i < buttonsRepeater.count; ++i) {
                            const b = buttonsRepeater.itemAt(i)
                            if (b) w += b.longWidth + (i > 0 ? spacing : 0)
                        }
                        return w
                    }
                    Repeater {
                        id: buttonsRepeater
                        model: [
                            { label: qsTr("Audio"), short: qsTr("Audio"), type: "audio" },
                            { label: qsTr("Subtitles"), short: qsTr("Subs"), type: "sub" },
                            { label: qsTr("Picture & sound"), short: qsTr("Picture"), type: "options" },
                        ].concat(Player.vod.sourceCount > 1 ? [{ label: qsTr("Source: %1").arg(Player.vod.sourceName || "?"), short: qsTr("Source"), type: "source" }] : []).concat(Player.vod.kind === "episode" ? [{ label: qsTr("Next ▸"), short: qsTr("Next ▸"), type: "next" }]
                               : root.catchup ? [{ label: qsTr("Live ⇥"), short: qsTr("Live ⇥"), type: "live" }] : [])
                        delegate: Rectangle {
                            readonly property real longWidth: longText.implicitWidth + 28
                            height: 36
                            width: btnText.implicitWidth + (controlsLine.shortNames ? 20 : 28)
                            radius: 18
                            color: btnMouse.containsMouse ? Theme.focus : "#40FFFFFF"
                            Text { id: longText; visible: false; text: modelData.label; font: btnText.font }
                            Text {
                                id: btnText
                                anchors.centerIn: parent
                                text: controlsLine.shortNames ? modelData.short : modelData.label
                                color: Theme.text
                                font.pixelSize: Theme.fontSmall + 1
                            }
                            MouseArea {
                                id: btnMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onPositionChanged: root.show()
                                onClicked: modelData.type === "next" ? root.nextRequested()
                                         : modelData.type === "live" ? root.liveRequested()
                                         : modelData.type === "options" ? root.optionsRequested()
                                         : modelData.type === "source" ? root.sourceRequested()
                                         : root.openTrackMenu(modelData.type, parent)
                            }
                        }
                    }
                    // The same as on live TV: multi-view and cast.
                    BarButton {
                        id: multiViewButton
                        anchors.verticalCenter: parent.verticalCenter
                        icon: Icons.multiview
                        label: qsTr("Multi-view (back to live TV)"); shortcut: Keymap.label("multiview"); showLabel: false
                        onClicked: root.multiViewRequested()
                    }
                    BarButton {
                        id: castButton
                        anchors.verticalCenter: parent.verticalCenter
                        icon: Icons.cast
                        label: Cast.active ? qsTr("Casting · stop or move it") : qsTr("Cast to a TV or Chromecast")
                        shortcut: Keymap.label("cast"); showLabel: false
                        active: Cast.active
                        onClicked: root.castRequested()
                    }
                }
            }
        }
    }

    RelayMenu {
        id: trackMenu
        property string type
        property var items: []
        onClosed: hideTimer.restart()
        Repeater {
            model: trackMenu.type === "sub" ? [{ id: -1, label: qsTr("Off"), selected: !trackMenu.items.some(t => t.selected) }].concat(trackMenu.items)
                                            : trackMenu.items
            delegate: RelayMenuItem {
                text: (modelData.selected ? "✓  " : "     ") + modelData.label
                onTriggered: root.video.setTrack(trackMenu.type, modelData.id)
            }
        }
    }
}

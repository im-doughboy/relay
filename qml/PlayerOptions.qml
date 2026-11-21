import QtQuick
import Relay.Core

// Picture and sound (O, or the screen button in the menu bar): picture shape,
// audio track and delay, subtitles, deinterlacing, speed (movies) and the
// sleep timer. ↑ ↓ pick a row, ← → (or the arrows with the mouse) change it.
FocusScope {
    id: root
    property var video
    property bool opened: false
    // Sleep timer: minutes left (0 = off), kept by Main.
    property int sleepMinutesLeft: 0
    property int sleepChoice: 0          // what it was set to
    signal closed()
    signal sleepRequested(int minutes)

    property var audioTracks: []
    property var subTracks: []
    property int current: 0

    readonly property bool movie: Player.vodActive && Player.vod.kind !== "catchup"
    readonly property var shapes: [
        { value: "auto", label: qsTr("Original") },
        { value: "16:9", label: qsTr("16:9") },
        { value: "4:3", label: qsTr("4:3") },
        { value: "fill", label: qsTr("Fill the window (crop)") },
        { value: "stretch", label: qsTr("Stretch") },
    ]
    readonly property var deinterlaceModes: [
        { value: "auto", label: qsTr("Automatic") },
        { value: "on", label: qsTr("Always") },
        { value: "off", label: qsTr("Off") },
    ]
    readonly property var speeds: [0.5, 0.75, 1, 1.25, 1.5, 2]
    readonly property var sleepChoices: [0, 15, 30, 45, 60, 90, 120]

    function shapeLabel(v) { const s = shapes.find(x => x.value === v); return s ? s.label : shapes[0].label }
    function refreshTracks() {
        audioTracks = video.tracks("audio")
        subTracks = video.tracks("sub")
    }
    function open() {
        refreshTracks()
        current = 0
        opened = true
        forceActiveFocus()
    }
    function close() {
        if (!opened) return
        opened = false
        closed()
    }
    function step(list, value, dir, key) {
        let i = list.findIndex(x => (key ? x[key] : x) === value)
        if (i < 0) i = 0
        return list[(i + dir + list.length) % list.length]
    }
    // Used by the Z key too.
    function nextShape(dir) {
        Settings.pictureShape = step(shapes, Settings.pictureShape, dir, "value").value
        return qsTr("Picture: %1").arg(shapeLabel(Settings.pictureShape))
    }
    function nextAudio(dir) {
        const list = video.tracks("audio")
        if (list.length < 2) return list.length ? qsTr("Only one audio track") : qsTr("No audio tracks")
        const cur = list.findIndex(t => t.selected)
        const next = list[(cur + dir + list.length) % list.length]
        video.setTrack("audio", next.id)
        refreshLater.restart()
        return qsTr("Audio: %1").arg(next.label)
    }
    function nextSub(dir) {
        const list = video.tracks("sub")
        if (!list.length) return qsTr("No subtitles")
        // off, 0, 1, … in a ring
        const cur = list.findIndex(t => t.selected)
        let next = cur + dir
        if (next < -1) next = list.length - 1
        if (next >= list.length) next = -1
        video.setTrack("sub", next < 0 ? -1 : list[next].id)
        refreshLater.restart()
        return next < 0 ? qsTr("Subtitles off") : qsTr("Subtitles: %1").arg(list[next].label)
    }
    function delayText(ms) {
        return ms === 0 ? qsTr("None") : ms > 0 ? qsTr("Sound %1 ms later").arg(ms) : qsTr("Sound %1 ms earlier").arg(-ms)
    }

    // The rows (those that apply to what's playing).
    readonly property var rows: {
        const a = audioTracks.find(t => t.selected)
        const s = subTracks.find(t => t.selected)
        const list = [
            { id: "shape", label: qsTr("Picture shape"), value: shapeLabel(Settings.pictureShape), key: "Z" },
            { id: "audio", label: qsTr("Audio track"), key: "A",
              value: a ? a.label : (audioTracks.length ? audioTracks[0].label : qsTr("None")),
              fixed: audioTracks.length < 2 },
            { id: "delay", label: qsTr("Lip-sync"), value: delayText(Settings.audioDelayMs) },
            { id: "sub", label: qsTr("Subtitles / captions"), key: "C",
              value: s ? s.label : (subTracks.length ? qsTr("Off") : qsTr("None on this channel")),
              fixed: subTracks.length === 0 },
            { id: "deint", label: qsTr("Deinterlacing"),
              value: (deinterlaceModes.find(x => x.value === Settings.deinterlace) || deinterlaceModes[0]).label },
        ]
        if (movie)
            list.push({ id: "speed", label: qsTr("Speed"), value: video.speed === 1 ? qsTr("Normal") : video.speed + "×" })
        list.push({ id: "sleep", label: qsTr("Sleep timer"),
                    value: sleepMinutesLeft > 0 ? qsTr("Stops in %1 min").arg(sleepMinutesLeft) : qsTr("Off") })
        return list
    }

    function change(id, dir) {
        switch (id) {
        case "shape": nextShape(dir); break
        case "audio": nextAudio(dir); break
        case "delay": Settings.audioDelayMs = Settings.audioDelayMs + dir * 50; break
        case "sub": nextSub(dir); break
        case "deint": Settings.deinterlace = step(deinterlaceModes, Settings.deinterlace, dir, "value").value; break
        case "speed": video.speed = step(speeds, video.speed, dir); break
        case "sleep": {
            sleepRequested(step(sleepChoices, sleepMinutesLeft > 0 ? sleepChoice : 0, dir))
            break
        }
        }
    }

    Timer { id: refreshLater; interval: 250; onTriggered: root.refreshTracks() }
    // Tracks can appear a moment after a channel starts.
    Timer { interval: 2000; repeat: true; running: root.opened; onTriggered: root.refreshTracks() }

    visible: opened
    anchors.fill: parent
    z: 22

    Keys.onPressed: (e) => {
        switch (e.key) {
        case Qt.Key_Up: current = (current - 1 + rows.length) % rows.length; break
        case Qt.Key_Down: current = (current + 1) % rows.length; break
        case Qt.Key_Left: change(rows[current].id, -1); break
        case Qt.Key_Right:
        case Qt.Key_Return:
        case Qt.Key_Enter:
        case Qt.Key_Space: change(rows[current].id, 1); break
        case Qt.Key_Escape:
        case Qt.Key_Back:
        case Qt.Key_Backspace:
        case Qt.Key_O: close(); break
        default: break
        }
        e.accepted = true
    }

    // Click outside closes.
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onClicked: root.close(); onWheel: (w) => w.accepted = true }

    Rectangle {
        id: panel
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Math.min(440, parent.width - 40)
        color: Theme.panel
        border.width: 1
        border.color: Theme.border
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }

        Row {
            x: 24; y: 22
            spacing: 12
            Icon { anchors.verticalCenter: parent.verticalCenter; width: 22; height: 22; path: Icons.options; color: Theme.accent }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Picture and sound")
                color: Theme.text
                font.pixelSize: Theme.fontLarge
                font.weight: Font.DemiBold
            }
        }
        CloseButton {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 12
            onClicked: root.close()
        }

        Column {
            x: 12
            y: 76
            width: parent.width - 24
            spacing: 4
            Repeater {
                model: root.rows
                delegate: Rectangle {
                    id: rowItem
                    readonly property int rowIndex: index
                    readonly property string rowId: modelData.id
                    readonly property bool sel: index === root.current
                    width: parent.width
                    height: 68
                    radius: 8
                    color: sel ? Theme.focus : (rowMouse.containsMouse ? Theme.raised : "transparent")
                    border.width: sel ? 2 : 0
                    border.color: Theme.accent
                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: { root.current = index; if (!modelData.fixed) root.change(modelData.id, 1) }
                    }
                    Row {
                        x: 14; y: 10
                        spacing: 8
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.label
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSmall + 1
                        }
                        KeyCap { anchors.verticalCenter: parent.verticalCenter; visible: !!modelData.key; text: modelData.key || "" }
                    }
                    Text {
                        x: 14
                        y: 34
                        width: parent.width - 28 - 84
                        text: modelData.value
                        color: modelData.fixed ? Theme.textMuted : Theme.text
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                    }
                    Row {
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        visible: !modelData.fixed
                        Repeater {
                            model: [-1, 1]
                            delegate: Rectangle {
                                width: 36; height: 36; radius: 18
                                color: arrowMouse.pressed ? Theme.focus : (arrowMouse.containsMouse ? "#2EFFFFFF" : "transparent")
                                Icon {
                                    anchors.centerIn: parent
                                    width: 16; height: 16
                                    path: Icons.back
                                    rotation: modelData > 0 ? 180 : 0
                                    color: Theme.text
                                }
                                MouseArea {
                                    id: arrowMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: { root.current = rowItem.rowIndex; root.change(rowItem.rowId, modelData) }
                                }
                            }
                        }
                    }
                }
            }
        }

        Text {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 20
            wrapMode: Text.Wrap
            text: qsTr("↑ ↓ choose  ·  ← → change  ·  Esc close\nPicture shape, lip-sync and deinterlacing are remembered.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            lineHeight: 1.25
        }
    }
}

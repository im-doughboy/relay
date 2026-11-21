import QtQuick
import QtQuick.Controls

// Every keyboard shortcut, by screen (H, ?, F1, or the ? button in the menu
// bar). Esc, H, a click outside or the ✕ closes it.
FocusScope {
    id: root
    property bool opened: false
    signal closed()

    function open() { opened = true; flick.contentY = 0; forceActiveFocus() }
    function close() {
        if (!opened) return
        opened = false
        closed()
    }

    visible: opened
    anchors.fill: parent
    z: 25

    Keys.onPressed: (e) => {
        if (e.key === Keymap.keyOf("help")) { root.close(); e.accepted = true; return }
        switch (e.key) {
        case Qt.Key_Escape:
        case Qt.Key_Back:
        case Qt.Key_H:
        case Qt.Key_Question:
        case Qt.Key_F1:
        case Qt.Key_Backspace:
            root.close(); break
        case Qt.Key_Down: flick.flick(0, -900); break
        case Qt.Key_Up: flick.flick(0, 900); break
        case Qt.Key_PageDown: flick.contentY = Math.min(flick.contentHeight - flick.height, flick.contentY + flick.height * 0.8); break
        case Qt.Key_PageUp: flick.contentY = Math.max(0, flick.contentY - flick.height * 0.8); break
        default: return
        }
        e.accepted = true
    }

    readonly property var sections: [
        { title: qsTr("Watching live TV"), rows: [
            ["↑  ↓", qsTr("Previous / next channel")],
            ["0–9", qsTr("Go to a channel number")],
            ["Enter  ←", qsTr("Channel list")],
            ["Backspace  " + Keymap.label("last"), qsTr("Last channel watched")],
            [Keymap.label("info") + "  →", qsTr("What's on (again: more details)")],
            [Keymap.label("pause"), qsTr("Pause / play")],
            ["Shift+←  →", qsTr("Back / forward 10 seconds")],
            ["End", qsTr("Back to live")],
            [Keymap.label("fromStart"), qsTr("Watch the show from the start")],
            ["Ctrl+R", qsTr("Record the show on now")],
            [Keymap.label("captions"), qsTr("Captions / subtitles")],
            [Keymap.label("audio"), qsTr("Audio track (language)")],
            [Keymap.label("shape"), qsTr("Picture shape")],
            [Keymap.label("options"), qsTr("Picture & sound, lip-sync, sleep timer")],
            [Keymap.label("external"), qsTr("Open in the external player (Settings)")],
            [Keymap.label("cast"), qsTr("Cast to a TV or Chromecast")],
            [Keymap.label("mute") + "   +  −", qsTr("Mute · volume")],
            [Keymap.label("fullscreen") + "  F11", qsTr("Full screen")],
            [Keymap.label("retry"), qsTr("Reconnect the channel")],
        ]},
        { title: qsTr("Go to"), rows: [
            [Keymap.label("guide"), qsTr("TV guide")],
            [Keymap.label("movies"), qsTr("Movies, series and recordings")],
            [Keymap.label("search") + "  Ctrl+F", qsTr("Search channels and shows")],
            [Keymap.label("multiview"), qsTr("Multi-view (up to four channels)")],
            [Keymap.label("pip"), qsTr("Picture-in-picture")],
            [Keymap.label("swap") + "  Shift+W", qsTr("Swap · move picture-in-picture")],
            [Keymap.label("playlist"), qsTr("Switch playlist")],
            [Keymap.label("settings"), qsTr("Settings (also: change these keys)")],
            [Keymap.label("help") + "  ?", qsTr("This list")],
            ["Esc", qsTr("Close · go back")],
        ]},
        { title: qsTr("TV guide"), rows: [
            ["Arrows", qsTr("Move around")],
            ["Page Up / Down", qsTr("A page up / down")],
            ["Home", qsTr("Back to now")],
            ["Enter", qsTr("Watch (past shows: catch-up)")],
            ["B", qsTr("Watch from the start")],
            ["R", qsTr("Record")],
            ["Shift+R", qsTr("Record every episode (series)")],
            ["M", qsTr("Remind me")],
            ["F  H", qsTr("Favorite · hide the channel")],
        ]},
        { title: qsTr("Channel list"), rows: [
            ["Enter", qsTr("Watch")],
            ["F", qsTr("Favorite")],
            ["Shift+F", qsTr("Your own lists: add to, remove from")],
            ["N", qsTr("New list (in the groups column)")],
            ["E", qsTr("Rename")],
            ["H", qsTr("Hide")],
            ["K", qsTr("Lock with the PIN")],
            ["U", qsTr("Guide listings: pick them by hand, move their times")],
            ["Ctrl+↑  ↓", qsTr("Move the channel or group")],
            [qsTr("Right-click"), qsTr("All of the above")],
        ]},
        { title: qsTr("Movies and series"), rows: [
            ["Tab", qsTr("Movies · series · recordings")],
            ["O", qsTr("Sort")],
            ["F", qsTr("Favorite")],
            ["Delete", qsTr("Remove from Continue watching")],
            ["W", qsTr("Episode watched / not watched")],
        ]},
        { title: qsTr("Playing a movie or episode"), rows: [
            ["←  →", qsTr("Back / forward 10 seconds")],
            ["↓  ↑", qsTr("Back / forward 1 minute")],
            ["Page Down / Up", qsTr("Back / forward 10 minutes")],
            ["Home", qsTr("Start over")],
            ["Space  K", qsTr("Pause / play")],
            ["A  C", qsTr("Audio · subtitles")],
            ["Z  O", qsTr("Picture shape · picture and sound, speed")],
            ["N", qsTr("Next episode")],
            ["Esc", qsTr("Stop and go back")],
        ]},
        { title: qsTr("Multi-view"), rows: [
            ["Arrows", qsTr("Select a tile")],
            ["Enter", qsTr("Change its channel")],
            ["Insert  Delete", qsTr("Add · remove a channel")],
            ["Page Up / Down", qsTr("Next / previous channel in the tile")],
            ["L", qsTr("Layout")],
            ["F  Esc", qsTr("Watch the selected one full screen")],
        ]},
    ]

    Rectangle {
        anchors.fill: parent
        color: "#B3000000"
        MouseArea { anchors.fill: parent; onClicked: root.close(); onWheel: (w) => w.accepted = true }
    }

    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 1040)
        height: Math.min(parent.height - 48, header.height + flow.implicitHeight + 56)
        radius: 14
        color: Theme.panelSolid
        border.width: 1
        border.color: Theme.border
        MouseArea { anchors.fill: parent; onWheel: (w) => { w.accepted = false } }

        Item {
            id: header
            width: parent.width
            height: mouseTips.y + mouseTips.height + 16
            Row {
                x: 28; y: 24
                spacing: 12
                Icon { anchors.verticalCenter: parent.verticalCenter; width: 24; height: 24; path: Icons.keyboard; color: Theme.accent }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Keyboard shortcuts")
                    color: Theme.text
                    font.pixelSize: Theme.fontLarge + 2
                    font.weight: Font.DemiBold
                }
            }
            Text {
                id: mouseTips
                x: 28; y: 62
                width: parent.width - 56
                wrapMode: Text.Wrap
                text: qsTr("With the mouse: move it over the picture for the menu bar · click the picture for the channel list · double-click for full screen · right-click for what's on · scroll for volume")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                lineHeight: 1.2
            }
            CloseButton {
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 16
                onClicked: root.close()
            }
        }

        Flickable {
            id: flick
            anchors.top: header.bottom
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 12
            anchors.topMargin: 4
            clip: true
            contentHeight: flow.implicitHeight + 16
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: flick.contentHeight > flick.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff }

            Flow {
                id: flow
                x: 16
                width: flick.width - 32
                spacing: 28
                // Two columns when there's room, one when there isn't.
                readonly property real colWidth: width >= 820 ? (width - spacing) / 2 : width
                Repeater {
                    model: root.sections
                    delegate: Column {
                        width: flow.colWidth
                        spacing: 6
                        Text {
                            text: modelData.title
                            color: Theme.accent
                            font.pixelSize: Theme.fontSmall + 1
                            font.weight: Font.DemiBold
                            font.capitalization: Font.AllUppercase
                            font.letterSpacing: 0.8
                            bottomPadding: 4
                        }
                        Repeater {
                            model: modelData.rows
                            delegate: Item {
                                width: flow.colWidth
                                height: 28
                                Row {
                                    width: 150
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 4
                                    Repeater {
                                        // "Shift+←  →" → keys "Shift+←" and "→"
                                        model: modelData[0].split(/\s{2,}/)
                                        delegate: KeyCap { text: modelData }
                                    }
                                }
                                Text {
                                    x: 158
                                    width: parent.width - 158
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: modelData[1]
                                    color: Theme.text
                                    font.pixelSize: Theme.fontBody - 1
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

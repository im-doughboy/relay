import QtQuick
import Relay.Core

// Quick playlist switcher (P). Enter switches, Esc closes.
FocusScope {
    id: root
    property bool opened: false
    signal closed()
    signal manageRequested()

    function open() {
        const list = Settings.playlists
        let active = 0
        for (let i = 0; i < list.length; ++i) if (list[i].active) active = i
        view.currentIndex = active
        opened = true
        view.forceActiveFocus()
    }
    function close() {
        if (!opened) return
        opened = false
        closed()
    }
    function choose(i) {
        const p = Settings.playlists[i]
        if (!p) return
        close()
        if (!p.active) Session.switchPlaylist(p.id)
    }

    visible: opened
    anchors.fill: parent

    Rectangle {
        anchors.fill: parent
        color: "#A0000000"
        MouseArea { anchors.fill: parent; onClicked: root.close(); onWheel: (w) => w.accepted = true }
    }
    Rectangle {
        anchors.centerIn: parent
        width: Math.min(520, parent.width - 64)
        height: Math.min(parent.height - 80, 120 + view.count * 64)
        radius: 12
        color: Theme.panelSolid
        border.width: 1
        border.color: Theme.border
        MouseArea { anchors.fill: parent }

        Text {
            x: 22; y: 18
            text: qsTr("Playlists")
            color: Theme.text
            font.pixelSize: Theme.fontLarge
            font.weight: Font.DemiBold
        }
        ListView {
            id: view
            x: 10; y: 60
            width: parent.width - 20
            height: parent.height - 104
            clip: true
            model: Settings.playlists
            keyNavigationEnabled: false
            highlightMoveDuration: 0
            Keys.onPressed: (e) => {
                switch (e.key) {
                case Qt.Key_Up: if (currentIndex > 0) currentIndex--; break
                case Qt.Key_Down: if (currentIndex < count - 1) currentIndex++; break
                case Qt.Key_Return:
                case Qt.Key_Enter: root.choose(currentIndex); break
                case Qt.Key_S: root.close(); root.manageRequested(); break
                case Qt.Key_Escape:
                case Qt.Key_P:
                case Qt.Key_Back: root.close(); break
                default:
                    if (e.key >= Qt.Key_1 && e.key <= Qt.Key_9) { root.choose(e.key - Qt.Key_1); break }
                    return
                }
                e.accepted = true
            }
            delegate: Rectangle {
                readonly property bool sel: ListView.isCurrentItem
                width: view.width
                height: 60
                radius: 8
                color: sel ? Theme.focus : (rowMouse.containsMouse ? Theme.raised : "transparent")
                border.width: sel ? 2 : 0
                border.color: Theme.accent
                Text {
                    id: num
                    x: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: index < 9 ? String(index + 1) : ""
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontBody
                }
                Column {
                    x: 44
                    width: parent.width - 60
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    Text {
                        width: parent.width
                        text: modelData.name
                        color: Theme.text
                        font.pixelSize: Theme.fontBody + 1
                        font.weight: modelData.active ? Font.DemiBold : Font.Normal
                        elide: Text.ElideRight
                    }
                    Text {
                        text: modelData.active ? qsTr("Watching now") + "  ·  " + modelData.detail : modelData.detail
                        color: modelData.active ? Theme.accent : Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }
                }
                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: root.choose(index)
                }
            }
        }
        Text {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 14
            text: qsTr("Enter switch  ·  1–9 quick pick  ·  S manage in Settings  ·  Esc close")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
        }
    }
}

import QtQuick
import Relay.Core

// Where a movie or episode plays from, when several playlists have it:
// ↑ ↓ choose, Enter plays from there, Esc closes. Marks the best one, the
// one playing, one that didn't play, and accounts busy (recording).
FocusScope {
    id: root
    property bool opened: false
    property string title
    property var list: []
    property int current: -1        // playing now
    property int failed: -1         // didn't play
    property int at: 0
    signal chosen(int index)
    signal dismissed()

    function open(sources, heading, playing, didntPlay) {
        list = sources || []
        title = heading
        current = playing === undefined ? -1 : playing
        failed = didntPlay === undefined ? -1 : didntPlay
        // Start on the best one that isn't the failed or busy one.
        at = 0
        let pick = -1
        for (let i = 0; i < list.length; ++i)
            if (i !== failed && i !== current && !list[i].busy && (pick < 0 || list[i].best)) pick = i
        if (pick < 0) for (let i = 0; i < list.length; ++i) if (i !== failed && i !== current) { pick = i; break }
        at = Math.max(0, pick)
        opened = true
        forceActiveFocus()
    }
    function close() {
        if (!opened) return
        opened = false
        dismissed()
    }
    function describe(s, i) {
        const parts = []
        if (s.quality) parts.push(s.quality)
        if (s.best) parts.push(qsTr("best"))
        if (i === root.current) parts.push(qsTr("playing"))
        if (i === root.failed) parts.push(qsTr("didn't play"))
        if (s.busy) parts.push(qsTr("busy: recording"))
        return parts.join("  ·  ")
    }

    visible: opened
    anchors.fill: parent
    z: 90
    Keys.onPressed: (e) => {
        switch (e.key) {
        case Qt.Key_Up: at = Math.max(0, at - 1); break
        case Qt.Key_Down: at = Math.min(list.length - 1, at + 1); break
        case Qt.Key_Return:
        case Qt.Key_Enter:
        case Qt.Key_Select: { const i = at; opened = false; chosen(i); break }
        case Qt.Key_Escape:
        case Qt.Key_Back:
        case Qt.Key_Backspace: close(); break
        default: return
        }
        e.accepted = true
    }

    Rectangle {
        anchors.fill: parent
        color: "#B0000000"
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onClicked: root.close(); onWheel: (w) => w.accepted = true }
    }
    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 560)
        height: content.implicitHeight + 48
        radius: 16
        color: Theme.panelSolid
        border.width: 1
        border.color: Theme.border
        MouseArea { anchors.fill: parent }
        Column {
            id: content
            x: 24; y: 24
            width: parent.width - 48
            spacing: 6
            Text {
                width: parent.width
                text: root.title
                color: Theme.text
                font.pixelSize: Theme.fontLarge
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
            }
            Item { width: 1; height: 8 }
            Repeater {
                model: root.list
                delegate: Item {
                    id: row
                    required property var modelData
                    required property int index
                    readonly property bool lit: root.at === index
                    width: content.width
                    height: 58
                    Rectangle {
                        anchors.fill: parent
                        radius: height / 2
                        color: row.lit ? Theme.text : (rowMouse.containsMouse ? "#22FFFFFF" : "transparent")
                    }
                    Column {
                        x: 22
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 44
                        spacing: 2
                        Text {
                            width: parent.width
                            text: row.modelData.name || qsTr("Source %1").arg(row.index + 1)
                            color: row.lit ? Theme.background : Theme.text
                            font.pixelSize: Theme.fontBody
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            visible: text.length > 0
                            text: root.describe(row.modelData, row.index)
                            color: row.lit ? "#5A000000" : (row.index === root.failed || row.modelData.busy ? Theme.danger : Theme.textSecondary)
                            font.pixelSize: Theme.fontSmall
                            elide: Text.ElideRight
                        }
                    }
                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { root.opened = false; root.chosen(row.index) }
                    }
                }
            }
            Item { width: 1; height: 6 }
            Text {
                text: qsTr("↑ ↓ choose   ·   Enter: play from there   ·   Esc: close")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
            }
        }
    }
}

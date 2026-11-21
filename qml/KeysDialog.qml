import QtQuick
import QtQuick.Controls.Basic
import Relay.Core

// Settings → Keys: change the keys used while watching live TV.
// ↑ ↓ pick an action, Enter then the new key; Delete puts the usual key back.
FocusScope {
    id: root
    property bool opened: false
    property int capturing: -1          // row waiting for a key
    property string note
    signal finished()

    function open() {
        opened = true
        capturing = -1
        note = ""
        list.currentIndex = 0
        list.forceActiveFocus()
    }
    function close() {
        if (!opened) return
        opened = false
        capturing = -1
        finished()
    }
    // Esc: stop waiting for a key, or close.
    function back() {
        if (capturing >= 0) { capturing = -1; note = "" }
        else close()
    }
    function assign(row, key) {
        const a = Keymap.actions[row]
        if (Keymap.reserved(key)) { note = qsTr("%1 already does something else while watching. Pick another key.").arg(Keymap.keyName(key)); return }
        const other = Keymap.userOf(key, a.id)
        if (other.length) { note = qsTr("%1 is already used for “%2”. Change that one first, or pick another key.").arg(Keymap.keyName(key)).arg(other); return }
        Settings.setKey(a.id, key === a.key ? 0 : key)
        note = qsTr("%1: %2").arg(a.label).arg(Keymap.keyName(key))
        capturing = -1
    }
    function restore(row) {
        const a = Keymap.actions[row]
        const other = Keymap.userOf(a.key, a.id)
        if (other.length) { note = qsTr("%1 is now used for “%2”. Change that one first.").arg(Keymap.keyName(a.key)).arg(other); return }
        Settings.setKey(a.id, 0)
        note = qsTr("%1: back to %2").arg(a.label).arg(Keymap.keyName(a.key))
    }

    visible: opened
    anchors.fill: parent
    z: 30

    Rectangle {
        anchors.fill: parent
        color: "#B3000000"
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onClicked: root.close(); onWheel: (w) => w.accepted = true }
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(620, parent.width - 48)
        height: Math.min(parent.height - 48, 720)
        radius: 14
        color: Theme.panelSolid
        border.width: 1
        border.color: Theme.border
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }

        Text {
            id: title
            x: 28; y: 24
            text: qsTr("Keys while watching")
            color: Theme.text
            font.pixelSize: Theme.fontLarge + 2
            font.weight: Font.DemiBold
        }
        CloseButton { anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 16; onClicked: root.close() }
        Text {
            id: help
            x: 28
            anchors.top: title.bottom
            anchors.topMargin: 6
            width: parent.width - 56
            wrapMode: Text.Wrap
            text: root.capturing >= 0 ? qsTr("Press the new key for “%1” · Esc to keep the current one").arg(Keymap.actions[root.capturing].label)
                : root.note.length ? root.note
                : qsTr("Enter (or a click) then the new key · Delete puts the usual key back. Arrows, Enter, Esc, numbers and volume keys stay as they are.")
            color: root.capturing >= 0 ? Theme.accent : Theme.textMuted
            font.pixelSize: Theme.fontSmall
            lineHeight: 1.2
        }

        ListView {
            id: list
            anchors.top: help.bottom
            anchors.topMargin: 12
            anchors.bottom: footer.top
            anchors.bottomMargin: 12
            x: 16
            width: parent.width - 32
            clip: true
            model: Keymap.actions
            boundsBehavior: Flickable.StopAtBounds
            keyNavigationEnabled: false
            highlightMoveDuration: 0
            ScrollBar.vertical: ScrollBar { policy: list.contentHeight > list.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff }

            Keys.onPressed: (e) => {
                e.accepted = true
                if (root.capturing >= 0) {
                    if (e.key === Qt.Key_Escape) { root.capturing = -1; root.note = ""; return }
                    if ([Qt.Key_Shift, Qt.Key_Control, Qt.Key_Alt, Qt.Key_Meta, Qt.Key_AltGr].indexOf(e.key) >= 0) return
                    root.assign(root.capturing, e.key)
                    return
                }
                switch (e.key) {
                case Qt.Key_Up: if (currentIndex > 0) currentIndex--; break
                case Qt.Key_Down: if (currentIndex < count - 1) currentIndex++; break
                case Qt.Key_PageUp: currentIndex = Math.max(0, currentIndex - 8); break
                case Qt.Key_PageDown: currentIndex = Math.min(count - 1, currentIndex + 8); break
                case Qt.Key_Return:
                case Qt.Key_Enter: root.note = ""; root.capturing = currentIndex; break
                case Qt.Key_Delete: root.restore(currentIndex); break
                case Qt.Key_Escape:
                case Qt.Key_Backspace: root.close(); break
                default: e.accepted = false; return
                }
                positionViewAtIndex(currentIndex, ListView.Contain)
            }

            delegate: Item {
                width: list.width - 12
                height: 44
                readonly property bool sel: ListView.isCurrentItem
                readonly property bool changed: Keymap.custom[modelData.id] !== undefined
                Rectangle {
                    anchors.fill: parent
                    radius: 6
                    color: root.capturing === index ? "#33F2A33A" : sel ? Theme.focus : (rowMouse.containsMouse ? Theme.raised : "transparent")
                    border.width: root.capturing === index ? 1 : 0
                    border.color: Theme.accent
                }
                Text {
                    x: 14
                    width: parent.width - 200
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.label
                    color: Theme.text
                    font.pixelSize: Theme.fontBody - 1
                    elide: Text.ElideRight
                }
                Row {
                    anchors.right: parent.right
                    anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: changed && root.capturing !== index
                        text: qsTr("was %1").arg(Keymap.keyName(modelData.key))
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: root.capturing === index
                        text: qsTr("press a key…")
                        color: Theme.accent
                        font.pixelSize: Theme.fontSmall
                    }
                    KeyCap {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: root.capturing !== index
                        text: Keymap.keyName(Keymap.custom[modelData.id] || modelData.key)
                    }
                }
                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { list.currentIndex = index; list.forceActiveFocus(); root.note = ""; root.capturing = index }
                }
            }
        }

        Row {
            id: footer
            x: 28
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 20
            spacing: 10
            ActionButton {
                text: qsTr("Put all keys back")
                enabled: Object.keys(Keymap.custom).length > 0
                onClicked: { Settings.resetKeys(); root.capturing = -1; root.note = qsTr("All keys are back to the usual ones"); list.forceActiveFocus() }
            }
            ActionButton { text: qsTr("Done"); primary: true; onClicked: root.close() }
        }
    }
}

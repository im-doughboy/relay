import QtQuick
import Relay.Core

// "Add to a list…" for a channel: tick the favorites lists it belongs to, or
// make a new list. ↑ ↓ move, Enter / Space ticks, N starts a new list, Esc closes.
FocusScope {
    id: root
    property bool opened: false
    property int channel: -1               // global index
    property string channelName
    property var lists: []                 // [{id, name, member}]
    property bool naming: false
    signal finished()
    signal message(string text)

    function open(globalIndex, name) {
        channel = globalIndex
        channelName = name
        naming = false
        reload()
        opened = true
        list.currentIndex = 0
        if (lists.length === 0) startNaming()
        else list.forceActiveFocus()
    }
    function close() {
        if (!opened) return
        opened = false
        finished()
    }
    function reload() { lists = Channels.listsFor(channel) }
    function toggle(i) {
        if (i === lists.length) { startNaming(); return }
        const l = lists[i]
        if (!l) return
        Channels.setInList(channel, l.id, !l.member)
        message(l.member ? qsTr("Removed from %1").arg(l.name) : qsTr("Added to %1").arg(l.name))
        reload()
        list.currentIndex = i
    }
    function startNaming() {
        naming = true
        input.text = ""
        input.field.forceActiveFocus()
    }
    function createList() {
        const name = input.text.trim()
        if (!name.length) { cancelNaming(); return }
        const group = Channels.createList(name)
        if (group < 0) return
        Channels.setInList(channel, Channels.groupName(group), true)
        message(qsTr("%1 added to the new list %2").arg(channelName).arg(name))
        naming = false
        reload()
        list.currentIndex = lists.length - 1
        list.forceActiveFocus()
    }
    function cancelNaming() {
        if (lists.length === 0) { close(); return }
        naming = false
        list.forceActiveFocus()
    }

    visible: opened
    anchors.fill: parent
    Keys.onEscapePressed: naming ? cancelNaming() : close()

    Rectangle {
        anchors.fill: parent
        color: "#A0000000"
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onClicked: root.close(); onWheel: (w) => w.accepted = true }
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(480, parent.width - 48)
        height: col.implicitHeight + 48
        radius: 12
        color: Theme.panelSolid
        border.width: 1
        border.color: Theme.border
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }

        Column {
            id: col
            x: 24; y: 24
            width: parent.width - 48
            spacing: 14

            Item {
                width: parent.width
                height: 44
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 56
                    spacing: 2
                    Text {
                        width: parent.width
                        text: qsTr("Favorites lists")
                        color: Theme.text
                        font.pixelSize: Theme.fontLarge
                        font.weight: Font.DemiBold
                    }
                    Text {
                        width: parent.width
                        text: root.channelName
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall + 1
                        elide: Text.ElideRight
                    }
                }
                CloseButton { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; onClicked: root.close() }
            }

            ListView {
                id: list
                width: parent.width
                height: Math.min(contentHeight, 44 * 7)
                clip: true
                model: root.lists.length + 1
                boundsBehavior: Flickable.StopAtBounds
                highlightMoveDuration: 0
                Keys.onReturnPressed: root.toggle(currentIndex)
                Keys.onEnterPressed: root.toggle(currentIndex)
                Keys.onSpacePressed: root.toggle(currentIndex)
                Keys.onPressed: (e) => {
                    if (e.key === Qt.Key_N) { root.startNaming(); e.accepted = true }
                    else if (e.key === Qt.Key_Backspace) { root.close(); e.accepted = true }
                }
                delegate: Item {
                    width: list.width
                    height: 44
                    readonly property bool isNew: index === root.lists.length
                    readonly property var entry: isNew ? null : root.lists[index]
                    readonly property bool selected: ListView.isCurrentItem && list.activeFocus
                    Rectangle {
                        anchors.fill: parent
                        radius: 6
                        color: selected ? Theme.focus : (rowMouse.containsMouse ? Theme.raised : "transparent")
                    }
                    Rectangle {   // tick box
                        visible: !isNew
                        x: 12
                        anchors.verticalCenter: parent.verticalCenter
                        width: 20; height: 20; radius: 5
                        color: entry && entry.member ? Theme.accent : "transparent"
                        border.width: entry && entry.member ? 0 : 2
                        border.color: Theme.textMuted
                        Text {
                            anchors.centerIn: parent
                            visible: entry ? entry.member : false
                            text: "✓"
                            color: Theme.accentText
                            font.pixelSize: 14
                            font.weight: Font.Bold
                        }
                    }
                    Icon {
                        visible: isNew
                        x: 13
                        anchors.verticalCenter: parent.verticalCenter
                        width: 18; height: 18
                        path: Icons.plus
                        color: Theme.accent
                    }
                    Text {
                        x: 46
                        width: parent.width - 58
                        anchors.verticalCenter: parent.verticalCenter
                        text: isNew ? qsTr("New list…") : entry.name
                        color: isNew ? Theme.accent : Theme.text
                        font.pixelSize: Theme.fontBody
                        elide: Text.ElideRight
                    }
                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { list.currentIndex = index; list.forceActiveFocus(); root.toggle(index) }
                    }
                }
            }

            FieldInput {
                id: input
                visible: root.naming
                width: parent.width
                label: qsTr("Name of the new list")
                placeholder: qsTr("Sports, Kids, News…")
                onAccepted: root.createList()
            }
            Row {
                visible: root.naming
                spacing: 10
                ActionButton { text: qsTr("Create list"); primary: true; onClicked: root.createList() }
                ActionButton { text: qsTr("Cancel"); onClicked: root.cancelNaming() }
            }
            Text {
                visible: !root.naming
                width: parent.width
                wrapMode: Text.Wrap
                text: qsTr("Lists appear as groups under All channels. Rename, move or delete a list from the groups column.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
            }
        }
    }
}

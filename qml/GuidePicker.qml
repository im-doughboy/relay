import QtQuick
import Relay.Core

// "Guide listings…" for a channel: pick which guide channel its listings come
// from when the automatic match is wrong or missing, and move its listings in
// time. Type to filter, ↑ ↓ and Enter pick, Ctrl+← / → shift the time.
FocusScope {
    id: root
    property bool opened: false
    property int channel: -1               // global index
    property string channelName
    property string choice                 // "", "-" or a guide channel id
    property int offset: 0                 // minutes
    property var results: []
    signal finished()
    signal message(string text)

    function open(globalIndex, name) {
        channel = globalIndex
        channelName = name
        choice = Session.guideChoice(globalIndex)
        offset = Session.guideOffset(globalIndex)
        input.text = ""
        search()
        opened = true
        list.currentIndex = Math.max(0, results.findIndex(r => r.id === choice))
        input.field.forceActiveFocus()
    }
    function close() {
        if (!opened) return
        opened = false
        finished()
    }
    function search() {
        const fixed = [
            { id: "", name: qsTr("Automatic"), detail: qsTr("Match by the playlist's guide id or the channel's name") },
            { id: "-", name: qsTr("No listings"), detail: qsTr("Show this channel without a guide") },
        ]
        const found = Epg.guideChannels(input.text, 200).map(c => ({ id: c.id, name: c.name, detail: c.id }))
        results = (input.text.trim().length ? [] : fixed).concat(found)
        list.currentIndex = 0
    }
    function pick(i) {
        const r = results[i]
        if (!r) return
        Session.setGuideChoice(channel, r.id)
        message(r.id === "" ? qsTr("Guide listings: automatic")
              : r.id === "-" ? qsTr("Guide listings turned off for %1").arg(channelName)
              : qsTr("%1 now uses the listings of %2").arg(channelName).arg(r.name))
        close()
    }
    function shift(delta) {
        offset = Math.max(-720, Math.min(720, offset + delta))
        Session.setGuideOffset(channel, offset)
    }
    function offsetText(m) {
        if (m === 0) return qsTr("Not moved")
        const h = Math.floor(Math.abs(m) / 60), min = Math.abs(m) % 60
        const t = (h ? h + " h" : "") + (h && min ? " " : "") + (min ? min + " min" : "")
        return m > 0 ? qsTr("%1 later").arg(t) : qsTr("%1 earlier").arg(t)
    }

    visible: opened
    anchors.fill: parent
    Keys.onEscapePressed: close()

    Rectangle {
        anchors.fill: parent
        color: "#A0000000"
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onClicked: root.close(); onWheel: (w) => w.accepted = true }
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(560, parent.width - 48)
        height: Math.min(parent.height - 64, 640)
        radius: 12
        color: Theme.panelSolid
        border.width: 1
        border.color: Theme.border
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = true }

        Text {
            id: heading
            x: 24; y: 20
            width: parent.width - 100
            text: qsTr("Guide listings for %1").arg(root.channelName)
            color: Theme.text
            font.pixelSize: Theme.fontLarge
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
        CloseButton {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 12
            onClicked: root.close()
        }

        FieldInput {
            id: input
            x: 24
            y: 64
            width: parent.width - 48
            label: qsTr("Find a guide channel")
            placeholder: qsTr("Type a name, e.g. bbc one")
            onTextChanged: if (root.opened) root.search()
            onAccepted: root.pick(list.currentIndex)
            // Ctrl+← → move the listings (the text box would move by a word).
            onKeyPressed: (e) => {
                if (!(e.modifiers & Qt.ControlModifier)) return
                if (e.key === Qt.Key_Left) { root.shift(-30); e.accepted = true }
                else if (e.key === Qt.Key_Right) { root.shift(30); e.accepted = true }
            }
            Keys.onPressed: (e) => {
                if (e.key === Qt.Key_Down) { list.currentIndex = Math.min(list.count - 1, list.currentIndex + 1); e.accepted = true }
                else if (e.key === Qt.Key_Up) { list.currentIndex = Math.max(0, list.currentIndex - 1); e.accepted = true }
                else if (e.key === Qt.Key_PageDown) { list.currentIndex = Math.min(list.count - 1, list.currentIndex + 8); e.accepted = true }
                else if (e.key === Qt.Key_PageUp) { list.currentIndex = Math.max(0, list.currentIndex - 8); e.accepted = true }
            }
        }

        ListView {
            id: list
            x: 12
            y: 146
            width: parent.width - 24
            height: parent.height - y - 76
            clip: true
            model: root.results
            highlightMoveDuration: 0
            delegate: Rectangle {
                readonly property bool sel: ListView.isCurrentItem
                width: list.width
                height: 52
                radius: 8
                color: sel ? Theme.focus : (rowMouse.containsMouse ? Theme.raised : "transparent")
                border.width: sel ? 2 : 0
                border.color: Theme.accent
                Column {
                    x: 14
                    width: parent.width - 60
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 1
                    Text {
                        width: parent.width
                        text: modelData.name
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        visible: modelData.detail !== modelData.name
                        text: modelData.detail
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        elide: Text.ElideRight
                    }
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    visible: modelData.id === root.choice
                    text: "✓"
                    color: Theme.accent
                    font.pixelSize: Theme.fontBody + 2
                }
                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: root.pick(index)
                }
            }
            Text {
                anchors.centerIn: parent
                width: parent.width - 60
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                visible: list.count === 0 || (list.count === 2 && !input.text.trim().length)
                y: 140
                text: input.text.trim().length ? qsTr("No guide channel matches that")
                      : qsTr("The list of guide channels fills in the next time the guide updates (Settings → Update guide).")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall + 1
            }
        }

        // Time shift for this channel's listings.
        Row {
            x: 24
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 18
            spacing: 10
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Listings shown:")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall + 1
            }
            BarButton { size: 36; label: qsTr("30 min earlier"); shortcut: "Ctrl+←"; icon: Icons.back; showLabel: false; onClicked: root.shift(-30) }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: 120
                horizontalAlignment: Text.AlignHCenter
                text: root.offsetText(root.offset)
                color: Theme.text
                font.pixelSize: Theme.fontBody - 1
                font.weight: Font.Medium
            }
            Item {
                width: 36; height: 36
                BarButton { anchors.fill: parent; size: 36; rotation: 180; icon: Icons.back; showLabel: false; onClicked: root.shift(30) }
            }
        }
    }
}

import QtQuick
import Relay.Core

// Lists hidden groups and channels with a way to bring each back.
// ↑ ↓ move, Enter shows the selected item again, Esc goes back.
FocusScope {
    id: root
    signal closeRequested()

    readonly property var items: {
        const out = []
        for (const g of Channels.hiddenGroups)
            out.push({ kind: "group", key: g.name, label: g.name,
                       detail: g.count === 1 ? qsTr("Group · 1 channel")
                                          : qsTr("Group · %L1 channels").arg(g.count) })
        for (const c of Channels.hiddenChannels)
            out.push({ kind: "channel", key: c.key, label: c.name, detail: qsTr("Channel") })
        return out
    }

    function unhide(i) {
        const it = items[i]
        if (!it) return
        if (it.kind === "group") Channels.unhideGroup(it.key)
        else Channels.unhideChannel(it.key)
        list.currentIndex = Math.min(i, list.count - 1)
    }

    Keys.onEscapePressed: closeRequested()
    Keys.onBackPressed: closeRequested()

    // Opaque background that also swallows clicks and scrolling, so they
    // can't reach the live-TV screen underneath (which would open the
    // channel list behind this screen and take the keyboard with it).
    Rectangle {
        anchors.fill: parent
        color: Theme.background
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            hoverEnabled: true
            onWheel: (w) => w.accepted = true
        }
    }

    Column {
        id: head
        anchors.horizontalCenter: parent.horizontalCenter
        y: 48
        width: Math.min(640, root.width - 64)
        spacing: 8
        Text {
            text: qsTr("Hidden groups and channels")
            color: Theme.text
            font.pixelSize: Theme.fontTitle
            font.weight: Font.DemiBold
        }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: root.items.length
                  ? qsTr("Hidden items don't appear in the channel list, the guide or channel up/down. Select one and press Enter to show it again.")
                  : qsTr("Nothing is hidden. Press H on a channel or group in the channel list to hide it.")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
        }
    }

    ListView {
        id: list
        anchors.top: head.bottom
        anchors.topMargin: 24
        anchors.bottom: back.top
        anchors.bottomMargin: 20
        anchors.horizontalCenter: parent.horizontalCenter
        width: head.width
        clip: true
        focus: true
        model: root.items
        highlightMoveDuration: 0
        boundsBehavior: Flickable.StopAtBounds
        Keys.onReturnPressed: root.unhide(currentIndex)
        Keys.onEnterPressed: root.unhide(currentIndex)
        KeyNavigation.down: back

        delegate: Rectangle {
            width: list.width
            height: 60
            radius: 6
            readonly property bool sel: ListView.isCurrentItem && list.activeFocus
            color: sel ? Theme.focus : "transparent"
            border.width: sel ? 2 : 0
            border.color: Theme.accent

            Column {
                x: 16
                width: parent.width - 140
                anchors.verticalCenter: parent.verticalCenter
                spacing: 3
                Text {
                    width: parent.width
                    text: modelData.label
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                    elide: Text.ElideRight
                }
                Text {
                    text: modelData.detail
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                }
            }
            ActionButton {
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                implicitHeight: 38
                text: qsTr("Show")
                focusPolicy: Qt.NoFocus
                onClicked: root.unhide(index)
            }
        }
    }

    ActionButton {
        id: back
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 40
        anchors.horizontalCenter: parent.horizontalCenter
        text: qsTr("Done")
        primary: true
        onClicked: root.closeRequested()
        KeyNavigation.up: list
    }
}

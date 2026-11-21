import QtQuick
import Relay.Core

// "Starting now" card for a reminder. Doesn't take the keyboard: Main.qml
// sends Enter here while it's showing. Hides itself after 30 seconds.
Rectangle {
    id: root
    property var item: ({})
    property bool shown: false
    signal watchRequested(var item)

    function show(it) {
        item = it
        shown = true
        hideTimer.restart()
    }
    function dismiss() { shown = false }
    function watch() {
        shown = false
        watchRequested(item)
    }

    visible: shown
    anchors.right: parent.right
    anchors.rightMargin: 24
    y: 24
    width: 420
    height: col.implicitHeight + 32
    radius: 12
    color: Theme.panelSolid
    border.width: 2
    border.color: Theme.accent

    Timer { id: hideTimer; interval: 30000; onTriggered: root.shown = false }
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }

    Column {
        id: col
        x: 18; y: 16
        width: parent.width - 36
        spacing: 6
        Text {
            text: (new Date(root.item.start || 0)) <= new Date() ? qsTr("◷  Starting now") : qsTr("◷  Starting in a minute")
            color: Theme.accent
            font.pixelSize: Theme.fontSmall
            font.weight: Font.DemiBold
        }
        Text {
            width: parent.width
            text: root.item.title || ""
            color: Theme.text
            font.pixelSize: Theme.fontLarge
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
        Text {
            width: parent.width
            text: root.item.channelName || ""
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
            elide: Text.ElideRight
        }
        Row {
            spacing: 8
            topPadding: 6
            ActionButton { text: qsTr("Watch  (Enter)"); primary: true; focusPolicy: Qt.NoFocus; onClicked: root.watch() }
            ActionButton { text: qsTr("Dismiss"); focusPolicy: Qt.NoFocus; onClicked: root.dismiss() }
        }
    }
}

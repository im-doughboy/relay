import QtQuick
import Relay.Core

// A new version is out: a small card in the bottom-right corner (never takes
// the keyboard away from what you're watching). Update now, Later (don't ask
// about this version again), or ✕ for now. Also in Settings → Updates.
Rectangle {
    id: root
    property bool dismissed: false
    property bool suppressed: false   // Settings is open (it has its own)
    readonly property bool busy: Updates.state === "downloading" || Updates.state === "installing"

    visible: !suppressed && ((Updates.offered && !dismissed) || busy || (Updates.state === "error" && shownError))
    property bool shownError: false
    Connections {
        target: Updates
        // Keep the card up to show what went wrong with an update you started.
        function onChanged() { if (Updates.state === "downloading") root.shownError = true }
    }

    anchors.right: parent.right
    anchors.bottom: parent.bottom
    anchors.margins: 24
    width: Math.min(440, parent.width - 48)
    height: col.implicitHeight + 32
    radius: 12
    color: Theme.panelSolid
    border.width: 1
    border.color: Theme.accent

    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }

    Column {
        id: col
        x: 18; y: 16
        width: parent.width - 36
        spacing: 10
        Row {
            spacing: 10
            width: parent.width
            Icon { anchors.verticalCenter: parent.verticalCenter; width: 22; height: 22; path: Icons.download; color: Theme.accent }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 32 - 40
                text: Updates.state === "installing" ? qsTr("Installing Relay %1…").arg(Updates.latestVersion)
                      : qsTr("Relay %1 is available").arg(Updates.latestVersion)
                color: Theme.text
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
        }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: Updates.state === "downloading" || Updates.state === "installing" || Updates.state === "error"
                  ? Updates.status
                  : qsTr("You're on %1. Updating keeps your playlists and settings; Relay closes and opens again.").arg(Updates.currentVersion)
            color: Updates.state === "error" ? Theme.danger : Theme.textSecondary
            font.pixelSize: Theme.fontSmall
        }
        Rectangle {
            visible: Updates.state === "downloading"
            width: parent.width
            height: 6
            radius: 3
            color: Theme.raised
            Rectangle { width: parent.width * Updates.progress; height: parent.height; radius: 3; color: Theme.accent }
        }
        Row {
            visible: !root.busy
            spacing: 8
            ActionButton {
                primary: true
                text: Updates.state === "error" ? qsTr("Try again") : Updates.canInstall ? qsTr("Update now") : qsTr("Download")
                onClicked: Updates.updateNow()
            }
            ActionButton {
                visible: Updates.releaseUrl.length > 0
                text: qsTr("What's new")
                onClicked: Qt.openUrlExternally(Updates.releaseUrl)
            }
            ActionButton {
                text: qsTr("Later")
                onClicked: { Updates.later(); root.shownError = false }
            }
        }
    }

    CloseButton {
        visible: !root.busy
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 8
        size: 32
        label: qsTr("Not now")
        onClicked: { root.dismissed = true; root.shownError = false }
    }
}

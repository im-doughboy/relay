import QtQuick
import Relay.Core

// The mouse's way around live TV: appears along the top when the mouse moves
// over the picture and fades out when it stops. Every button shows (or, when
// the window is narrow, tips) the key that does the same thing.
Item {
    id: bar
    // Mouse moved over the picture recently (set by Main).
    property bool wanted: false
    // Nothing full-screen is open on top of live TV (set by Main).
    property bool allowed: true
    property bool fullscreen: false
    property bool muted: false
    property bool captionsOn: false
    property bool pipOn: false

    signal channelsClicked()
    signal guideClicked()
    signal moviesClicked()
    signal searchClicked()
    signal multiViewClicked()
    signal recordingsClicked()
    signal pipClicked()
    signal captionsClicked()
    signal optionsClicked()
    signal muteClicked()
    signal fullscreenClicked()
    signal playlistsClicked()
    signal settingsClicked()
    signal helpClicked()
    signal castClicked()
    property bool casting: false

    readonly property bool shown: allowed && (wanted || hover.hovered)

    // Narrow windows: drop the key hints, then the labels (measured, so it
    // fits whatever the font and playlist name).
    readonly property var navItems: [
        { label: qsTr("Channels"), key: "Enter" },
        { label: qsTr("Guide"), key: Keymap.label("guide") },
        { label: qsTr("Movies & Series"), key: Keymap.label("movies") },
        { label: qsTr("Recordings"), key: "" },
        { label: qsTr("Multi-view"), key: Keymap.label("multiview") },
        { label: qsTr("Search"), key: Keymap.label("search") },
    ]
    readonly property real navSpace: tools.x - 16 - (brand.x + brand.width + 12)
    function navWidth(withKeys) {
        let w = (navItems.length - 1) * 4
        for (const it of navItems) {
            w += 20 + 8 + labelMetrics.advanceWidth(it.label) + 20
            if (withKeys && it.key) w += 8 + Math.max(22, keyMetrics.advanceWidth(it.key) + 12)
        }
        return w
    }
    readonly property bool roomForLabels: navWidth(false) <= navSpace
    readonly property bool roomForKeys: navWidth(true) <= navSpace
    FontMetrics { id: labelMetrics; font.pixelSize: Theme.fontBody - 1; font.weight: Font.Medium }
    FontMetrics { id: keyMetrics; font.pixelSize: Theme.fontSmall - 1; font.weight: Font.DemiBold }

    anchors.left: parent.left
    anchors.right: parent.right
    anchors.top: parent.top
    height: 88
    opacity: shown ? 1 : 0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: Theme.animNormal } }

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: "#E6000000" }
            GradientStop { position: 0.7; color: "#80000000" }
            GradientStop { position: 1; color: "#00000000" }
        }
    }
    HoverHandler { id: hover }
    // Clicks on empty bar space stay here (not on the picture underneath).
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = true }

    // Logo and the playlist being watched (click: switch playlist).
    Rectangle {
        id: brand
        x: 16
        y: 14
        height: 44
        width: brandRow.implicitWidth + 20
        radius: 22
        color: brandMouse.containsMouse ? "#2EFFFFFF" : "transparent"
        Row {
            id: brandRow
            anchors.centerIn: parent
            spacing: 10
            Image {
                anchors.verticalCenter: parent.verticalCenter
                source: "qrc:/icons/relay.png"
                width: 28; height: 28
                sourceSize: Qt.size(56, 56)
                smooth: true
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: bar.width >= 900
                text: Settings.activeName || "Relay"
                color: Theme.text
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
                width: Math.min(implicitWidth, 180)
                elide: Text.ElideRight
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: bar.width >= 900 && Settings.playlists.length > 1
                text: "▾"
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
            }
        }
        MouseArea {
            id: brandMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: bar.playlistsClicked()
        }
    }

    // Where to go.
    Row {
        id: nav
        anchors.left: brand.right
        anchors.leftMargin: 12
        y: 16
        spacing: 4
        BarButton { icon: Icons.channels; label: qsTr("Channels"); shortcut: "Enter"; showLabel: bar.roomForLabels; showShortcut: bar.roomForKeys; onClicked: bar.channelsClicked() }
        BarButton { icon: Icons.guide; label: qsTr("Guide"); shortcut: Keymap.label("guide"); showLabel: bar.roomForLabels; showShortcut: bar.roomForKeys; onClicked: bar.guideClicked() }
        BarButton { icon: Icons.movies; label: qsTr("Movies & Series"); shortcut: Keymap.label("movies"); showLabel: bar.roomForLabels; showShortcut: bar.roomForKeys; onClicked: bar.moviesClicked() }
        BarButton { icon: Icons.recordings; label: qsTr("Recordings"); showLabel: bar.roomForLabels; showShortcut: bar.roomForKeys; onClicked: bar.recordingsClicked() }
        BarButton { icon: Icons.multiview; label: qsTr("Multi-view"); shortcut: Keymap.label("multiview"); showLabel: bar.roomForLabels; showShortcut: bar.roomForKeys; onClicked: bar.multiViewClicked() }
        BarButton { icon: Icons.search; label: qsTr("Search"); shortcut: Keymap.label("search"); showLabel: bar.roomForLabels; showShortcut: bar.roomForKeys; onClicked: bar.searchClicked() }
    }

    // What's playing.
    Row {
        id: tools
        anchors.right: parent.right
        anchors.rightMargin: 16
        y: 16
        spacing: 4
        BarButton { icon: Icons.captions; label: qsTr("Captions"); shortcut: Keymap.label("captions"); showLabel: false; active: bar.captionsOn; onClicked: bar.captionsClicked() }
        BarButton { icon: Icons.options; label: qsTr("Picture and sound"); shortcut: "O"; showLabel: false; onClicked: bar.optionsClicked() }
        BarButton { icon: Icons.cast; label: bar.casting ? qsTr("Casting · stop or move it") : qsTr("Cast to a TV or Chromecast"); shortcut: Keymap.label("cast"); showLabel: false; active: bar.casting; onClicked: bar.castClicked() }
        BarButton { icon: Icons.pip; label: bar.pipOn ? qsTr("Close picture-in-picture") : qsTr("Picture-in-picture"); shortcut: Keymap.label("pip"); showLabel: false; active: bar.pipOn; onClicked: bar.pipClicked() }
        BarButton { icon: bar.muted ? Icons.muted : Icons.volume; label: bar.muted ? qsTr("Sound on") : qsTr("Mute (scroll for volume)"); shortcut: Keymap.label("mute"); showLabel: false; active: bar.muted; onClicked: bar.muteClicked() }
        BarButton { icon: bar.fullscreen ? Icons.exitFullscreen : Icons.fullscreen; label: bar.fullscreen ? qsTr("Leave full screen") : qsTr("Full screen"); shortcut: Keymap.label("fullscreen"); showLabel: false; onClicked: bar.fullscreenClicked() }
        Item { width: 8; height: 1 }
        BarButton { icon: Icons.settings; label: qsTr("Settings"); shortcut: Keymap.label("settings"); showLabel: false; onClicked: bar.settingsClicked() }
        BarButton { icon: Icons.help; label: qsTr("Keyboard shortcuts"); shortcut: Keymap.label("help"); showLabel: false; onClicked: bar.helpClicked() }
    }
}

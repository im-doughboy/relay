import QtQuick
import QtQuick.Controls
import Relay.Core
import Relay.Video

// Picture-in-picture: a second live channel, silent, in a corner of the
// screen. W pins the channel being watched here, X (or a click) swaps it with
// the big picture, Shift+W or dragging moves it to another corner.
Item {
    id: root
    anchors.fill: parent

    property string key: ""                     // channel key; "" = closed
    readonly property bool active: key.length > 0
    // Hidden behind full-screen screens (the guide, settings…); keeps playing.
    property bool suspended: false
    // Space kept free at the bottom (the info bar), so bottom corners move up.
    property real bottomInset: 0
    readonly property int index: (Channels.totalCount, active ? Channels.indexOfKey(key) : -1)
    readonly property var info: index >= 0 ? Channels.info(index) : ({})
    property string error: ""
    // The main picture's channel to put here once the swap's tune went through
    // (it may wait for the PIN).
    property int pendingFrom: -1

    signal message(string text)
    signal swapRequested()

    function open(globalIndex) {
        const k = Channels.keyAt(globalIndex)
        if (!k) return
        if (k === key) { flash(); return }
        key = k
        start()
    }
    function close() {
        if (!active) return
        key = ""
        pendingFrom = -1
        error = ""
        video.stop()
    }
    function start() {
        if (index < 0) { close(); return }
        error = ""
        video.play(info.url, info.userAgent || "", info.referrer || "")
        flash()
    }
    function flash() { label.shown = true; labelTimer.restart() }
    function moveCorner(delta) { Settings.pipCorner = Settings.pipCorner + delta }

    // The channel went away (playlist edited or switched).
    onIndexChanged: if (active && index < 0) close()
    Connections {
        target: Settings
        function onSourceChanged() { root.close() }
    }
    Connections {
        target: Player
        function onCurrentChanged() {
            if (root.pendingFrom >= 0 && Player.currentIndex === root.index) {
                const from = root.pendingFrom
                root.pendingFrom = -1
                root.open(from)
            } else {
                root.pendingFrom = -1
            }
        }
    }

    Rectangle {
        id: frame
        readonly property real margin: 24
        readonly property int corner: Settings.pipCorner
        readonly property bool sideRight: corner === 0 || corner === 1
        readonly property bool sideBottom: corner === 1 || corner === 2
        readonly property real homeX: sideRight ? root.width - width - margin : margin
        readonly property real homeY: sideBottom ? root.height - height - margin - root.bottomInset : margin

        visible: root.active && !root.suspended
        width: Math.round(Math.max(240, Math.min(root.width * 0.28, 640)))
        height: Math.round(width * 9 / 16)
        x: homeX
        y: homeY
        z: 4
        color: "black"
        border.width: 1
        border.color: mouse.containsMouse ? Theme.accent : Theme.border

        Behavior on x { enabled: !mouse.drag.active; NumberAnimation { duration: Theme.animNormal; easing.type: Easing.OutCubic } }
        Behavior on y { enabled: !mouse.drag.active; NumberAnimation { duration: Theme.animNormal; easing.type: Easing.OutCubic } }

        MpvVideo {
            id: video
            anchors.fill: parent
            anchors.margins: 1
            timeshiftMb: 0
            deinterlace: Settings.deinterlace
            hardwareDecoding: Settings.hardwareDecoding
            preferredAudio: Settings.audioLanguage
            muted: true
            onPlaybackError: (m) => root.error = m
            onPlaybackStarted: root.error = ""
        }
        BusyIndicator {
            anchors.centerIn: parent
            running: frame.visible && video.buffering && !root.error
            visible: running
            palette.dark: Theme.accent
        }
        Text {
            anchors.centerIn: parent
            width: parent.width - 24
            visible: root.error.length > 0
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            maximumLineCount: 3
            elide: Text.ElideRight
            text: root.error
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
        }

        // Channel name: for a few seconds after a change, and on hover.
        Rectangle {
            id: label
            property bool shown: false
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 1
            height: labelText.implicitHeight + 12
            color: "#B0000000"
            opacity: shown || mouse.containsMouse ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.animNormal } }
            Text {
                id: labelText
                x: 10
                width: parent.width - 20
                anchors.verticalCenter: parent.verticalCenter
                text: root.info.number !== undefined
                      ? root.info.number + "  " + root.info.name + (root.info.nowTitle ? "  ·  " + root.info.nowTitle : "")
                      : ""
                color: Theme.text
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
            }
        }
        Timer { id: labelTimer; interval: 4000; onTriggered: label.shown = false }

        // Click swaps; drag moves it to the nearest corner.
        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: drag.active ? Qt.ClosedHandCursor : Qt.PointingHandCursor
            drag.target: frame
            drag.threshold: 12
            drag.minimumX: 0
            drag.maximumX: root.width - frame.width
            drag.minimumY: 0
            drag.maximumY: root.height - frame.height
            property bool dragged: false
            onPressed: dragged = false
            onPositionChanged: if (drag.active) dragged = true
            onReleased: {
                if (!dragged) return
                const cx = frame.x + frame.width / 2, cy = frame.y + frame.height / 2
                const right = cx > root.width / 2, bottom = cy > root.height / 2
                const corner = right ? (bottom ? 1 : 0) : (bottom ? 2 : 3)
                Settings.pipCorner = corner
                // Back onto the (possibly unchanged) corner's position.
                frame.x = Qt.binding(() => frame.homeX)
                frame.y = Qt.binding(() => frame.homeY)
            }
            onClicked: if (!dragged) root.swapRequested()
        }
    }
}

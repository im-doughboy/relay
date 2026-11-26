import QtQuick
import Relay.Core

// The banner on channel change: number, logo, name, what's on now (with
// progress) and next, stream details and the clock. Pressing I again while
// it's showing expands it with the programme description.
Rectangle {
    id: root
    property var video
    property bool shown: false
    property bool expanded: false
    property double nowMs: Date.now()

    readonly property var now: Player.nowProgram
    readonly property var next: Player.nextProgram
    readonly property bool hasGuide: now.valid === true
    readonly property string description: {
        if (!hasGuide) return ""
        const parts = []
        if (now.subTitle) parts.push(now.subTitle)
        if (now.desc) parts.push(now.desc)
        return parts.join(" — ")
    }

    function show() { shown = true; hideTimer.interval = 5000; hideTimer.restart() }
    function hide() { shown = false; expanded = false }
    function toggle() {
        if (!shown) show()
        else if (!expanded && description.length > 0) {
            expanded = true
            hideTimer.interval = 15000
            hideTimer.restart()
        } else hide()
    }

    function timeText(ms) {
        return Qt.formatTime(new Date(ms), Qt.locale().timeFormat(Locale.ShortFormat).replace(":ss", ""))
    }
    function minutesLeft(stopMs) {
        const m = Math.max(0, Math.round((stopMs - nowMs) / 60000))
        if (m >= 60) return qsTr("%1 h %2 min left").arg(Math.floor(m / 60)).arg(m % 60)
        return qsTr("%1 min left").arg(m)
    }
    function resolutionLabel(h) {
        if (h >= 2000) return "4K"
        if (h >= 1000) return "1080p"
        if (h >= 700) return "720p"
        return h > 0 ? h + "p" : ""
    }

    anchors.left: parent.left
    anchors.right: parent.right
    anchors.margins: 32
    height: content.height + 40
    y: shown ? parent.height - height - 32 : parent.height + 8
    radius: 12
    color: Theme.panel
    border.width: 1
    border.color: Theme.border
    visible: Player.hasChannel && y < parent.height
    Behavior on y { NumberAnimation { duration: Theme.animNormal; easing.type: Easing.OutCubic } }
    Behavior on height { NumberAnimation { duration: Theme.animFast } }

    Timer { id: hideTimer; interval: 5000; onTriggered: root.hide() }
    // Stays up while the pointer is over it (its buttons, on a TV).
    HoverHandler {
        onHoveredChanged: if (hovered) hideTimer.stop(); else if (root.shown) hideTimer.restart()
    }
    Timer {
        interval: 15000; repeat: true; running: root.visible; triggeredOnStart: true
        onTriggered: root.nowMs = Date.now()
    }

    Column {
        id: content
        x: 24
        y: 20
        width: parent.width - 48
        spacing: 14

        Item {
            width: parent.width
            height: Math.max(88, details.height)

            Text {
                id: number
                width: Math.max(92, implicitWidth)   // room for 4 digits; wider for 10,000 and up
                anchors.verticalCenter: logo.verticalCenter
                horizontalAlignment: Text.AlignRight
                text: Player.currentNumber
                color: Theme.accent
                font.pixelSize: Theme.fontDisplay
                font.weight: Font.DemiBold
            }

            ChannelLogo {
                id: logo
                anchors.left: number.right
                anchors.leftMargin: 22
                y: 4
                width: 120
                height: 80
                source: Player.currentLogo
                name: Player.currentName
            }

            Column {
                id: details
                anchors.left: logo.right
                anchors.leftMargin: 22
                anchors.right: side.left
                anchors.rightMargin: 24
                spacing: 6

                // Channel name · group
                Row {
                    width: parent.width
                    spacing: 10
                    Text {
                        id: channelName
                        width: Math.min(implicitWidth, parent.width - groupName.implicitWidth - 10)
                        text: Player.currentName
                        color: Theme.text
                        font.pixelSize: Theme.fontLarge
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Text {
                        id: groupName
                        anchors.baseline: channelName.baseline
                        text: Player.currentGroup
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        elide: Text.ElideRight
                    }
                }

                // Now
                Row {
                    width: parent.width
                    spacing: 10
                    Text {
                        id: nowTitle
                        width: Math.min(implicitWidth, parent.width - episode.width - 10)
                        text: root.hasGuide ? root.now.title : qsTr("No guide listings")
                        color: root.hasGuide ? Theme.text : Theme.textMuted
                        font.pixelSize: root.hasGuide ? Theme.fontTitle : Theme.fontBody
                        font.weight: root.hasGuide ? Font.Medium : Font.Normal
                        elide: Text.ElideRight
                    }
                    Text {
                        id: episode
                        visible: text.length > 0
                        anchors.baseline: nowTitle.baseline
                        // Without listings, catch-up can still go back to the top of the hour.
                        readonly property double hourStart: { const d = new Date(root.nowMs); d.setMinutes(0, 0, 0); return d.getTime() }
                        readonly property bool hourCatchup: !root.hasGuide && root.shown && Player.hasChannel
                                                            && root.nowMs - hourStart > 60000
                                                            && Player.canCatchup(Player.currentIndex, hourStart)
                        text: root.hasGuide ? root.now.episode
                              : hourCatchup ? qsTr("↺  B: watch from %1").arg(root.timeText(hourStart)) : ""
                        color: root.hasGuide ? Theme.textSecondary : Theme.accent
                        font.pixelSize: Theme.fontBody
                    }
                }

                // Time range, progress, time left
                Row {
                    visible: root.hasGuide
                    spacing: 12
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.hasGuide ? root.timeText(root.now.start) + " – " + root.timeText(root.now.stop) : ""
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                    }
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 220
                        height: 4
                        radius: 2
                        color: Theme.track
                        Rectangle {
                            height: parent.height
                            radius: 2
                            color: Theme.accent
                            width: {
                                if (!root.hasGuide) return 0
                                const p = (root.nowMs - root.now.start) / (root.now.stop - root.now.start)
                                return parent.width * Math.max(0, Math.min(1, p))
                            }
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.hasGuide ? root.minutesLeft(root.now.stop) : ""
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }
                    Text {   // catch-up of the show on now
                        anchors.verticalCenter: parent.verticalCenter
                        visible: root.hasGuide && root.shown && Player.canCatchup(Player.currentIndex, root.now.start)
                        text: qsTr("↺  B: from the start")
                        color: Theme.accent
                        font.pixelSize: Theme.fontSmall
                    }
                }

                // Next
                Row {
                    visible: root.next.valid === true
                    width: parent.width
                    spacing: 10
                    Text {
                        text: qsTr("Next")
                        color: Theme.accent
                        font.pixelSize: Theme.fontSmall
                        font.weight: Font.DemiBold
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: 1
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: root.next.valid ? root.timeText(root.next.start) : ""
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        width: parent.width - 120
                        text: root.next.valid ? root.next.title : ""
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                        elide: Text.ElideRight
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }

            Column {
                id: side
                anchors.right: parent.right
                y: 2
                spacing: 12

                Text {
                    id: clock
                    anchors.right: parent.right
                    color: Theme.text
                    font.pixelSize: Theme.fontLarge
                    font.weight: Font.Medium
                    text: root.timeText(root.nowMs)
                }

                Row {
                    anchors.right: parent.right
                    spacing: 6
                    Repeater {
                        model: {
                            const v = root.video
                            if (!v) return []
                            const tags = []
                            const res = root.resolutionLabel(v.videoHeight)
                            if (res) tags.push(res)
                            if (v.fps > 0) tags.push(Math.round(v.fps) + " fps")
                            if (v.videoCodec) tags.push(v.videoCodec)
                            if (v.audioCodec) tags.push(v.audioCodec)
                            return tags
                        }
                        delegate: Rectangle {
                            height: 24
                            width: tag.implicitWidth + 16
                            radius: 4
                            color: "transparent"
                            border.width: 1
                            border.color: Theme.border
                            Text {
                                id: tag
                                anchors.centerIn: parent
                                text: modelData
                                color: Theme.textSecondary
                                font.pixelSize: 12
                                font.weight: Font.Medium
                            }
                        }
                    }
                }

                // On a TV: where the remote's missing buttons would go, for
                // the Magic Remote's pointer.
                Row {
                    visible: Settings.tv
                    anchors.right: parent.right
                    spacing: 8
                    PillButton { icon: Icons.guide; text: qsTr("Guide"); fontSize: Theme.fontSmall; onClicked: Remote.trigger("guide") }
                    PillButton { icon: Icons.movies; text: qsTr("Movies"); fontSize: Theme.fontSmall; onClicked: Remote.trigger("movies") }
                    PillButton { icon: Icons.search; text: qsTr("Search"); fontSize: Theme.fontSmall; onClicked: Remote.trigger("search") }
                }

                Text {
                    anchors.right: parent.right
                    visible: root.hasGuide && root.now.category
                    text: root.hasGuide ? root.now.category : ""
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                }
            }
        }

        // Description (expanded)
        Text {
            visible: root.expanded
            x: 92 + 22 + 120 + 22
            width: parent.width - x
            text: root.description
            wrapMode: Text.WordWrap
            maximumLineCount: 4
            elide: Text.ElideRight
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
            lineHeight: 1.25
        }
    }

    // Hint that more is available
    Text {
        visible: root.shown && !root.expanded && root.description.length > 0
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 12
        text: qsTr("I  more info")
        color: Theme.textMuted
        font.pixelSize: 11
    }
}

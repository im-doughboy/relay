import QtQuick
import QtQuick.Controls.Basic
import Relay.Core

// Recordings, scheduled recordings and reminders. Reached from the Movies /
// Series screen (third tab). Enter plays a recording, Delete removes (press
// twice), Esc goes back to TV.
FocusScope {
    id: root
    property bool opened: false
    property int section: 0              // 0 recorded, 1 scheduled, 2 series, 3 reminders
    property string confirmId: ""        // Delete pressed once on this item
    signal closed()
    signal message(string text)
    signal playRequested(var descriptor)
    signal moviesRequested(int section)
    signal pinRequested(string reason, var onOk)

    readonly property var all: (Schedule.revision, Schedule.items)
    readonly property var recorded: all.filter(i => i.type === "recording" && i.status !== "scheduled")
    readonly property var scheduled: all.filter(i => i.type === "recording" && i.status === "scheduled").reverse()
    readonly property var reminders: all.filter(i => i.type === "reminder").reverse()
    readonly property var seriesRules: (Schedule.revision, Schedule.series)
    readonly property var rows: section === 0 ? recorded : section === 1 ? scheduled : section === 2 ? seriesRules : reminders

    function open(sec) {
        if (sec !== undefined) section = sec
        opened = true
        confirmId = ""
        list.currentIndex = 0
        list.forceActiveFocus()
    }
    function close() {
        if (!opened) return
        opened = false
        closed()
    }
    function timeText(ms) {
        return Qt.formatTime(new Date(ms), Qt.locale().timeFormat(Locale.ShortFormat).replace(":ss", ""))
    }
    function whenText(it) {
        const d = new Date(it.start), t = new Date()
        const days = Math.round((new Date(d.getFullYear(), d.getMonth(), d.getDate())
                                 - new Date(t.getFullYear(), t.getMonth(), t.getDate())) / 86400000)
        const day = days === 0 ? qsTr("Today") : days === 1 ? qsTr("Tomorrow") : days === -1 ? qsTr("Yesterday")
                  : Qt.formatDate(d, "ddd d MMM")
        return day + "  " + timeText(it.start) + "–" + timeText(it.stop)
    }
    function sizeText(bytes) {
        if (bytes >= 1073741824) return (bytes / 1073741824).toFixed(2) + " GB"
        return Math.max(0.1, bytes / 1048576).toFixed(1) + " MB"
    }
    function statusText(it) {
        switch (it.status) {
        case "recording": return it.retrying ? qsTr("● Recording · reconnecting… · %1").arg(sizeText(it.bytes))
                                             : qsTr("● Recording · %1").arg(sizeText(it.bytes))
        case "done": return sizeText(it.bytes) + (it.error ? "  ·  " + it.error : "")
        case "failed": return qsTr("Failed") + (it.error ? ": " + it.error : "")
        case "missed": return qsTr("Missed (Relay wasn't running)")
        case "scheduled": return it.type === "reminder" ? qsTr("Reminder a minute before") : qsTr("Will record")
        }
        return ""
    }
    function activate(i) {
        const it = rows[i]
        if (!it) return
        if (it.type === "recording" && (it.status === "done" || it.status === "recording")) {
            if (it.globalIndex >= 0 && Parental.blocked(it.globalIndex)) {
                pinRequested(qsTr("\"%1\" is locked").arg(it.channelName), () => root.activate(i))
                return
            }
            playRequested({ kind: "recording", id: it.id, url: it.file, title: it.title,
                            subtitle: it.channelName + " · " + whenText(it), start: 0 })
        } else if (it.status === "failed" || it.status === "missed") {
            message(statusText(it))
        }
    }
    function removeAt(i) {
        const it = rows[i]
        if (!it) return
        if (section === 2) {
            Schedule.removeSeries(it.id)
            message(qsTr("No longer recording every episode of %1").arg(it.title))
            list.currentIndex = Math.max(0, Math.min(i, rows.length - 1))
            return
        }
        if (it.status === "recording") {
            Schedule.stopRecording(it.id)
            message(qsTr("Recording stopped"))
            return
        }
        if (it.status === "done" && confirmId !== it.id) {
            confirmId = it.id
            message(qsTr("Press Delete again to delete this recording"))
            return
        }
        confirmId = ""
        Schedule.remove(it.id, true)
        message(it.type === "reminder" ? qsTr("Reminder removed")
                : it.status === "scheduled" ? qsTr("Recording cancelled") : qsTr("Recording deleted"))
        list.currentIndex = Math.min(i, rows.length - 1)
    }

    visible: opened
    Keys.onEscapePressed: close()

    Rectangle {
        anchors.fill: parent
        color: Theme.background
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = true }
    }

    // ---- Header (same tabs as Movies / Series) ------------------------------------
    Item {
        id: header
        width: parent.width
        height: 84
        Row {
            x: 32
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            Repeater {
                model: [qsTr("Movies"), qsTr("Series"), qsTr("Recordings")]
                delegate: Rectangle {
                    readonly property bool current: index === 2
                    height: 44
                    width: tabText.implicitWidth + 36
                    radius: 22
                    color: current ? Theme.accent : (tabMouse.containsMouse ? Theme.focus : Theme.raised)
                    Text {
                        id: tabText
                        anchors.centerIn: parent
                        text: modelData + (index === 2 && Schedule.recordingCount > 0 ? "  ●" : "")
                        color: parent.current ? Theme.accentText : Theme.text
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.DemiBold
                    }
                    MouseArea {
                        id: tabMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: if (index < 2) root.moviesRequested(index)
                    }
                }
            }
        }
        CloseButton {
            id: closeButton
            anchors.right: parent.right
            anchors.rightMargin: 32
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.close()
        }
        Text {
            anchors.right: closeButton.left
            anchors.rightMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, parent.width - 580)
            elide: Text.ElideLeft
            text: qsTr("Saved to %1").arg(Schedule.folder)
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
        }
    }

    // ---- Sections --------------------------------------------------------------------
    ListView {
        id: sections
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        x: 16
        width: 280
        model: [
            { label: qsTr("Recorded"), count: root.recorded.length },
            { label: qsTr("Scheduled"), count: root.scheduled.length },
            { label: qsTr("Series"), count: root.seriesRules.length },
            { label: qsTr("Reminders"), count: root.reminders.length },
        ]
        currentIndex: root.section
        keyNavigationEnabled: false
        Keys.onPressed: (e) => {
            switch (e.key) {
            case Qt.Key_Up: if (root.section > 0) { root.section--; list.currentIndex = 0 } break
            case Qt.Key_Down: if (root.section < 3) { root.section++; list.currentIndex = 0 } break
            case Qt.Key_Right:
            case Qt.Key_Return:
            case Qt.Key_Enter: list.forceActiveFocus(); break
            case Qt.Key_Tab: root.moviesRequested(0); break
            case Qt.Key_Escape:
            case Qt.Key_Backspace: root.close(); break
            default: return
            }
            e.accepted = true
        }
        delegate: Rectangle {
            readonly property bool current: index === root.section
            width: sections.width - 12
            height: 46
            radius: 6
            color: current ? (sections.activeFocus ? Theme.focus : Theme.raised) : "transparent"
            border.width: current && sections.activeFocus ? 2 : 0
            border.color: Theme.accent
            Text {
                x: 14
                anchors.verticalCenter: parent.verticalCenter
                text: modelData.label
                color: parent.current ? Theme.text : Theme.textSecondary
                font.pixelSize: Theme.fontBody - 1
                font.weight: parent.current ? Font.DemiBold : Font.Normal
            }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                text: modelData.count
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
            }
            MouseArea {
                anchors.fill: parent
                onClicked: { root.section = index; list.currentIndex = 0; sections.forceActiveFocus() }
            }
        }
    }

    // ---- Items -------------------------------------------------------------------------
    ListView {
        id: list
        anchors.top: header.bottom
        anchors.bottom: hint.top
        anchors.left: sections.right
        anchors.leftMargin: 16
        anchors.right: parent.right
        anchors.rightMargin: 32
        clip: true
        model: root.rows
        keyNavigationEnabled: false
        highlightMoveDuration: 0
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        onCountChanged: if (currentIndex >= count) currentIndex = count - 1

        Keys.onPressed: (e) => {
            switch (e.key) {
            case Qt.Key_Up: if (currentIndex > 0) currentIndex--; break
            case Qt.Key_Down: if (currentIndex < count - 1) currentIndex++; break
            case Qt.Key_Left: sections.forceActiveFocus(); break
            case Qt.Key_Return:
            case Qt.Key_Enter: root.activate(currentIndex); break
            case Qt.Key_Delete: root.removeAt(currentIndex); break
            case Qt.Key_Tab: root.moviesRequested(0); break
            case Qt.Key_Escape:
            case Qt.Key_Backspace: root.close(); break
            default: return
            }
            if (e.key !== Qt.Key_Delete) root.confirmId = ""
            positionViewAtIndex(currentIndex, ListView.Contain)
            e.accepted = true
        }

        delegate: Rectangle {
            readonly property bool sel: ListView.isCurrentItem && list.activeFocus
            readonly property var it: modelData
            width: list.width
            height: 76
            radius: 8
            color: sel ? Theme.focus : (rowMouse.containsMouse ? Theme.raised : "transparent")
            border.width: sel ? 2 : 0
            border.color: root.confirmId === it.id ? Theme.danger : Theme.accent

            Rectangle {   // live recording dot
                visible: it.status === "recording"
                x: 18
                anchors.verticalCenter: parent.verticalCenter
                width: 12; height: 12; radius: 6
                color: "#E5484D"
                SequentialAnimation on opacity {
                    running: parent.visible
                    loops: Animation.Infinite
                    NumberAnimation { to: 0.3; duration: 700 }
                    NumberAnimation { to: 1; duration: 700 }
                }
            }
            Column {
                x: 44
                width: parent.width - 60
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4
                Text {
                    width: parent.width
                    text: it.title
                    color: it.status === "failed" || it.status === "missed" ? Theme.textMuted : Theme.text
                    font.pixelSize: Theme.fontBody + 1
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    text: root.section === 2
                          ? it.channelName + "   ·   " + (it.upcoming > 0
                                ? (it.upcoming === 1 ? qsTr("1 episode scheduled") : qsTr("%1 episodes scheduled").arg(it.upcoming)) + "   ·   " + qsTr("next: %1").arg(root.whenText({ start: it.next, stop: it.next }).split("–")[0])
                                : qsTr("No episodes in the guide right now"))
                          : it.channelName + "   ·   " + root.whenText(it) + "   ·   " + (it.series && it.status === "scheduled" ? qsTr("Will record (series)") : root.statusText(it))
                    color: it.status === "recording" ? Theme.accent
                         : it.status === "failed" ? Theme.danger : Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                    elide: Text.ElideRight
                }
            }
            MouseArea {
                id: rowMouse
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onClicked: (m) => {
                    list.currentIndex = index
                    list.forceActiveFocus()
                    if (m.button === Qt.RightButton && root.section !== 2) { itemMenu.target = it; itemMenu.popup() }
                }
                onDoubleClicked: root.activate(index)
            }
        }
    }

    Text {
        anchors.centerIn: list
        visible: list.count === 0
        width: Math.min(560, list.width - 40)
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        text: root.section === 0 ? qsTr("No recordings yet. In the guide, press R on a show to record it, or Ctrl+R while watching.")
            : root.section === 1 ? qsTr("Nothing scheduled. Press R on a future show in the guide.")
            : root.section === 2 ? qsTr("No series. In the guide, press Shift+R on a show to record every episode on that channel.")
            : qsTr("No reminders. Press M on a future show in the guide.")
        color: Theme.textSecondary
        font.pixelSize: Theme.fontBody
    }

    Text {
        id: hint
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 18
        text: qsTr("Enter play   ·   Delete remove (twice for a recording)   ·   Tab Movies   ·   Esc back to TV")
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
    }

    RelayMenu {
        id: itemMenu
        property var target: ({})
        RelayMenuItem {
            text: qsTr("Play")
            enabled: itemMenu.target.status === "done" || itemMenu.target.status === "recording"
            onTriggered: root.activate(list.currentIndex)
        }
        RelayMenuItem {
            text: itemMenu.target.status === "recording" ? qsTr("Stop recording")
                : itemMenu.target.status === "scheduled" ? qsTr("Cancel") : qsTr("Delete")
            onTriggered: {
                if (itemMenu.target.status === "done") root.confirmId = itemMenu.target.id   // the menu is the confirmation
                root.removeAt(list.currentIndex)
            }
        }
    }
}

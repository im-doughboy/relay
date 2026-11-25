import QtQuick
import Relay.Core

// Full-screen TV guide: details of the selected show and the live video at the
// top, a group switcher, a timeline, and one row per channel below.
//
// Keys: arrows move (Up from the first row reaches the group switcher), Enter
// tunes, Page Up/Down page through channels, Home jumps back to now, Esc / G /
// Backspace close. Mouse: click selects, click again (or double-click) tunes,
// wheel scrolls channels, Shift+wheel scrolls time.
FocusScope {
    id: guide

    property bool opened: false
    signal closed()
    signal message(string text)
    signal searchRequested()

    // Where Main.qml places the live video while the guide is open.
    readonly property rect previewRect: Qt.rect(preview.x, preview.y, preview.width, preview.height)
    // The mouse is over the small picture (Main shows "watch full screen").
    readonly property bool previewHovered: previewMouse.containsMouse

    // ---- timeline state (all times in ms since epoch) ----
    readonly property double halfHour: 30 * 60 * 1000
    readonly property double spanMs: 2 * 60 * 60 * 1000              // visible width = 2 hours
    readonly property double minStart: floorHalfHour(Date.now()) - 48 * 3600 * 1000
    readonly property double maxStart: floorHalfHour(Date.now()) + 7 * 24 * 3600 * 1000
    property double windowStart: floorHalfHour(Date.now())
    readonly property double windowEnd: windowStart + spanMs
    property double focusTime: Date.now()
    property double nowMs: Date.now()
    readonly property real pxPerMs: timeline.width / spanMs

    property bool groupFocused: false
    readonly property var selected: list.currentItem ? list.currentItem.programmeAt(focusTime) : null
    // Catch-up for the selection: "past" (Enter plays it), "now" (B plays it
    // from the start) or "".
    readonly property string selectedCatchup: {
        const p = selected
        if (!p || !list.currentItem || list.currentItem.catchupDays <= 0) return ""
        if (p.start < nowMs - list.currentItem.catchupDays * 86400000) return ""
        if (p.stop <= nowMs) return "past"
        return p.start <= nowMs ? "now" : ""
    }

    function floorHalfHour(t) { return t - (t % (30 * 60 * 1000)) }
    function floorHour(t) { const d = new Date(t); d.setMinutes(0, 0, 0); return d.getTime() }
    function nextHour(t) { const d = new Date(floorHour(t)); d.setHours(d.getHours() + 1); return d.getTime() }
    // Programmes in [from, to) with the empty stretches filled by hour-long
    // blocks named after the channel (placeholder: true), so channels without
    // listings — and gaps in listings — can still be selected, played from
    // catch-up and recorded.
    function withPlaceholders(real, from, to, name) {
        const out = []
        let t = floorHour(from)
        const gap = (until) => {
            while (t < until) {
                const end = Math.min(nextHour(t), until)
                if (end - t >= 60000) out.push({ title: name, start: t, stop: end, placeholder: true })
                t = end
            }
        }
        for (let i = 0; i < real.length; ++i) {
            gap(real[i].start)
            out.push(real[i])
            t = Math.max(t, real[i].stop)
        }
        gap(to)
        return out
    }
    function timeText(ms) {
        return Qt.formatTime(new Date(ms), Qt.locale().timeFormat(Locale.ShortFormat).replace(":ss", ""))
    }
    function dayText(ms) {
        const d = new Date(ms), today = new Date()
        const days = Math.round((new Date(d.getFullYear(), d.getMonth(), d.getDate())
                                 - new Date(today.getFullYear(), today.getMonth(), today.getDate())) / 86400000)
        if (days === 0) return qsTr("Today")
        if (days === 1) return qsTr("Tomorrow")
        if (days === -1) return qsTr("Yesterday")
        return Qt.formatDate(d, "ddd d MMM")
    }

    function open() {
        nowMs = Date.now()
        windowStart = floorHalfHour(nowMs)
        focusTime = nowMs
        groupFocused = false
        opened = true
        const row = Channels.rowOf(Player.currentIndex)
        list.currentIndex = row >= 0 ? row : 0
        list.positionViewAtIndex(list.currentIndex, ListView.Center)
        list.forceActiveFocus()
    }
    function close() {
        if (!opened) return
        opened = false
        closed()
    }
    function tuneCurrent() {
        if (!list.currentItem) return
        // A show that has finished: play it from the archive when there is one.
        const p = selected
        if (p && p.stop <= Date.now()) {
            if (selectedCatchup === "past" && Player.playCatchup(list.currentItem.globalIndex, p, 0)) { close(); return }
            message(qsTr("No catch-up for this show · showing the channel live"))
        }
        Player.tune(list.currentItem.globalIndex, Channels.currentGroup)
        close()
    }
    // R: record the selected show (again: cancel / stop).
    function toggleRecording() {
        const p = selected
        if (!list.currentItem || !p) return
        const gi = list.currentItem.globalIndex
        const id = Schedule.idFor(gi, p.start)
        if (id && Schedule.markFor(gi, p.start) === "recording") {
            const it = Schedule.item(id)
            if (it.status === "recording") { Schedule.stopRecording(id); message(qsTr("Recording stopped · it's in Recordings")) }
            else { Schedule.remove(id, false); message(qsTr("Recording cancelled")) }
            return
        }
        if (p.stop <= Date.now()) {
            message(selectedCatchup ? qsTr("This show is over · press Enter to watch it from catch-up")
                                    : qsTr("This show is over"))
            return
        }
        if (Schedule.addRecording(gi, p))
            message(p.start <= Date.now() ? qsTr("Recording %1 now").arg(p.title)
                                          : qsTr("Will record %1").arg(p.title))
    }
    // Shift+R: record every episode of the selected show on this channel (again: stop doing so).
    function toggleSeries() {
        const p = selected
        if (!list.currentItem || !p) return
        const gi = list.currentItem.globalIndex
        const id = Schedule.seriesFor(gi, p.title)
        if (id) {
            Schedule.removeSeries(id)
            message(qsTr("No longer recording every episode of %1").arg(p.title))
            return
        }
        if (p.placeholder) { message(qsTr("Recording a series needs guide listings for this channel")); return }
        if (Schedule.addSeries(gi, p))
            message(qsTr("Will record every episode of %1 on this channel").arg(p.title))
    }
    // M: remind me when the selected show starts (again: cancel).
    function toggleReminder() {
        const p = selected
        if (!list.currentItem || !p) return
        const gi = list.currentItem.globalIndex
        const id = Schedule.idFor(gi, p.start)
        if (id && Schedule.markFor(gi, p.start) === "reminder") {
            Schedule.remove(id, false)
            message(qsTr("Reminder removed"))
            return
        }
        if (p.placeholder) { message(qsTr("Reminders need guide listings for this channel")); return }
        if (p.start <= Date.now()) { message(qsTr("Reminders are for shows that haven't started")); return }
        if (Schedule.addReminder(gi, p)) message(qsTr("I'll remind you when %1 starts").arg(p.title))
    }
    function watchFromStart() {
        if (!list.currentItem || !selected) return
        if (selectedCatchup && Player.playCatchup(list.currentItem.globalIndex, selected, 0)) close()
        else message(qsTr("No catch-up for this show"))
    }

    // Keep the window on half-hour steps and inside the stored guide range.
    function setWindow(start) {
        windowStart = Math.max(minStart, Math.min(maxStart, floorHalfHour(start)))
    }
    // When the selection nears an edge, jump the timeline so there's context
    // around it: moving right keeps 30 minutes behind the selection, moving
    // left puts it in the middle.
    function ensureVisible() {
        if (focusTime >= windowEnd - halfHour) setWindow(focusTime - halfHour)
        else if (focusTime < windowStart) setWindow(focusTime - spanMs / 2)
        focusTime = Math.max(focusTime, windowStart)
    }
    function moveRight() {
        const p = selected
        focusTime = p ? p.stop : floorHalfHour(focusTime) + halfHour
        ensureVisible()
    }
    function moveLeft() {
        const p = list.currentItem ? list.currentItem.programmeAt(focusTime) : null
        const start = p ? Math.max(p.start, windowStart) : focusTime
        const before = list.currentItem ? list.currentItem.programmeAt(start - 1) : null
        focusTime = before ? before.start : floorHalfHour(start - 1)
        ensureVisible()
        const now = list.currentItem ? list.currentItem.programmeAt(focusTime) : null
        if (now) focusTime = Math.max(now.start, windowStart)
    }
    function jumpToNow() {
        nowMs = Date.now()
        setWindow(nowMs)
        focusTime = nowMs
    }
    function page(delta) {
        const visibleRows = Math.max(1, Math.floor(list.height / list.rowHeight) - 1)
        list.currentIndex = Math.max(0, Math.min(list.count - 1, list.currentIndex + delta * visibleRows))
    }
    function toggleFavoriteCurrent() {
        if (!list.currentItem) return
        const row = list.currentIndex
        const fav = Channels.toggleFavorite(list.currentItem.globalIndex)
        if (Channels.currentGroup === Channels.favoritesGroup)
            list.currentIndex = Math.min(row, list.count - 1)
        message(fav ? qsTr("Added to Favorites") : qsTr("Removed from Favorites"))
    }
    function hideCurrent() {
        if (!list.currentItem) return
        const row = list.currentIndex
        Channels.hideChannel(list.currentItem.globalIndex)
        list.currentIndex = Math.min(row, list.count - 1)
        message(qsTr("Channel hidden · unhide it in Playlist settings (S)"))
    }
    function stepGroup(delta) {
        const n = Channels.groups.length
        if (n > 0) Channels.currentGroup = (Channels.currentGroup + delta + n) % n
    }

    visible: opened
    // The group chooser (All channels, Favorites…), from anywhere in the list.
    function focusGroups() {
        guide.groupFocused = true
        groupChip.forceActiveFocus()
    }
    Keys.onPressed: (e) => {
        if (e.key === Qt.Key_Escape || e.key === Qt.Key_Back || e.key === Qt.Key_G
                || e.key === Qt.Key_Backspace) { close(); e.accepted = true }
    }

    Timer {
        interval: 30000; repeat: true; running: guide.opened
        onTriggered: guide.nowMs = Date.now()
    }

    Connections {
        target: Channels
        enabled: guide.opened   // closed, it has no rows to place
        function onCurrentGroupChanged() {
            const row = Channels.rowOf(Player.currentIndex)
            list.currentIndex = row >= 0 ? row : 0
            list.positionViewAtIndex(list.currentIndex, ListView.Center)
        }
    }

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

    // ---- Header: selected programme + live preview -------------------------------
    Item {
        id: header
        width: parent.width
        height: preview.height + 48 + actions.height + 4

        Item {
            id: preview
            x: parent.width - width - 32
            y: 24
            width: Math.round(Math.min(parent.width * 0.32, 520))
            height: Math.round(width * 9 / 16)
            Rectangle {   // frame drawn around the video (which sits on top)
                anchors.fill: parent
                anchors.margins: -1
                color: "transparent"
                border.width: 1
                border.color: Theme.border
                radius: 2
            }
            // Click the small picture: back to watching full screen.
            MouseArea {
                id: previewMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: guide.close()
            }
        }

        // A narrow window: first the keys go from the buttons, then the labels
        // (all but Watch; hovering still names them). 0 everything, 1 no
        // keys, 2 icons.
        readonly property int squeeze: {
            const left = [watchButton, fromStartButton, recordButton, seriesButton, remindButton]
            const right = [nowButton, searchButton]
            const width = (level) => {
                let sum = 0, n = 0
                for (const b of left.concat(right)) {
                    if (!b.visible) continue
                    const labelled = b !== seriesButton || actions.seriesOn
                    sum += !labelled ? b.size : level === 0 ? b.fullWidth
                         : level === 1 || b.accent ? b.labelledWidth : b.size
                    ++n
                }
                return sum + n * 8 + 40   // spacing, and ✕
            }
            const room = header.width - 64 - 24
            return width(0) <= room ? 0 : width(1) <= room ? 1 : 2
        }

        // The mouse's way to do what the keys do here.
        Row {
            id: actions
            x: 32
            y: preview.y + preview.height + 16
            height: 40
            spacing: 8
            readonly property var p: guide.selected
            readonly property string mark: (Schedule.revision, p && list.currentItem ? Schedule.markFor(list.currentItem.globalIndex, p.start) : "")
            readonly property bool seriesOn: (Schedule.revision, !!p && !!list.currentItem && Schedule.seriesFor(list.currentItem.globalIndex, p.title) !== "")
            readonly property bool future: !!p && p.start > guide.nowMs
            readonly property bool ended: !!p && p.stop <= guide.nowMs
            BarButton {
                id: watchButton
                showShortcut: header.squeeze === 0
                accent: true
                icon: Icons.play; iconFilled: true
                label: guide.selectedCatchup === "past" ? qsTr("Watch catch-up") : qsTr("Watch")
                shortcut: "Enter"
                visible: !!list.currentItem
                onClicked: guide.tuneCurrent()
            }
            BarButton {
                id: fromStartButton
                showShortcut: header.squeeze === 0; showLabel: header.squeeze < 2
                icon: Icons.fromStart
                label: qsTr("From the start")
                shortcut: "B"
                visible: guide.selectedCatchup === "now"
                onClicked: guide.watchFromStart()
            }
            BarButton {
                id: recordButton
                showShortcut: header.squeeze === 0; showLabel: header.squeeze < 2
                icon: Icons.record; iconFilled: actions.mark === "recording"
                label: actions.mark === "recording" ? qsTr("Cancel recording") : qsTr("Record")
                shortcut: "R"
                active: actions.mark === "recording"
                visible: !!actions.p && !actions.ended
                onClicked: guide.toggleRecording()
            }
            BarButton {
                id: seriesButton
                showShortcut: header.squeeze === 0
                icon: Icons.recordSeries
                label: actions.seriesOn ? qsTr("Stop recording series") : qsTr("Record series")
                shortcut: "Shift+R"
                showLabel: actions.seriesOn && header.squeeze < 2
                active: actions.seriesOn
                visible: !!actions.p && !actions.p.placeholder
                onClicked: guide.toggleSeries()
            }
            BarButton {
                id: remindButton
                showShortcut: header.squeeze === 0; showLabel: header.squeeze < 2
                icon: Icons.bell
                label: actions.mark === "reminder" ? qsTr("Remove reminder") : qsTr("Remind me")
                shortcut: "M"
                active: actions.mark === "reminder"
                visible: actions.future && !actions.p.placeholder
                onClicked: guide.toggleReminder()
            }
        }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: 32
            y: actions.y
            spacing: 8
            BarButton { id: nowButton; showShortcut: header.squeeze === 0; showLabel: header.squeeze < 2
                        icon: Icons.now; label: qsTr("Now"); shortcut: "Home"; onClicked: guide.jumpToNow() }
            BarButton { id: searchButton; showShortcut: header.squeeze === 0; showLabel: header.squeeze < 2
                        icon: Icons.search; label: qsTr("Search"); shortcut: "/"; onClicked: guide.searchRequested() }
            CloseButton { size: 40; onClicked: guide.close() }
        }

        Column {
            id: about
            x: 32
            y: 28
            width: preview.x - 64
            spacing: 8

            Text {
                width: parent.width
                text: list.currentItem ? list.currentItem.number + "  ·  " + list.currentItem.name : ""
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                text: guide.selected ? guide.selected.title
                      : list.currentItem && list.currentItem.hidden ? qsTr("🔒 Locked channel") : qsTr("No guide information")
                color: guide.selected && !guide.selected.placeholder ? Theme.text : Theme.textSecondary
                font.pixelSize: Theme.fontTitle + 4
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                visible: !!guide.selected
                text: {
                    const p = guide.selected
                    if (!p) return ""
                    const parts = [guide.dayText(p.start) + "  " + guide.timeText(p.start) + " – " + guide.timeText(p.stop)]
                    if (p.placeholder) parts.push(qsTr("No guide listings"))
                    if (p.episode) parts.push(p.episode)
                    if (p.category) parts.push(p.category)
                    const mark = (Schedule.revision, list.currentItem ? Schedule.markFor(list.currentItem.globalIndex, p.start) : "")
                    if (list.currentItem && Schedule.seriesFor(list.currentItem.globalIndex, p.title) !== "") parts.push(qsTr("● Every episode is recorded"))
                    if (mark === "recording") parts.push(qsTr("● Recording (R to cancel)"))
                    else if (mark === "reminder") parts.push(qsTr("◷ Reminder set (M to remove)"))
                    if (guide.selectedCatchup === "past") parts.push(qsTr("↺ Catch-up · Enter to watch"))
                    else if (guide.selectedCatchup === "now") parts.push(qsTr("↺ B: watch from the start"))
                    return parts.join("   ·   ")
                }
                color: Theme.accent
                font.pixelSize: Theme.fontBody
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                visible: !!guide.selected
                text: {
                    const p = guide.selected
                    if (!p) return ""
                    return [p.subTitle, p.desc].filter(s => !!s).join(" — ")
                }
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
                wrapMode: Text.WordWrap
                maximumLineCount: 4
                elide: Text.ElideRight
                lineHeight: 1.2
                // Only what fits above the buttons (a small window has room for less).
                readonly property real room: actions.y - 14 - about.y - y
                height: Math.max(0, Math.min(implicitHeight, room))
                clip: true
                opacity: room >= font.pixelSize * 1.2 ? 1 : 0
            }
        }
    }

    // ---- Group switcher + timeline -----------------------------------------------
    Item {
        id: bar
        anchors.top: header.bottom
        width: parent.width
        height: 44

        Rectangle {
            id: groupChip
            x: 16
            anchors.verticalCenter: parent.verticalCenter
            width: guide.channelColumn - 24
            height: 34
            radius: 17
            color: guide.groupFocused ? Theme.focus : Theme.raised
            border.width: guide.groupFocused ? 2 : 1
            border.color: guide.groupFocused ? Theme.accent : Theme.border

            Text {
                anchors.left: parent.left; anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: "‹"
                color: guide.groupFocused ? Theme.accent : Theme.textMuted
                font.pixelSize: 20
            }
            Text {
                anchors.centerIn: parent
                width: parent.width - 60
                horizontalAlignment: Text.AlignHCenter
                text: {
                    const g = Channels.groups[Channels.currentGroup]
                    return g ? g.name : ""
                }
                color: Theme.text
                font.pixelSize: Theme.fontSmall + 1
                font.weight: Font.Medium
                elide: Text.ElideRight
            }
            Text {
                anchors.right: parent.right; anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: "›"
                color: guide.groupFocused ? Theme.accent : Theme.textMuted
                font.pixelSize: 20
            }
            MouseArea {
                anchors.fill: parent
                onClicked: (m) => guide.stepGroup(m.x < width / 2 ? -1 : 1)
            }

            Keys.onPressed: (e) => {
                switch (e.key) {
                case Qt.Key_Left:
                case Qt.Key_AudioRewind: guide.stepGroup(-1); break
                case Qt.Key_Right:
                case Qt.Key_AudioForward: guide.stepGroup(1); break
                case Qt.Key_Down:
                case Qt.Key_Return:
                case Qt.Key_Enter:
                    guide.groupFocused = false; list.forceActiveFocus(); break
                case Qt.Key_Escape:
                case Qt.Key_G:
                case Qt.Key_Backspace:
                    guide.close(); break
                default: return
                }
                e.accepted = true
            }
            onActiveFocusChanged: if (!activeFocus) guide.groupFocused = false
        }

        // Day + half-hour labels
        Item {
            id: timeline
            x: guide.channelColumn
            width: parent.width - x
            height: parent.height
            clip: true

            Repeater {
                model: Math.ceil(guide.spanMs / guide.halfHour)
                delegate: Item {
                    readonly property double t: guide.windowStart + index * guide.halfHour
                    x: (t - guide.windowStart) * guide.pxPerMs
                    width: guide.halfHour * guide.pxPerMs
                    height: timeline.height
                    Rectangle { width: 1; height: 10; anchors.bottom: parent.bottom; color: Theme.border }
                    Text {
                        x: 8
                        anchors.verticalCenter: parent.verticalCenter
                        text: (index === 0 || new Date(t).getHours() === 0 && new Date(t).getMinutes() === 0
                               ? guide.dayText(t) + "  " : "") + guide.timeText(t)
                        color: index === 0 ? Theme.text : Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                        font.weight: index === 0 ? Font.Medium : Font.Normal
                    }
                }
            }
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
    }

    readonly property int channelColumn: 300

    // ---- Channel rows ------------------------------------------------------------
    ListView {
        id: list
        readonly property int rowHeight: 64
        anchors.top: bar.bottom
        anchors.bottom: parent.bottom
        width: parent.width
        clip: true
        // Only while open: a hidden list still builds its rows, and each
        // guide row queries the guide and builds its programmes, so every
        // change to the channels (a group, the channel list) cost seconds on a TV.
        model: guide.opened ? Channels : null
        focus: true
        highlightMoveDuration: 0
        highlightFollowsCurrentItem: false
        boundsBehavior: Flickable.StopAtBounds
        cacheBuffer: rowHeight * 4
        keyNavigationEnabled: false

        Keys.onPressed: (e) => {
            switch (e.key) {
            case Qt.Key_Up:
                if (currentIndex === 0) { guide.groupFocused = true; groupChip.forceActiveFocus() }
                else currentIndex -= 1
                break
            case Qt.Key_Down: if (currentIndex < count - 1) currentIndex += 1; break
            // The controller's LB / RB, the phone's ⏪ ⏩: the group before / after.
            case Qt.Key_AudioRewind: guide.stepGroup(-1); break
            case Qt.Key_AudioForward: guide.stepGroup(1); break
            case Qt.Key_Right: guide.moveRight(); break
            case Qt.Key_Left: guide.moveLeft(); break
            case Qt.Key_PageDown: guide.page(1); break
            case Qt.Key_PageUp: guide.page(-1); break
            case Qt.Key_Home: guide.jumpToNow(); break
            case Qt.Key_F: guide.toggleFavoriteCurrent(); break
            case Qt.Key_Slash: guide.searchRequested(); break
            case Qt.Key_H: guide.hideCurrent(); break
            case Qt.Key_B: guide.watchFromStart(); break
            case Qt.Key_R: if (e.modifiers & Qt.ShiftModifier) guide.toggleSeries(); else guide.toggleRecording(); break
            case Qt.Key_M: guide.toggleReminder(); break
            case Qt.Key_Return:
            case Qt.Key_Enter: guide.tuneCurrent(); break
            default: return
            }
            positionViewAtIndex(currentIndex, ListView.Contain)
            e.accepted = true
        }

        WheelHandler {
            acceptedModifiers: Qt.ShiftModifier
            onWheel: (w) => {
                const d = w.angleDelta.y !== 0 ? w.angleDelta.y : w.angleDelta.x
                guide.setWindow(guide.windowStart + (d < 0 ? guide.halfHour : -guide.halfHour))
                guide.focusTime = Math.max(guide.focusTime, guide.windowStart)
            }
        }

        delegate: Item {
            id: row
            width: list.width
            height: list.rowHeight

            readonly property bool isCurrent: ListView.isCurrentItem
            readonly property int globalIndex: model.globalIndex
            readonly property int number: model.number
            readonly property string name: model.name
            readonly property string epgId: model.epgId
            readonly property int catchupDays: model.catchupDays
            // Locked channel, PIN not entered: no programme details.
            readonly property bool hidden: model.locked && !Parental.unlocked
            property var progs: []

            function load() {
                if (hidden) { progs = []; return }
                const real = epgId.length ? Epg.programmes(epgId, guide.windowStart, guide.windowEnd) : []
                progs = guide.withPlaceholders(real, guide.windowStart, guide.windowEnd, name)
            }
            onHiddenChanged: load()
            function programmeAt(t) {
                for (let i = 0; i < progs.length; ++i)
                    if (progs[i].start <= t && t < progs[i].stop) return progs[i]
                return null
            }

            Component.onCompleted: load()
            onEpgIdChanged: load()
            Connections {
                target: guide
                function onWindowStartChanged() { row.load() }
            }
            Connections {
                target: Epg
                function onGuideUpdated() { row.load() }
            }

            Rectangle {
                anchors.fill: parent
                color: row.isCurrent && !guide.groupFocused ? Theme.panelSolid : "transparent"
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.raised }

            // Channel column
            Item {
                width: guide.channelColumn
                height: parent.height
                Rectangle {
                    visible: row.isCurrent && !guide.groupFocused
                    width: 3; height: parent.height - 16; x: 0
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.accent
                }
                Text {
                    x: 12
                    width: 52
                    anchors.verticalCenter: parent.verticalCenter
                    horizontalAlignment: Text.AlignRight
                    text: row.number
                    color: row.globalIndex === Player.currentIndex ? Theme.accent : Theme.textMuted
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.Medium
                }
                ChannelLogo {
                    x: 76
                    anchors.verticalCenter: parent.verticalCenter
                    width: 60
                    height: 40
                    source: model.logo
                    name: row.name
                }
                Text {
                    visible: model.favorite
                    x: guide.channelColumn - 24
                    anchors.verticalCenter: parent.verticalCenter
                    text: "★"
                    color: Theme.accent
                    font.pixelSize: 14
                }
                Text {
                    x: 148
                    width: parent.width - x - 30
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.name
                    color: row.isCurrent ? Theme.text : Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                    font.weight: row.isCurrent ? Font.Medium : Font.Normal
                    elide: Text.ElideRight
                    maximumLineCount: 2
                    wrapMode: Text.Wrap
                }
                Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border }
            }

            // Programme cells
            Item {
                x: guide.channelColumn
                width: parent.width - x
                height: parent.height
                clip: true

                Rectangle {   // channel without guide data
                    visible: row.hidden
                    x: 2; y: 4
                    width: parent.width - 4
                    height: parent.height - 8
                    radius: 4
                    readonly property bool sel: row.isCurrent && !guide.groupFocused
                    color: sel ? Theme.focus : Theme.panelSolid
                    border.width: sel ? 2 : 0
                    border.color: Theme.accent
                    Text {
                        x: 14
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("🔒 Locked · Enter and type the PIN to watch")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }
                }

                Repeater {
                    model: row.progs
                    delegate: Rectangle {
                        readonly property var p: modelData
                        readonly property real x0: Math.max(0, (p.start - guide.windowStart) * guide.pxPerMs)
                        readonly property real x1: Math.min(parent.width, (p.stop - guide.windowStart) * guide.pxPerMs)
                        readonly property bool sel: row.isCurrent && !guide.groupFocused
                                                    && p.start <= guide.focusTime && guide.focusTime < p.stop
                        readonly property bool airing: p.start <= guide.nowMs && guide.nowMs < p.stop
                        readonly property bool past: p.stop <= guide.nowMs
                        readonly property bool archived: row.catchupDays > 0 && p.start < guide.nowMs
                                                         && p.start >= guide.nowMs - row.catchupDays * 86400000

                        visible: x1 - x0 > 3
                        x: x0 + 2
                        y: 4
                        width: x1 - x0 - 4
                        height: parent.height - 8
                        radius: 4
                        color: sel ? Theme.focus : (airing ? Theme.raised : Theme.panelSolid)
                        opacity: p.placeholder && !sel ? 0.6 : 1
                        border.width: sel ? 2 : 0
                        border.color: Theme.accent

                        Column {
                            x: 12
                            width: parent.width - 20
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2
                            Text {
                                width: parent.width
                                text: (p.start < guide.windowStart ? "‹ " : "") + (archived ? "↺ " : "") + p.title
                                color: (past && !archived) || (p.placeholder && !sel) ? Theme.textMuted : (sel ? Theme.text : Theme.textSecondary)
                                font.pixelSize: Theme.fontBody - 1
                                font.weight: sel ? Font.Medium : Font.Normal
                                elide: Text.ElideRight
                            }
                            Text {
                                width: parent.width
                                visible: parent.width > 90
                                text: guide.timeText(p.start) + " – " + guide.timeText(p.stop)
                                color: Theme.textMuted
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }
                        }

                        // Scheduled recording (red dot) or reminder (clock)
                        readonly property string mark: (Schedule.revision, Schedule.markFor(row.globalIndex, p.start))
                        Rectangle {
                            visible: parent.mark === "recording"
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 7
                            width: 9; height: 9; radius: 4.5
                            color: "#E5484D"
                        }
                        Text {
                            visible: parent.mark === "reminder"
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.rightMargin: 6
                            anchors.topMargin: 2
                            text: "◷"
                            color: Theme.accent
                            font.pixelSize: 14
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                const already = parent.sel
                                list.currentIndex = list.indexAt(row.x + 1, row.y + 1)
                                guide.focusTime = Math.max(p.start, guide.windowStart)
                                guide.groupFocused = false
                                list.forceActiveFocus()
                                if (already) guide.tuneCurrent()
                            }
                            onDoubleClicked: guide.tuneCurrent()
                        }
                    }
                }
            }

            // Click on the channel name tunes after selecting.
            MouseArea {
                width: guide.channelColumn
                height: parent.height
                onClicked: {
                    const already = row.isCurrent
                    list.currentIndex = list.indexAt(row.x + 1, row.y + 1)
                    guide.groupFocused = false
                    list.forceActiveFocus()
                    if (already) guide.tuneCurrent()
                }
            }
        }
    }

    // "Now" line across the rows
    Rectangle {
        readonly property real px: guide.channelColumn + (guide.nowMs - guide.windowStart) * guide.pxPerMs
        visible: guide.nowMs >= guide.windowStart && guide.nowMs < guide.windowEnd
        x: px - 1
        y: bar.y + bar.height - 8
        width: 2
        height: guide.height - y
        color: Theme.accent
        opacity: 0.85
        Rectangle { width: 10; height: 10; radius: 5; x: -4; y: -2; color: Theme.accent }
    }
}

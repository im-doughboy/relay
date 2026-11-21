import QtQuick
import QtQuick.Controls.Basic
import Relay.Core

// Search channels (name or number), shows (title), movies and series. Type to search,
// ↓ to move into the results, Enter to watch, Esc to close.
FocusScope {
    id: root
    property bool opened: false
    signal closed()
    signal vodRequested(string kind, string id, var preview)

    property var results: []      // [{kind: "header"|"channel"|"show", ...}]
    property int channelCount: 0
    property int showCount: 0
    property int movieCount: 0
    property int seriesCount: 0

    function open() {
        opened = true
        field.text = ""
        results = []
        field.forceActiveFocus()
    }
    function close() {
        if (!opened) return
        opened = false
        closed()
    }
    function runSearch() {
        const q = field.text.trim()
        if (!q.length) { results = []; channelCount = showCount = movieCount = seriesCount = 0; return }
        const channels = Channels.searchChannels(q, 40)
        const shows = q.length >= 2 ? Channels.searchProgrammes(q, 40) : []
        const out = []
        if (channels.length) {
            out.push({ kind: "header", label: qsTr("Channels") })
            for (const c of channels) out.push(Object.assign({ kind: "channel" }, c))
        }
        if (shows.length) {
            out.push({ kind: "header", label: qsTr("Shows") })
            for (const s of shows) out.push(Object.assign({ kind: "show" }, s))
        }
        const vod = q.length >= 2 ? Vod.search(q, 20) : []
        const movies = vod.filter(v => v.kind === "movie"), series = vod.filter(v => v.kind === "series")
        if (movies.length) {
            out.push({ kind: "header", label: qsTr("Movies") })
            for (const m of movies) out.push(m)
        }
        if (series.length) {
            out.push({ kind: "header", label: qsTr("Series") })
            for (const m of series) out.push(m)
        }
        channelCount = channels.length
        showCount = shows.length
        movieCount = movies.length
        seriesCount = series.length
        results = out
        list.currentIndex = firstSelectable(0, 1)
    }
    function firstSelectable(from, dir) {
        for (let i = from; i >= 0 && i < results.length; i += dir)
            if (results[i].kind !== "header") return i
        return -1
    }
    function activate(i) {
        const r = results[i]
        if (!r || r.kind === "header") return
        if (r.kind === "movie" || r.kind === "series") {
            close()
            vodRequested(r.kind, r.id, r)
            return
        }
        Player.tune(r.globalIndex, Channels.allGroup)
        close()
    }
    function timeText(ms) {
        return Qt.formatTime(new Date(ms), Qt.locale().timeFormat(Locale.ShortFormat).replace(":ss", ""))
    }
    function whenText(r) {
        const now = Date.now()
        if (r.start <= now && now < r.stop) return qsTr("On now · until %1").arg(timeText(r.stop))
        const d = new Date(r.start), t = new Date()
        const days = Math.round((new Date(d.getFullYear(), d.getMonth(), d.getDate())
                                 - new Date(t.getFullYear(), t.getMonth(), t.getDate())) / 86400000)
        const day = days === 0 ? qsTr("Today") : days === 1 ? qsTr("Tomorrow") : Qt.formatDate(d, "ddd d MMM")
        return day + "  " + timeText(r.start)
    }

    visible: opened
    Keys.onEscapePressed: close()

    Timer { id: debounce; interval: 180; onTriggered: root.runSearch() }

    // Dim the screen; clicks outside the card close search.
    Rectangle {
        anchors.fill: parent
        color: "#B0000000"
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onClicked: root.close()
            onWheel: (w) => w.accepted = true
        }
    }

    Rectangle {
        id: card
        anchors.horizontalCenter: parent.horizontalCenter
        y: 48
        width: Math.min(880, parent.width - 64)
        height: Math.min(parent.height - 96, header.height + (results.length ? list.contentHeight + 24 : 64) + 8)
        radius: 12
        color: Theme.panelSolid
        border.width: 1
        border.color: Theme.border
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = false }

        Item {
            id: header
            width: parent.width
            height: 76

            TextField {
                id: field
                x: 20; y: 16
                width: parent.width - 40
                height: 46
                placeholderText: Vod.loaded ? qsTr("Search channels, shows, movies and series")
                                            : qsTr("Search channels and shows")
                placeholderTextColor: Theme.textMuted
                color: Theme.text
                font.pixelSize: Theme.fontLarge
                leftPadding: 44
                selectByMouse: true
                selectionColor: Theme.accent
                selectedTextColor: Theme.accentText
                background: Rectangle {
                    radius: Theme.radius
                    color: Theme.raised
                    border.width: field.activeFocus ? 2 : 1
                    border.color: field.activeFocus ? Theme.accent : Theme.border
                }
                Text {   // magnifier
                    x: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: "⌕"
                    color: Theme.textMuted
                    font.pixelSize: 22
                }
                onTextChanged: debounce.restart()
                Keys.onDownPressed: if (root.results.length) { list.forceActiveFocus(); list.currentIndex = root.firstSelectable(0, 1) }
                Keys.onReturnPressed: { debounce.stop(); root.runSearch(); root.activate(list.currentIndex) }
                Keys.onEnterPressed: { debounce.stop(); root.runSearch(); root.activate(list.currentIndex) }
                Keys.onEscapePressed: root.close()
            }
        }

        Text {
            anchors.top: header.bottom
            x: 24
            visible: field.text.trim().length > 0 && root.results.length === 0 && !debounce.running
            text: qsTr("Nothing found for \"%1\"").arg(field.text.trim())
            color: Theme.textMuted
            font.pixelSize: Theme.fontBody
        }

        ListView {
            id: list
            anchors.top: header.bottom
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 12
            width: parent.width
            clip: true
            model: root.results
            highlightMoveDuration: 0
            boundsBehavior: Flickable.StopAtBounds
            keyNavigationEnabled: false

            Keys.onPressed: (e) => {
                if (e.key === Qt.Key_Down) {
                    const n = root.firstSelectable(currentIndex + 1, 1)
                    if (n >= 0) currentIndex = n
                } else if (e.key === Qt.Key_Up) {
                    const p = root.firstSelectable(currentIndex - 1, -1)
                    if (p >= 0) currentIndex = p
                    else field.forceActiveFocus()
                } else if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter) {
                    root.activate(currentIndex)
                } else if (e.key === Qt.Key_Escape) {
                    root.close()
                } else if (e.text.length && e.text.charCodeAt(0) >= 32) {
                    // Keep typing: send the character back to the search box.
                    field.forceActiveFocus()
                    field.insert(field.cursorPosition, e.text)
                } else if (e.key === Qt.Key_Backspace) {
                    field.forceActiveFocus()
                    field.remove(field.cursorPosition - 1, field.cursorPosition)
                } else return
                positionViewAtIndex(currentIndex, ListView.Contain)
                e.accepted = true
            }

            delegate: Item {
                width: list.width
                height: modelData.kind === "header" ? 34 : 60
                readonly property bool sel: ListView.isCurrentItem && modelData.kind !== "header"

                Text {   // section header
                    visible: modelData.kind === "header"
                    x: 24
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 6
                    text: modelData.kind === "header"
                          ? modelData.label + "  " + (modelData.label === qsTr("Channels") ? root.channelCount
                                                      : modelData.label === qsTr("Shows") ? root.showCount
                                                      : modelData.label === qsTr("Movies") ? root.movieCount : root.seriesCount)
                          : ""
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                    font.weight: Font.Medium
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: 1.2
                }

                Rectangle {
                    visible: modelData.kind !== "header"
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    anchors.topMargin: 2
                    anchors.bottomMargin: 2
                    radius: 6
                    color: parent.sel ? Theme.focus : "transparent"
                    border.width: parent.sel ? 2 : 0
                    border.color: Theme.accent
                }

                Row {
                    visible: modelData.kind !== "header"
                    x: 26
                    width: parent.width - 52
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 14

                    Text {
                        width: 52
                        anchors.verticalCenter: parent.verticalCenter
                        horizontalAlignment: Text.AlignRight
                        text: modelData.number !== undefined ? modelData.number : ""
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontBody
                    }
                    ChannelLogo {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 56
                        height: modelData.kind === "movie" || modelData.kind === "series" ? 52 : 38
                        source: modelData.logo || modelData.poster || ""
                        name: modelData.kind === "show" ? (modelData.channelName || "") : (modelData.name || "")
                    }
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 52 - 56 - 28
                        spacing: 3
                        Text {
                            width: parent.width
                            text: modelData.kind === "show"
                                  ? modelData.title + (modelData.episode ? "  ·  " + modelData.episode : "")
                                  : modelData.kind === "movie" || modelData.kind === "series"
                                  ? modelData.title
                                  : (modelData.name || "")
                            color: Theme.text
                            font.pixelSize: Theme.fontBody + 1
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            text: modelData.kind === "show"
                                  ? root.whenText(modelData) + "   ·   " + modelData.channelName
                                  : modelData.kind === "movie" || modelData.kind === "series"
                                  ? [modelData.kind === "movie" ? qsTr("Movie") : qsTr("Series"), modelData.year,
                                     modelData.rating > 0 ? "★ " + Number(modelData.rating).toFixed(1) : ""].filter(s => s).join("   ·   ")
                                  : (modelData.group || "") + (modelData.nowTitle ? "   ·   " + qsTr("Now: ") + modelData.nowTitle : "")
                            color: modelData.kind === "show" && modelData.start <= Date.now() ? Theme.accent : Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            elide: Text.ElideRight
                        }
                    }
                }

                MouseArea {
                    enabled: modelData.kind !== "header"
                    anchors.fill: parent
                    onClicked: {
                        if (list.currentIndex === index) root.activate(index)
                        else { list.currentIndex = index; list.forceActiveFocus() }
                    }
                    onDoubleClicked: root.activate(index)
                }
            }
        }
    }
}

import QtQuick
import QtQuick.Controls.Basic
import Relay.Core
import Relay.Video

// Details for one movie or series: poster, plot and cast, Play / Resume /
// Start over, and for series the seasons and episodes.
FocusScope {
    id: root
    property bool opened: false
    property string kind
    property string itemId
    property var preview: ({})     // title/poster from the grid, shown while loading
    property var info: ({})        // Vod.detailsReady()
    property bool loading: false
    property string failure
    property int seasonIndex: 0
    signal closed()
    signal play(var descriptor)
    signal multiView(var descriptor)
    signal message(string text)

    readonly property bool isSeries: kind === "series"
    readonly property var seasons: info.seasons || []
    readonly property var episodes: {
        const all = info.episodes || []
        const s = seasons[seasonIndex]
        const out = []
        for (let i = 0; i < all.length; ++i)
            if (!s || all[i].season === s.number) out.push(Object.assign({ index: i }, all[i]))
        return out
    }
    readonly property var resumeEpisode: (info.episodes || [])[info.resumeIndex] || null

    function open(k, id, pre) {
        kind = k
        itemId = id
        preview = pre || ({})
        info = ({})
        failure = ""
        loading = true
        seasonIndex = 0
        opened = true
        Vod.requestDetails(k, id)
        primary.forceActiveFocus()
    }
    function close() {
        if (!opened) return
        opened = false
        closed()
    }
    // After watching: show updated progress.
    function refresh() { if (opened) Vod.requestDetails(kind, itemId) }

    function clock(secs) {
        secs = Math.max(0, Math.round(secs))
        const h = Math.floor(secs / 3600), m = Math.floor(secs % 3600 / 60), s = secs % 60
        const mm = h > 0 ? String(m).padStart(2, "0") : String(m)
        return (h > 0 ? h + ":" : "") + mm + ":" + String(s).padStart(2, "0")
    }
    function code(e) { return "S" + e.season + " E" + e.episode }
    function playMain(fromStart) {
        const d = Vod.playback(info, -1, fromStart)
        if (d.url) play(d)
    }
    // In multi-view, next to live channels (a series: the episode Play would play).
    function watchInMultiView() {
        const d = Vod.playback(info, -1, false)
        if (d.url) multiView(d)
    }
    function playEpisode(e) {
        const d = Vod.playback(info, e.index, false)
        if (d.url) play(d)
    }
    function toggleWatched(e) {
        const d = e ? Vod.playback(info, e.index, true) : Vod.playback(info, -1, true)
        const watched = e ? e.watched : info.watched
        Vod.setWatched(d, !watched)
        message(watched ? qsTr("Marked as not watched") : qsTr("Marked as watched"))
        refresh()
    }
    // The trailer, full size over this page, with sound. Esc ends it.
    property bool trailerOpen: false
    property string trailerWanted: ""
    function playTrailer() {
        if (!info.trailer) return
        if (!Trailers.available && info.trailer.indexOf("http") !== 0) {   // YouTube without yt-dlp: the browser
            Qt.openUrlExternally(Trailers.pageUrl(info.trailer))
            return
        }
        trailerWanted = info.trailer
        trailerOpen = true
        trailerBusy = true
        trailerLayer.forceActiveFocus()
        Trailers.resolve(info.trailer)
    }
    property bool trailerBusy: false
    function closeTrailer() {
        if (!trailerOpen) return
        trailerOpen = false
        trailerWanted = ""
        trailerVideo.stop()
        trailerButton.visible ? trailerButton.forceActiveFocus() : primary.forceActiveFocus()
    }
    onOpenedChanged: if (!opened) closeTrailer()
    Connections {
        target: Trailers
        function onResolved(t, video, audio) { if (t === root.trailerWanted) { root.trailerBusy = false; trailerVideo.playTrailer(video, audio) } }
        function onFailed(t, msg) {
            if (t !== root.trailerWanted) return
            root.closeTrailer()
            // No yt-dlp, or YouTube said no: the browser can still show it.
            Qt.openUrlExternally(Trailers.pageUrl(t))
            root.message(msg + " · " + qsTr("opened in the browser"))
        }
    }

    function toggleFavorite() {
        const fav = Vod.toggleFavorite(kind, itemId)
        info = Object.assign({}, info, { favorite: fav })
        message(fav ? qsTr("Added to favorites") : qsTr("Removed from favorites"))
    }

    visible: opened
    Keys.onEscapePressed: close()
    Keys.onBackPressed: close()

    Connections {
        target: Vod
        function onDetailsReady(d) {
            if (!root.opened || d.kind !== root.kind || d.id !== root.itemId) return
            const first = root.loading
            root.info = d
            root.loading = false
            if (first && root.activeFocus && !primary.activeFocus && !seasonList.activeFocus && !episodeList.activeFocus)
                primary.forceActiveFocus()   // it couldn't take focus while loading
            if (first && root.isSeries) {
                // Open on the season of the episode to continue.
                const e = (d.episodes || [])[d.resumeIndex]
                const s = d.seasons || []
                for (let i = 0; i < s.length; ++i) if (e && s[i].number === e.season) root.seasonIndex = i
                episodeList.currentIndex = Math.max(0, root.episodes.findIndex(x => x.index === d.resumeIndex))
                episodeList.positionViewAtIndex(episodeList.currentIndex, ListView.Center)
            }
        }
        function onDetailsFailed(k, id, msg) {
            if (!root.opened || k !== root.kind || id !== root.itemId) return
            root.loading = false
            root.failure = msg
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.background
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = true }
    }

    // Backdrop, faded into the background on the left and bottom.
    Image {
        id: backdrop
        anchors.right: parent.right
        width: parent.width * 0.7
        height: parent.height
        source: root.info.backdrop || ""
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        opacity: status === Image.Ready ? 0.8 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.animNormal } }
        sourceSize.width: 1280
    }
    Rectangle {
        anchors.fill: backdrop
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: Theme.background }
            GradientStop { position: 0.4; color: "#D00B0D12" }
            GradientStop { position: 0.75; color: "#500B0D12" }
            GradientStop { position: 1.0; color: "#100B0D12" }
        }
    }
    Rectangle {   // and into the page at the bottom
        anchors.bottom: backdrop.bottom
        anchors.right: backdrop.right
        width: backdrop.width
        height: backdrop.height * 0.5
        gradient: Gradient {
            GradientStop { position: 0.0; color: "transparent" }
            GradientStop { position: 1.0; color: Theme.background }
        }
    }

    Poster {
        id: poster
        x: 48
        y: 40
        width: Math.min(260, root.width * 0.2)
        height: Math.round(width * 1.5)
        source: root.info.poster || root.preview.poster || ""
        title: root.info.title || root.preview.title || ""
        favorite: !!root.info.favorite
        watched: !!root.info.watched
    }

    Column {
        id: textCol
        anchors.left: poster.right
        anchors.leftMargin: 40
        anchors.right: parent.right
        anchors.rightMargin: 48
        y: 40
        spacing: 12

        Text {
            width: parent.width
            text: root.info.title || root.preview.title || ""
            color: Theme.text
            font.pixelSize: Theme.fontDisplay - 6
            font.weight: Font.DemiBold
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
        }
        Text {
            width: parent.width
            visible: text.length > 0
            text: [root.info.year || root.preview.year,
                   root.info.genre,
                   root.info.duration,
                   root.info.rating > 0 ? "★ " + Number(root.info.rating).toFixed(1) : "",
                   root.isSeries && root.seasons.length ? (root.seasons.length === 1 ? qsTr("1 season") : qsTr("%1 seasons").arg(root.seasons.length)) : ""]
                  .filter(s => s).join("   ·   ")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
            elide: Text.ElideRight
        }
        Text {
            width: parent.width
            visible: text.length > 0
            text: root.info.plot || ""
            color: Theme.text
            opacity: 0.9
            font.pixelSize: Theme.fontBody
            lineHeight: 1.25
            wrapMode: Text.Wrap
            maximumLineCount: root.isSeries ? 3 : 7
            elide: Text.ElideRight
        }
        Text {
            width: parent.width
            visible: text.length > 0
            text: [root.info.cast ? qsTr("Cast: %1").arg(root.info.cast) : "",
                   root.info.director ? qsTr("Director: %1").arg(root.info.director) : ""].filter(s => s).join("\n")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall + 1
            wrapMode: Text.Wrap
            maximumLineCount: root.isSeries ? 1 : 3
            elide: Text.ElideRight
        }

        Item { width: 1; height: 4 }

        Row {
            id: buttons
            spacing: 10

            ActionButton {
                id: primary
                primary: true
                enabled: !root.loading && !root.failure
                busy: root.loading
                text: {
                    if (root.isSeries) {
                        const e = root.resumeEpisode
                        if (!e) return qsTr("Play")
                        if (e.pos > 0 && !e.watched) return qsTr("Resume %1 · %2").arg(root.code(e)).arg(root.clock(e.pos))
                        return qsTr("Play %1").arg(root.code(e))
                    }
                    return root.info.pos > 0 && !root.info.watched
                           ? qsTr("Resume from %1").arg(root.clock(root.info.pos)) : qsTr("Play")
                }
                onClicked: root.playMain(false)
                KeyNavigation.right: startOver.visible ? startOver : trailerButton.visible ? trailerButton : favButton
                Keys.onDownPressed: if (root.isSeries) (seasonList.count > 1 ? seasonList : episodeList).forceActiveFocus()
            }
            ActionButton {
                id: startOver
                visible: !root.isSeries && (root.info.pos > 0 || !!root.info.watched)
                text: qsTr("Start over")
                onClicked: root.playMain(true)
                KeyNavigation.left: primary
                KeyNavigation.right: trailerButton.visible ? trailerButton : favButton
            }
            ActionButton {
                id: trailerButton
                visible: !!root.info.trailer
                text: qsTr("▶  Trailer")
                onClicked: root.playTrailer()
                KeyNavigation.left: startOver.visible ? startOver : primary
                KeyNavigation.right: favButton
            }
            ActionButton {
                id: favButton
                enabled: !root.loading
                text: root.info.favorite ? qsTr("★  Favorite") : qsTr("☆  Add to favorites")
                onClicked: root.toggleFavorite()
                KeyNavigation.left: trailerButton.visible ? trailerButton : startOver.visible ? startOver : primary
                KeyNavigation.right: watchedButton.visible ? watchedButton : multiButton
                Keys.onDownPressed: if (root.isSeries) (seasonList.count > 1 ? seasonList : episodeList).forceActiveFocus()
            }
            ActionButton {
                id: watchedButton
                visible: !root.isSeries && !root.loading && !root.failure
                text: root.info.watched ? qsTr("Mark not watched") : qsTr("Mark watched")
                onClicked: root.toggleWatched(null)
                KeyNavigation.left: favButton
                KeyNavigation.right: multiButton
            }
            ActionButton {
                id: multiButton
                enabled: !root.loading && !root.failure
                text: qsTr("⊞  Multi-view")
                onClicked: root.watchInMultiView()
                KeyNavigation.left: watchedButton.visible ? watchedButton : favButton
                Keys.onDownPressed: if (root.isSeries) (seasonList.count > 1 ? seasonList : episodeList).forceActiveFocus()
            }
        }

        Text {
            visible: root.failure.length > 0
            width: parent.width
            text: root.failure
            color: Theme.danger
            font.pixelSize: Theme.fontBody
            wrapMode: Text.Wrap
        }
    }

    // ---- Seasons and episodes (series) -----------------------------------------
    ListView {
        id: seasonList
        visible: root.isSeries && count > 1
        anchors.left: textCol.left
        anchors.right: textCol.right
        anchors.top: textCol.bottom
        anchors.topMargin: 22
        height: visible ? 40 : 0
        orientation: ListView.Horizontal
        spacing: 8
        clip: true
        model: root.seasons
        currentIndex: root.seasonIndex
        keyNavigationEnabled: false
        highlightMoveDuration: 0
        boundsBehavior: Flickable.StopAtBounds

        Keys.onPressed: (e) => {
            switch (e.key) {
            case Qt.Key_Left: if (root.seasonIndex > 0) { root.seasonIndex--; episodeList.currentIndex = 0 } break
            case Qt.Key_Right: if (root.seasonIndex < count - 1) { root.seasonIndex++; episodeList.currentIndex = 0 } break
            case Qt.Key_Up: primary.forceActiveFocus(); break
            case Qt.Key_Down:
            case Qt.Key_Return:
            case Qt.Key_Enter: episodeList.forceActiveFocus(); break
            case Qt.Key_Escape:
            case Qt.Key_Backspace: root.close(); break
            default: return
            }
            positionViewAtIndex(root.seasonIndex, ListView.Contain)
            e.accepted = true
        }

        delegate: Rectangle {
            readonly property bool current: index === root.seasonIndex
            height: 40
            width: seasonText.implicitWidth + 32
            radius: 20
            color: current ? (seasonList.activeFocus ? Theme.accent : Theme.focus) : Theme.raised
            border.width: current && !seasonList.activeFocus ? 1 : 0
            border.color: Theme.accent
            Text {
                id: seasonText
                anchors.centerIn: parent
                text: modelData.name
                color: parent.current && seasonList.activeFocus ? Theme.accentText : Theme.text
                font.pixelSize: Theme.fontSmall + 1
                font.weight: Font.Medium
            }
            MouseArea {
                anchors.fill: parent
                onClicked: { root.seasonIndex = index; episodeList.currentIndex = 0; seasonList.forceActiveFocus() }
            }
        }
    }

    ListView {
        id: episodeList
        visible: root.isSeries
        anchors.left: textCol.left
        anchors.right: textCol.right
        anchors.top: seasonList.visible ? seasonList.bottom : textCol.bottom
        anchors.topMargin: 16
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 48
        clip: true
        model: root.episodes
        keyNavigationEnabled: false
        highlightMoveDuration: 0
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Keys.onPressed: (e) => {
            switch (e.key) {
            case Qt.Key_Up:
                if (currentIndex > 0) currentIndex--
                else (seasonList.visible ? seasonList : primary).forceActiveFocus()
                break
            case Qt.Key_Down: if (currentIndex < count - 1) currentIndex++; break
            case Qt.Key_PageUp: currentIndex = Math.max(0, currentIndex - 5); break
            case Qt.Key_PageDown: currentIndex = Math.min(count - 1, currentIndex + 5); break
            case Qt.Key_Left:
                if (root.seasonIndex > 0) { root.seasonIndex--; currentIndex = 0 }
                break
            case Qt.Key_Right:
                if (root.seasonIndex < root.seasons.length - 1) { root.seasonIndex++; currentIndex = 0 }
                break
            case Qt.Key_Return:
            case Qt.Key_Enter: if (root.episodes[currentIndex]) root.playEpisode(root.episodes[currentIndex]); break
            case Qt.Key_W: if (root.episodes[currentIndex]) root.toggleWatched(root.episodes[currentIndex]); break
            case Qt.Key_F: root.toggleFavorite(); break
            case Qt.Key_Escape:
            case Qt.Key_Backspace: root.close(); break
            default: return
            }
            positionViewAtIndex(currentIndex, ListView.Contain)
            e.accepted = true
        }

        delegate: Rectangle {
            readonly property bool sel: ListView.isCurrentItem && episodeList.activeFocus
            readonly property bool upNext: modelData.index === root.info.resumeIndex
            width: episodeList.width - 12
            height: 78
            radius: 8
            color: sel ? Theme.focus : (epMouse.containsMouse ? Theme.raised : "transparent")
            border.width: sel ? 2 : 0
            border.color: Theme.accent

            Rectangle {   // still / number
                id: thumb
                x: 10
                anchors.verticalCenter: parent.verticalCenter
                width: 104
                height: 58
                radius: 6
                color: Theme.raised
                clip: true
                Text {
                    anchors.centerIn: parent
                    visible: thumbImg.status !== Image.Ready
                    text: "E" + modelData.episode
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontLarge
                    font.weight: Font.DemiBold
                }
                Image {
                    id: thumbImg
                    anchors.fill: parent
                    source: modelData.poster || ""
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    sourceSize.width: 240
                }
                Rectangle {
                    visible: modelData.dur > 0 && !modelData.watched
                    anchors.bottom: parent.bottom
                    width: parent.width * Math.min(1, modelData.pos / Math.max(1, modelData.dur))
                    height: 4
                    color: Theme.accent
                }
            }
            Column {
                anchors.left: thumb.right
                anchors.leftMargin: 16
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4
                Row {
                    width: parent.width
                    spacing: 10
                    Text {
                        id: epTitle
                        width: Math.min(implicitWidth, parent.width - epMeta.implicitWidth - 20)
                        text: root.code(modelData) + "   " + (modelData.title || qsTr("Episode %1").arg(modelData.episode))
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                    }
                    Text {
                        id: epMeta
                        anchors.verticalCenter: epTitle.verticalCenter
                        text: modelData.watched ? qsTr("✓ Watched")
                              : modelData.pos > 0 ? qsTr("%1 left").arg(root.clock(Math.max(0, modelData.dur - modelData.pos)))
                              : upNext && root.info.started ? qsTr("Up next")
                              : (modelData.duration || "")
                        color: modelData.watched || modelData.pos > 0 || (upNext && root.info.started) ? Theme.accent : Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }
                }
                Text {
                    width: parent.width
                    visible: text.length > 0
                    text: modelData.plot || ""
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }
            }
            MouseArea {
                id: epMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: { episodeList.currentIndex = index; episodeList.forceActiveFocus() }
                onDoubleClicked: root.playEpisode(modelData)
            }
        }
    }

    BusyIndicator {
        visible: root.loading && root.isSeries
        running: visible
        anchors.horizontalCenter: episodeList.horizontalCenter
        y: episodeList.y + 40
        palette.dark: Theme.accent
    }

    Text {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 18
        text: root.isSeries ? qsTr("Enter: play episode   ·   ← →: season   ·   W: watched   ·   Esc: back")
                            : qsTr("Enter: play   ·   Esc: back")
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
    }

    // Back to the rows (the same as Esc), for the mouse.
    CloseButton {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 24
        z: 10
        visible: !root.trailerOpen
        label: qsTr("Back")
        onClicked: root.close()
    }

    // ---- Trailer over the page ----------------------------------------------------
    FocusScope {
        id: trailerLayer
        anchors.fill: parent
        visible: root.trailerOpen
        z: 50
        Keys.onEscapePressed: root.closeTrailer()
        Keys.onBackPressed: root.closeTrailer()
        Keys.onSpacePressed: trailerVideo.togglePause()
        Keys.onPressed: (e) => { if (e.key === Qt.Key_Backspace || e.key === Qt.Key_MediaTogglePlayPause) { e.key === Qt.Key_Backspace ? root.closeTrailer() : trailerVideo.togglePause(); e.accepted = true } }
        Rectangle { anchors.fill: parent; color: "black" }
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onClicked: trailerVideo.togglePause(); onWheel: (w) => w.accepted = true }
        MpvVideo {
            id: trailerVideo
            anchors.fill: parent
            timeshiftMb: 0
            volume: 90
            onFinished: root.closeTrailer()
            onPlaybackError: (m) => { root.message(qsTr("The trailer didn't play")); root.closeTrailer() }
        }
        BusyIndicator { anchors.centerIn: parent; running: root.trailerBusy || trailerVideo.buffering }
        Text {
            x: 32; y: 28
            text: (root.info.title || "") + "  ·  " + qsTr("Trailer")
            color: "white"
            font.pixelSize: Theme.fontLarge
            font.weight: Font.DemiBold
            style: Text.Raised; styleColor: "#80000000"
        }
        CloseButton {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 24
            label: qsTr("Close the trailer")
            onClicked: root.closeTrailer()
        }
    }
}

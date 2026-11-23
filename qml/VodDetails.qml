import QtQuick
import QtQuick.Controls.Basic
import Relay.Core
import Relay.Video

// One movie or series. What it is on the left (its logo or name, year,
// genre, length, age rating, plot, cast), a list of actions under it, and
// its backdrop on the right, where the trailer starts playing (muted) after
// a moment. Left alone for a while, the text steps back to let the trailer
// show; any key brings it back.
//
// A series' episodes open on their own page (seasons on the left), and
// Credits & more info lists everyone involved.
FocusScope {
    id: root
    property bool opened: false
    property string kind
    property string itemId
    property var preview: ({})     // title/poster from the rows, shown while loading
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
    readonly property var shown: Object.assign({}, preview, info)   // the preview until the details are here

    readonly property real side: Math.round(Math.max(32, width * 0.04))
    readonly property int font1: Math.round(Math.max(Theme.fontBody, Math.min(24, height * 0.024)))

    function open(k, id, pre) {
        kind = k
        itemId = id
        preview = pre || ({})
        info = ({})
        failure = ""
        loading = true
        seasonIndex = 0
        actions.at = 0
        episodesPage.opened = false
        credits.close()
        opened = true
        Vod.requestDetails(k, id)
        actions.forceActiveFocus()
        wake()
    }
    function close() {
        if (!opened) return
        stopBackgroundTrailer()
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
    // From the best source (several playlists may have it); the player
    // starts when Vod.playbackReady() arrives.
    function playMain(fromStart) { Vod.startPlayback(info, -1, fromStart) }
    readonly property var sourceList: (info.id, info.key, Vod.sources(kind, itemId))
    function watchInMultiView() {
        const d = Vod.playback(info, -1, false)
        if (d.url) multiView(d)
    }
    function playEpisode(e) { Vod.startPlayback(info, e.index, false) }
    // Up next: after what's playing (here or on the TV).
    function addToQueue(episodeIndex) {
        const d = Vod.playback(info, episodeIndex, false)
        if (!d.url) return
        const name = d.kind === "episode" ? d.subtitle : d.title
        message(Vod.enqueue(d) ? qsTr("Added to the queue: %1").arg(name) : qsTr("Already in the queue"))
    }
    function toggleWatched(e) {
        const d = e ? Vod.playback(info, e.index, true) : Vod.playback(info, -1, true)
        const watched = e ? e.watched : info.watched
        Vod.setWatched(d, !watched)
        message(watched ? qsTr("Marked as not watched") : qsTr("Marked as watched"))
        refresh()
    }
    function toggleFavorite() {
        const fav = Vod.toggleFavorite(kind, itemId)
        info = Object.assign({}, info, { favorite: fav })
        message(fav ? qsTr("Added to favorites") : qsTr("Removed from favorites"))
    }

    // ---- The actions, top to bottom --------------------------------------------------
    readonly property var actionList: {
        if (loading && !info.id) return [{ id: "wait", label: qsTr("Loading…"), icon: "" }]
        if (failure) return [{ id: "back", label: qsTr("Back"), icon: Icons.back }]
        const out = []
        if (isSeries) {
            const e = resumeEpisode
            const resume = e && e.pos > 0 && !e.watched
            out.push({ id: "play", icon: Icons.play, filled: true,
                       label: !e ? qsTr("Play") : resume ? qsTr("Resume %1").arg(code(e)) : qsTr("Play %1").arg(code(e)),
                       progress: resume && e.dur > 0 ? e.pos / e.dur : -1 })
            if (seasons.length || (info.episodes || []).length) out.push({ id: "episodes", icon: Icons.channels, label: qsTr("Episodes") })
        } else {
            const resume = info.pos > 0 && !info.watched
            out.push({ id: "play", icon: Icons.play, filled: true,
                       label: resume ? qsTr("Resume from %1").arg(clock(info.pos)) : qsTr("Play"),
                       progress: resume && info.dur > 0 ? info.pos / info.dur : -1 })
            if (info.pos > 0 || info.watched) out.push({ id: "fromStart", icon: Icons.fromStart, label: qsTr("Play from the beginning") })
        }
        if (sourceList.length > 1) {
            const best = sourceList.find(x => x.best) || sourceList[0]
            out.push({ id: "source", icon: Icons.playlists,
                       label: qsTr("Source: %1 (%2 in all)").arg(best.name + (best.quality ? " · " + best.quality : "")).arg(sourceList.length) })
        }
        if (info.trailer) out.push({ id: "trailer", icon: Icons.movies, label: qsTr("Watch the trailer") })
        out.push({ id: "queue", icon: Icons.plus,
                   label: isSeries ? qsTr("Add %1 to the queue").arg(resumeEpisode ? code(resumeEpisode) : qsTr("the next episode"))
                                   : qsTr("Add to the queue") + (Vod.queue.length ? "  (" + Vod.queue.length + ")" : "") })
        out.push({ id: "favorite", icon: Icons.heart, filled: !!info.favorite,
                   label: info.favorite ? qsTr("In your favorites") : qsTr("Add to favorites") })
        if (!isSeries) out.push({ id: "watched", icon: Icons.recordings, label: info.watched ? qsTr("Mark as not watched") : qsTr("Mark as watched") })
        out.push({ id: "multiview", icon: Icons.multiview, label: qsTr("Watch in multi-view") })
        out.push({ id: "credits", icon: Icons.info, label: qsTr("Credits & more info") })
        return out
    }
    function run(id) {
        switch (id) {
        case "play": playMain(false); break
        case "fromStart": playMain(true); break
        case "episodes": episodesPage.show(); break
        case "trailer": playTrailer(); break
        case "queue": addToQueue(-1); break
        case "favorite": toggleFavorite(); break
        case "watched": toggleWatched(null); break
        case "multiview": watchInMultiView(); break
        case "source": sourcePicker.open(root.sourceList, qsTr("Play from"), -1, -1); break
        case "credits": credits.open(Object.assign({ kind: kind }, preview), info); break
        case "back": close(); break
        }
    }

    // ---- Background trailer, and the text stepping back while it plays ------------------
    property string bgTrailerFor: ""
    property bool bgMuted: true
    property bool resting: false           // left alone: just the title over the trailer
    function wake() { resting = false; restTimer.restart() }
    function startBackgroundTrailer() {
        if (!opened || !info.trailer || !Trailers.autoplay || trailerOpen || episodesPage.opened || credits.opened) return
        if (!Trailers.available && info.trailer.indexOf("http") !== 0) return
        bgTrailerFor = info.trailer
        Trailers.resolve(info.trailer)
    }
    function stopBackgroundTrailer() {
        bgTimer.stop()
        if (bgTrailerFor !== "") { bgTrailerFor = ""; bgTrailer.stop() }
    }
    Timer { id: bgTimer; interval: 2200; onTriggered: root.startBackgroundTrailer() }
    Timer { id: restTimer; interval: 10000; onTriggered: if (bgTrailer.playing && root.bgTrailerFor !== "") root.resting = true }

    // ---- Full-screen trailer with sound (Watch the trailer) -------------------------------
    property bool trailerOpen: false
    property string trailerWanted: ""
    property bool trailerBusy: false
    function playTrailer() {
        if (!info.trailer) return
        if (!Trailers.available && info.trailer.indexOf("http") !== 0) {   // YouTube without yt-dlp: the browser
            Qt.openUrlExternally(Trailers.pageUrl(info.trailer))
            return
        }
        stopBackgroundTrailer()
        trailerWanted = info.trailer
        trailerOpen = true
        trailerBusy = true
        trailerLayer.forceActiveFocus()
        Trailers.resolve(info.trailer)
    }
    function closeTrailer() {
        if (!trailerOpen) return
        trailerOpen = false
        trailerWanted = ""
        trailerVideo.stop()
        actions.forceActiveFocus()
        wake()
    }
    onOpenedChanged: if (!opened) closeTrailer()
    Connections {
        target: Trailers
        function onResolved(t, video, audio) {
            if (t === root.trailerWanted) { root.trailerBusy = false; trailerVideo.playTrailer(video, audio) }
            else if (t === root.bgTrailerFor) bgTrailer.playTrailer(video, audio)
        }
        function onFailed(t, msg) {
            if (t === root.bgTrailerFor) { root.bgTrailerFor = ""; return }
            if (t !== root.trailerWanted) return
            root.closeTrailer()
            // No yt-dlp, or YouTube said no: the browser can still show it.
            Qt.openUrlExternally(Trailers.pageUrl(t))
            root.message(msg + " · " + qsTr("opened in the browser"))
        }
    }

    visible: opened

    Connections {
        target: Vod
        function onDetailsReady(d) {
            if (!root.opened || d.kind !== root.kind || d.id !== root.itemId) return
            const first = root.loading
            root.info = d
            root.loading = false
            if (first) {
                actions.at = 0
                if (d.trailer && Trailers.autoplay) bgTimer.restart()
            }
            if (first && root.isSeries) {
                // Open on the season of the episode to continue.
                const e = (d.episodes || [])[d.resumeIndex]
                const s = d.seasons || []
                for (let i = 0; i < s.length; ++i) if (e && s[i].number === e.season) root.seasonIndex = i
                episodeList.currentIndex = Math.max(0, root.episodes.findIndex(x => x.index === d.resumeIndex))
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
        color: "black"
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            hoverEnabled: true
            onPositionChanged: root.wake()
            onWheel: (w) => w.accepted = true
        }
    }

    // ---- Backdrop and background trailer, on the right -----------------------------------
    Item {
        id: stage
        anchors.right: parent.right
        width: parent.width * (root.resting ? 1 : 0.72)
        height: parent.height
        Behavior on width { NumberAnimation { duration: 900; easing.type: Easing.InOutCubic } }
        Image {
            id: backdrop
            anchors.fill: parent
            source: root.shown.backdrop || ""
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            sourceSize.width: 1920
            opacity: status === Image.Ready && !(bgTrailer.playing && root.bgTrailerFor !== "") ? 0.9 : 0
            Behavior on opacity { NumberAnimation { duration: 600 } }
        }
        Image {   // no backdrop: the poster, soft
            anchors.fill: parent
            visible: backdrop.status !== Image.Ready
            source: root.shown.poster || ""
            fillMode: Image.PreserveAspectCrop
            sourceSize.width: 400
            asynchronous: true
            opacity: 0.35
        }
        MpvVideo {
            id: bgTrailer
            anchors.fill: parent
            timeshiftMb: 0
            pictureShape: "fill"      // cropped to the card, no black bars
            muted: root.bgMuted
            volume: 70
            opacity: playing && root.bgTrailerFor !== "" ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 800 } }
            onFinished: { root.bgTrailerFor = ""; root.wake() }
            onPlaybackError: root.bgTrailerFor = ""
        }
        // Into black on the left and at the bottom.
        Rectangle {
            anchors.fill: parent
            opacity: root.resting ? 0.35 : 1
            Behavior on opacity { NumberAnimation { duration: 900 } }
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: "black" }
                GradientStop { position: 0.3; color: "#B0000000" }
                GradientStop { position: 0.65; color: "#20000000" }
                GradientStop { position: 1.0; color: "transparent" }
            }
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: parent.height * 0.35
            gradient: Gradient {
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 1.0; color: "#C0000000" }
            }
        }
    }

    // ---- What it is, and the actions --------------------------------------------------------
    Column {
        id: about
        x: root.side
        y: Math.round(root.height * 0.08)
        width: Math.min(root.width * 0.44, 820)
        spacing: Math.round(root.font1 * 0.55)

        TitleArt {
            item: root.shown.title ? { title: root.shown.title } : null
            info: root.info
            maxWidth: about.width * 0.75
            maxHeight: root.height * 0.17
            fontSize: Math.round(Math.max(34, Math.min(68, root.height * 0.07)))
        }
        // Year · genre · length (or seasons) · age rating · stars
        Row {
            spacing: Math.round(root.font1 * 0.6)
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: [root.shown.year,
                       root.info.genre ? root.info.genre.split(",")[0].trim() : "",
                       root.isSeries ? (root.seasons.length ? (root.seasons.length === 1 ? qsTr("1 season") : qsTr("%1 seasons").arg(root.seasons.length)) : "")
                                     : root.info.duration || ""].filter(x => x).join("  •  ")
                color: Theme.text
                font.pixelSize: root.font1 - 2
            }
            Rectangle {
                visible: !!root.info.certification
                anchors.verticalCenter: parent.verticalCenter
                width: certText.implicitWidth + 12
                height: certText.implicitHeight + 4
                radius: 3
                color: "transparent"
                border.width: 1
                border.color: Theme.textSecondary
                Text { id: certText; anchors.centerIn: parent; text: root.info.certification || ""; color: Theme.text; font.pixelSize: root.font1 - 5; font.weight: Font.DemiBold }
            }
            Text {
                visible: root.shown.rating > 0
                anchors.verticalCenter: parent.verticalCenter
                text: "★ " + Number(root.shown.rating || 0).toFixed(1)
                color: Theme.accent
                font.pixelSize: root.font1 - 2
                font.weight: Font.DemiBold
            }
        }

        Column {
            id: extra
            width: parent.width
            spacing: parent.spacing
            opacity: root.resting ? 0 : 1
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: 700 } }

            Text {   // the episode up next
                width: parent.width
                visible: root.isSeries && !!root.resumeEpisode
                text: root.resumeEpisode ? root.code(root.resumeEpisode) + "  " + (root.resumeEpisode.title || "") : ""
                color: Theme.text
                font.pixelSize: root.font1
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                visible: text.length > 0
                text: (root.isSeries && root.resumeEpisode && root.resumeEpisode.plot) ? root.resumeEpisode.plot : (root.info.plot || "")
                color: Theme.text
                opacity: 0.92
                font.pixelSize: root.font1
                lineHeight: 1.2
                wrapMode: Text.Wrap
                maximumLineCount: root.height > 760 ? 4 : 3
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                visible: text.length > 0
                text: [root.info.cast ? qsTr("Cast: %1").arg(root.info.cast) : "",
                       root.info.director ? (root.isSeries ? qsTr("Created by: %1") : qsTr("Director: %1")).arg(root.info.director) : ""].filter(s => s).join("\n")
                color: Theme.textSecondary
                font.pixelSize: root.font1 - 4
                lineHeight: 1.2
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }
            Text {
                visible: root.failure.length > 0
                width: parent.width
                text: root.failure
                color: Theme.danger
                font.pixelSize: root.font1 - 1
                wrapMode: Text.Wrap
            }
            Item { width: 1; height: Math.round(root.font1 * 0.4) }

            // The actions: ↑ ↓ choose, Enter does it.
            FocusScope {
                id: actions
                width: Math.min(about.width * 0.72, 460)
                height: actionColumn.height
                property int at: 0
                readonly property int count: root.actionList.length
                Keys.onPressed: (e) => {
                    // Resting (only the title over the trailer): the first key just brings the page back.
                    const wasResting = root.resting
                    root.wake()
                    if (wasResting && e.key !== Qt.Key_Escape && e.key !== Qt.Key_Back && e.key !== Qt.Key_Backspace) { e.accepted = true; return }
                    switch (e.key) {
                    case Qt.Key_Up: at = Math.max(0, at - 1); break
                    case Qt.Key_Down: at = Math.min(count - 1, at + 1); break
                    case Qt.Key_Return:
                    case Qt.Key_Enter:
                    case Qt.Key_Select:
                        if (root.actionList[at]) root.run(root.actionList[at].id); break
                    case Qt.Key_P: root.playMain(false); break
                    case Qt.Key_F: root.toggleFavorite(); break
                    case Qt.Key_M: root.bgMuted = !root.bgMuted; break
                    case Qt.Key_E: if (root.isSeries) episodesPage.show(); break
                    case Qt.Key_Escape:
                    case Qt.Key_Back:
                    case Qt.Key_Backspace: root.close(); break
                    default: return
                    }
                    e.accepted = true
                }
                Column {
                    id: actionColumn
                    width: parent.width
                    spacing: 2
                    Repeater {
                        model: root.actionList
                        delegate: PillButton {
                            required property var modelData
                            required property int index
                            width: actionColumn.width
                            wide: true
                            icon: modelData.icon || ""
                            iconFilled: !!modelData.filled
                            text: modelData.label
                            progress: modelData.progress !== undefined ? modelData.progress : -1
                            fontSize: root.font1 - 1
                            enabled: modelData.id !== "wait"
                            lit: actions.activeFocus && actions.at === index
                            onClicked: { actions.at = index; actions.forceActiveFocus(); root.run(modelData.id) }
                        }
                    }
                }
            }
        }
    }

    // Trailer sound (when one is playing behind), top right.
    PillButton {
        anchors.right: backButton.left
        anchors.rightMargin: 12
        anchors.verticalCenter: backButton.verticalCenter
        visible: bgTrailer.playing && root.bgTrailerFor !== "" && !root.trailerOpen
        icon: root.bgMuted ? Icons.muted : Icons.volume
        fontSize: Theme.fontBody
        onClicked: root.bgMuted = !root.bgMuted
    }
    // Back to the rows (the same as Esc), for the mouse.
    CloseButton {
        id: backButton
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 24
        z: 10
        visible: !root.trailerOpen && !episodesPage.opened && !credits.opened
        label: qsTr("Back")
        onClicked: root.close()
    }
    Text {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 18
        opacity: root.resting ? 0 : 1
        text: root.isSeries ? qsTr("↑ ↓ choose   ·   Enter: do it   ·   E: episodes   ·   M: trailer sound   ·   Esc: back")
                            : qsTr("↑ ↓ choose   ·   Enter: do it   ·   M: trailer sound   ·   Esc: back")
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
    }

    // ---- Episodes (series): seasons on the left, episodes on the right ------------------------
    FocusScope {
        id: episodesPage
        anchors.fill: parent
        visible: opened
        z: 30
        property bool opened: false
        function show() {
            root.stopBackgroundTrailer()
            opened = true
            episodeList.forceActiveFocus()
            episodeList.positionViewAtIndex(episodeList.currentIndex, ListView.Center)
        }
        function hide() {
            opened = false
            actions.forceActiveFocus()
            root.wake()
        }
        Keys.onEscapePressed: hide()
        Keys.onBackPressed: hide()

        Rectangle {
            anchors.fill: parent
            color: "black"
            MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = true }
        }

        Column {
            id: seasonCol
            x: root.side
            y: Math.round(root.height * 0.08)
            width: Math.min(root.width * 0.26, 380)
            spacing: Math.round(root.font1 * 0.5)
            TitleArt {
                item: root.shown.title ? { title: root.shown.title } : null
                info: root.info
                maxWidth: seasonCol.width * 0.85
                maxHeight: root.height * 0.11
                fontSize: Math.round(root.font1 * 1.7)
            }
            Item { width: 1; height: root.font1 }
            ListView {
                id: seasonList
                width: parent.width
                height: Math.min(contentHeight, root.height - y - seasonCol.y - root.side)
                spacing: 2
                clip: true
                model: root.seasons
                currentIndex: root.seasonIndex
                keyNavigationEnabled: false
                boundsBehavior: Flickable.StopAtBounds
                Keys.onPressed: (e) => {
                    switch (e.key) {
                    case Qt.Key_Up: if (root.seasonIndex > 0) { root.seasonIndex--; episodeList.currentIndex = 0 } break
                    case Qt.Key_Down: if (root.seasonIndex < count - 1) { root.seasonIndex++; episodeList.currentIndex = 0 } break
                    case Qt.Key_Right:
                    case Qt.Key_Return:
                    case Qt.Key_Enter: episodeList.forceActiveFocus(); break
                    case Qt.Key_Left:
                    case Qt.Key_Backspace: episodesPage.hide(); break
                    default: return
                    }
                    positionViewAtIndex(root.seasonIndex, ListView.Contain)
                    e.accepted = true
                }
                delegate: PillButton {
                    required property var modelData
                    required property int index
                    width: seasonList.width
                    wide: true
                    text: modelData.name
                    fontSize: root.font1 - 1
                    lit: seasonList.activeFocus && index === root.seasonIndex
                    color: lit ? Theme.text : (index === root.seasonIndex ? "#33FFFFFF" : (hovered ? "#22FFFFFF" : "transparent"))
                    onClicked: { root.seasonIndex = index; episodeList.currentIndex = 0; seasonList.forceActiveFocus() }
                }
            }
        }

        ListView {
            id: episodeList
            anchors.left: seasonCol.right
            anchors.leftMargin: root.side
            anchors.right: parent.right
            anchors.rightMargin: root.side
            y: seasonCol.y
            height: root.height - y - root.side
            clip: true
            spacing: 6
            model: root.episodes
            keyNavigationEnabled: false
            highlightMoveDuration: 0
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            Keys.onPressed: (e) => {
                switch (e.key) {
                case Qt.Key_Up: if (currentIndex > 0) currentIndex--; break
                case Qt.Key_Down: if (currentIndex < count - 1) currentIndex++; break
                case Qt.Key_PageUp: currentIndex = Math.max(0, currentIndex - 5); break
                case Qt.Key_PageDown: currentIndex = Math.min(count - 1, currentIndex + 5); break
                case Qt.Key_Left: (seasonList.count > 1 ? seasonList.forceActiveFocus() : episodesPage.hide()); break
                case Qt.Key_Return:
                case Qt.Key_Enter: if (root.episodes[currentIndex]) root.playEpisode(root.episodes[currentIndex]); break
                case Qt.Key_W: if (root.episodes[currentIndex]) root.toggleWatched(root.episodes[currentIndex]); break
                case Qt.Key_Q: if (root.episodes[currentIndex]) root.addToQueue(root.episodes[currentIndex].index); break
                case Qt.Key_F: root.toggleFavorite(); break
                case Qt.Key_Backspace: episodesPage.hide(); break
                default: return
                }
                positionViewAtIndex(currentIndex, ListView.Contain)
                e.accepted = true
            }

            delegate: Rectangle {
                id: ep
                required property var modelData
                required property int index
                readonly property bool sel: ListView.isCurrentItem && episodeList.activeFocus
                readonly property bool upNext: modelData.index === root.info.resumeIndex
                width: episodeList.width - 12
                height: Math.round(root.font1 * 6.2)
                radius: 10
                color: sel ? "#26FFFFFF" : (epMouse.containsMouse ? "#14FFFFFF" : "transparent")
                border.width: sel ? 2 : 0
                border.color: Theme.text

                Rectangle {   // still / number
                    id: thumb
                    x: 10
                    anchors.verticalCenter: parent.verticalCenter
                    height: parent.height - 20
                    width: Math.round(height * 16 / 9)
                    radius: 6
                    color: Theme.raised
                    clip: true
                    Text {
                        anchors.centerIn: parent
                        visible: thumbImg.status !== Image.Ready
                        text: "E" + ep.modelData.episode
                        color: Theme.textMuted
                        font.pixelSize: root.font1 + 2
                        font.weight: Font.DemiBold
                    }
                    Image {
                        id: thumbImg
                        anchors.fill: parent
                        source: ep.modelData.poster || ""
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        sourceSize.width: 320
                    }
                    Rectangle {
                        visible: ep.modelData.dur > 0 && !ep.modelData.watched && ep.modelData.pos > 0
                        anchors.bottom: parent.bottom
                        width: parent.width * Math.min(1, ep.modelData.pos / Math.max(1, ep.modelData.dur))
                        height: 4
                        color: Theme.accent
                    }
                }
                Column {
                    anchors.left: thumb.right
                    anchors.leftMargin: 18
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 5
                    Row {
                        width: parent.width
                        spacing: 12
                        Text {
                            id: epTitle
                            width: Math.min(implicitWidth, parent.width - epMeta.implicitWidth - 24)
                            text: ep.modelData.episode + ".  " + (ep.modelData.title || qsTr("Episode %1").arg(ep.modelData.episode))
                            color: Theme.text
                            font.pixelSize: root.font1
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        Text {
                            id: epMeta
                            anchors.verticalCenter: epTitle.verticalCenter
                            text: ep.modelData.watched ? qsTr("✓ Watched")
                                  : ep.modelData.pos > 0 ? qsTr("%1 left").arg(root.clock(Math.max(0, ep.modelData.dur - ep.modelData.pos)))
                                  : ep.upNext && root.info.started ? qsTr("Up next")
                                  : (ep.modelData.duration || "")
                            color: ep.modelData.watched || ep.modelData.pos > 0 || (ep.upNext && root.info.started) ? Theme.accent : Theme.textMuted
                            font.pixelSize: root.font1 - 4
                        }
                    }
                    Text {
                        width: parent.width
                        visible: text.length > 0
                        text: ep.modelData.plot || ""
                        color: Theme.textSecondary
                        font.pixelSize: root.font1 - 3
                        wrapMode: Text.Wrap
                        elide: Text.ElideRight
                        maximumLineCount: 2
                    }
                }
                MouseArea {
                    id: epMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: { episodeList.currentIndex = ep.index; episodeList.forceActiveFocus() }
                    onDoubleClicked: root.playEpisode(ep.modelData)
                }
            }
        }
        BusyIndicator {
            visible: root.loading
            running: visible
            anchors.centerIn: episodeList
            palette.dark: Theme.accent
        }
        Text {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 18
            text: qsTr("Enter: play   ·   Q: add to queue   ·   ← seasons   ·   W: watched   ·   Esc: back")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
        }
        CloseButton {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 24
            label: qsTr("Back")
            onClicked: episodesPage.hide()
        }
    }

    SourcePicker {
        id: sourcePicker
        onChosen: (i) => { Vod.startPlayback(root.info, -1, false, i); actions.forceActiveFocus() }
        onDismissed: actions.forceActiveFocus()
    }

    // ---- Credits & more info -------------------------------------------------------------------
    VodCredits {
        id: credits
        anchors.fill: parent
        z: 40
        onClosed: { actions.forceActiveFocus(); root.wake() }
        onOpenedChanged: if (opened) root.stopBackgroundTrailer()
    }

    // ---- Trailer over the page, with sound ----------------------------------------------------
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

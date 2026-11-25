import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Window
import Relay.Core
import Relay.Video

// Movies or series, browsed like a streaming service:
//  - a spotlight card on top (a featured title, its trailer playing in it,
//    Play and More info), the page tinted by its picture;
//  - the provider's categories as a row of chips;
//  - rows of posters: Continue watching, Favorites, Recently added, Top
//    rated, then the categories. The selected row rises to the top, its
//    selected card widens into a 16:9 picture that plays the trailer after
//    a moment, and what the title is shows under the row.
//
// Keys: arrows move, Enter opens (or plays from the spotlight), P plays,
// F adds to favorites, M turns the trailer's sound on or off. Up from the
// spotlight reaches the tabs.
FocusScope {
    id: root
    signal openDetails(string kind, string id, var preview)
    signal seeAll(string shelfId)
    signal play(var descriptor)
    signal message(string text)
    signal upFromTop()

    property int topInset: 0               // the tabs float over the page
    property var shelves: []
    property var featured: null            // the spotlight's title
    property var infos: ({})                // details as they arrive, by "kind:id"
    property var selected: null             // what the keys are on (a card, or the spotlight's title)

    readonly property real side: Math.round(Math.max(24, Math.min(64, width * 0.035)))
    readonly property real viewH: height - topInset
    readonly property int cardH: Math.round(Math.max(170, Math.min(430, viewH * 0.42)))
    readonly property int cardW: Math.round(cardH * 2 / 3)
    readonly property int wideW: Math.round(cardH * 16 / 9)
    readonly property int heroH: Math.round(Math.max(230, Math.min(760, viewH * 0.72)))
    readonly property int chipH: Math.round(Math.max(46, Math.min(96, viewH * 0.1)))
    readonly property int font1: Math.round(Math.max(Theme.fontSmall, Math.min(22, viewH * 0.024)))

    // On screen (not minimized or hidden): trailers may play.
    readonly property bool active: visible && root.Window.visibility !== Window.Minimized && root.Window.visibility !== Window.Hidden
                                   && Qt.application.state !== Qt.ApplicationHidden && Qt.application.state !== Qt.ApplicationSuspended
    readonly property var chips: (Vod.categories || []).filter(c => String(c.id).startsWith("c:"))
    readonly property var entries: {
        const out = [{ type: "hero" }]
        if (chips.length) out.push({ type: "chips" })
        for (const s of shelves) out.push({ type: "shelf", shelf: s })
        return out
    }

    // A card's poster. On a TV it comes at the size the card shows: TMDB has
    // the providers' 600×900 posters at 300×450 too, a third of the download
    // and a quarter of the decoding (on the TV they appeared twice as fast).
    function cardPicture(item) {
        const url = item ? item.poster || "" : ""
        return Settings.tv ? Vod.picture(url, cardW) : url
    }
    // A row of posters' height (not the selected row), for rows still being made.
    property real shelfH: cardH + 80
    function keyOf(item) { return item ? item.kind + ":" + item.id : "" }
    function infoOf(item) { return item ? root.infos[keyOf(item)] || null : null }

    function reload() {
        shelves = Vod.shelves(30)
        // The spotlight: one of the newest (or best rated) titles with a picture, kept while browsing.
        const pool = []
        for (const id of ["recent", "top"]) {
            const s = shelves.find(x => x.id === id)
            if (s) for (const it of s.items.slice(0, 10)) if (it.poster) pool.push(it)
            if (pool.length) break
        }
        if (!pool.length && shelves.length) pool.push(...shelves[0].items.slice(0, 6))
        if (!featured || !pool.some(i => i.id === featured.id && i.kind === featured.kind))
            featured = pool.length ? pool[Math.floor(Math.random() * pool.length)] : null
        if (featured) Vod.requestDetails(featured.kind, featured.id)
        syncSelection()
    }
    // Back to the top (opening the page, or a new tab).
    function reset() {
        rows.cols = ({})
        rows.currentIndex = 0
        rows.positionViewAtBeginning()
        heroButtons.current = 0
        syncSelection()
    }
    function focusRows() { rows.forceActiveFocus(); syncSelection() }

    function syncSelection() {
        const e = entries[rows.currentIndex]
        let item = null
        if (e && e.type === "hero") item = featured
        else if (e && e.type === "shelf") item = e.shelf.items[rows.col(rows.currentIndex)] || null
        select(item)
    }
    function select(item) {
        if (item && selected && item.id === selected.id && item.kind === selected.kind) return
        selected = item
        stopTrailer()
        if (item) detailsTimer.restart()
        // On a TV the selected card widens (with its backdrop, logo and badges)
        // once the remote rests on it, as TV apps do: moving through the
        // titles only slides the posters, which a TV draws smoothly.
        if (Settings.tv) { settled = false; settleTimer.restart() }
    }
    property bool settled: true
    Timer { id: settleTimer; interval: 350; onTriggered: root.settled = true }
    function playItem(item) {
        if (!item) return
        const info = infoOf(item)
        if (item.kind === "series" || !info) { openDetails(item.kind, item.id, item); return }
        Vod.startPlayback(info, -1, false)   // the player starts on Vod.playbackReady()
    }
    function toggleFavorite(item) {
        if (!item) return
        const fav = Vod.toggleFavorite(item.kind, item.id)
        message(fav ? qsTr("Added to favorites") : qsTr("Removed from favorites"))
        reloadTimer.restart()
    }
    function metaLine(item, info) {
        if (!item) return ""
        const i = info || ({})
        const parts = [item.kind === "series" ? qsTr("Series") : qsTr("Movie")]
        if (i.genre) parts.push(i.genre.split(",")[0].trim())
        if (i.year || item.year) parts.push(i.year || item.year)
        if (item.kind === "series" && i.seasons && i.seasons.length)
            parts.push(i.seasons.length === 1 ? qsTr("1 season") : qsTr("%1 seasons").arg(i.seasons.length))
        else if (i.duration) parts.push(i.duration)
        if (i.certification) parts.push(i.certification)
        const r = i.rating || item.rating
        if (r > 0) parts.push("★ " + Number(r).toFixed(1))
        return parts.join("  •  ")
    }
    function badges(item, info) {
        if (!item) return []
        const out = []
        const now = Date.now() / 1000
        if (item.added > 0 && now - item.added < 21 * 86400) out.push(qsTr("Recently added"))
        const r = (info && info.rating) || item.rating
        if (r >= 7.5) out.push(qsTr("Top rated"))
        if (info && info.favorite) out.push(qsTr("In favorites"))
        return out
    }

    // ---- Trailer: one player, shown over the selected card or the spotlight ----------
    property string trailerFor: ""
    property bool trailerMuted: true
    property Item trailerHost: null        // where it plays (the spotlight's picture or a card's)
    function startTrailer() {
        const info = infoOf(selected)
        if (!info || !info.trailer || !Trailers.autoplay || !active || !rows.activeFocus) return
        trailerFor = info.trailer
        Trailers.resolve(info.trailer)
    }
    function stopTrailer() {
        trailerTimer.stop()
        if (trailerFor !== "") { trailerFor = ""; trailer.stop() }
    }
    onActiveChanged: if (!active) stopTrailer()

    Timer { id: detailsTimer; interval: 250; onTriggered: if (root.selected && root.visible) Vod.requestDetails(root.selected.kind, root.selected.id) }
    Timer { id: trailerTimer; interval: 1600; onTriggered: root.startTrailer() }
    Timer { id: reloadTimer; interval: 150; onTriggered: if (root.visible) root.reload() }

    Connections {
        target: Vod
        function onDetailsReady(d) {
            const k = d.kind + ":" + d.id
            const all = Object.keys(root.infos).length > 120 ? ({}) : Object.assign({}, root.infos)
            all[k] = d
            root.infos = all
            if (root.selected && k === root.keyOf(root.selected) && d.trailer && Trailers.autoplay && root.trailerFor === "")
                trailerTimer.restart()
        }
        function onStateChanged() { reloadTimer.restart() }
        function onSectionChanged() { root.featured = null; root.selected = null; reloadTimer.restart() }
        function onCategoriesChanged() { reloadTimer.restart() }
    }
    Connections {
        target: Trailers
        function onResolved(t, video, audio) { if (t === root.trailerFor) trailer.playTrailer(video, audio) }
        function onFailed(t, msg) { if (t === root.trailerFor) root.trailerFor = "" }
    }
    onVisibleChanged: {
        if (!visible) { stopTrailer(); return }
        reload()
        if (selected) detailsTimer.restart()
    }

    // ---- The page's tint: the selected title's picture, blurred and dimmed ---------
    Item {
        id: tint
        anchors.fill: parent
        readonly property var info: root.infoOf(root.selected) || root.infoOf(root.featured)
        readonly property string wanted: info && info.backdrop ? info.backdrop : (root.selected ? root.selected.poster || "" : "")
        property bool useA: true
        onWantedChanged: {
            if (useA) tintB.source = wanted; else tintA.source = wanted
            useA = !useA
        }
        Rectangle { anchors.fill: parent; color: Theme.background }
        Image {
            id: tintA
            anchors.fill: parent
            sourceSize: Qt.size(48, 27)     // tiny, then stretched: a soft wash of the picture's colours
            smooth: true
            asynchronous: true
            opacity: tint.useA && status === Image.Ready ? 0.5 : 0
            Behavior on opacity { NumberAnimation { duration: 700 } }
        }
        Image {
            id: tintB
            anchors.fill: parent
            sourceSize: Qt.size(48, 27)
            smooth: true
            asynchronous: true
            opacity: !tint.useA && status === Image.Ready ? 0.5 : 0
            Behavior on opacity { NumberAnimation { duration: 700 } }
        }
        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#66000000" }
                GradientStop { position: 0.45; color: "#D0000000" }
                GradientStop { position: 1.0; color: Theme.background }
            }
        }
    }

    // The trailer, under the page: the card it belongs to is see-through while it plays.
    MpvVideo {
        id: trailer
        timeshiftMb: 0
        pictureShape: "fill"      // cropped to the card, no black bars
        muted: root.trailerMuted
        volume: 80
        visible: root.trailerHost !== null && root.trailerFor !== "" && playing
        opacity: visible ? 1 : 0
        onPlaybackError: root.trailerFor = ""
        onFinished: root.trailerFor = ""
        layer.enabled: root.canRound
        layer.effect: roundedFx
        // Follows its card as the rows move.
        Timer {
            interval: 16
            repeat: true
            running: root.trailerHost !== null && root.trailerFor !== ""
            triggeredOnStart: true
            onTriggered: {
                const h = root.trailerHost
                if (!h) return
                const p = h.mapToItem(root, 0, 0)
                trailer.x = p.x; trailer.y = p.y; trailer.width = h.width; trailer.height = h.height
            }
        }
    }
    readonly property bool trailerShowing: trailer.visible
    // Rounded corners where Qt can (6.5+) and the graphics draw effects
    // (software drawing doesn't: the corners stay square there). Not on a TV:
    // each rounded card is drawn twice (once offscreen), which a TV's
    // graphics can't keep up with while the rows scroll.
    property Component roundedFx: Settings.tv ? null : Qt.createComponent("RoundedEffect.qml", Component.PreferSynchronous)
    readonly property bool canRound: !!roundedFx && roundedFx.status === Component.Ready
                                     && root.GraphicsInfo.api !== GraphicsInfo.Software && root.GraphicsInfo.api !== GraphicsInfo.Unknown

    // ---- The page ---------------------------------------------------------------
    ListView {
        id: rows
        anchors.fill: parent
        anchors.topMargin: root.topInset      // nothing behind the tabs
        clip: true
        model: root.entries
        focus: true
        interactive: false
        keyNavigationEnabled: false
        cacheBuffer: Math.max(1200, height * 2)
        // The selected row sits just under the tabs.
        highlightRangeMode: ListView.StrictlyEnforceRange
        preferredHighlightBegin: 4
        preferredHighlightEnd: 5
        highlightMoveDuration: 280
        highlightMoveVelocity: -1
        footer: Item { width: 1; height: rows.height * 0.5 }

        property var cols: ({})              // the column each row was left at
        function col(r) { return cols[r] || 0 }
        function setCol(r, c) { const all = Object.assign({}, cols); all[r] = c; cols = all }
        function go(r) {
            if (r < 0 || r >= count) return
            currentIndex = r
            root.syncSelection()
        }
        function step(dc) {
            const e = root.entries[currentIndex]
            if (!e) return
            if (e.type === "hero") { heroButtons.move(dc); return }
            const n = e.type === "chips" ? root.chips.length - 1 : e.shelf.items.length   // the last: See all
            setCol(currentIndex, Math.max(0, Math.min(n, col(currentIndex) + dc)))
            root.syncSelection()
        }
        function activate() {
            const e = root.entries[currentIndex]
            if (!e) return
            if (e.type === "hero") { heroButtons.press(); return }
            if (e.type === "chips") { const c = root.chips[col(currentIndex)]; if (c) root.seeAll(c.id); return }
            const c = col(currentIndex)
            if (c >= e.shelf.items.length) root.seeAll(e.shelf.id)
            else { const it = e.shelf.items[c]; root.openDetails(it.kind, it.id, it) }
        }
        onActiveFocusChanged: {
            if (activeFocus) { root.syncSelection(); if (root.trailerFor === "") trailerTimer.restart() }
            else root.stopTrailer()
        }

        Keys.onPressed: (e) => {
            switch (e.key) {
            case Qt.Key_Left: step(-1); break
            case Qt.Key_Right: step(1); break
            case Qt.Key_Up: if (currentIndex > 0) go(currentIndex - 1); else root.upFromTop(); break
            case Qt.Key_Down: go(currentIndex + 1); break
            case Qt.Key_PageUp: go(Math.max(0, currentIndex - 3)); break
            case Qt.Key_PageDown: go(Math.min(count - 1, currentIndex + 3)); break
            case Qt.Key_Home: setCol(currentIndex, 0); root.syncSelection(); break
            case Qt.Key_Return:
            case Qt.Key_Enter: activate(); break
            case Qt.Key_P: root.playItem(root.selected); break
            case Qt.Key_F: root.toggleFavorite(root.selected); break
            case Qt.Key_M: root.trailerMuted = !root.trailerMuted; break
            default: return
            }
            e.accepted = true
        }
        // Dragging up or down moves between rows.
        DragHandler {
            target: null
            xAxis.enabled: false
            cursorShape: Qt.ClosedHandCursor
            property int startRow: 0
            onActiveChanged: if (active) { startRow = rows.currentIndex; rows.forceActiveFocus() }
            onTranslationChanged: {
                if (!active) return
                const r = Math.max(0, Math.min(rows.count - 1, startRow + Math.round(-translation.y / (root.cardH * 0.7))))
                if (r !== rows.currentIndex) rows.go(r)
            }
        }
        // The wheel moves a row at a time (Shift: along the row).
        WheelHandler {
            property real acc: 0
            onWheel: (ev) => {
                const d = ev.angleDelta.y !== 0 ? ev.angleDelta.y : ev.angleDelta.x
                acc += d
                if (Math.abs(acc) < 100) return
                const dir = acc > 0 ? -1 : 1
                acc = 0
                if (ev.modifiers & Qt.ShiftModifier || ev.angleDelta.x !== 0) rows.step(dir)
                else rows.go(Math.max(0, Math.min(rows.count - 1, rows.currentIndex + dir)))
            }
        }

        delegate: Item {
            id: entry
            required property var modelData
            required property int index
            readonly property bool isCurrent: rows.currentIndex === index
            readonly property bool isShelf: modelData.type === "shelf"
            width: rows.width
            // While a row of posters is still being made (on a TV, a little at
            // a time), it already takes a row's room, so nothing jumps.
            height: loader.item ? loader.item.height : (isShelf ? root.shelfH : 0)
            Loader {
                id: loader
                width: parent.width
                property var entry: entry.modelData
                property int entryIndex: entry.index
                property bool isCurrent: entry.isCurrent
                // On a TV a row of posters is made over several frames instead of
                // in one go (which held the screen up to a second or more when a
                // new row came into reach); rows are made before they're in view.
                asynchronous: Settings.tv && entry.isShelf
                sourceComponent: entry.modelData.type === "hero" ? heroComp
                               : entry.modelData.type === "chips" ? chipsComp : shelfComp
                onLoaded: if (entry.isShelf && !entry.isCurrent) root.shelfH = item.height
            }
        }

        // Nothing to show (still loading, or an empty catalogue).
        Text {
            anchors.centerIn: parent
            visible: root.shelves.length === 0
            width: parent.width - 80
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: Vod.loading ? qsTr("Loading…") : Vod.error || qsTr("No titles yet")
            color: Theme.textMuted
            font.pixelSize: Theme.fontBody
        }
    }

    // ---- Spotlight ---------------------------------------------------------------------
    // The buttons' state lives here (the card itself may be recreated).
    QtObject {
        id: heroButtons
        property int current: 0
        readonly property int count: root.trailerShowing && root.trailerHost && root.trailerHost.objectName === "hero" ? 3 : 2
        function move(d) { current = Math.max(0, Math.min(count - 1, current + d)) }
        function press() {
            if (current === 0) root.playItem(root.featured)
            else if (current === 1) { if (root.featured) root.openDetails(root.featured.kind, root.featured.id, root.featured) }
            else root.trailerMuted = !root.trailerMuted
        }
    }

    Component {
        id: heroComp
        Item {
            id: hero
            readonly property bool keys: parent.isCurrent && rows.activeFocus
            readonly property var item: root.featured
            readonly property var info: root.infoOf(item)
            width: parent.width
            height: root.heroH + 18
            // The trailer plays in the card's picture while the spotlight is selected.
            function claim() { if (keys) root.trailerHost = picture; else if (root.trailerHost === picture) root.trailerHost = null }
            onKeysChanged: claim()
            Component.onCompleted: claim()
            Component.onDestruction: if (root.trailerHost === picture) root.trailerHost = null

            Rectangle {
                id: card
                x: root.side
                width: parent.width - 2 * root.side
                height: root.heroH
                radius: 14
                color: "#22FFFFFF"
                layer.enabled: root.canRound
                layer.effect: roundedFx

                Item {
                    id: picture
                    objectName: "hero"
                    anchors.fill: parent
                    Image {
                        id: heroBackdrop
                        anchors.fill: parent
                        source: hero.info && hero.info.backdrop ? hero.info.backdrop : ""
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        sourceSize.width: 1600
                        opacity: status === Image.Ready && !(root.trailerShowing && root.trailerHost === picture) ? 1 : 0
                        Behavior on opacity { NumberAnimation { duration: 500 } }
                    }
                    Image {   // no backdrop: the poster, large and soft
                        anchors.fill: parent
                        visible: heroBackdrop.status !== Image.Ready
                        source: hero.item ? hero.item.poster || "" : ""
                        fillMode: Image.PreserveAspectCrop
                        sourceSize.width: 400
                        asynchronous: true
                        opacity: 0.45
                    }
                }
                // Text stays readable over any picture.
                Rectangle {
                    anchors.fill: parent
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: "#C0000000" }
                        GradientStop { position: 0.45; color: "#40000000" }
                        GradientStop { position: 0.75; color: "transparent" }
                    }
                }
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: parent.height * 0.55
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: "transparent" }
                        GradientStop { position: 1.0; color: "#B0000000" }
                    }
                }

                Column {
                    id: about
                    x: Math.round(card.width * 0.035)
                    width: Math.min(card.width * 0.5, 760)
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: Math.round(card.height * 0.07)
                    spacing: Math.round(root.font1 * 0.6)

                    TitleArt {
                        item: hero.item
                        info: hero.info
                        maxWidth: Math.min(about.width, card.width * 0.36)
                        maxHeight: card.height * 0.26
                        fontSize: Math.round(Math.max(28, Math.min(64, card.height * 0.1)))
                    }
                    Text {
                        width: parent.width
                        text: root.metaLine(hero.item, hero.info)
                        color: Theme.text
                        font.pixelSize: root.font1 - 2
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                        style: Text.Raised; styleColor: "#60000000"
                    }
                    Text {
                        width: parent.width
                        visible: hero.keys && hero.info && !!hero.info.plot && card.height > 320
                        text: hero.info ? hero.info.plot || "" : ""
                        color: Theme.text
                        opacity: 0.9
                        font.pixelSize: root.font1 - 1
                        lineHeight: 1.15
                        wrapMode: Text.Wrap
                        maximumLineCount: 3
                        elide: Text.ElideRight
                        style: Text.Raised; styleColor: "#60000000"
                    }
                    Row {
                        spacing: 12
                        topPadding: 6
                        PillButton {
                            icon: Icons.play; iconFilled: true
                            text: hero.item && hero.item.kind === "series" ? qsTr("Episodes") : qsTr("Play")
                            fontSize: root.font1 - 1
                            lit: hero.keys && heroButtons.current === 0
                            onClicked: root.playItem(root.featured)
                        }
                        PillButton {
                            icon: Icons.info
                            text: qsTr("More info")
                            fontSize: root.font1 - 1
                            lit: hero.keys && heroButtons.current === 1
                            onClicked: if (root.featured) root.openDetails(root.featured.kind, root.featured.id, root.featured)
                        }
                        PillButton {
                            visible: heroButtons.count === 3
                            icon: root.trailerMuted ? Icons.muted : Icons.volume
                            fontSize: root.font1 - 1
                            lit: hero.keys && heroButtons.current === 2
                            onClicked: root.trailerMuted = !root.trailerMuted
                        }
                    }
                }
                Badges {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: Math.round(card.height * 0.05)
                    list: root.badges(hero.item, hero.info)
                    fontSize: root.font1 - 4
                }
                MouseArea {
                    anchors.fill: parent
                    z: -1
                    onClicked: { rows.forceActiveFocus(); rows.go(hero.parent.entryIndex) }
                    onDoubleClicked: if (root.featured) root.openDetails(root.featured.kind, root.featured.id, root.featured)
                }
            }
        }
    }

    // ---- Category chips -------------------------------------------------------------------
    Component {
        id: chipsComp
        Item {
            id: chipsRow
            readonly property bool keys: parent.isCurrent && rows.activeFocus
            readonly property int at: rows.col(parent.entryIndex)
            width: parent.width
            height: root.chipH + 26
            ListView {
                id: chipList
                y: 6
                width: parent.width
                height: root.chipH + 12
                orientation: ListView.Horizontal
                spacing: Math.round(root.chipH * 0.18)
                model: root.chips
                currentIndex: chipsRow.at
                interactive: false
                highlightRangeMode: ListView.ApplyRange
                preferredHighlightBegin: root.side
                preferredHighlightEnd: width - root.side
                highlightMoveDuration: 220
                header: Item { width: root.side; height: 1 }
                footer: Item { width: root.side; height: 1 }
                DragHandler {
                    target: null
                    yAxis.enabled: false
                    cursorShape: Qt.ClosedHandCursor
                    property int startCol: 0
                    onActiveChanged: if (active) startCol = chipsRow.at
                    onTranslationChanged: {
                        if (!active) return
                        const c = Math.max(0, Math.min(root.chips.length - 1, startCol + Math.round(-translation.x / (root.chipH * 2.6))))
                        if (c !== chipsRow.at) rows.setCol(chipsRow.parent.entryIndex, c)
                    }
                }
                delegate: Rectangle {
                    id: chip
                    required property var modelData
                    required property int index
                    readonly property bool sel: chipsRow.keys && index === chipsRow.at
                    width: Math.max(root.chipH * 2.3, chipText.implicitWidth + root.chipH)
                    height: root.chipH
                    y: 6
                    radius: Math.round(root.chipH * 0.2)
                    color: sel || chipMouse.containsMouse ? "#38FFFFFF" : "#1FFFFFFF"
                    border.width: sel ? 3 : 1
                    border.color: sel ? Theme.text : "#26FFFFFF"
                    scale: sel ? 1.04 : 1
                    Behavior on scale { NumberAnimation { duration: 140 } }
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: chip.sel ? "#48FFFFFF" : "#2AFFFFFF" }
                        GradientStop { position: 1.0; color: chip.sel ? "#24FFFFFF" : "#0FFFFFFF" }
                    }
                    Text {
                        id: chipText
                        anchors.centerIn: parent
                        width: Math.min(implicitWidth, root.chipH * 4)
                        text: chip.modelData.label
                        color: Theme.text
                        font.pixelSize: Math.round(root.chipH * 0.3)
                        font.weight: Font.Bold
                        font.letterSpacing: 1
                        font.capitalization: Font.AllUppercase
                        elide: Text.ElideRight
                    }
                    MouseArea {
                        id: chipMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.seeAll(chip.modelData.id)
                    }
                }
            }
        }
    }

    // ---- A row of posters ---------------------------------------------------------------
    Component {
        id: shelfComp
        Column {
            id: shelf
            readonly property var data_: parent.entry.shelf
            // Once per row: reading data_.items converts the row's whole list,
            // which each card did again for its one title.
            readonly property var items: data_.items
            readonly property int row: parent.entryIndex
            readonly property bool keys: parent.isCurrent && rows.activeFocus
            readonly property int at: rows.col(row)
            readonly property var current: items[at] || null
            readonly property var currentInfo: keys ? root.infoOf(current) : null
            width: parent.width
            spacing: 10
            bottomPadding: 14

            // On a TV: the next posters along the selected row are fetched
            // before their cards exist. A card is made one step before it
            // shows, which is too late for a poster that still has to be
            // downloaded. Only once the remote has rested on the row, so the
            // rows passed on the way down fetch nothing extra.
            property bool ahead: false
            onKeysChanged: { ahead = false; if (keys && Settings.tv) aheadTimer.restart(); else aheadTimer.stop() }
            Component.onCompleted: if (keys && Settings.tv) aheadTimer.start()
            Timer { id: aheadTimer; interval: 500; onTriggered: shelf.ahead = shelf.keys }
            Item {
                visible: false
                Repeater {
                    model: shelf.ahead ? 8 : 0
                    Image {
                        required property int index
                        asynchronous: true
                        sourceSize.width: 300    // as the cards ask for it, so they share what's fetched
                        // (the selected card and the five after it are on screen or made already)
                        source: root.cardPicture(shelf.items[shelf.at + 6 + index])
                    }
                }
            }

            Row {
                x: root.side
                spacing: 14
                Text {
                    text: shelf.data_.label
                    color: shelf.keys ? Theme.text : Theme.textSecondary
                    font.pixelSize: root.font1 + 1
                    font.weight: Font.DemiBold
                }
                Text {
                    anchors.baseline: parent.children[0].baseline
                    text: qsTr("See all ›")
                    color: seeAllMouse.containsMouse ? Theme.accent : Theme.textMuted
                    font.pixelSize: root.font1 - 4
                    MouseArea {
                        id: seeAllMouse
                        anchors.fill: parent
                        anchors.margins: -6
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.seeAll(shelf.data_.id)
                    }
                }
            }

            ListView {
                id: cards
                width: parent.width
                height: root.cardH
                orientation: ListView.Horizontal
                model: shelf.items.length + 1     // + See all
                spacing: Math.round(root.cardW * 0.06)
                currentIndex: shelf.at
                interactive: false
                // The selected card stays at the left edge; the rest move past it.
                highlightRangeMode: ListView.StrictlyEnforceRange
                preferredHighlightBegin: root.side
                preferredHighlightEnd: root.side + 1
                highlightMoveDuration: 240
                highlightMoveVelocity: -1
                highlightResizeDuration: 0
                // A TV makes one card ahead (more as the row moves): a new row
                // then costs about half as much while moving down the rows.
                cacheBuffer: Settings.tv ? root.cardW : root.wideW * 2

                // Grab and drag the posters along (any row; the selected row's
                // card follows). A click still opens the title.
                DragHandler {
                    target: null
                    yAxis.enabled: false
                    cursorShape: Qt.ClosedHandCursor
                    property int startCol: 0
                    onActiveChanged: if (active) startCol = shelf.at
                    onTranslationChanged: {
                        if (!active) return
                        const step = Math.round(-translation.x / (root.cardW + cards.spacing))
                        const c = Math.max(0, Math.min(shelf.items.length, startCol + step))
                        if (c === shelf.at) return
                        rows.setCol(shelf.row, c)
                        if (shelf.row === rows.currentIndex) root.syncSelection()
                    }
                }

                delegate: Item {
                    id: card
                    required property int index
                    readonly property bool isMore: index === shelf.items.length
                    readonly property var item: isMore ? null : shelf.items[index]
                    readonly property bool sel: shelf.keys && index === shelf.at
                    readonly property bool wide: sel && !isMore && root.settled
                    readonly property var info: wide ? root.infoOf(item) : null
                    width: wide ? root.wideW : root.cardW
                    height: root.cardH
                    Behavior on width { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
                    function claim() { if (wide) root.trailerHost = frame; else if (root.trailerHost === frame) root.trailerHost = null }
                    onWideChanged: claim()
                    Component.onCompleted: claim()
                    Component.onDestruction: if (root.trailerHost === frame) root.trailerHost = null

                    Rectangle {
                        id: frame
                        anchors.fill: parent
                        radius: 8
                        color: card.wide && root.trailerShowing && root.trailerHost === frame ? "transparent" : Theme.raised
                        layer.enabled: card.wide && root.canRound
                        layer.effect: roundedFx

                        Image {
                            id: poster
                            anchors.fill: parent
                            visible: !card.isMore
                            source: root.cardPicture(card.item)
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            // Fixed: a resize doesn't reload every poster. (A TV's
                            // comes at 300 wide, and isn't resized again here.)
                            sourceSize.width: Settings.tv ? 300 : 380
                            opacity: card.wide && backdrop.status === Image.Ready ? 0 : 1
                            Behavior on opacity { NumberAnimation { duration: 250 } }
                        }
                        // Widened: the title's backdrop (16:9), then its trailer.
                        Image {
                            id: backdrop
                            anchors.fill: parent
                            source: !(card.wide && card.info && card.info.backdrop) ? ""
                                    : Settings.tv ? Vod.picture(card.info.backdrop, root.wideW) : card.info.backdrop
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            // A TV gets it at the card's size (TMDB's 780 wide), not larger.
                            sourceSize.width: Settings.tv ? 780 : 960
                            opacity: status === Image.Ready && !(root.trailerShowing && root.trailerHost === frame) ? 1 : 0
                            Behavior on opacity { NumberAnimation { duration: 300 } }
                        }
                        Text {   // no picture
                            visible: !card.isMore && poster.status !== Image.Ready && backdrop.status !== Image.Ready
                            anchors.fill: parent
                            anchors.margins: 12
                            text: card.item ? card.item.title : ""
                            color: Theme.textSecondary
                            font.pixelSize: root.font1 - 2
                            font.weight: Font.DemiBold
                            wrapMode: Text.Wrap
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        // Widened: the title (logo where TMDB has one) and badges over
                        // the picture. Made only for the widened card: every card
                        // making (and hiding) them slowed each new row.
                        Loader {
                            anchors.fill: parent
                            active: card.wide
                            sourceComponent: Item {
                                Rectangle {
                                    anchors.bottom: parent.bottom
                                    width: parent.width
                                    height: parent.height * 0.5
                                    gradient: Gradient {
                                        GradientStop { position: 0.0; color: "transparent" }
                                        GradientStop { position: 1.0; color: "#B0000000" }
                                    }
                                }
                                TitleArt {
                                    anchors.left: parent.left
                                    anchors.bottom: parent.bottom
                                    anchors.margins: Math.round(root.cardH * 0.06)
                                    item: card.item
                                    info: card.info
                                    maxWidth: root.wideW * 0.42
                                    maxHeight: root.cardH * 0.26
                                    fontSize: Math.round(root.cardH * 0.1)
                                }
                                Badges {
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    anchors.margins: Math.round(root.cardH * 0.06)
                                    list: root.badges(card.item, card.info).slice(0, 2)
                                    fontSize: root.font1 - 5
                                }
                            }
                        }
                        // Watched part.
                        Rectangle {
                            visible: !card.isMore && card.item && card.item.progress > 0
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 5
                            color: "#99000000"
                            Rectangle { width: parent.width * (card.item ? Math.max(0, card.item.progress) : 0); height: parent.height; color: Theme.accent }
                        }
                        Rectangle {   // watched
                            visible: !card.isMore && card.item && card.item.watched
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.margins: 8
                            width: 24; height: 24; radius: 12
                            color: "#CC0B0D12"
                            Text { anchors.centerIn: parent; text: "✓"; color: Theme.accent; font.pixelSize: 14; font.weight: Font.Bold }
                        }
                        // See all (the last card only)
                        Loader {
                            anchors.centerIn: parent
                            active: card.isMore
                            sourceComponent: Column {
                                spacing: 6
                                Text { anchors.horizontalCenter: parent.horizontalCenter; text: qsTr("See all"); color: Theme.text; font.pixelSize: root.font1 + 2; font.weight: Font.DemiBold }
                                Text { anchors.horizontalCenter: parent.horizontalCenter; text: Number(shelf.data_.count).toLocaleString(Qt.locale(), "f", 0); color: Theme.textMuted; font.pixelSize: root.font1 - 4 }
                            }
                        }
                    }
                    // The selected card's outline.
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -3
                        radius: 11
                        color: "transparent"
                        border.width: 3
                        border.color: Theme.text
                        visible: card.sel || cardMouse.containsMouse
                        opacity: card.sel ? 1 : 0.5
                    }
                    MouseArea {
                        id: cardMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            rows.forceActiveFocus()
                            rows.currentIndex = shelf.row
                            rows.setCol(shelf.row, card.index)
                            root.syncSelection()
                            if (card.isMore) root.seeAll(shelf.data_.id)
                            else root.openDetails(card.item.kind, card.item.id, card.item)
                        }
                    }
                }
            }

            // What the selected title is, under the row.
            Item {
                width: parent.width
                height: shelf.keys && shelf.current ? infoCol.implicitHeight + 6 : 0
                clip: true
                Behavior on height { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
                Column {
                    id: infoCol
                    x: root.side
                    y: 4
                    width: Math.min(root.wideW * 1.3, parent.width - 2 * root.side)
                    spacing: 4
                    Text {   // an episode in progress
                        width: parent.width
                        visible: !!(shelf.current && shelf.current.subtitle)
                        text: shelf.current ? shelf.current.subtitle || "" : ""
                        color: Theme.textSecondary
                        font.pixelSize: root.font1 - 2
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        text: shelf.current && shelf.current.progress > 0 ? qsTr("Resume") : root.metaLine(shelf.current, shelf.currentInfo)
                        color: Theme.text
                        font.pixelSize: root.font1 - 1
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        visible: text.length > 0 && !(shelf.current && shelf.current.progress > 0)
                        text: shelf.currentInfo ? shelf.currentInfo.plot || "" : ""
                        color: Theme.textSecondary
                        font.pixelSize: root.font1 - 1
                        lineHeight: 1.15
                        wrapMode: Text.Wrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }
}

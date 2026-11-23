import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Window
import Relay.Core
import Relay.Video

// Movies or series, browsed like a streaming service: a spotlight on the
// selected title (backdrop, what it is, a trailer after a moment), and rows
// of posters under it: Continue watching, Favorites, Recently added, Top
// rated, then the provider's categories. The last card of a row is "See all".
//
// Keys: arrows move, Enter opens the title (or See all), P plays it, F adds
// it to favorites, Up from the first row reaches the spotlight's buttons.
FocusScope {
    id: root
    signal openDetails(string kind, string id, var preview)
    signal seeAll(string shelfId)
    signal play(var descriptor)
    signal message(string text)

    property var shelves: []
    property var selected: null          // the title in the spotlight
    property var info: ({})              // its details, once they're here
    readonly property bool infoReady: !!selected && info.id === selected.id && info.kind === selected.kind
    readonly property int cardWidth: Math.round(Math.max(118, Math.min(190, width / 7.6)))
    readonly property int cardHeight: Math.round(cardWidth * 1.5)
    // On screen (not minimized or hidden): trailers may play.
    readonly property bool active: visible && root.Window.visibility !== Window.Minimized && root.Window.visibility !== Window.Hidden
                                   && Qt.application.state !== Qt.ApplicationHidden && Qt.application.state !== Qt.ApplicationSuspended

    function reload() {
        const before = selected
        shelves = Vod.shelves(24)
        if (!before || !shelves.some(s => s.items.some(i => i.id === before.id && i.kind === before.kind)))
            select(shelves.length ? shelves[0].items[0] : null)
    }
    function select(item) {
        if (item && selected && item.id === selected.id && item.kind === selected.kind) return
        selected = item
        stopTrailer()
        if (item) detailsTimer.restart()
    }
    function focusRows() { rows.forceActiveFocus() }
    function playSelected() {
        if (!selected) return
        if (selected.kind === "series" || !infoReady) { openDetails(selected.kind, selected.id, selected); return }
        const d = Vod.playback(info, -1, false)
        if (d.url) play(d)
        else openDetails(selected.kind, selected.id, selected)
    }
    function toggleFavorite() {
        if (!selected) return
        const fav = Vod.toggleFavorite(selected.kind, selected.id)
        message(fav ? qsTr("Added to favorites") : qsTr("Removed from favorites"))
        reloadTimer.restart()
    }

    // ---- Trailer -------------------------------------------------------------
    property string trailerFor: ""       // the trailer playing (or being fetched)
    property bool trailerMuted: true
    function startTrailer() {
        if (!infoReady || !info.trailer || !Trailers.autoplay || !active) return
        trailerFor = info.trailer
        Trailers.resolve(info.trailer)
    }
    function stopTrailer() {
        trailerTimer.stop()
        if (trailerFor !== "") { trailerFor = ""; trailer.stop() }
    }
    onActiveChanged: if (!active) stopTrailer()

    Timer { id: detailsTimer; interval: 300; onTriggered: if (root.selected && root.visible) Vod.requestDetails(root.selected.kind, root.selected.id) }
    Timer { id: trailerTimer; interval: 1800; onTriggered: root.startTrailer() }
    Timer { id: reloadTimer; interval: 150; onTriggered: if (root.visible) root.reload() }

    Connections {
        target: Vod
        function onDetailsReady(d) {
            if (!root.selected || d.id !== root.selected.id || d.kind !== root.selected.kind) return
            root.info = d
            if (d.trailer && Trailers.autoplay) trailerTimer.restart()
        }
        function onStateChanged() { reloadTimer.restart() }
        function onSectionChanged() { root.selected = null; reloadTimer.restart() }
        function onCategoriesChanged() { reloadTimer.restart() }
    }
    Connections {
        target: Trailers
        function onResolved(t, video, audio) { if (t === root.trailerFor) trailer.playTrailer(video, audio) }
        function onFailed(t, msg) { if (t === root.trailerFor) root.trailerFor = "" }
    }
    // Only while on screen: then the spotlight's title (again) gets its details and trailer.
    onVisibleChanged: {
        if (!visible) { stopTrailer(); return }
        reload()
        if (selected) detailsTimer.restart()
    }

    // ---- Spotlight ---------------------------------------------------------------
    FocusScope {
        id: spotlight
        width: parent.width
        height: Math.round(Math.max(230, Math.min(parent.height * 0.5, 520)))
        clip: true

        // Backdrop, or the poster large and dimmed when there's none.
        Image {
            id: backdrop
            anchors.right: parent.right
            width: parent.width * 0.72
            height: parent.height
            source: root.infoReady && root.info.backdrop ? root.info.backdrop : ""
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            cache: true
            opacity: status === Image.Ready && !trailer.playing ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 400 } }
        }
        Image {
            anchors.right: parent.right
            width: parent.width * 0.72
            height: parent.height
            visible: backdrop.status !== Image.Ready
            source: root.selected ? root.selected.poster || "" : ""
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            opacity: status === Image.Ready ? 0.28 : 0
            Behavior on opacity { NumberAnimation { duration: 400 } }
        }
        // The trailer, muted unless asked, over the backdrop.
        MpvVideo {
            id: trailer
            anchors.right: parent.right
            width: parent.width * 0.72
            height: parent.height
            timeshiftMb: 0
            muted: root.trailerMuted
            volume: 80
            opacity: playing && root.trailerFor !== "" ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 500 } }
            onPlaybackError: root.trailerFor = ""
        }
        // Fade into the page from the left and at the bottom.
        Rectangle {
            anchors.right: backdrop.right
            width: backdrop.width
            height: parent.height
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: Theme.background }
                GradientStop { position: 0.38; color: "#C00B0D12" }
                GradientStop { position: 0.7; color: "#200B0D12" }
                GradientStop { position: 1.0; color: "transparent" }
            }
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: parent.height * 0.45
            gradient: Gradient {
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 1.0; color: Theme.background }
            }
        }

        Column {
            id: about
            x: 40
            width: parent.width < 900 ? parent.width - 80 : Math.min(parent.width * 0.48, 620)
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 22
            spacing: 10

            Text {
                width: parent.width
                text: root.selected ? root.selected.title : (Vod.loading ? qsTr("Loading…") : qsTr("Nothing here yet"))
                color: Theme.text
                font.pixelSize: Math.round(Theme.fontTitle * 1.6)
                font.weight: Font.Bold
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                visible: text.length > 0
                text: {
                    const s = root.selected
                    if (!s) return ""
                    const i = root.infoReady ? root.info : ({})
                    const parts = []
                    if (i.year || s.year) parts.push(i.year || s.year)
                    if (i.duration) parts.push(i.duration)
                    const r = i.rating || s.rating
                    if (r > 0) parts.push("★ " + Number(r).toFixed(1))
                    if (i.genre) parts.push(i.genre.split(",").slice(0, 3).map(g => g.trim()).join(" · "))
                    return parts.join("   ·   ")
                }
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody - 1
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                visible: root.infoReady && !!root.info.plot && spotlight.height > 300
                text: root.infoReady ? root.info.plot || "" : ""
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody - 1
                wrapMode: Text.Wrap
                maximumLineCount: spotlight.height > 420 ? 3 : 2
                elide: Text.ElideRight
                lineHeight: 1.15
            }
            Row {
                id: buttons
                spacing: 10
                topPadding: 4
                property int current: 0
                readonly property var list: [playButton, infoButton, favButton].concat(muteButton.visible ? [muteButton] : [])
                function move(d) { current = Math.max(0, Math.min(list.length - 1, current + d)) }
                focus: true
                Keys.onLeftPressed: move(-1)
                Keys.onRightPressed: move(1)
                Keys.onDownPressed: rows.forceActiveFocus()
                Keys.onReturnPressed: list[current].clicked()
                Keys.onEnterPressed: list[current].clicked()
                readonly property bool keys: spotlight.activeFocus

                BarButton {
                    id: playButton
                    accent: true
                    icon: Icons.play; iconFilled: true
                    label: root.selected && root.selected.kind === "series" ? qsTr("Episodes")
                         : root.selected && root.selected.progress > 0 ? qsTr("Resume") : qsTr("Play")
                    shortcut: "P"
                    showShortcut: false
                    border.width: buttons.keys && buttons.current === 0 ? 2 : 0
                    border.color: Theme.text
                    onClicked: root.playSelected()
                }
                BarButton {
                    id: infoButton
                    icon: Icons.info
                    label: qsTr("More info")
                    showShortcut: false
                    active: buttons.keys && buttons.current === 1
                    onClicked: if (root.selected) root.openDetails(root.selected.kind, root.selected.id, root.selected)
                }
                BarButton {
                    id: favButton
                    icon: Icons.heart
                    iconFilled: root.infoReady && !!root.info.favorite
                    label: qsTr("Favorite")
                    shortcut: "F"
                    showLabel: false
                    active: buttons.keys && buttons.current === 2
                    onClicked: { root.toggleFavorite(); if (root.selected) Vod.requestDetails(root.selected.kind, root.selected.id) }
                }
                BarButton {
                    id: muteButton
                    visible: trailer.playing && root.trailerFor !== ""
                    icon: root.trailerMuted ? Icons.muted : Icons.volume
                    label: root.trailerMuted ? qsTr("Trailer sound on") : qsTr("Trailer sound off")
                    showLabel: false
                    active: buttons.keys && buttons.current === 3
                    onClicked: root.trailerMuted = !root.trailerMuted
                }
            }
        }
    }

    // ---- Rows ------------------------------------------------------------------------
    ListView {
        id: rows
        anchors.top: spotlight.bottom
        anchors.bottom: parent.bottom
        width: parent.width
        clip: true
        model: root.shelves
        spacing: 6
        bottomMargin: 30
        keyNavigationEnabled: false
        highlightMoveDuration: 220
        highlightRangeMode: ListView.ApplyRange
        preferredHighlightBegin: 0
        preferredHighlightEnd: root.cardHeight * 1.2 + 100
        boundsBehavior: Flickable.StopAtBounds
        cacheBuffer: 600
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        focus: true

        // The column each row was left at.
        property var columns: ({})
        function cardsOf(r) { const it = itemAtIndex(r); return it ? it.cards : null }
        function selectAt(r, c) {
            if (r < 0 || r >= count) return
            currentIndex = r
            const items = root.shelves[r].items
            c = Math.max(0, Math.min(items.length, c))   // items.length: See all
            columns[r] = c
            const cards = cardsOf(r)
            if (cards) { cards.currentIndex = c; cards.positionViewAtIndex(c, ListView.Contain) }
            if (c < items.length) root.select(items[c])
        }
        function column() { return columns[currentIndex] || 0 }
        onActiveFocusChanged: if (activeFocus && count) selectAt(Math.max(0, currentIndex), column())

        Keys.onPressed: (e) => {
            const r = currentIndex, c = column()
            const items = r >= 0 && r < root.shelves.length ? root.shelves[r].items : []
            switch (e.key) {
            case Qt.Key_Left: selectAt(r, c - 1); break
            case Qt.Key_Right: selectAt(r, c + 1); break
            case Qt.Key_Up:
                if (r > 0) selectAt(r - 1, columns[r - 1] !== undefined ? columns[r - 1] : 0)
                else spotlight.forceActiveFocus()
                break
            case Qt.Key_Down: selectAt(r + 1, columns[r + 1] !== undefined ? columns[r + 1] : 0); break
            case Qt.Key_PageUp: selectAt(Math.max(0, r - 3), 0); break
            case Qt.Key_PageDown: selectAt(Math.min(count - 1, r + 3), 0); break
            case Qt.Key_Home: selectAt(r, 0); break
            case Qt.Key_End: selectAt(r, items.length); break
            case Qt.Key_Return:
            case Qt.Key_Enter:
                if (c >= items.length) root.seeAll(root.shelves[r].id)
                else root.openDetails(items[c].kind, items[c].id, items[c])
                break
            case Qt.Key_P:
                if (c < items.length) root.playSelected()
                break
            case Qt.Key_F:
                if (c < items.length) root.toggleFavorite()
                break
            default:
                return
            }
            e.accepted = true
        }

        delegate: Column {
            id: shelf
            required property var modelData
            required property int index
            property alias cards: cards
            width: rows.width
            spacing: Math.round(root.cardHeight * 0.07) + 6   // the selected card grows into this

            Row {
                x: 40
                spacing: 12
                height: 30
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: shelf.modelData.label
                    color: rows.currentIndex === shelf.index && rows.activeFocus ? Theme.text : Theme.textSecondary
                    font.pixelSize: Theme.fontLarge
                    font.weight: Font.DemiBold
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("See all ›")
                    color: seeAllMouse.containsMouse ? Theme.accent : Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                    MouseArea {
                        id: seeAllMouse
                        anchors.fill: parent
                        anchors.margins: -6
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.seeAll(shelf.modelData.id)
                    }
                }
            }

            ListView {
                id: cards
                width: parent.width
                height: root.cardHeight + 50
                orientation: ListView.Horizontal
                model: shelf.modelData.items.length + 1     // + See all
                spacing: 14
                leftMargin: 40
                rightMargin: 40
                clip: false
                interactive: true
                boundsBehavior: Flickable.StopAtBounds
                highlightMoveDuration: 200
                keyNavigationEnabled: false
                cacheBuffer: root.cardWidth * 4
                // A wheel over a row scrolls the page, not the row (shift: the row).
                WheelHandler {
                    acceptedModifiers: Qt.NoModifier
                    target: null
                    onWheel: (ev) => { rows.flick(0, ev.angleDelta.y * 8); ev.accepted = true }
                }

                delegate: Item {
                    id: card
                    required property int index
                    readonly property bool isMore: index === shelf.modelData.items.length
                    readonly property var item: isMore ? null : shelf.modelData.items[index]
                    readonly property bool selected: rows.currentIndex === shelf.index && cards.currentIndex === index && rows.activeFocus
                    readonly property bool hovered: cardMouse.containsMouse
                    width: root.cardWidth
                    height: cards.height
                    z: selected || hovered ? 2 : 0

                    Rectangle {
                        id: frame
                        width: parent.width
                        height: root.cardHeight
                        radius: 8
                        color: Theme.raised
                        clip: true
                        scale: card.selected || card.hovered ? 1.07 : 1
                        transformOrigin: Item.Bottom
                        Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                        border.width: card.selected ? 3 : 0
                        border.color: Theme.text

                        Image {
                            id: posterImage
                            anchors.fill: parent
                            anchors.margins: card.selected ? 3 : 0
                            visible: !card.isMore
                            source: card.item ? card.item.poster || "" : ""
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            sourceSize.width: 380   // fixed: a window resize doesn't reload every poster
                        }
                        // No picture: the title on a quiet card.
                        Text {
                            visible: !card.isMore && posterImage.status !== Image.Ready
                            anchors.fill: parent
                            anchors.margins: 12
                            text: card.item ? card.item.title : ""
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontBody - 1
                            font.weight: Font.DemiBold
                            wrapMode: Text.Wrap
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        // Watched part (Continue watching and anything started).
                        Rectangle {
                            visible: !card.isMore && card.item && card.item.progress > 0
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: card.selected ? 3 : 0
                            x: card.selected ? 3 : 0
                            width: parent.width - 2 * x
                            height: 4
                            color: "#80000000"
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
                        // See all
                        Column {
                            visible: card.isMore
                            anchors.centerIn: parent
                            spacing: 6
                            Text { anchors.horizontalCenter: parent.horizontalCenter; text: qsTr("See all"); color: Theme.text; font.pixelSize: Theme.fontLarge; font.weight: Font.DemiBold }
                            Text { anchors.horizontalCenter: parent.horizontalCenter; text: Number(shelf.modelData.count).toLocaleString(Qt.locale(), "f", 0); color: Theme.textMuted; font.pixelSize: Theme.fontSmall }
                        }
                    }
                    Text {
                        anchors.top: frame.bottom
                        anchors.topMargin: card.selected || card.hovered ? 14 : 10
                        width: parent.width
                        visible: !card.isMore
                        text: card.item ? (card.item.subtitle || card.item.title) : ""
                        color: card.selected ? Theme.text : Theme.textSecondary
                        font.pixelSize: Theme.fontSmall + 1
                        elide: Text.ElideRight
                    }
                    MouseArea {
                        id: cardMouse
                        anchors.fill: frame
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        // Resting on a card puts it in the spotlight.
                        onContainsMouseChanged: if (containsMouse && card.item) hoverTimer.restart()
                        Timer { id: hoverTimer; interval: 250; onTriggered: if (cardMouse.containsMouse) root.select(card.item) }
                        onClicked: {
                            rows.forceActiveFocus()
                            rows.selectAt(shelf.index, card.index)
                            if (card.isMore) root.seeAll(shelf.modelData.id)
                            else root.openDetails(card.item.kind, card.item.id, card.item)
                        }
                    }
                }
            }
        }

        // Nothing to show (still loading, or an empty catalogue).
        Text {
            anchors.centerIn: parent
            visible: rows.count === 0
            width: parent.width - 80
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: Vod.loading ? qsTr("Loading…") : Vod.error || qsTr("No titles yet")
            color: Theme.textMuted
            font.pixelSize: Theme.fontBody
        }
    }
}

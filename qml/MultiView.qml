import QtQuick
import QtQuick.Controls.Basic
import Relay.Core
import Relay.Video

// Several channels at once (up to four). The selected tile has the sound.
//
// Keys: arrows select a tile, Enter changes its channel (on the empty slot:
// adds one), Insert adds, Delete removes, Page Up/Down zaps the selected
// tile, L changes the layout, F or Esc goes back to watching the selected
// channel.
//
// Layouts: auto (the arrangement that makes tiles biggest for the window's
// shape — stacked on a portrait screen, side by side or a grid on a wide
// one), stack, row, grid (2×2) and focus (the selected tile big, the others
// small beside it, or underneath it on a portrait screen).
FocusScope {
    id: root
    property bool opened: false
    property int selected: 0
    property var mainVideo                 // for volume and mute
    signal closed(int globalIndex)         // channel to carry on watching (-1: none)
    signal message(string text)
    signal pinRequested(string reason, var onOk)

    readonly property int maxTiles: 4
    readonly property var layouts: ["auto", "stack", "row", "grid", "focus"]
    readonly property string layout: layouts.indexOf(Settings.multiviewLayout) >= 0 ? Settings.multiviewLayout : "auto"
    // Tile rectangles, plus one for the "add" slot when there's a free cell.
    readonly property var rects: computeRects(grid.width, grid.height, tiles.count, selected, layout)
    readonly property bool hasAddSlot: rects.length > tiles.count
    onRectsChanged: if (rects.length > 0 && selected >= rects.length) selected = rects.length - 1

    // Tiles: { pid, key } — the playlist and the channel key. Tiles can come
    // from any playlist; the others' channel lists load in the background.
    ListModel { id: tiles }

    function isActive(pid) { return pid === Settings.activeId }
    function playlistName(pid) {
        for (const p of Settings.playlists) if (p.id === pid) return p.name
        return ""
    }
    function tileInfo(pid, key) {
        if (isActive(pid)) {
            const gi = Channels.indexOfKey(key)
            return gi >= 0 ? Channels.info(gi) : ({})
        }
        return OtherPlaylists.info(pid, key)
    }
    function locked(pid, key) {
        if (isActive(pid)) return Parental.blocked(Channels.indexOfKey(key))
        return !Parental.unlocked && OtherPlaylists.isLocked(pid, key)
    }
    // Saved as "pid::key" (older versions: a plain key of the playlist being watched).
    function parseEntry(entry) {
        const at = entry.indexOf("::")
        return at < 0 ? { pid: Settings.activeId, key: entry } : { pid: entry.substring(0, at), key: entry.substring(at + 2) }
    }

    function open(currentIndex) {
        tiles.clear()
        const list = []
        const has = (t) => list.some(x => x.pid === t.pid && x.key === t.key)
        if (currentIndex >= 0) list.push({ pid: Settings.activeId, key: Channels.keyAt(currentIndex) })
        for (const entry of Settings.multiviewKeys) {
            const t = parseEntry(entry)
            if (list.length >= maxTiles || has(t)) continue
            if (isActive(t.pid)) {
                const gi = Channels.indexOfKey(t.key)
                if (gi < 0 || Parental.blocked(gi)) continue
            } else {
                if (!Settings.playlists.some(p => p.id === t.pid)) continue   // playlist removed
                OtherPlaylists.ensure(t.pid)
            }
            list.push(t)
        }
        for (const t of list) tiles.append(t)
        selected = 0
        opened = true
        grid.forceActiveFocus()
        hints.flash()
    }
    function close() {
        if (!opened) return
        saveKeys()
        const t = selected < tiles.count ? tiles.get(selected) : (tiles.count ? tiles.get(0) : null)
        const pid = t ? t.pid : "", key = t ? t.key : ""
        picker.close()
        opened = false
        tiles.clear()
        if (t && !isActive(pid)) {
            // Carry on with that channel: switch to its playlist.
            Session.switchPlaylistAndTune(pid, key)
            closed(-1)
            return
        }
        closed(t ? Channels.indexOfKey(key) : -1)
    }
    function saveKeys() {
        const keys = []
        for (let i = 0; i < tiles.count; ++i) {
            const t = tiles.get(i)
            keys.push(t.pid + "::" + t.key)
        }
        Settings.multiviewKeys = keys
    }
    function addTile() {
        if (tiles.count >= maxTiles) { message(qsTr("Multi-view shows up to four channels")); return }
        picker.open(-1)
    }
    function changeTile() {
        if (selected >= tiles.count) { addTile(); return }
        const t = tiles.get(selected)
        if (locked(t.pid, t.key)) {
            pinRequested(qsTr("\"%1\" is locked").arg(tileInfo(t.pid, t.key).name || ""), () => {})
            return
        }
        picker.open(selected)
    }
    function setTile(index, pid, key) {
        for (let i = 0; i < tiles.count; ++i) {
            const t = tiles.get(i)
            if (t.pid === pid && t.key === key && i !== index) { selected = i; message(qsTr("That channel is already showing")); return }
        }
        if (index < 0) { tiles.append({ pid: pid, key: key }); selected = tiles.count - 1 }
        else { tiles.setProperty(index, "pid", pid); tiles.setProperty(index, "key", key) }
        saveKeys()
    }
    function removeSelected() {
        if (selected >= tiles.count) return
        if (tiles.count <= 1) { message(qsTr("Esc leaves multi-view")); return }
        tiles.remove(selected)
        selected = Math.min(selected, tiles.count - 1)
        saveKeys()
    }
    // Next/previous channel in the selected tile's playlist, skipping
    // channels already showing (and locked ones while locked).
    function zap(delta) {
        if (selected >= tiles.count) return
        const cur = tiles.get(selected)
        const showing = []
        for (let i = 0; i < tiles.count; ++i) if (tiles.get(i).pid === cur.pid) showing.push(tiles.get(i).key)
        if (isActive(cur.pid)) {
            let next = Channels.indexOfKey(cur.key)
            for (let n = 0; n < 8; ++n) {
                next = Channels.step(next, delta, Channels.allGroup)
                if (next < 0 || showing.indexOf(Channels.keyAt(next)) < 0) break
            }
            if (next >= 0 && showing.indexOf(Channels.keyAt(next)) < 0) setTile(selected, cur.pid, Channels.keyAt(next))
        } else {
            const next = OtherPlaylists.step(cur.pid, cur.key, delta, showing, !Parental.unlocked)
            if (next) setTile(selected, cur.pid, next)
        }
    }
    function layoutName(l) {
        return ({ auto: qsTr("Auto"), stack: qsTr("Stacked"), row: qsTr("Side by side"),
                  grid: qsTr("Grid"), focus: qsTr("Focus") })[l] || l
    }
    function cycleLayout() {
        const next = layouts[(layouts.indexOf(layout) + 1) % layouts.length]
        Settings.multiviewLayout = next
        message(qsTr("Layout: %1").arg(layoutName(next)))
    }

    // Biggest 16:9 rectangle that fits w×h, centred in it.
    function fit(x, y, w, h) {
        let tw = w, th = tw * 9 / 16
        if (th > h) { th = h; tw = th * 16 / 9 }
        return Qt.rect(x + (w - tw) / 2, y + (h - th) / 2, Math.max(0, tw), Math.max(0, th))
    }
    // n tiles in `cols` columns, packed together and centred in W×H; a short
    // last row is centred too.
    function gridRects(W, H, n, cols) {
        const gap = 12
        const rows = Math.ceil(n / cols)
        const cw = (W - gap * (cols - 1)) / cols, ch = (H - gap * (rows - 1)) / rows
        const one = fit(0, 0, cw, ch)
        const bw = one.width * cols + gap * (cols - 1), bh = one.height * rows + gap * (rows - 1)
        const ox = (W - bw) / 2, oy = (H - bh) / 2
        const out = []
        for (let i = 0; i < n; ++i) {
            const row = Math.floor(i / cols), col = i % cols
            const inRow = row === rows - 1 ? n - row * cols : cols
            const rx = ox + (bw - (one.width * inRow + gap * (inRow - 1))) / 2
            out.push(Qt.rect(rx + col * (one.width + gap), oy + row * (one.height + gap), one.width, one.height))
        }
        return out
    }
    // Columns that make the tiles biggest.
    function bestCols(W, H, n) {
        let best = 1, area = -1
        for (let c = 1; c <= n; ++c) {
            const r = gridRects(W, H, n, c)[0]
            if (r.width * r.height > area + 1) { area = r.width * r.height; best = c }
        }
        return best
    }
    function computeRects(W, H, count, sel, l) {
        if (W <= 0 || H <= 0) return []
        const n = Math.max(1, count)
        if (l === "focus" && count > 1) {
            const gap = 12, others = count - 1, out = []
            const s = Math.min(Math.max(sel, 0), count - 1)
            let big, small
            // The big tile takes most of the space; the others share a strip
            // beside it (wide window) or underneath (tall window). The two
            // are packed together and centred.
            if (W >= H) {
                big = fit(0, 0, (W - gap) * 0.72, H)
                const sw = Math.min(W - big.width - gap, (W - gap) * 0.28, big.width * 0.5)
                small = gridRects(sw, H, others, 1)
                const used = big.width + gap + small[0].width, ox = (W - used) / 2
                big = Qt.rect(ox, (H - big.height) / 2, big.width, big.height)
                small = small.map(r => Qt.rect(ox + big.width + gap, r.y, r.width, r.height))
            } else {
                big = fit(0, 0, W, (H - gap) * 0.62)
                const sh = Math.min(H - big.height - gap, (H - gap) * 0.38, big.height * 0.5)
                small = gridRects(W, sh, others, others)
                const used = big.height + gap + small[0].height, oy = (H - used) / 2
                big = Qt.rect((W - big.width) / 2, oy, big.width, big.height)
                small = small.map(r => Qt.rect(r.x, oy + big.height + gap, r.width, r.height))
            }
            for (let i = 0, k = 0; i < count; ++i) out.push(i === s ? big : small[k++])
            return out
        }
        // The empty "add" slot takes a free cell (only when there's one).
        let cols
        if (l === "stack") cols = 1
        else if (l === "row") cols = n
        else if (l === "grid") cols = n === 1 ? 1 : 2
        else cols = bestCols(W, H, n)
        const rows = Math.ceil(n / cols)
        const withAdd = count > 0 && count < maxTiles && rows * cols > count && l !== "stack" && l !== "row"
        return gridRects(W, H, withAdd ? count + 1 : n, cols).slice(0, withAdd ? count + 1 : count)
    }
    // Arrows: the nearest slot in that direction on screen (focus layout:
    // previous/next tile).
    function move(dx, dy) {
        const n = rects.length
        if (n <= 1) return
        if (layout === "focus" && tiles.count > 1) {
            selected = (selected + (dx || dy) + tiles.count) % tiles.count
            return
        }
        const cur = rects[Math.min(selected, n - 1)]
        const cx = cur.x + cur.width / 2, cy = cur.y + cur.height / 2
        let best = -1, bestScore = 1e12
        for (let i = 0; i < n; ++i) {
            if (i === selected) continue
            const r = rects[i]
            const ddx = r.x + r.width / 2 - cx, ddy = r.y + r.height / 2 - cy
            const along = dx ? ddx * dx : ddy * dy           // distance in the arrow's direction
            const across = Math.abs(dx ? ddy : ddx)
            if (along <= 1) continue
            const score = along + across * 2
            if (score < bestScore) { bestScore = score; best = i }
        }
        if (best >= 0) selected = best
    }

    visible: opened

    Rectangle {
        anchors.fill: parent
        color: "black"
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = true }
    }

    Item {
        id: grid
        anchors.fill: parent
        anchors.margins: 6
        focus: true

        Keys.onPressed: (e) => {
            switch (e.key) {
            case Qt.Key_Left: root.move(-1, 0); break
            case Qt.Key_Right: root.move(1, 0); break
            case Qt.Key_Up: root.move(0, -1); break
            case Qt.Key_Down: root.move(0, 1); break
            case Qt.Key_Return:
            case Qt.Key_Enter: root.changeTile(); break
            case Qt.Key_Insert:
            case Qt.Key_Plus:
            case Qt.Key_A: root.addTile(); break
            case Qt.Key_Delete:
            case Qt.Key_Minus: root.removeSelected(); break
            case Qt.Key_PageUp:
            case Qt.Key_ChannelUp: root.zap(1); break
            case Qt.Key_PageDown:
            case Qt.Key_ChannelDown: root.zap(-1); break
            case Qt.Key_H: hints.flash(); break
            case Qt.Key_L: root.cycleLayout(); break
            case Qt.Key_M:
                root.mainVideo.toggleMute()
                root.message(root.mainVideo.muted ? qsTr("Muted") : qsTr("Sound on"))
                break
            case Qt.Key_F:
            case Qt.Key_Q:
            case Qt.Key_Escape:
            case Qt.Key_Back: root.close(); break
            default: return
            }
            e.accepted = true
        }

        Repeater {
            model: tiles
            delegate: Item {
                id: tile
                readonly property string pid: model.pid
                readonly property string key: model.key
                readonly property bool other: !root.isActive(pid)
                readonly property string loadState: (OtherPlaylists.revision, other ? OtherPlaylists.state(pid) : "ready")
                readonly property var info: (OtherPlaylists.revision, Parental.revision, root.tileInfo(pid, key))
                readonly property bool isLocked: (Parental.revision, Parental.unlocked, OtherPlaylists.revision, root.locked(pid, key))
                // Plays once its playlist is loaded and it isn't locked.
                readonly property bool canPlay: loadState === "ready" && !isLocked && !!info.url
                readonly property rect r: root.rects[index] || Qt.rect(0, 0, 0, 0)
                readonly property bool isSelected: index === root.selected
                property string error: ""
                x: r.x; y: r.y; width: r.width; height: r.height
                Behavior on x { NumberAnimation { duration: Theme.animNormal; easing.type: Easing.OutCubic } }
                Behavior on y { NumberAnimation { duration: Theme.animNormal; easing.type: Easing.OutCubic } }
                Behavior on width { NumberAnimation { duration: Theme.animNormal; easing.type: Easing.OutCubic } }
                Behavior on height { NumberAnimation { duration: Theme.animNormal; easing.type: Easing.OutCubic } }

                function start() {
                    error = ""
                    if (!canPlay) { video.stop(); return }
                    const c = root.tileInfo(pid, key)
                    if (c.url) video.play(c.url, c.userAgent, c.referrer)
                }
                onKeyChanged: start()
                onPidChanged: start()
                onCanPlayChanged: start()

                Rectangle { anchors.fill: parent; color: "#101318" }
                MpvVideo {
                    id: video
                    anchors.fill: parent
                    timeshiftMb: 0                       // no pause buffer for tiles
                    deinterlace: Settings.deinterlace
                    hardwareDecoding: Settings.hardwareDecoding
                    muted: !tile.isSelected || root.mainVideo.muted
                    volume: root.mainVideo.volume
                    onPlaybackError: (m) => tile.error = m
                    onPlaybackStarted: tile.error = ""
                    Component.onCompleted: tile.start()
                }
                BusyIndicator {
                    anchors.centerIn: parent
                    running: video.buffering && !tile.error
                    visible: running
                    palette.dark: Theme.accent
                }
                Text {   // waiting for another playlist, or locked
                    anchors.centerIn: parent
                    width: parent.width - 40
                    visible: !tile.canPlay
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: tile.isLocked ? qsTr("🔒 Locked · select it and press Enter for the PIN")
                        : tile.loadState === "loading" ? qsTr("Loading %1…").arg(root.playlistName(tile.pid))
                        : tile.loadState === "failed" ? qsTr("Couldn't load %1: %2").arg(root.playlistName(tile.pid)).arg(OtherPlaylists.error(tile.pid))
                        : qsTr("This channel isn't in the playlist any more")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall + 1
                }
                Text {
                    anchors.centerIn: parent
                    width: parent.width - 40
                    visible: tile.error.length > 0 && tile.canPlay
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: tile.error + "\n" + qsTr("(Many providers allow only one or two streams at a time.)")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }
                // Channel label
                Rectangle {
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    anchors.margins: 10
                    height: 30
                    width: Math.min(parent.width - 20, label.implicitWidth + 24)
                    radius: 6
                    color: "#B0000000"
                    Text {
                        id: label
                        anchors.verticalCenter: parent.verticalCenter
                        x: 12
                        width: parent.width - 24
                        elide: Text.ElideRight
                        text: (tile.isSelected ? "♪  " : "") + (tile.other ? root.playlistName(tile.pid) + "  ·  " : "")
                              + (tile.info.number !== undefined ? tile.info.number + "  " : "") + (tile.info.name || "")
                              + (tile.info.nowTitle ? "  ·  " + tile.info.nowTitle : "")
                        color: Theme.text
                        font.pixelSize: Theme.fontSmall
                        font.weight: tile.isSelected ? Font.DemiBold : Font.Normal
                    }
                }
                Rectangle {   // selection
                    anchors.fill: parent
                    color: "transparent"
                    border.width: tile.isSelected ? 3 : 1
                    border.color: tile.isSelected ? Theme.accent : Theme.border
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: { root.selected = index; grid.forceActiveFocus() }
                    onDoubleClicked: { root.selected = index; root.close() }
                }
            }
        }

        // Empty "add" slot in a free cell
        Rectangle {
            readonly property int slot: tiles.count
            readonly property rect r: root.hasAddSlot ? root.rects[slot] : Qt.rect(0, 0, 0, 0)
            visible: root.hasAddSlot
            x: r.x; y: r.y; width: r.width; height: r.height
            color: "#0C0F14"
            border.width: root.selected === slot ? 3 : 1
            border.color: root.selected === slot ? Theme.accent : Theme.border
            Text {
                anchors.centerIn: parent
                text: qsTr("+  Add channel")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontLarge
            }
            MouseArea {
                anchors.fill: parent
                onClicked: { root.selected = parent.slot; root.addTile() }
            }
        }
    }

    // Key hints: shown for a few seconds on opening, H shows them again.
    // Buttons for the mouse (each shows its key); they appear on opening
    // and whenever the mouse moves.
    HoverHandler { onPointChanged: if (root.opened) hints.flash() }
    Rectangle {
        id: hints
        property bool shown: false
        readonly property bool hasTile: root.selected < tiles.count
        readonly property bool wide: root.width >= 1100
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        width: hintRow.implicitWidth + 16
        height: 56
        radius: 28
        color: "#E6141820"
        border.width: 1
        border.color: Theme.border
        opacity: (shown || hintHover.hovered) && !picker.opened ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: Theme.animNormal } }
        HoverHandler { id: hintHover }
        MouseArea { anchors.fill: parent; onWheel: (w) => w.accepted = true }
        Row {
            id: hintRow
            anchors.centerIn: parent
            spacing: 4
            BarButton {
                icon: Icons.back; label: qsTr("Previous channel"); shortcut: "Page Down"; showLabel: false
                visible: hints.hasTile
                onClicked: root.zap(-1)
            }
            BarButton {
                icon: Icons.back; rotation: 180; label: qsTr("Next channel"); shortcut: "Page Up"; showLabel: false
                visible: hints.hasTile
                onClicked: root.zap(1)
            }
            BarButton {
                icon: Icons.edit; label: qsTr("Change channel"); shortcut: "Enter"; showLabel: hints.wide
                onClicked: root.changeTile()
            }
            BarButton {
                icon: Icons.plus; label: qsTr("Add"); shortcut: "Insert"; showLabel: hints.wide
                visible: tiles.count < root.maxTiles
                onClicked: root.addTile()
            }
            BarButton {
                icon: Icons.remove; label: qsTr("Remove"); shortcut: "Delete"; showLabel: hints.wide
                visible: hints.hasTile && tiles.count > 1
                onClicked: root.removeSelected()
            }
            BarButton {
                icon: Icons.layout; label: qsTr("Layout: %1").arg(root.layoutName(root.layout)); shortcut: "L"; showLabel: hints.wide
                onClicked: root.cycleLayout()
            }
            BarButton {
                accent: true
                icon: Icons.fullscreen; label: qsTr("Watch full screen"); shortcut: "Esc"; showLabel: hints.wide
                onClicked: root.close()
            }
        }
        Timer { id: hintTimer; interval: 4000; onTriggered: hints.shown = false }
        function flash() { shown = true; hintTimer.restart() }
    }

    // ---- Channel picker -------------------------------------------------------------
    FocusScope {
        id: picker
        property bool opened: false
        property int target: -1            // tile to change; -1 = new tile
        property var results: []
        property string pid                // playlist to pick from
        readonly property bool multiple: Settings.playlists.length > 1
        readonly property string loadState: (OtherPlaylists.revision, root.isActive(pid) ? "ready" : OtherPlaylists.state(pid))
        visible: opened
        anchors.fill: parent

        function open(t) {
            target = t
            pid = t >= 0 && t < tiles.count ? tiles.get(t).pid : Settings.activeId
            field.text = ""
            refresh()
            opened = true
            field.forceActiveFocus()
        }
        // Tab (or a click on its name) picks the playlist to choose from.
        function setPlaylist(id) {
            pid = id
            if (!root.isActive(id)) OtherPlaylists.ensure(id)
            refresh()
        }
        function nextPlaylist() {
            const list = Settings.playlists
            if (list.length < 2) return
            let i = list.findIndex(p => p.id === pid)
            setPlaylist(list[(i + 1) % list.length].id)
        }
        Connections {
            target: OtherPlaylists
            function onChanged() { if (picker.opened && !root.isActive(picker.pid)) picker.refresh() }
        }
        function close() {
            if (!opened) return
            opened = false
            grid.forceActiveFocus()
        }
        function refresh() {
            const q = field.text.trim()
            if (!root.isActive(pid)) {
                results = OtherPlaylists.channels(pid, q, q.length ? 60 : 300)
            } else if (q.length) {
                results = Channels.searchChannels(q, 60)
            } else {
                const favs = Channels.listGroup(Channels.favoritesGroup, 300)
                results = favs.length ? favs : Channels.listGroup(Channels.allGroup, 300)
            }
            list.currentIndex = results.length ? 0 : -1
        }
        function choose(i) {
            const c = results[i]
            if (!c) return
            close()
            const t = target, p = pid
            const key = c.key !== undefined ? c.key : Channels.keyAt(c.globalIndex)
            if (root.locked(p, key))
                root.pinRequested(qsTr("\"%1\" is locked").arg(c.name), () => root.setTile(t, p, key))
            else root.setTile(t, p, key)
        }

        Rectangle {
            anchors.fill: parent
            color: "#A0000000"
            MouseArea { anchors.fill: parent; onClicked: picker.close(); onWheel: (w) => w.accepted = true }
        }
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(620, parent.width - 64)
            height: Math.min(parent.height - 80, 560)
            radius: 12
            color: Theme.panelSolid
            border.width: 1
            border.color: Theme.border
            MouseArea { anchors.fill: parent; onWheel: (w) => w.accepted = false }

            Text {
                x: 20; y: 16
                text: picker.target < 0 ? qsTr("Add a channel") : qsTr("Change channel")
                color: Theme.text
                font.pixelSize: Theme.fontLarge
                font.weight: Font.DemiBold
            }
            // Playlist chips (with more than one playlist)
            Row {
                id: chips
                visible: picker.multiple
                anchors.right: parent.right
                anchors.rightMargin: 20
                y: 14
                spacing: 6
                Repeater {
                    model: Settings.playlists
                    delegate: Rectangle {
                        readonly property bool current: modelData.id === picker.pid
                        height: 30
                        width: Math.min(150, chipText.implicitWidth + 24)
                        radius: 15
                        color: current ? Theme.accent : Theme.raised
                        Text {
                            id: chipText
                            anchors.centerIn: parent
                            width: Math.min(implicitWidth, 126)
                            elide: Text.ElideRight
                            text: modelData.name
                            color: parent.current ? Theme.accentText : Theme.textSecondary
                            font.pixelSize: Theme.fontSmall
                            font.weight: Font.Medium
                        }
                        MouseArea { anchors.fill: parent; onClicked: { picker.setPlaylist(modelData.id); field.forceActiveFocus() } }
                    }
                }
            }
            TextField {
                id: field
                x: 20; y: 52
                width: parent.width - 40
                height: 44
                placeholderText: qsTr("Type a channel name or number")
                placeholderTextColor: Theme.textMuted
                color: Theme.text
                font.pixelSize: Theme.fontBody
                selectionColor: Theme.accent
                background: Rectangle {
                    radius: Theme.radius
                    color: Theme.raised
                    border.width: field.activeFocus ? 2 : 1
                    border.color: field.activeFocus ? Theme.accent : Theme.border
                }
                onTextChanged: searchTimer.restart()
                Keys.onDownPressed: if (picker.results.length) list.forceActiveFocus()
                Keys.onReturnPressed: { searchTimer.stop(); picker.refresh(); picker.choose(0) }
                Keys.onEnterPressed: { searchTimer.stop(); picker.refresh(); picker.choose(0) }
                Keys.onEscapePressed: picker.close()
                Keys.onTabPressed: picker.nextPlaylist()
            }
            Timer { id: searchTimer; interval: 180; onTriggered: picker.refresh() }
            Text {
                x: 24; y: 118
                width: parent.width - 48
                visible: picker.results.length === 0
                wrapMode: Text.Wrap
                text: picker.loadState === "loading" ? qsTr("Loading %1's channels…").arg(root.playlistName(picker.pid))
                    : picker.loadState === "failed" ? qsTr("Couldn't load %1: %2").arg(root.playlistName(picker.pid)).arg(OtherPlaylists.error(picker.pid))
                    : field.text.trim().length ? qsTr("Nothing found") : ""
                color: picker.loadState === "failed" ? Theme.danger : Theme.textSecondary
                font.pixelSize: Theme.fontBody
            }
            Text {
                visible: picker.multiple
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 10
                text: qsTr("Tab: other playlist")
                color: Theme.textMuted
                font.pixelSize: 11
            }

            ListView {
                id: list
                x: 8
                y: 108
                width: parent.width - 16
                height: parent.height - y - 12
                clip: true
                model: picker.results
                keyNavigationEnabled: false
                highlightMoveDuration: 0
                boundsBehavior: Flickable.StopAtBounds
                Keys.onPressed: (e) => {
                    switch (e.key) {
                    case Qt.Key_Down: if (currentIndex < count - 1) currentIndex++; break
                    case Qt.Key_Up: if (currentIndex > 0) currentIndex--; else field.forceActiveFocus(); break
                    case Qt.Key_PageDown: currentIndex = Math.min(count - 1, currentIndex + 8); break
                    case Qt.Key_PageUp: currentIndex = Math.max(0, currentIndex - 8); break
                    case Qt.Key_Return:
                    case Qt.Key_Enter: picker.choose(currentIndex); break
                    case Qt.Key_Escape: picker.close(); break
                    case Qt.Key_Tab: picker.nextPlaylist(); break
                    default:
                        if (e.text.length && e.text.charCodeAt(0) > 32) {
                            field.forceActiveFocus()
                            field.insert(field.cursorPosition, e.text)
                            break
                        }
                        return
                    }
                    positionViewAtIndex(currentIndex, ListView.Contain)
                    e.accepted = true
                }
                delegate: Rectangle {
                    readonly property bool sel: ListView.isCurrentItem && list.activeFocus
                    width: list.width
                    height: 52
                    radius: 6
                    color: sel ? Theme.focus : (pickMouse.containsMouse ? Theme.raised : "transparent")
                    border.width: sel ? 2 : 0
                    border.color: Theme.accent
                    Row {
                        x: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 12
                        Text {
                            width: 48
                            horizontalAlignment: Text.AlignRight
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.number
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontBody
                        }
                        ChannelLogo {
                            width: 48; height: 32
                            anchors.verticalCenter: parent.verticalCenter
                            source: modelData.logo || ""
                            name: modelData.name
                        }
                        Text {
                            width: list.width - 160
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.name + (modelData.nowTitle ? "   ·   " + modelData.nowTitle : "")
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                            elide: Text.ElideRight
                        }
                    }
                    MouseArea {
                        id: pickMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: picker.choose(index)
                    }
                }
            }
        }
    }
}

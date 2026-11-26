import QtQuick
import QtQuick.Controls.Basic
import Relay.Core

// Movies and series. Tabs on top, categories on the left, posters on the
// right. Type to search, Tab switches Movies/Series, O changes the order,
// F favorites, Enter opens details, Esc goes back to TV.
FocusScope {
    id: root
    property bool opened: false
    signal closed()
    signal message(string text)
    signal playRequested(var descriptor)
    signal multiViewRequested(var descriptor)
    signal recordingsRequested()
    signal pinRequested(string reason, var onOk)

    readonly property int columns: Math.max(3, Math.floor(grid.width / 190))
    // The browse home (rows and a spotlight) or a category's full grid.
    property bool browsing: true

    function showHome() {
        browsing = true
        searchOpen = false
        if (searchField.text.length) searchField.text = ""
        Vod.filter = ""
        home.focusRows()
    }
    function showGrid() {
        browsing = false
        grid.forceActiveFocus()
    }
    // "See all" on a row of the browse home.
    function seeAll(id) {
        if (id === "recent" || id === "top") {
            Vod.sort = id === "recent" ? 1 : 3
            Vod.category = "all"
        } else {
            const i = Vod.categories.findIndex(c => c.id === id)
            if (i >= 0) selectCategory(i)
            else Vod.category = id
        }
        syncCategory()
        showGrid()
    }
    // Esc in the grid: clear the search, then back to browsing.
    function back() {
        if (searchField.text.length) { searchField.text = ""; Vod.filter = ""; if (!browsing) grid.forceActiveFocus(); return }
        if (!browsing) showHome()
        else close()
    }

    function open(section) {
        if (section !== undefined) Vod.section = section
        opened = true
        Vod.ensureLoaded()
        syncCategory()
        if (Vod.filter.length) showGrid()
        else { showHome(); home.reset() }
    }
    // Straight to one title (from search, or back from playback).
    function openDetails(kind, id, preview) {
        if (!opened) {
            Vod.section = kind === "movie" ? 0 : 1
            opened = true
            Vod.ensureLoaded()
            syncCategory()
        }
        details.open(kind, id, preview || ({}))
    }
    function close() {
        if (!opened) return
        details.close()
        opened = false
        closed()
    }
    function syncCategory() {
        const list = Vod.categories
        for (let i = 0; i < list.length; ++i)
            if (list[i].id === Vod.category) { categories.currentIndex = i; return }
        categories.currentIndex = -1
    }
    function selectCategory(i) {
        const c = Vod.categories[i]
        if (!c) return
        if (c.id.startsWith("c:") && Parental.categoryBlocked(Vod.section, c.id.substring(2))) {
            pinRequested(qsTr("\"%1\" is locked").arg(c.label), () => root.selectCategory(i))
            return
        }
        categories.currentIndex = i
        Vod.category = c.id
    }
    // K on a category: lock / unlock it (needs the PIN).
    function toggleCategoryLock(i) {
        const c = Vod.categories[i]
        if (!c || !c.id.startsWith("c:")) { message(qsTr("Only categories from your provider can be locked")); return }
        if (!Parental.hasPin) { message(qsTr("Set a parental PIN in Settings (S) first")); return }
        const doIt = () => {
            const locked = Parental.toggleCategoryLock(Vod.section, c.id.substring(2))
            message(locked ? qsTr("Category locked") : qsTr("Category unlocked"))
        }
        if (Parental.unlocked) doIt()
        else pinRequested(qsTr("Enter the PIN to change locks"), doIt)
    }
    // Tab: Movies → Series → Recordings
    function switchSection() {
        if (Vod.section === 0) { Vod.section = 1; syncCategory(); if (browsing) { home.reset(); home.focusRows() } }
        else recordingsRequested()
    }
    // A remote's or the phone's Search: the box at the top.
    function focusSearch() {
        details.close()
        searchOpen = true
        searchField.forceActiveFocus()
        searchField.selectAll()
    }
    // The controller's LB / RB (a remote's ⏪ / ⏩): the tab before / after,
    // round from Recordings to Movies.
    function stepSection(delta) {
        details.close()
        const next = (Vod.section + delta + 3) % 3
        if (next === 2) { recordingsRequested(); return }
        Vod.section = next
        syncCategory()
        showHome()
        home.reset()
    }
    function cycleSort() {
        Vod.sort = (Vod.sort + 1) % Vod.sortNames.length
        message(qsTr("Order: %1").arg(Vod.sortNames[Vod.sort]))
    }
    function openRow(i) {
        const r = Vod.rowAt(i)
        if (r.id) details.open(r.kind, r.id, r)
    }
    function toggleFavorite(i) {
        const r = Vod.rowAt(i)
        if (!r.id) return
        const fav = Vod.toggleFavorite(r.kind, r.id)
        message(fav ? qsTr("Added to favorites") : qsTr("Removed from favorites"))
    }
    function forget(i) {
        const r = Vod.rowAt(i)
        if (!r.id || Vod.category !== "continue") return
        Vod.forgetProgress(r.kind === "series" ? "series:" + r.id : r.progressKey)
        message(qsTr("Removed from Continue watching"))
    }

    visible: opened
    Keys.onEscapePressed: close()
    Keys.onPressed: (e) => {
        if (e.key === Qt.Key_AudioRewind) { stepSection(-1); e.accepted = true }
        else if (e.key === Qt.Key_AudioForward) { stepSection(1); e.accepted = true }
    }

    Connections {
        target: Vod
        function onCategoriesChanged() { root.syncCategory() }
        function onCategoryChanged() { root.syncCategory() }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.background
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = true }
    }

    // ---- Header: tabs in the middle, search and order on the right -------------------------
    // Up from the top of the page reaches the tabs; ← → move along them,
    // Enter or ↓ opens one.
    property bool searchOpen: false
    Item {
        id: header
        width: parent.width
        height: Math.round(Math.max(64, Math.min(100, root.height * 0.095)))
        z: 5
        visible: !details.opened     // a title's page has its own Back
        Rectangle {   // over the page's tint
            anchors.fill: parent
            anchors.bottomMargin: -parent.height * 0.4
            visible: root.browsing && !details.opened
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#C0000000" }
                GradientStop { position: 1.0; color: "transparent" }
            }
        }

        FocusScope {
            id: tabBar
            anchors.centerIn: parent
            width: tabRow.width
            height: tabRow.height
            readonly property var tabs: [
                { id: "search", icon: Icons.search, label: "" },
                { id: "movies", icon: "", label: qsTr("Movies") },
                { id: "series", icon: "", label: qsTr("Series") },
                { id: "recordings", icon: "", label: qsTr("Recordings") },
            ]
            readonly property int sectionTab: Vod.section === 1 ? 2 : 1
            property int at: sectionTab
            onActiveFocusChanged: if (activeFocus) at = root.searchOpen ? 0 : sectionTab
            function openTab(i) {
                const t = tabs[i].id
                if (t === "search") { root.focusSearch(); return }
                if (t === "recordings") { root.recordingsRequested(); return }
                root.searchOpen = false
                const section = t === "series" ? 1 : 0
                if (Vod.section !== section) { Vod.section = section; root.syncCategory() }
                root.showHome()
                home.reset()
            }
            Keys.onPressed: (e) => {
                switch (e.key) {
                case Qt.Key_Left: at = Math.max(0, at - 1); break
                case Qt.Key_Right: at = Math.min(tabs.length - 1, at + 1); break
                case Qt.Key_Return:
                case Qt.Key_Enter: openTab(at); break
                case Qt.Key_Down:
                    if (at !== sectionTab && at !== 0) openTab(at)
                    else if (at === 0) root.focusSearch()
                    else root.browsing ? home.focusRows() : grid.forceActiveFocus()
                    break
                case Qt.Key_Escape:
                case Qt.Key_Backspace: root.close(); break
                default: return
                }
                e.accepted = true
            }

            Row {
                id: tabRow
                spacing: Math.round(header.height * 0.12)
                Repeater {
                    model: tabBar.tabs
                    delegate: Rectangle {
                        id: tab
                        required property var modelData
                        required property int index
                        readonly property bool current: index === 0 ? root.searchOpen : (!root.searchOpen && index === tabBar.sectionTab)
                        readonly property bool lit: tabBar.activeFocus && tabBar.at === index
                        height: Math.round(header.height * 0.5)
                        width: modelData.label ? tabText.implicitWidth + height * 0.9 : height
                        radius: height / 2
                        color: lit ? Theme.text : current ? "#40FFFFFF" : (tabMouse.containsMouse ? "#22FFFFFF" : "transparent")
                        Behavior on color { ColorAnimation { duration: Theme.animFast } }
                        Text {
                            id: tabText
                            visible: tab.modelData.label.length > 0
                            anchors.centerIn: parent
                            text: tab.modelData.label + (tab.index === 3 && Schedule.recordingCount > 0 ? "  ●" : "")
                            color: tab.lit ? Theme.background : Theme.text
                            opacity: tab.lit || tab.current ? 1 : 0.8
                            font.pixelSize: Math.round(Math.max(Theme.fontBody, Math.min(24, header.height * 0.24)))
                            font.weight: tab.lit || tab.current ? Font.DemiBold : Font.Medium
                        }
                        Icon {
                            visible: tab.modelData.icon.length > 0
                            anchors.centerIn: parent
                            width: Math.round(parent.height * 0.5)
                            height: width
                            path: tab.modelData.icon
                            color: tab.lit ? Theme.background : Theme.text
                        }
                        MouseArea {
                            id: tabMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: tabBar.openTab(tab.index)
                        }
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
        Row {
            id: headerRight
            anchors.right: closeButton.left
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10
            visible: !root.browsing || root.searchOpen
            // Room between the tabs and ✕ (a narrow window: the search box
            // shrinks, then the order button shows only ⇅).
            readonly property real room: closeButton.x - 12 - (tabBar.x + tabBar.width + 20)
            readonly property bool compactSort: room - (sortFull.width + 32) - spacing < 160
            // Very narrow: no order button at all (O still changes the order).
            readonly property bool noSort: room - 44 - spacing < 120
            TextMetrics { id: sortFull; font: sortText.font; text: "⇅  " + Vod.sortNames[Vod.sort] }

            Rectangle {   // order
                visible: !root.browsing && !headerRight.noSort
                height: 44
                width: headerRight.compactSort ? 44 : sortFull.width + 32
                radius: 22
                color: sortMouse.containsMouse ? "#38FFFFFF" : "#22FFFFFF"
                Text {
                    id: sortText
                    anchors.centerIn: parent
                    text: headerRight.compactSort ? "⇅" : "⇅  " + Vod.sortNames[Vod.sort]
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall + 1
                }
                MouseArea {
                    id: sortMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: sortMenu.popup()
                }
                RelayMenu {
                    id: sortMenu
                    Repeater {
                        model: Vod.sortNames
                        delegate: RelayMenuItem {
                            text: (index === Vod.sort ? "✓  " : "     ") + modelData
                            onTriggered: { Vod.sort = index; grid.forceActiveFocus() }
                        }
                    }
                }
            }

            TextField {
                id: searchField
                width: Math.max(96, Math.min(360, root.width * 0.26,
                                             headerRight.room - (root.browsing || headerRight.noSort ? 0
                                                                 : (headerRight.compactSort ? 44 : sortFull.width + 32) + headerRight.spacing)))
                height: 44
                placeholderText: Vod.section === 0 ? qsTr("Search movies") : qsTr("Search series")
                placeholderTextColor: Theme.textMuted
                color: Theme.text
                font.pixelSize: Theme.fontBody
                leftPadding: 40
                selectByMouse: true
                selectionColor: Theme.accent
                selectedTextColor: Theme.accentText
                background: Rectangle {
                    radius: 22
                    color: "#22FFFFFF"
                    border.width: searchField.activeFocus ? 2 : 0
                    border.color: Theme.text
                }
                Icon {
                    x: 14
                    anchors.verticalCenter: parent.verticalCenter
                    width: 18; height: 18
                    path: Icons.search
                    color: Theme.textMuted
                }
                onTextChanged: {
                    filterTimer.restart()
                    if (!text.length) return
                    root.browsing = false
                    // A search looks through everything, not the category that was open.
                    if (Vod.category !== "all") { Vod.category = "all"; root.syncCategory() }
                }
                Keys.onUpPressed: tabBar.forceActiveFocus()
                Keys.onDownPressed: root.browsing ? home.focusRows() : grid.forceActiveFocus()
                Keys.onReturnPressed: { filterTimer.stop(); Vod.filter = text; grid.forceActiveFocus() }
                Keys.onEnterPressed: { filterTimer.stop(); Vod.filter = text; grid.forceActiveFocus() }
                Keys.onEscapePressed: {
                    if (text.length) { text = ""; Vod.filter = ""; root.searchOpen = false; root.showHome() }
                    else { root.searchOpen = false; root.browsing ? home.focusRows() : grid.forceActiveFocus() }
                }
                Keys.onTabPressed: { root.switchSection(); root.browsing ? home.focusRows() : grid.forceActiveFocus() }
            }
        }
        Timer { id: filterTimer; interval: 200; onTriggered: Vod.filter = searchField.text }
    }

    // ---- Categories -----------------------------------------------------------
    ListView {
        id: categories
        visible: !root.browsing
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        x: 16
        width: 280
        clip: true
        model: Vod.categories
        keyNavigationEnabled: false
        highlightMoveDuration: 0
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Keys.onPressed: (e) => {
            switch (e.key) {
            case Qt.Key_Up: if (currentIndex > 0) root.selectCategory(currentIndex - 1); break
            case Qt.Key_Down: if (currentIndex < count - 1) root.selectCategory(currentIndex + 1); break
            case Qt.Key_PageUp: root.selectCategory(Math.max(0, currentIndex - 10)); break
            case Qt.Key_PageDown: root.selectCategory(Math.min(count - 1, currentIndex + 10)); break
            case Qt.Key_Home: root.selectCategory(0); break
            case Qt.Key_End: root.selectCategory(count - 1); break
            case Qt.Key_Right:
            case Qt.Key_Return:
            case Qt.Key_Enter:
                if (grid.count) grid.forceActiveFocus(); break
            case Qt.Key_Tab: root.switchSection(); break
            case Qt.Key_O: root.cycleSort(); break
            case Qt.Key_K: root.toggleCategoryLock(currentIndex); break
            case Qt.Key_Escape:
            case Qt.Key_Backspace: root.back(); break
            default:
                if (e.text.length && e.text.charCodeAt(0) > 32) {
                    searchField.forceActiveFocus()
                    searchField.insert(searchField.cursorPosition, e.text)
                    break
                }
                return
            }
            positionViewAtIndex(currentIndex, ListView.Contain)
            e.accepted = true
        }

        delegate: Rectangle {
            readonly property bool current: ListView.isCurrentItem
            width: categories.width - 12
            height: 46
            radius: 6
            color: current ? (categories.activeFocus ? Theme.focus : Theme.raised) : "transparent"
            border.width: current && categories.activeFocus ? 2 : 0
            border.color: Theme.accent
            Text {
                x: 14
                width: parent.width - 80
                anchors.verticalCenter: parent.verticalCenter
                text: ((Parental.revision, modelData.id.startsWith("c:") && Parental.isCategoryLocked(Vod.section, modelData.id.substring(2)))
                       ? "🔒 " : "") + modelData.label
                color: parent.current ? Theme.text : Theme.textSecondary
                font.pixelSize: Theme.fontBody - 1
                font.weight: parent.current ? Font.DemiBold : Font.Normal
                elide: Text.ElideRight
            }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                text: Number(modelData.count).toLocaleString(Qt.locale(), "f", 0)
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
            }
            MouseArea {
                anchors.fill: parent
                onClicked: { root.selectCategory(index); categories.forceActiveFocus() }
            }
        }
    }

    // ---- Posters --------------------------------------------------------------
    GridView {
        id: grid
        visible: !root.browsing
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.left: categories.right
        anchors.leftMargin: 16
        anchors.right: parent.right
        anchors.rightMargin: 16
        clip: true
        model: Vod.items
        cellWidth: Math.floor(width / root.columns)
        cellHeight: Math.round((cellWidth - 20) * 1.5) + 66
        keyNavigationEnabled: false
        highlightMoveDuration: 0
        cacheBuffer: cellHeight * 2
        boundsBehavior: Flickable.StopAtBounds
        topMargin: 8
        bottomMargin: 24
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        // After the list changes (new category, search, section) start at the first poster.
        onCountChanged: if (count > 0 && (currentIndex < 0 || currentIndex >= count)) currentIndex = 0

        function move(to) {
            if (to < 0 || to >= count) return
            currentIndex = to
            positionViewAtIndex(to, GridView.Contain)
        }

        Keys.onPressed: (e) => {
            const cols = root.columns
            if (currentIndex < 0 && count > 0) currentIndex = 0
            const i = currentIndex
            switch (e.key) {
            case Qt.Key_Left:
                if (i % cols === 0 || i < 0) categories.forceActiveFocus()
                else move(i - 1)
                break
            case Qt.Key_Right:
                if ((i + 1) % cols !== 0) move(i + 1)
                break
            case Qt.Key_Up:
                if (i - cols >= 0) move(i - cols)
                else searchField.forceActiveFocus()
                break
            case Qt.Key_Down:
                if (i + cols < count) move(i + cols)
                else if (Math.floor(i / cols) < Math.floor((count - 1) / cols)) move(count - 1)
                break
            case Qt.Key_PageUp: move(Math.max(i % cols, i - cols * 3)); break
            case Qt.Key_PageDown: move(Math.min(count - 1, i + cols * 3)); break
            case Qt.Key_Home: move(0); break
            case Qt.Key_End: move(count - 1); break
            case Qt.Key_Return:
            case Qt.Key_Enter: root.openRow(i); break
            case Qt.Key_F: root.toggleFavorite(i); break
            case Qt.Key_Delete: root.forget(i); break
            case Qt.Key_Tab: root.switchSection(); break
            case Qt.Key_O: root.cycleSort(); break
            case Qt.Key_Escape:
            case Qt.Key_Backspace: root.back(); break
            default:
                if (e.text.length && e.text.charCodeAt(0) > 32 && !(e.modifiers & Qt.ControlModifier)) {
                    searchField.forceActiveFocus()
                    searchField.insert(searchField.cursorPosition, e.text)
                    break
                }
                return
            }
            e.accepted = true
        }

        delegate: Item {
            width: grid.cellWidth
            height: grid.cellHeight
            readonly property bool sel: GridView.isCurrentItem && grid.activeFocus

            Poster {
                id: poster
                x: 10
                y: 8
                width: parent.width - 20
                height: Math.round(width * 1.5)
                source: model.poster
                title: model.title
                progress: model.progress
                watched: model.watched
                favorite: model.favorite
                selected: parent.sel
            }
            Text {
                anchors.top: poster.bottom
                anchors.topMargin: 10
                x: 12
                width: parent.width - 24
                text: model.title
                color: parent.sel ? Theme.text : Theme.textSecondary
                font.pixelSize: Theme.fontSmall + 1
                font.weight: parent.sel ? Font.DemiBold : Font.Normal
                elide: Text.ElideRight
            }
            Text {
                anchors.top: poster.bottom
                anchors.topMargin: 31
                x: 12
                width: parent.width - 24
                text: model.subtitle || [model.year, model.rating > 0 ? "★ " + Number(model.rating).toFixed(1) : ""]
                                         .filter(s => s).join("   ")
                color: model.subtitle ? Theme.accent : Theme.textMuted
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
            }
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onClicked: (e) => {
                    grid.currentIndex = index
                    grid.forceActiveFocus()
                    if (e.button === Qt.RightButton) { itemMenu.favorite = model.favorite; itemMenu.popup() }
                }
                onDoubleClicked: root.openRow(index)
            }
        }
    }

    // Right-click on a poster
    RelayMenu {
        id: itemMenu
        property bool favorite: false
        RelayMenuItem { text: qsTr("Open"); onTriggered: root.openRow(grid.currentIndex) }
        RelayMenuItem {
            text: itemMenu.favorite ? qsTr("Remove from favorites") : qsTr("Add to favorites")
            onTriggered: root.toggleFavorite(grid.currentIndex)
        }
        RelayMenuItem {
            enabled: Vod.category === "continue"
            height: enabled ? implicitHeight : 0
            visible: enabled
            text: qsTr("Remove from Continue watching")
            onTriggered: root.forget(grid.currentIndex)
        }
    }

    // ---- Loading / empty / error ----------------------------------------------
    Column {
        anchors.centerIn: grid
        spacing: 14
        visible: Vod.loading && grid.count === 0
        BusyIndicator {
            anchors.horizontalCenter: parent.horizontalCenter
            running: parent.visible
            palette.dark: Theme.accent
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: Vod.status
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
        }
    }
    Column {
        anchors.centerIn: grid
        width: Math.min(520, grid.width - 40)
        spacing: 16
        visible: !Vod.loading && grid.count === 0
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: !Vod.available ? qsTr("This playlist has no movies or series.")
                  : Vod.error ? Vod.error
                  : Vod.filter ? qsTr("Nothing found for \"%1\"").arg(Vod.filter)
                  : Vod.category === "continue" ? qsTr("Nothing in progress. Movies and episodes you start show up here.")
                  : Vod.category === "favorites" ? qsTr("No favorites yet. Press F on a poster to add one.")
                  : qsTr("Nothing here.")
            color: Vod.error ? Theme.danger : Theme.textSecondary
            font.pixelSize: Theme.fontBody
        }
        ActionButton {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: Vod.error.length > 0
            text: qsTr("Try again")
            onClicked: Vod.reload()
        }
    }
    // Background refresh of an already-shown list
    Rectangle {
        visible: Vod.loading && grid.count > 0
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 24
        height: 36
        width: refreshText.implicitWidth + 28
        radius: 18
        color: Theme.panel
        Text {
            id: refreshText
            anchors.centerIn: parent
            text: Vod.status || qsTr("Updating…")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
        }
    }

    VodHome {
        id: home
        visible: root.browsing && root.opened && !details.opened
        anchors.fill: parent
        topInset: header.height
        onUpFromTop: tabBar.forceActiveFocus()
        onOpenDetails: (kind, id, preview) => details.open(kind, id, preview)
        onSeeAll: (id) => root.seeAll(id)
        onPlay: (d) => root.playRequested(d)
        onMessage: (text) => root.message(text)
        Keys.onTabPressed: root.switchSection()
        Keys.onPressed: (e) => {
            if (e.key === Qt.Key_Backspace) { root.close(); e.accepted = true; return }
            // Typing searches.
            if (e.text.length && e.text.charCodeAt(0) > 32 && !(e.modifiers & Qt.ControlModifier)
                    && e.key !== Qt.Key_P && e.key !== Qt.Key_F && e.key !== Qt.Key_M) {
                root.searchOpen = true
                searchField.forceActiveFocus()
                searchField.insert(searchField.cursorPosition, e.text)
                e.accepted = true
            }
        }
    }

    VodDetails {
        id: details
        anchors.fill: parent
        onClosed: root.browsing ? home.focusRows() : grid.forceActiveFocus()
        onPlay: (d) => root.playRequested(d)
        onMultiView: (d) => root.multiViewRequested(d)
        onMessage: (text) => root.message(text)
    }
}

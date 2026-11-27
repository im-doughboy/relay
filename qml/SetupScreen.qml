import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import Relay.Core

// First-run and "Playlist settings" screen: add an M3U link/file or sign in
// with Xtream Codes. The playlist is saved only after it loads successfully.
FocusScope {
    id: root
    property bool canClose: Session.ready
    // 0 = M3U, 1 = Xtream, 2 = Stalker / MAC portal (kept as an Xtream login whose output is "portal")
    readonly property bool savedIsPortal: Settings.sourceType === "xtream" && Settings.xtreamOutput === "portal"
    property int mode: savedIsPortal ? 2 : Settings.sourceType === "xtream" ? 1 : 0
    // Adding another playlist (the form is empty and makes a new one).
    property bool newPlaylist: false
    readonly property bool editing: Settings.hasSource && !newPlaylist
    property string confirmRemove: ""     // playlist id: Remove pressed once
    property string backupStatus: ""
    property bool backupFailed: false
    property string confirmRestore: ""    // backup file chosen, waiting for "Restore and restart"
    property string restoreSummary: ""

    // A caption with a row of choices; the current one is highlighted.
    component ChoiceRow: Column {
        id: choiceRow
        property string caption
        property var choices: []          // [{label, value}]
        property var value
        signal chosen(var value)
        width: card.width
        spacing: 8
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: choiceRow.caption
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
        }
        Flow {
            width: parent.width
            spacing: 8
            Repeater {
                model: choiceRow.choices
                delegate: ActionButton {
                    implicitWidth: Math.max(92, contentItem.implicitWidth + 28)
                    text: modelData.label
                    primary: choiceRow.value === modelData.value
                    onClicked: choiceRow.chosen(modelData.value)
                }
            }
        }
    }
    function offsetText(m) {
        if (m === 0) return qsTr("Not moved")
        const h = Math.floor(Math.abs(m) / 60), min = Math.abs(m) % 60
        const t = (h ? h + " h" : "") + (h && min ? " " : "") + (min ? min + " min" : "")
        return m > 0 ? qsTr("%1 later").arg(t) : qsTr("%1 earlier").arg(t)
    }
    // Each language in its own name, so it's easy to find.
    readonly property var languageChoices: [
        { label: "English", value: "en" }, { label: "Español", value: "es" }, { label: "Français", value: "fr" },
        { label: "Deutsch", value: "de" }, { label: "Italiano", value: "it" }, { label: "Português", value: "pt" },
        { label: "Nederlands", value: "nl" }, { label: "Polski", value: "pl" }, { label: "Türkçe", value: "tr" },
        { label: "Русский", value: "ru" }, { label: "العربية", value: "ar" }, { label: "हिन्दी", value: "hi" } ]
    signal closeRequested()
    signal pinRequested(string mode, string reason, var onOk)

    // ---- Sections ------------------------------------------------------------------------
    // Once there's a playlist, Settings is a list of sections on the left and
    // the chosen one on the right. Before that (and while adding a playlist)
    // it's just the playlist form.
    readonly property var sections: [
        { id: "playlist", title: qsTr("Playlist"), icon: Icons.playlists },
        { id: "guide", title: qsTr("TV guide"), icon: Icons.guide },
        { id: "playback", title: qsTr("Playback & recording"), icon: Icons.play },
        { id: "vod", title: qsTr("Movies & series"), icon: Icons.movies },
        { id: "trakt", title: qsTr("Trakt"), icon: Icons.recordings },
        { id: "controls", title: qsTr("Controls"), icon: Icons.keyboard },
        { id: "parental", title: qsTr("Parental controls"), icon: Icons.settings },
        { id: "about", title: qsTr("Updates & backup"), icon: Icons.download },
    ]
    property string section: "playlist"
    readonly property bool sidebar: Settings.hasSource && !newPlaylist
    // Before the first playlist: set up from a phone (ROADMAP 0.6). The phone's
    // page runs while this screen shows, and stops once a playlist is saved
    // (unless the phone remote is on in Settings).
    readonly property bool phoneSetup: !Settings.hasSource
    readonly property real phonePanelWidth: 300
    readonly property bool phoneBeside: phoneSetup && scroller.width >= 560 + 48 + phonePanelWidth + 64
    Binding { target: Phone; property: "setup"; value: root.visible && root.phoneSetup }
    // A provider sent from the phone's Add a provider form: into this form,
    // then Add playlist, as if typed here. The phone hears "busy" while it
    // loads and "failed" if it doesn't (the reason shows here).
    property bool phoneSent: false
    Binding { target: Phone; property: "setupStatus"; value: !root.phoneSent ? "" : Playlists.busy ? "busy" : Session.lastError.length ? "failed" : "busy" }
    Connections {
        target: Phone
        function onProviderSubmitted(p) {
            if (!root.visible || !root.phoneSetup || Playlists.busy) return
            root.mode = p.kind === "m3u" ? 0 : p.kind === "portal" ? 2 : 1
            nameField.text = p.name
            m3uField.text = p.kind === "m3u" ? p.link : ""
            serverField.text = p.kind === "xtream" ? p.server : ""
            userField.text = p.kind === "xtream" ? p.user : ""
            passField.text = p.kind === "xtream" ? p.pass : ""
            portalField.text = p.kind === "portal" ? p.portal : ""
            macField.text = p.kind === "portal" ? p.mac : ""
            Session.clearError()
            root.phoneSent = true
            root.submit()
        }
    }
    readonly property string shown: sidebar ? section : "playlist"
    function showSection(id) {
        if (section === id) return
        section = id
        scroller.contentY = 0
    }
    // Into the section: its first field or button.
    function enterSection() {
        const inside = (it) => { for (let p = it; p; p = p.parent) if (p === scroller.contentItem) return true; return false }
        let next = sectionList.nextItemInFocusChain(true)
        for (let guard = 0; next && guard < 400 && (!inside(next) || !next.visible || !next.enabled); ++guard)
            next = next.nextItemInFocusChain(true)
        if (next && inside(next)) next.forceActiveFocus(Qt.TabFocusReason)
    }

    function focusFirst() {
        if (mode === 0) m3uField.field.forceActiveFocus()
        else if (mode === 2) portalField.field.forceActiveFocus()
        else serverField.field.forceActiveFocus()
    }
    // Where the keyboard rests on opening: in the address box only when
    // there's no playlist yet (typing it is the first thing to do). With one,
    // nothing is in a text box, so the key that opened Settings (S) or a
    // stray key can't change the address. Tab, ↓ or Enter (or a click) go in.
    function focusStart() {
        if (sidebar) sectionList.forceActiveFocus()
        else if (editing) idleFocus.forceActiveFocus()
        else focusFirst()
    }
    Item {
        id: idleFocus
        Keys.onPressed: (e) => {
            if (e.key === Qt.Key_Tab || e.key === Qt.Key_Down || e.key === Qt.Key_Return || e.key === Qt.Key_Enter) {
                root.focusFirst()
                e.accepted = true
            }
        }
    }
    function guideStatusText() {
        if (Epg.busy) return Epg.status
        if (Epg.lastError) return Epg.lastError
        if (!Epg.hasGuide) return qsTr("No guide loaded yet.")
        return qsTr("%L1 programmes · updated %2")
            .arg(Epg.programmeCount)
            .arg(Qt.formatDate(Epg.lastUpdated, Locale.ShortFormat) + " "
                 + Qt.formatTime(Epg.lastUpdated, Qt.locale().timeFormat(Locale.ShortFormat).replace(":ss", "")))
    }

    function submit() {
        if (Playlists.busy) return
        const output = hlsToggle.checked ? "m3u8" : "ts"
        if (!editing) {
            Session.setPendingUserAgent(uaField.text)
            if (mode === 0) Session.addNewM3u(nameField.text, m3uField.text, epgField.text)
            else if (mode === 2) Session.addNewXtream(nameField.text, portalField.text, macField.text, "", "portal", epgField.text)
            else Session.addNewXtream(nameField.text, serverField.text, userField.text, passField.text, output, epgField.text)
            return
        }
        if (nameField.text.trim().length) Settings.renamePlaylist(Settings.activeId, nameField.text)
        Settings.epgOverride = epgField.text
        Settings.userAgent = uaField.text
        if (mode === 0) Session.addM3u(m3uField.text)
        else if (mode === 2) Session.addXtream(portalField.text, macField.text, "", "portal")
        else Session.addXtream(serverField.text, userField.text, passField.text, output)
    }
    function startNewPlaylist() {
        newPlaylist = true
        Session.clearError()
        nameField.text = m3uField.text = serverField.text = userField.text = passField.text = epgField.text = uaField.text = ""
        portalField.text = macField.text = ""
        hlsToggle.checked = false
        mode = 0
        Qt.callLater(() => nameField.field.forceActiveFocus())
    }
    function cancelNewPlaylist() {
        Session.setPendingUserAgent("")
        newPlaylist = false
        prepare()
    }
    function removePlaylist(id) {
        if (confirmRemove !== id) { confirmRemove = id; return }
        confirmRemove = ""
        Session.removePlaylistById(id)
    }

    function prepare() {
        if (!Playlists.busy) Session.clearError()
        newPlaylist = false
        confirmRemove = ""
        mode = savedIsPortal ? 2 : Settings.sourceType === "xtream" ? 1 : 0
        nameField.text = Settings.activeName
        m3uField.text = Settings.m3uLocation
        serverField.text = savedIsPortal ? "" : Settings.xtreamServer
        userField.text = savedIsPortal ? "" : Settings.xtreamUser
        portalField.text = savedIsPortal ? Settings.xtreamServer : ""
        macField.text = savedIsPortal ? Settings.xtreamUser : ""
        passField.text = ""
        hlsToggle.checked = Settings.xtreamOutput === "m3u8"
        epgField.text = Settings.epgOverride
        uaField.text = Settings.userAgent
        backupStatus = ""
        confirmRestore = ""
        Qt.callLater(focusStart)
    }
    onVisibleChanged: if (visible) prepare()
    Component.onCompleted: if (visible) prepare()

    property bool showHidden: false
    // Esc: leave the Hidden list first, then settings (when there's a
    // playlist to go back to). Main.qml calls this from a window-wide
    // shortcut so it works whatever has keyboard focus.
    function goBack() {
        if (keysDialog.opened) keysDialog.back()
        else if (showHidden) { showHidden = false; focusStart() }
        else if (sidebar && !sectionList.activeFocus) sectionList.forceActiveFocus()   // a section: back to the list
        else if (newPlaylist && Settings.hasSource) cancelNewPlaylist()
        else if (canClose) closeRequested()
    }
    Keys.onEscapePressed: goBack()
    // Arrows move between fields and buttons (remotes, controllers, the
    // phone): ↑ ↓ to the row above or below (the nearest control in it),
    // ← → along a row (← from a row's first control: the section list).
    // In a text box ← → stay with the text.
    function moveFocus(key) {
        const w = root.Window.window
        const item = w ? w.activeFocusItem : null
        if (!item || keysDialog.opened || root.showHidden) return false
        if (item === idleFocus) { root.focusFirst(); return true }
        const inside = (it) => { for (let p = it; p; p = p.parent) if (p === scroller.contentItem) return true; return false }
        if (!inside(item)) return false
        const usable = (it) => it && inside(it) && it.visible && it.enabled && it.width > 0
        const pos = (it) => { const p = it.mapToItem(scroller.contentItem, 0, 0); return { x: p.x + it.width / 2, y: p.y, h: it.height } }
        const sameRow = (a, b) => Math.abs(a.y - b.y) < Math.min(a.h, b.h) / 2
        const here = pos(item)
        const forward = key === Qt.Key_Down || key === Qt.Key_Right
        // The controls in focus order, one way, until the chain leaves the section.
        let next = item.nextItemInFocusChain(forward)
        const seen = []
        for (let guard = 0; next && guard < 400 && next !== item; ++guard) {
            if (usable(next)) seen.push(next)
            next = next.nextItemInFocusChain(forward)
        }
        let target = null
        if (key === Qt.Key_Left || key === Qt.Key_Right) {
            target = seen.find(it => sameRow(pos(it), here) && (forward ? pos(it).x > here.x : pos(it).x < here.x)) || null
            if (!target && key === Qt.Key_Left && root.sidebar) { sectionList.forceActiveFocus(); return true }
        } else {
            // The first row past this one, then its control nearest across.
            const first = seen.find(it => !sameRow(pos(it), here) && (forward ? pos(it).y > here.y : pos(it).y < here.y))
            if (first) {
                const row = seen.filter(it => sameRow(pos(it), pos(first)))
                target = row.reduce((a, b) => Math.abs(pos(a).x - here.x) <= Math.abs(pos(b).x - here.x) ? a : b)
            } else if (!forward && root.sidebar) { sectionList.forceActiveFocus(); return true }
        }
        if (target) target.forceActiveFocus(Qt.TabFocusReason)
        return true
    }
    Keys.onPressed: (e) => {
        if (e.key !== Qt.Key_Up && e.key !== Qt.Key_Down && e.key !== Qt.Key_Left && e.key !== Qt.Key_Right) return
        const w = root.Window.window
        const item = w ? w.activeFocusItem : null
        const typing = item && item.cursorPosition !== undefined && item.selectByMouse !== undefined
        if (typing && (e.key === Qt.Key_Left || e.key === Qt.Key_Right)) return
        if (moveFocus(e.key)) e.accepted = true
    }

    // Safety net: never leave the screen without keyboard focus, or no key
    // (not even Esc) would reach it.
    function restoreFocus() {
        if (!visible || activeFocus || !focusGuard || Qt.application.state !== Qt.ApplicationActive) return
        if (keysDialog.opened) return
        if (showHidden) hiddenManager.forceActiveFocus()
        else focusStart()
    }
    // Off while another dialog (the PIN) is on top and needs the keyboard.
    property bool focusGuard: true
    onActiveFocusChanged: if (visible && !activeFocus && focusGuard) Qt.callLater(restoreFocus)

    Connections {
        target: Session
        function onReadyChanged() { if (Session.ready && root.visible) root.closeRequested() }
    }
    Connections {
        target: Playlists
        function onLoaded() { if (root.visible) root.closeRequested() }
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

    FileDialog {
        id: fileDialog
        title: qsTr("Choose a playlist file")
        nameFilters: [qsTr("Playlists (*.m3u *.m3u8)"), qsTr("All files (*)")]
        onAccepted: m3uField.text = selectedFile.toString()
    }

    // Scrolls when the window is shorter than the settings.
    Flickable {
        id: scroller
        anchors.fill: parent
        contentWidth: width
        contentHeight: Math.max(height, card.implicitHeight + 64)
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        // Keep the field with keyboard focus on screen.
        Connections {
            target: root.Window.window
            function onActiveFocusItemChanged() {
                const item = root.Window.window ? root.Window.window.activeFocusItem : null
                if (!item || !root.visible) return
                let inside = false
                for (let it = item; it; it = it.parent) if (it === scroller.contentItem) { inside = true; break }
                if (!inside) return
                const p = item.mapToItem(scroller.contentItem, 0, 0)
                if (p.y < scroller.contentY + 24) scroller.contentY = Math.max(0, p.y - 24)
                else if (p.y + item.height > scroller.contentY + scroller.height - 24)
                    scroller.contentY = Math.min(scroller.contentHeight - scroller.height,
                                                 p.y + item.height - scroller.height + 24)
            }
        }

    Column {
        id: card
        // Beside the section list (once there's a playlist), centred alone before.
        readonly property real leftEdge: root.sidebar ? sidePanel.width + 24 : 0
        x: root.phoneBeside ? (scroller.width - width - 48 - root.phonePanelWidth) / 2
                            : leftEdge + (scroller.width - leftEdge - width) / 2
        y: root.sidebar ? 32 : Math.max(32, (scroller.height - implicitHeight) / 2)
        width: Math.min(root.sidebar ? 640 : 560, root.width - leftEdge - 64)
        spacing: 28
        // The interface size when Settings opened (a change needs a restart).
        property int startScale: 100
        Component.onCompleted: startScale = Settings.uiScale

        Column {
            visible: !root.sidebar
            spacing: 8
            Row {
                spacing: 12
                Rectangle {
                    width: 14; height: 14; radius: 3
                    color: Theme.accent
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    text: "Relay"
                    color: Theme.text
                    font.pixelSize: 34
                    font.weight: Font.Bold
                    font.letterSpacing: -0.5
                }
            }
            Text {
                text: !Settings.hasSource ? qsTr("Add your playlist to start watching")
                      : root.newPlaylist ? qsTr("Add another playlist")
                      : qsTr("Settings · %1").arg(Settings.activeName)
                color: Theme.textSecondary
                font.pixelSize: Theme.fontLarge
            }
        }

        // Set up from the phone, above the form when the window is narrow.
        PhoneSetup {
            visible: root.phoneSetup && !root.phoneBeside
            width: card.width
        }

        // On a TV, typing a playlist address with the remote is slow: the
        // phone remote comes first, and its page types into the field. Before
        // the first playlist it's turned on (and PhoneSetup shows its code).
        Column {
            visible: Settings.tv && root.newPlaylist
            width: card.width
            spacing: 10
            Component.onCompleted: if (Settings.tv && !Settings.hasSource && !Phone.enabled) Phone.enabled = true
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: Phone.running ? qsTr("Pair your phone, then type the address on it: it goes into the box with the orange border.")
                                    : qsTr("Long address? Type it on your phone instead: no app to install, it opens in the phone's browser on the same Wi-Fi.")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
            }
            ActionButton {
                text: Phone.enabled ? qsTr("Phone remote: on") : qsTr("Type with my phone")
                primary: Phone.enabled
                onClicked: { Phone.enabled = !Phone.enabled; root.focusFirst() }
            }
            PhonePairing {
                visible: Phone.running
                width: card.width
            }
        }


        // ---- Playlist ----
        Column {
            visible: root.shown === "playlist"
            width: card.width
            spacing: card.spacing
            Text {
                visible: root.sidebar
                text: qsTr("Playlist")
                color: Theme.text
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }
            // Playlists
            Column {
                visible: Settings.playlists.length > 0
                width: card.width
                spacing: 8
                Text {
                    text: qsTr("Playlists")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                    font.weight: Font.Medium
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: 1.2
                }
                Repeater {
                    model: Settings.playlists
                    delegate: Rectangle {
                        width: card.width
                        height: 60
                        radius: Theme.radius
                        color: modelData.active ? Theme.raised : "transparent"
                        border.width: 1
                        border.color: modelData.active ? Theme.accent : Theme.border
                        Column {
                            x: 14
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - plButtons.width - 28
                            spacing: 2
                            Text {
                                width: parent.width
                                text: modelData.name + (modelData.active ? "   ·   " + qsTr("watching") : "")
                                color: Theme.text
                                font.pixelSize: Theme.fontBody
                                font.weight: modelData.active ? Font.DemiBold : Font.Normal
                                elide: Text.ElideRight
                            }
                            Text {
                                text: modelData.detail
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontSmall
                            }
                        }
                        Row {
                            id: plButtons
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 6
                            ActionButton {
                                visible: !modelData.active
                                implicitWidth: 86
                                implicitHeight: 38
                                text: qsTr("Switch")
                                onClicked: Session.switchPlaylist(modelData.id)
                            }
                            ActionButton {
                                implicitWidth: 40
                                implicitHeight: 38
                                visible: index > 0
                                text: "↑"
                                onClicked: Settings.movePlaylist(modelData.id, -1)
                            }
                            ActionButton {
                                implicitWidth: 86
                                implicitHeight: 38
                                text: root.confirmRemove === modelData.id ? qsTr("Sure?") : qsTr("Remove")
                                onClicked: root.removePlaylist(modelData.id)
                            }
                        }
                    }
                }
                ActionButton {
                    visible: !root.newPlaylist && Settings.hasSource
                    text: qsTr("+  Add another playlist")
                    onClicked: root.startNewPlaylist()
                }
            }

            FieldInput {
                id: nameField
                visible: Settings.hasSource || root.newPlaylist || Settings.playlists.length > 0
                width: card.width
                label: root.editing ? qsTr("Name of this playlist") : qsTr("Name (optional)")
                placeholder: qsTr("e.g. Main provider")
                onAccepted: root.focusFirst()
            }

            // Source type switch
            Row {
                spacing: 8
                Repeater {
                    model: [qsTr("M3U playlist"), qsTr("Xtream Codes"), qsTr("Portal (MAC)")]
                    delegate: ActionButton {
                        text: modelData
                        primary: root.mode === index
                        onClicked: { root.mode = index; root.focusFirst() }
                    }
                }
            }

            // M3U form
            Column {
                visible: root.mode === 0
                width: card.width
                spacing: 14

                FieldInput {
                    id: m3uField
                    width: card.width
                    label: qsTr("Playlist URL or file")
                    placeholder: "http://provider.example/get.php?…"
                    onAccepted: root.submit()
                    KeyNavigation.tab: Settings.tv ? submitButton : browseButton
                }
                ActionButton {
                    id: browseButton
                    visible: !Settings.tv   // the TV has no file picker
                    text: qsTr("Choose file…")
                    onClicked: fileDialog.open()
                    KeyNavigation.tab: submitButton
                }
            }

            // Stalker / MAC portal form
            Column {
                visible: root.mode === 2
                width: card.width
                spacing: 14

                FieldInput {
                    id: portalField
                    label: qsTr("Portal address")
                    placeholder: "http://provider.example:8080/c/"
                    onAccepted: macField.field.forceActiveFocus()
                    KeyNavigation.tab: macField.field
                }
                FieldInput {
                    id: macField
                    label: qsTr("MAC address (the one registered with your provider)")
                    placeholder: "00:1A:79:00:00:00"
                    onAccepted: root.submit()
                    KeyNavigation.tab: submitButton
                }
                Text {
                    width: card.width
                    wrapMode: Text.Wrap
                    text: qsTr("For Stalker / Ministra portals: live TV, the guide, catch-up, movies and series.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                }
            }

            // Xtream form
            Column {
                visible: root.mode === 1
                width: card.width
                spacing: 14

                FieldInput {
                    id: serverField
                    label: qsTr("Server address")
                    placeholder: "http://provider.example:8080"
                    onAccepted: userField.field.forceActiveFocus()
                    KeyNavigation.tab: userField.field
                }
                Row {
                    width: card.width
                    spacing: 14
                    FieldInput {
                        id: userField
                        width: (card.width - 14) / 2
                        label: qsTr("Username")
                        onAccepted: passField.field.forceActiveFocus()
                        KeyNavigation.tab: passField.field
                    }
                    FieldInput {
                        id: passField
                        width: (card.width - 14) / 2
                        label: qsTr("Password")
                        echoMode: TextInput.Password
                        placeholder: Settings.sourceType === "xtream" && !root.savedIsPortal ? qsTr("Unchanged") : ""
                        onAccepted: root.submit()
                        KeyNavigation.tab: hlsToggle
                    }
                }
                CheckBox {
                    id: hlsToggle
                    text: qsTr("Use HLS (.m3u8) instead of MPEG-TS")
                    font.pixelSize: Theme.fontBody
                    KeyNavigation.tab: submitButton
                    contentItem: Text {
                        leftPadding: hlsToggle.indicator.width + 10
                        text: hlsToggle.text
                        color: Theme.textSecondary
                        font: hlsToggle.font
                        verticalAlignment: Text.AlignVCenter
                    }
                    indicator: Rectangle {
                        x: 0; y: (hlsToggle.height - height) / 2
                        width: 20; height: 20; radius: 4
                        color: hlsToggle.checked ? Theme.accent : Theme.raised
                        border.width: hlsToggle.activeFocus ? 2 : 1
                        border.color: hlsToggle.activeFocus ? Theme.text : Theme.border
                        Text {
                            anchors.centerIn: parent
                            visible: hlsToggle.checked
                            text: "✓"
                            color: Theme.accentText
                            font.pixelSize: 14
                            font.weight: Font.Bold
                        }
                    }
                }
            }

            FieldInput {
                id: epgField
                width: card.width
                label: qsTr("Guide (EPG) address — optional; separate several with commas")
                placeholder: qsTr("Leave empty to use the playlist's own guide")
                onAccepted: root.submit()
            }
            ActionButton {
                visible: root.editing && epgField.text.trim().length > 0
                text: Settings.guideKeepOwn ? qsTr("Also use the playlist's own guide: on") : qsTr("Also use the playlist's own guide: off")
                onClicked: Settings.guideKeepOwn = !Settings.guideKeepOwn
            }
            FieldInput {
                id: uaField
                width: card.width
                label: qsTr("User agent — optional")
                placeholder: qsTr("Leave empty unless your provider asks for a particular one")
                onAccepted: root.submit()
            }

            // Status / error line
            Text {
                width: card.width
                wrapMode: Text.Wrap
                visible: text.length > 0
                text: Playlists.busy ? Playlists.status : Session.lastError
                color: Playlists.busy ? Theme.textSecondary : Theme.danger
                font.pixelSize: Theme.fontBody
            }

            Row {
                spacing: 12
                ActionButton {
                    id: submitButton
                    primary: true
                    busy: Playlists.busy
                    text: root.editing ? (Session.ready ? qsTr("Save and reload") : qsTr("Load playlist"))
                                       : qsTr("Add playlist")
                    onClicked: root.submit()
                }
                ActionButton {
                    visible: root.newPlaylist && Settings.hasSource
                    text: qsTr("Cancel")
                    onClicked: root.cancelNewPlaylist()
                }
                ActionButton {
                    visible: root.canClose && !root.newPlaylist
                    text: qsTr("Back to TV")
                    onClicked: root.closeRequested()
                }

            }

            // What was loaded, and what the user has hidden
            Row {
                visible: Session.ready && Session.summary.length > 0
                width: card.width
                spacing: 12
                Text {
                    width: parent.width - hiddenButton.width - 12
                    anchors.verticalCenter: parent.verticalCenter
                    wrapMode: Text.Wrap
                    text: qsTr("Playlist: ") + Session.summary
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }
                ActionButton {
                    id: hiddenButton
                    text: qsTr("Hidden (%1)").arg(Channels.hiddenGroups.length + Channels.hiddenChannels.length)
                    onClicked: root.showHidden = true
                }
            }


            // Playlist upkeep
            ChoiceRow {
                visible: Session.ready
                caption: qsTr("Refresh the playlist (new and changed channels) while Relay is open")
                choices: [ { label: qsTr("Only when Relay starts"), value: 0 }, { label: qsTr("Every 6 hours"), value: 6 },
                           { label: qsTr("Every 12 hours"), value: 12 }, { label: qsTr("Every day"), value: 24 } ]
                value: Settings.playlistRefreshHours
                onChosen: (v) => Settings.playlistRefreshHours = v
            }

        }

        // ---- TV guide ----
        Column {
            visible: root.shown === "guide"
            width: card.width
            spacing: card.spacing
            Text {
                visible: root.sidebar
                text: qsTr("TV guide")
                color: Theme.text
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }
            // Guide status
            Row {
                visible: Session.ready
                width: card.width
                spacing: 12
                Text {
                    width: parent.width - guideButton.width - 12
                    anchors.verticalCenter: parent.verticalCenter
                    wrapMode: Text.Wrap
                    text: qsTr("Guide: ") + root.guideStatusText()
                    color: Epg.lastError && !Epg.busy ? Theme.danger : Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }
                ActionButton {
                    id: guideButton
                    // Stays enabled while updating: disabling a focused button
                    // can leave the screen with no keyboard focus.
                    text: Epg.busy ? qsTr("Updating…") : qsTr("Update guide")
                    onClicked: {
                        if (Epg.busy) return
                        Settings.epgOverride = epgField.text
                        Session.refreshGuide()
                    }
                }
            }

            // Guide upkeep
            ChoiceRow {
                visible: Session.ready
                caption: qsTr("Update the guide every")
                choices: [ { label: qsTr("6 hours"), value: 6 }, { label: qsTr("12 hours"), value: 12 },
                           { label: qsTr("Day"), value: 24 }, { label: qsTr("2 days"), value: 48 } ]
                value: Settings.guideRefreshHours
                onChosen: (v) => Settings.guideRefreshHours = v
            }
            ChoiceRow {
                visible: Session.ready
                caption: qsTr("Keep guide listings for (from the next update; fewer days update faster)")
                choices: [ { label: qsTr("3 days"), value: 3 }, { label: qsTr("8 days"), value: 8 },
                           { label: qsTr("14 days"), value: 14 } ]
                value: Settings.guideDaysAhead
                onChosen: (v) => Settings.guideDaysAhead = v
            }
            Column {
                visible: Session.ready
                width: card.width
                spacing: 8
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("Guide times wrong for this whole playlist? Move every listing (one channel: U in the channel list).")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }
                Row {
                    spacing: 8
                    ActionButton { implicitWidth: 120; text: qsTr("− 30 min"); onClicked: Settings.guideOffsetMinutes = Settings.guideOffsetMinutes - 30 }
                    Text {
                        width: 150
                        anchors.verticalCenter: parent.verticalCenter
                        horizontalAlignment: Text.AlignHCenter
                        text: root.offsetText(Settings.guideOffsetMinutes)
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                    }
                    ActionButton { implicitWidth: 120; text: qsTr("+ 30 min"); onClicked: Settings.guideOffsetMinutes = Settings.guideOffsetMinutes + 30 }
                }
            }

            Text {
                visible: Session.ready
                width: card.width
                wrapMode: Text.Wrap
                text: Catchup.channelCount > 0
                      ? qsTr("Catch-up: %1 channels keep past shows · press Enter on a ↺ show in the guide")
                            .arg(Number(Catchup.channelCount).toLocaleString(Qt.locale(), "f", 0))
                      : qsTr("Catch-up: no channels with past shows")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
            }

        }

        // ---- Playback & recording ----
        Column {
            visible: root.shown === "playback"
            width: card.width
            spacing: card.spacing
            Text {
                visible: root.sidebar
                text: qsTr("Playback & recording")
                color: Theme.text
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }
            // Pause live TV buffer size
            Column {
                visible: Session.ready
                width: card.width
                spacing: 8
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("Pause live TV: disk space for the live buffer. More space lets you pause and rewind further (about 25 minutes of HD per GB).")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }
                Row {
                    spacing: 8
                    Repeater {
                        model: [ { label: qsTr("Off"), mb: 0 }, { label: "1 GB", mb: 1024 },
                                 { label: "2 GB", mb: 2048 }, { label: "4 GB", mb: 4096 } ]
                        delegate: ActionButton {
                            implicitWidth: 92
                            text: modelData.label
                            primary: Settings.timeshiftMb === modelData.mb
                            onClicked: Settings.timeshiftMb = modelData.mb
                        }
                    }
                }
            }

            // Languages
            ChoiceRow {
                visible: Session.ready
                caption: qsTr("Sound language: movies, series and channels start in it when they have it. You can still switch while watching.")
                choices: [ { label: qsTr("As the video says"), value: "" } ].concat(root.languageChoices)
                value: Settings.audioLanguage
                onChosen: (v) => Settings.audioLanguage = v
            }
            ChoiceRow {
                visible: Session.ready
                caption: qsTr("Subtitles in movies and series")
                choices: [ { label: qsTr("As the video says"), value: "" }, { label: qsTr("Off"), value: "off" } ].concat(root.languageChoices)
                value: Settings.subtitleLanguage
                onChosen: (v) => Settings.subtitleLanguage = v
            }

            // Picture
            ChoiceRow {
                visible: Session.ready
                caption: qsTr("Deinterlacing (smooths the combed look of some 1080i sports and news channels)")
                choices: [ { label: qsTr("Automatic"), value: "auto" }, { label: qsTr("Always"), value: "on" },
                           { label: qsTr("Off"), value: "off" } ]
                value: Settings.deinterlace
                onChosen: (v) => Settings.deinterlace = v
            }
            ChoiceRow {
                visible: Session.ready
                caption: qsTr("Video decoding (switch to Software if the picture shows glitches or stays black)")
                choices: [ { label: qsTr("Graphics card"), value: true }, { label: qsTr("Software"), value: false } ]
                value: Settings.hardwareDecoding
                onChosen: (v) => Settings.hardwareDecoding = v
            }

            // Recordings folder
            Row {
                visible: Session.ready && !Settings.tv   // the TV has no folder picker
                width: card.width
                spacing: 12
                Text {
                    width: parent.width - folderButton.width - 12
                    anchors.verticalCenter: parent.verticalCenter
                    wrapMode: Text.WrapAnywhere
                    text: qsTr("Recordings are saved to: %1").arg(Schedule.folder)
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }
                ActionButton {
                    id: folderButton
                    text: qsTr("Change…")
                    onClicked: folderDialog.open()
                }
            }
            FolderDialog {
                id: folderDialog
                title: qsTr("Choose where to save recordings")
                onAccepted: Schedule.folder = selectedFolder.toString()
            }

        }

        // ---- Movies & series ----
        Column {
            visible: root.shown === "vod"
            width: card.width
            spacing: card.spacing
            Text {
                visible: root.sidebar
                text: qsTr("Movies & series")
                color: Theme.text
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }
            // Movies and series status
            Row {
                visible: Session.ready && Vod.available
                width: card.width
                spacing: 12
                Text {
                    width: parent.width - vodButton.width - 12
                    anchors.verticalCenter: parent.verticalCenter
                    wrapMode: Text.Wrap
                    text: qsTr("Movies and series: ") + (Vod.loading ? (Vod.status || qsTr("loading…"))
                          : Vod.error ? Vod.error
                          : Vod.loaded ? qsTr("%1 movies · %2 series · press V to browse")
                                             .arg(Number(Vod.movieCount).toLocaleString(Qt.locale(), "f", 0))
                                             .arg(Number(Vod.seriesCount).toLocaleString(Qt.locale(), "f", 0))
                          : qsTr("not loaded yet"))
                    color: Vod.error && !Vod.loading ? Theme.danger : Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }
                ActionButton {
                    id: vodButton
                    text: Vod.loading ? qsTr("Updating…") : qsTr("Update movies")
                    onClicked: if (!Vod.loading) Vod.reload()
                }
            }

            // Posters and details from TMDB
            Column {
                id: tmdbBox
                width: card.width
                spacing: 8
                property string note
                property bool noteBad: false
                property bool checking: false
                function save() {
                    const k = tmdbField.text.trim()
                    if (!k.length) return
                    checking = true
                    note = qsTr("Checking the key…")
                    noteBad = false
                    Vod.checkTmdbKey(k)
                }
                Connections {
                    target: Vod
                    function onTmdbChecked(ok, message) {
                        if (!tmdbBox.checking) return
                        tmdbBox.checking = false
                        tmdbBox.noteBad = !ok
                        tmdbBox.note = ok ? qsTr("Saved. Posters fill in as you browse.") : message
                        if (ok) { Settings.setTmdbKey(tmdbField.text.trim()); tmdbField.text = "" }
                    }
                    function onTmdbKeyRejected() { tmdbBox.noteBad = true; tmdbBox.note = qsTr("TMDB stopped accepting the saved key. Enter it again or remove it.") }
                }
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: Settings.tmdbKey.length
                          ? qsTr("Posters and details from TMDB: on. Movies and series that come without a poster, plot, cast or episode names get them from TMDB.")
                          : qsTr("Posters and details from TMDB: off. With your own free TMDB key (themoviedb.org → Settings → API), movies and series that come without a poster, plot, cast or episode names get them from TMDB. Their titles are sent to TMDB to look them up.")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }
                Row {
                    spacing: 8
                    FieldInput {
                        id: tmdbField
                        width: Math.min(card.width - 220, 420)
                        label: Settings.tmdbKey.length ? qsTr("Replace the key") : qsTr("TMDB API key or read access token")
                        placeholder: Settings.tmdbKey.length ? qsTr("Saved") : ""
                        echoMode: TextInput.Password
                        onAccepted: tmdbBox.save()
                    }
                    ActionButton {
                        anchors.bottom: parent.bottom
                        text: tmdbBox.checking ? qsTr("Checking…") : qsTr("Save key")
                        enabled: !tmdbBox.checking && tmdbField.text.trim().length > 0
                        onClicked: tmdbBox.save()
                    }
                    ActionButton {
                        anchors.bottom: parent.bottom
                        visible: Settings.tmdbKey.length > 0
                        text: qsTr("Remove")
                        onClicked: { Settings.setTmdbKey(""); tmdbBox.note = qsTr("Removed. Posters already shown stay until Relay restarts."); tmdbBox.noteBad = false }
                    }
                }
                Text {
                    visible: tmdbBox.note.length > 0
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: tmdbBox.note
                    color: tmdbBox.noteBad ? Theme.danger : Theme.accent
                    font.pixelSize: Theme.fontSmall
                }
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("This product uses the TMDB API but is not endorsed or certified by TMDB.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall - 1
                }
            }

            // Trailers in Movies and Series
            Column {
                width: card.width
                spacing: 8
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("Trailers: Movies and Series play a title's trailer (from your provider or TMDB) quietly behind it after a moment. More info → Trailer plays it with sound.")
                          + (Trailers.available ? "" : " " + qsTr("yt-dlp isn't installed, so YouTube trailers open in the browser instead."))
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }
                ActionButton {
                    text: Trailers.autoplay ? qsTr("Trailers play by themselves: on") : qsTr("Trailers play by themselves: off")
                    onClicked: Trailers.autoplay = !Trailers.autoplay
                }
            }
            Column {
                width: card.width
                spacing: 8
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("All playlists together: the movies and series of every playlist show as one list, each title once. It plays from the best source (4K before FHD before HD); a title's page and the player can switch to another, and if one won't play Relay offers the others.")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }
                ActionButton {
                    text: Vod.combine ? qsTr("Movies & series from all playlists: on") : qsTr("Movies & series from all playlists: off")
                    onClicked: Vod.combine = !Vod.combine
                }
            }

        }

        // ---- Trakt ----
        Column {
            visible: root.shown === "trakt"
            width: card.width
            spacing: card.spacing
            Text {
                visible: root.sidebar
                text: qsTr("Trakt")
                color: Theme.text
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }
            // Trakt
            Column {
                id: traktBox
                width: card.width
                spacing: 8
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: Trakt.connected
                          ? (Trakt.user.length ? qsTr("Trakt: signed in as %1.").arg(Trakt.user) : qsTr("Trakt: signed in."))
                            + " " + qsTr("Movies and episodes you watch are added to your Trakt history, and what you've watched on Trakt shows as watched here.")
                            + (Trakt.lastSync.length ? " " + qsTr("Last synced %1.").arg(Trakt.lastSync) : "")
                          : qsTr("Trakt: off. Keep your Trakt history up to date and see what you've watched on Trakt. You'll need your own Trakt app, which is free: on developer.trakt.tv choose Create an App and name it Relay (Trakt asks you to connect a GitHub account first). Then enter its Client ID below. New Trakt apps have no Client Secret: leave that empty unless your app has one.")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }
                Row {
                    visible: !Trakt.connected && Trakt.userCode.length === 0
                    spacing: 8
                    FieldInput {
                        id: traktId
                        width: Math.min((card.width - 140) / 2, 300)
                        label: qsTr("Client ID")
                        text: Trakt.clientId
                        KeyNavigation.tab: traktSecret.field
                        onAccepted: { Trakt.signIn(traktId.text, traktSecret.text); traktSecret.text = "" }
                    }
                    FieldInput {
                        id: traktSecret
                        width: Math.min((card.width - 140) / 2, 300)
                        label: qsTr("Client Secret (older apps)")
                        placeholder: Trakt.hasSecret ? qsTr("Saved") : qsTr("None")
                        echoMode: TextInput.Password
                        onAccepted: { Trakt.signIn(traktId.text, traktSecret.text); traktSecret.text = "" }
                    }
                    ActionButton {
                        anchors.bottom: parent.bottom
                        text: qsTr("Sign in")
                        enabled: traktId.text.trim().length > 0
                        onClicked: { Trakt.signIn(traktId.text, traktSecret.text); traktSecret.text = "" }
                    }
                }
                Rectangle {   // the code to enter on Trakt
                    visible: Trakt.userCode.length > 0
                    width: parent.width
                    height: codeCol.implicitHeight + 28
                    radius: 10
                    color: Theme.raised
                    border.width: 1
                    border.color: Theme.accent
                    Column {
                        id: codeCol
                        x: 16; y: 14
                        width: parent.width - 32
                        spacing: 8
                        Text {
                            width: parent.width
                            wrapMode: Text.Wrap
                            text: qsTr("On your phone or computer, go to %1 and enter this code:").arg(Trakt.verifyUrl)
                            color: Theme.text
                            font.pixelSize: Theme.fontSmall + 1
                        }
                        Text {
                            text: Trakt.userCode
                            color: Theme.accent
                            font.pixelSize: 34
                            font.weight: Font.Bold
                            font.letterSpacing: 4
                        }
                        Row {
                            spacing: 8
                            ActionButton { text: qsTr("Open trakt.tv/activate"); onClicked: Qt.openUrlExternally(Trakt.verifyUrl) }
                            ActionButton { text: qsTr("Cancel"); onClicked: Trakt.cancelSignIn() }
                        }
                    }
                }
                Row {
                    visible: Trakt.connected
                    spacing: 8
                    ActionButton {
                        text: Trakt.sendWatching ? qsTr("Sending what I watch: on") : qsTr("Sending what I watch: off")
                        onClicked: Trakt.sendWatching = !Trakt.sendWatching
                    }
                    ActionButton { text: qsTr("Sync now"); onClicked: Trakt.syncNow() }
                    ActionButton { text: qsTr("Sign out"); onClicked: Trakt.signOut() }
                }
                Text {
                    visible: Trakt.status.length > 0
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: Trakt.status
                    color: Theme.accent
                    font.pixelSize: Theme.fontSmall
                }
            }

        }

        // ---- Controls ----
        Column {
            visible: root.shown === "controls"
            width: card.width
            spacing: card.spacing
            Text {
                visible: root.sidebar
                text: qsTr("Controls")
                color: Theme.text
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }
            ChoiceRow {
                caption: qsTr("Interface size (text, lists and buttons)")
                choices: [ { label: "85%", value: 85 }, { label: "100%", value: 100 }, { label: "115%", value: 115 },
                           { label: "130%", value: 130 }, { label: "150%", value: 150 } ]
                value: Settings.uiScale
                onChosen: (v) => Settings.uiScale = v
            }
            Row {
                visible: Settings.uiScale !== card.startScale
                spacing: 12
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("The new size is used the next time Relay starts.")
                    color: Theme.accent
                    font.pixelSize: Theme.fontSmall
                }
                ActionButton { text: qsTr("Restart Relay now"); onClicked: Backup.restart() }
            }
            Column {
                width: card.width
                spacing: 8
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    readonly property int changed: Object.keys(Keymap.custom).length
                    text: changed === 0 ? qsTr("Keys while watching: the usual ones (H shows them).")
                        : changed === 1 ? qsTr("Keys while watching: 1 changed.") : qsTr("Keys while watching: %1 changed.").arg(changed)
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }
                ActionButton { text: qsTr("Change keys…"); onClicked: keysDialog.open() }
            }
            Column {
                visible: !Settings.tv   // no other players or file picker on the TV
                width: card.width
                spacing: 8
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: Settings.externalPlayer.length
                          ? qsTr("External player: %1 · press %2 while watching to open the channel or movie in it.").arg(Settings.externalPlayer).arg(Keymap.label("external"))
                          : qsTr("External player: none. Choose another player's program (VLC, mpv, MPC…) to open a channel or movie in it with %1.").arg(Keymap.label("external"))
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }
                Row {
                    spacing: 8
                    ActionButton { text: qsTr("Choose player…"); onClicked: playerDialog.open() }
                    ActionButton { visible: Settings.externalPlayer.length > 0; text: qsTr("Don't use one"); onClicked: Settings.externalPlayer = "" }
                }
            }
            FileDialog {
                id: playerDialog
                title: qsTr("Choose the other player's program")
                nameFilters: Qt.platform.os === "windows" ? [qsTr("Programs (*.exe)"), qsTr("All files (*)")] : [qsTr("All files (*)")]
                onAccepted: Settings.externalPlayer = selectedFile.toString()
            }

            // Remotes, game controllers and the phone remote
            Column {
                width: card.width
                spacing: 8
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("Remotes and controllers: TV-style PC remotes and media keys work as they are (OK, Back, Home, Menu, Guide…).") + " "
                          + (!Gamepads.available ? qsTr("This copy of Relay was built without game controller support.")
                             : Gamepads.connected.length ? qsTr("Controller connected: %1.").arg(Gamepads.connected.join(", "))
                             : qsTr("Game controllers (Xbox, PlayStation, Switch) work when connected, by cable or Bluetooth."))
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }
                Text {
                    visible: Gamepads.available
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("Controller: A OK · B back · X menu · Y guide · LB/RB back/forward 30 s · LT/RT page · right stick volume · Start play/pause · View what's on · Xbox/PS button home")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                }
                Row {
                    spacing: 8
                    topPadding: 4
                    ActionButton {
                        text: Phone.enabled ? qsTr("Phone remote: on") : qsTr("Phone remote: off")
                        primary: Phone.enabled
                        onClicked: Phone.enabled = !Phone.enabled
                    }
                    ActionButton {
                        visible: Phone.paired > 0
                        text: qsTr("Forget paired phones (%1)").arg(Phone.paired)
                        onClicked: Phone.forgetPhones()
                    }
                }
                Text {
                    visible: !Phone.enabled
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("Use a phone on the same Wi-Fi as a remote: no app to install, it opens in the phone's browser.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                }
                PhonePairing {
                    visible: Phone.running
                    width: card.width
                    note: Settings.tv ? qsTr("Only phones on your home network that know this code can use it.")
                                      : qsTr("Only phones on your home network that know this code can use it. If Windows asks whether Relay may use the network, allow private networks.")
                }
            }

        }

        // ---- Parental controls ----
        Column {
            visible: root.shown === "parental"
            width: card.width
            spacing: card.spacing
            Text {
                visible: root.sidebar
                text: qsTr("Parental controls")
                color: Theme.text
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }
            // Parental PIN
            Column {
                visible: Session.ready
                width: card.width
                spacing: 8
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: !Parental.hasPin
                          ? qsTr("Parental PIN: off. With a PIN you can lock channels and groups (K in the channel list) and movie categories (K on a category).")
                          : qsTr("Parental PIN: on · %1 locked · %2").arg(Parental.lockCount)
                                .arg(Parental.unlocked ? qsTr("unlocked until Relay closes") : qsTr("locked"))
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }
                Flow {
                    width: parent.width
                    spacing: 8
                    ActionButton {
                        visible: !Parental.hasPin
                        text: qsTr("Set PIN…")
                        onClicked: root.pinRequested("set", qsTr("You'll need it to watch locked channels"), null)
                    }
                    ActionButton {
                        visible: Parental.hasPin
                        text: qsTr("Change PIN…")
                        onClicked: root.pinRequested("change", "", null)
                    }
                    ActionButton {
                        visible: Parental.hasPin
                        text: qsTr("Remove PIN…")
                        onClicked: root.pinRequested("remove", qsTr("Locks are kept, but nothing asks for a PIN"), null)
                    }
                    ActionButton {
                        visible: Parental.hasPin && Parental.unlocked
                        text: qsTr("Lock now")
                        onClicked: Parental.lock()
                    }
                    ActionButton {
                        visible: Parental.hasPin
                        text: Parental.lockSettings ? qsTr("Settings need the PIN: on") : qsTr("Settings need the PIN: off")
                        onClicked: Parental.lockSettings = !Parental.lockSettings
                    }
                }
            }

        }

        // ---- Updates & backup ----
        Column {
            visible: root.shown === "about"
            width: card.width
            spacing: card.spacing
            Text {
                visible: root.sidebar
                text: qsTr("Updates & backup")
                color: Theme.text
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }
            // Updates
            Column {
                id: updatesBox
                property bool showNotes: false
                width: card.width
                spacing: 8
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("Updates: this is Relay %1.").arg(Updates.currentVersion) + " "
                          + (Updates.available ? qsTr("Relay %1 is available.").arg(Updates.latestVersion)
                             : Updates.autoCheck ? qsTr("Relay checks for a new version when it starts and every six hours, and asks before installing it.")
                             : qsTr("Checking for new versions is off."))
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }
                Flow {
                    width: parent.width
                    spacing: 8
                    ActionButton {
                        visible: Updates.available
                        primary: true
                        text: Updates.canInstall ? qsTr("Update to %1").arg(Updates.latestVersion) : qsTr("Get %1").arg(Updates.latestVersion)
                        enabled: Updates.state !== "downloading" && Updates.state !== "installing"
                        onClicked: Updates.updateNow()
                    }
                    ActionButton {
                        text: Updates.state === "checking" ? qsTr("Checking…") : qsTr("Check now")
                        enabled: Updates.state === "" || Updates.state === "error"
                        onClicked: Updates.check(true)
                    }
                    ActionButton {
                        text: Updates.autoCheck ? qsTr("Check automatically: on") : qsTr("Check automatically: off")
                        onClicked: Updates.autoCheck = !Updates.autoCheck
                    }
                    ActionButton {
                        visible: Updates.releaseUrl.length > 0
                        text: updatesBox.showNotes ? qsTr("Hide notes") : qsTr("What's new")
                        onClicked: Updates.notes.length ? (updatesBox.showNotes = !updatesBox.showNotes) : Qt.openUrlExternally(Updates.releaseUrl)
                    }
                }
                Flickable {   // what's new in this version (its CHANGELOG.md section)
                visible: updatesBox.showNotes && Updates.notes.length > 0
                width: parent.width
                height: Math.min(updateNotesText.implicitHeight, 320)
                contentHeight: updateNotesText.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                Text {
                    id: updateNotesText
                    width: parent.width
                    text: Updates.notes
                    textFormat: Text.MarkdownText
                    wrapMode: Text.Wrap
                    color: Theme.textSecondary
                    linkColor: Theme.accent
                    font.pixelSize: Theme.fontSmall
                    onLinkActivated: (link) => Qt.openUrlExternally(link)
                }
            }
                Rectangle {   // download progress
                    visible: Updates.state === "downloading"
                    width: Math.min(parent.width, 420)
                    height: 6
                    radius: 3
                    color: Theme.raised
                    Rectangle { width: parent.width * Updates.progress; height: parent.height; radius: 3; color: Theme.accent }
                }
                Text {
                    visible: Updates.status.length > 0 && (Updates.state !== "" || !Updates.available)
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: Updates.status
                    color: Updates.state === "error" ? Theme.danger : Theme.accent
                    font.pixelSize: Theme.fontSmall
                }
                // Only when GitHub hides the releases (a private repository) or one is saved.
                Row {
                    visible: Updates.hasToken || Updates.needsToken
                    spacing: 8
                    FieldInput {
                        id: updateToken
                        width: Math.min(card.width - 220, 420)
                        label: qsTr("GitHub token (for a private copy of Relay's repository)")
                        placeholder: Updates.hasToken ? qsTr("Saved") : "github_pat_…"
                        echoMode: TextInput.Password
                        onAccepted: { Updates.setToken(text); text = "" }
                    }
                    ActionButton {
                        anchors.bottom: parent.bottom
                        text: qsTr("Save")
                        enabled: updateToken.text.trim().length > 0
                        onClicked: { Updates.setToken(updateToken.text); updateToken.text = "" }
                    }
                    ActionButton {
                        anchors.bottom: parent.bottom
                        visible: Updates.hasToken
                        text: qsTr("Remove")
                        onClicked: Updates.setToken("")
                    }
                }
            }

            // Backup and restore
            Column {
                visible: !Settings.tv   // needs a file picker
                width: card.width
                spacing: 8
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: root.confirmRestore.length
                          ? root.restoreSummary
                          : qsTr("Backup: saves your playlists (with their logins), favorites, hidden and locked channels, names, order, Continue watching, reminders and settings in one file. Keep it private.")
                    color: root.confirmRestore.length ? Theme.text : Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }
                Flow {
                    width: parent.width
                    spacing: 8
                    ActionButton {
                        visible: !root.confirmRestore.length && Settings.playlists.length > 0
                        text: qsTr("Back up…")
                        onClicked: { backupDialog.currentFile = backupDialog.currentFolder + "/" + Backup.suggestedName(); backupDialog.open() }
                    }
                    ActionButton {
                        visible: !root.confirmRestore.length
                        text: qsTr("Restore…")
                        onClicked: restoreDialog.open()
                    }
                    ActionButton {
                        visible: root.confirmRestore.length > 0
                        primary: true
                        text: qsTr("Restore and restart Relay")
                        onClicked: {
                            const err = Backup.restoreFrom(root.confirmRestore)
                            if (err) { root.backupFailed = true; root.backupStatus = err; root.confirmRestore = ""; return }
                            Backup.restart()
                        }
                    }
                    ActionButton {
                        visible: root.confirmRestore.length > 0
                        text: qsTr("Cancel")
                        onClicked: root.confirmRestore = ""
                    }
                }
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    visible: root.backupStatus.length > 0
                    text: root.backupStatus
                    color: root.backupFailed ? Theme.danger : Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }
            }
            FileDialog {
                id: backupDialog
                title: qsTr("Save a backup of Relay")
                fileMode: FileDialog.SaveFile
                defaultSuffix: "relaybackup"
                nameFilters: [qsTr("Relay backups (*.relaybackup)")]
                onAccepted: {
                    const err = Backup.saveTo(selectedFile.toString())
                    root.backupFailed = err.length > 0
                    root.backupStatus = err.length ? err : qsTr("Backup saved.")
                }
            }
            FileDialog {
                id: restoreDialog
                title: qsTr("Choose a Relay backup")
                nameFilters: [qsTr("Relay backups (*.relaybackup)"), qsTr("All files (*)")]
                onAccepted: {
                    const d = Backup.describe(selectedFile.toString())
                    if (!d.ok) { root.backupFailed = true; root.backupStatus = d.error; return }
                    root.backupStatus = ""
                    root.restoreSummary = qsTr("Restore the backup from %1 (%2)? It replaces the playlists, favorites and settings Relay has now.")
                        .arg(d.created).arg(d.playlists.length ? d.playlists.join(", ") : qsTr("no playlists"))
                    root.confirmRestore = selectedFile.toString()
                }
            }

            Text {
                width: card.width
                wrapMode: Text.Wrap
                text: qsTr("Relay is a player only. Use playlists from a provider you subscribe to or your own tuner.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
            }

        }
    }


    // Set up from the phone, beside the form when there's room.
    PhoneSetup {
        visible: root.phoneBeside
        x: card.x + card.width + 48
        y: Math.max(card.y, 32)
        width: root.phonePanelWidth
    }
    }   // Flickable

    // ---- The section list ------------------------------------------------------------
    Rectangle {
        id: sidePanel
        visible: root.sidebar
        width: Math.min(300, Math.max(250, root.width * 0.25))
        height: parent.height
        color: Theme.panelSolid
        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border }
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = true }

        Column {
            id: brand
            x: 24; y: 28
            spacing: 4
            Row {
                spacing: 10
                Rectangle { width: 12; height: 12; radius: 3; color: Theme.accent; anchors.verticalCenter: parent.verticalCenter }
                Text { text: qsTr("Settings"); color: Theme.text; font.pixelSize: Theme.fontTitle; font.weight: Font.Bold }
            }
            Text {
                width: sidePanel.width - 48
                text: Settings.activeName
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
            }
        }

        ListView {
            id: sectionList
            anchors.top: brand.bottom
            anchors.topMargin: 20
            anchors.bottom: hint.top
            x: 12
            width: parent.width - 24
            clip: true
            model: root.sections
            currentIndex: Math.max(0, root.sections.findIndex(s => s.id === root.section))
            keyNavigationEnabled: false
            boundsBehavior: Flickable.StopAtBounds
            Keys.onUpPressed: if (currentIndex > 0) root.showSection(root.sections[currentIndex - 1].id)
            Keys.onDownPressed: if (currentIndex < count - 1) root.showSection(root.sections[currentIndex + 1].id)
            Keys.onRightPressed: root.enterSection()
            Keys.onReturnPressed: root.enterSection()
            Keys.onEnterPressed: root.enterSection()
            Keys.onLeftPressed: {}
            delegate: Rectangle {
                required property var modelData
                required property int index
                readonly property bool current: modelData.id === root.section
                width: sectionList.width
                height: 48
                radius: 8
                color: current ? (sectionList.activeFocus ? Theme.focus : Theme.raised) : (rowMouse.containsMouse ? "#14FFFFFF" : "transparent")
                border.width: current && sectionList.activeFocus ? 2 : 0
                border.color: Theme.accent
                Icon {
                    x: 14
                    anchors.verticalCenter: parent.verticalCenter
                    width: 20; height: 20
                    path: modelData.icon
                    color: parent.current ? Theme.accent : Theme.textSecondary
                }
                Text {
                    x: 46
                    width: parent.width - 56
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.title
                    color: parent.current ? Theme.text : Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                    font.weight: parent.current ? Font.DemiBold : Font.Normal
                    elide: Text.ElideRight
                }
                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { root.showSection(modelData.id); sectionList.forceActiveFocus() }
                }
            }
        }
        Text {
            id: hint
            x: 24
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 20
            width: parent.width - 48
            wrapMode: Text.Wrap
            text: qsTr("↑ ↓ choose · → or OK to go in · Back returns here")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
        }
    }

    // The way out with the mouse (same as Esc). Stays in the corner while
    // the settings scroll.
    CloseButton {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 20
        z: 5
        visible: !keysDialog.opened && (root.canClose || root.showHidden || (root.newPlaylist && Settings.hasSource))
        label: root.showHidden || root.newPlaylist ? qsTr("Back") : qsTr("Close settings")
        onClicked: root.goBack()
    }

    KeysDialog {
        id: keysDialog
        onFinished: root.focusStart()
    }

    HiddenManager {
        id: hiddenManager
        anchors.fill: parent
        visible: root.showHidden
        focus: root.showHidden
        onVisibleChanged: if (visible) forceActiveFocus()
        onCloseRequested: { root.showHidden = false; root.focusStart() }
    }
}

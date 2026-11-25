import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Window
import Relay.Core
import Relay.Video

Window {
    id: win
    property bool startFullscreen: false
    property bool showSetup: !Settings.hasSource && !Session.ready
    property string playbackError: ""

    width: 1280
    height: 720
    minimumWidth: 800
    minimumHeight: 480
    visible: true
    color: Theme.background
    title: Player.vodActive ? Player.vod.title + " — Relay"
         : Player.hasChannel ? Player.currentName + " — Relay" : "Relay"

    Component.onCompleted: if (startFullscreen) win.visibility = Window.FullScreen

    function toggleFullscreen() {
        win.visibility = win.visibility === Window.FullScreen ? Window.Windowed : Window.FullScreen
    }
    function returnFocus() { if (!showSetup && !pinDialog.opened) keyHandler.forceActiveFocus() }

    // ---- Parental PIN ----
    function requestPin(reason, onOk, onCancel) {
        pinDialog.open("verify", reason, onOk, onCancel)
    }
    // After the PIN dialog: give the keyboard back to whatever is open.
    function restoreFocusAfterPin() {
        if (win.showSetup) setupScreen.restoreFocus()
        else if (multiView.opened) multiView.forceActiveFocus()
        else if (recordings.opened) recordings.forceActiveFocus()
        else if (vodBrowser.opened) vodBrowser.forceActiveFocus()
        else if (guide.opened) guide.forceActiveFocus()
        else if (channelPanel.opened) channelPanel.forceActiveFocus()
        else if (search.opened) search.forceActiveFocus()
        else if (playerOptions.opened) playerOptions.forceActiveFocus()
        else keyHandler.forceActiveFocus()
    }
    function openSetupGuarded() {
        if (Parental.hasPin && Parental.lockSettings && !Parental.unlocked)
            requestPin(qsTr("Settings are locked"), () => win.openSetup())
        else openSetup()
    }
    function openSetup() { channelPanel.close(); guide.close(); search.close(); vodBrowser.close(); recordings.close(); showSetup = true }
    function openRecordings(section) {
        channelPanel.close()
        guide.close()
        search.close()
        vodBrowser.close()
        infoBar.hide()
        shortcuts.opened = false
        recordings.open(section)
    }
    // Q: several channels at once. The main picture stops so its connection
    // is free for the tiles (many providers limit connections).
    function openMultiView(withoutChannel) {
        if (!Player.hasChannel && !withoutChannel) return
        channelPanel.close(); guide.close(); search.close(); infoBar.hide()
        shortcuts.opened = false
        win.playbackError = ""
        video.stop()
        pip.close()                      // its connection is needed for tiles too
        multiView.open(Player.hasChannel ? Player.currentIndex : -1)
    }
    // Multi-view with this channel next to the one on now (channel list,
    // right-click). Up to four: a full set swaps out the last tile.
    function openMultiViewWith(globalIndex) {
        if (globalIndex < 0) { openMultiView(); return }
        if (Parental.blocked(globalIndex)) { toast.show(qsTr("That channel is locked"), true); return }
        openMultiView()
        if (globalIndex === Player.currentIndex) return
        const key = Channels.keyAt(globalIndex)
        if (multiView.tileCount < multiView.maxTiles) multiView.setTile(-1, Settings.activeId, key)
        else multiView.setTile(multiView.tileCount - 1, Settings.activeId, key)
    }
    // W: pin the channel being watched in a corner and pick another for the
    // big picture (again: close it). In a movie it pins the live channel.
    function togglePip() {
        if (pip.active) { pip.close(); toast.show(qsTr("Picture-in-picture closed")); return }
        if (!Player.hasChannel) return
        pip.open(Player.currentIndex)
        if (Player.vodActive) { toast.show(qsTr("%1 in the corner · W closes it").arg(Player.currentName)); return }
        toast.show(qsTr("%1 in the corner · pick a channel to watch · X swaps").arg(Player.currentName))
        infoBar.hide()
        channelPanel.open()
    }
    // X (or a click on it): swap the corner channel and the big picture.
    function swapPip() {
        if (!pip.active) { toast.show(qsTr("W puts a channel in the corner")); return }
        if (Player.vodActive) { toast.show(qsTr("Swapping works while watching live TV")); return }
        if (pip.index === Player.currentIndex) { pip.close(); return }
        pip.pendingFrom = Player.currentIndex
        Player.tune(pip.index, -1)
    }
    // Ctrl+R: record the show on now (again: stop).
    function toggleRecordNow() {
        if (!Player.hasChannel) return
        const p = Player.nowProgram
        const id = p.valid ? Schedule.idFor(Player.currentIndex, p.start) : ""
        if (id && Schedule.item(id).status === "recording") {
            Schedule.stopRecording(id)
            toast.show(qsTr("Recording stopped · it's in Recordings"))
            return
        }
        if (Schedule.recordNow(Player.currentIndex))
            toast.show(p.valid ? qsTr("Recording %1 until it ends").arg(p.title) : qsTr("Recording for an hour"))
    }
    function openVod(section) {
        channelPanel.close()
        guide.close()
        search.close()
        infoBar.hide()
        shortcuts.opened = false
        vodBrowser.open(section)
    }
    // Leave a movie: save where it got to, go back to its details, and let
    // live TV carry on underneath.
    function leaveVod() {
        if (!Player.vodActive) return
        // A copy: Player.vod read from QML is a live view of the property
        // (Qt 6.5+), which stopVod() below empties.
        const d = Object.assign({}, Player.vod)
        if (d.kind === "recording") {
            win.playbackError = ""
            Player.stopVod()
            recordings.open(0)
            return
        }
        if (d.kind === "catchup") {      // catch-up goes back to the channel, live
            win.playbackError = ""
            Player.stopVod()
            return
        }
        saveVodProgress()
        win.playbackError = ""
        Player.stopVod()
        // The title's page, by its id. Without one (seen with a title from
        // another playlist), Movies & series instead: a page opened without
        // an id would leave the keys with live TV underneath.
        const id = d.kind === "episode" ? (d.seriesId || d.detailsId) : (d.id || d.detailsId)
        if (!id) {
            console.warn("Leaving a " + d.kind + " that has no title id; it has: " + Object.keys(d).join(", "))
            win.openVod(Vod.section)
            return
        }
        vodBrowser.openDetails(d.kind === "episode" ? "series" : "movie", id,
                               { title: d.kind === "episode" ? d.seriesTitle : d.title,
                                 poster: d.kind === "episode" ? d.seriesPoster : d.poster })
    }
    // Multi-view from a movie or episode: it goes on in a tile, from where
    // it got to, next to the live channels.
    function multiViewFromVod() {
        if (!Player.vodActive) { openMultiView(); return }
        const d = Object.assign({}, Player.vod)   // a copy: stopVod() below empties Player.vod
        const kind = d.kind
        const at = video.position
        if (kind === "movie" || kind === "episode") saveVodProgress()
        win.playbackError = ""
        Player.stopVod()
        vodBrowser.close(); recordings.close()
        openMultiView(true)
        if (kind === "movie" || kind === "episode" || kind === "recording") multiView.addVod(d, at)
    }
    // From a movie's or series' page: in a tile, from where you'd resume.
    function multiViewWithVod(d) {
        if (Player.vodActive) { saveVodProgress(); Player.stopVod() }
        vodBrowser.close(); recordings.close()
        openMultiView(true)
        multiView.addVod(d, d.start || 0)
    }
    // Catch-up of the show on now, from its beginning.
    function watchFromStart() {
        let p = Player.nowProgram
        if (!p.valid) {
            // No listing on now: go back to the top of the hour.
            const d = new Date(); d.setMinutes(0, 0, 0)
            const start = d.getTime()
            if (Date.now() - start < 60000) { toast.show(qsTr("No guide listings for this channel"), true); return }
            d.setHours(d.getHours() + 1)
            p = { title: Player.currentName, start: start, stop: d.getTime(), placeholder: true }
        }
        if (!Player.playCatchup(Player.currentIndex, p, 0))
            toast.show(qsTr("This channel has no catch-up"), true)
    }
    // A movie or episode playing on the TV instead of here.
    property bool vodOnTv: false
    function saveVodProgress() {
        if (Player.vodActive && win.vodOnTv && Cast.active && Cast.position > 0 && Cast.duration > 0) {
            Vod.saveProgress(Player.vod, Cast.position, Cast.duration)
            return
        }
        if (Player.vodActive && video.vod && video.position > 0) {
            Vod.saveProgress(Player.vod, video.position, video.duration)
            Trakt.scrobble("stop", Player.vod, win.vodPercent())
        }
    }
    // After a movie or episode: what's queued, else the next episode.
    function playWhatsNext() {
        const queued = Vod.takeNext()
        if (queued.url) {
            Player.playVod(queued)
            toast.show(qsTr("Up next: %1").arg(queued.title + (queued.kind === "episode" ? " · " + queued.subtitle : "")))
            return true
        }
        return Player.vod.kind === "episode" && win.playNextEpisode()
    }
    function vodPercent() { return video.duration > 0 ? 100 * video.position / video.duration : 0 }
    function playNextEpisode() {
        const next = Vod.nextEpisode(Player.vod)
        if (!next.url) { toast.show(qsTr("That was the last episode")); return false }
        saveVodProgress()
        Player.playVod(next)
        toast.show(qsTr("Playing %1").arg(next.subtitle))
        return true
    }
    function openSearch() {
        channelPanel.close()
        guide.close()
        infoBar.hide()
        shortcuts.opened = false
        search.open()
    }
    function openGuide() {
        if (!Player.hasChannel) return
        channelPanel.close()
        infoBar.hide()
        shortcuts.opened = false
        guide.open()
    }
    function openPlayerOptions() {
        channelPanel.close(); infoBar.hide(); shortcuts.opened = false
        playerOptions.open()
    }
    function changeVolume(delta) {
        video.volume = Math.max(0, Math.min(130, video.volume + delta))
        toast.show(qsTr("Volume %1%").arg(video.volume))
    }

    // ---- Video -------------------------------------------------------------
    MpvVideo {
        id: video
        // Full window normally; the guide's preview box while the guide is open
        // (drawn above the guide so the live picture keeps playing).
        x: guide.opened ? guide.previewRect.x : 0
        y: guide.opened ? guide.previewRect.y : 0
        width: guide.opened ? guide.previewRect.width : win.width
        height: guide.opened ? guide.previewRect.height : win.height
        z: guide.opened ? 20 : 0
        timeshiftMb: Settings.timeshiftMb
        pictureShape: Settings.pictureShape
        deinterlace: Settings.deinterlace
        hardwareDecoding: Settings.hardwareDecoding
        preferredAudio: Settings.audioLanguage
        preferredSubtitles: Settings.subtitleLanguage
        audioDelayMs: Settings.audioDelayMs
        defaultUserAgent: Settings.userAgent
        onPlaybackError: (message) => {
            win.playbackError = message
            // A movie or episode other playlists have too: offer them.
            if (Player.vodActive && Player.vod.sourceCount > 1)
                win.chooseSource(qsTr("%1 didn't play it. Try another source:").arg(Player.vod.sourceName || qsTr("That source")), Player.vod.sourceIndex)
        }
        onHardwareDecodingFailed: toast.show(qsTr("The graphics card's video decoder failed · Relay switched to software decoding until it restarts"), true)
        onPlaybackStarted: {
            win.playbackError = ""
            if (Player.vodActive) Trakt.scrobble("start", Player.vod, win.vodPercent())
        }
        onPausedChanged: if (Player.vodActive && video.vod) Trakt.scrobble(video.paused ? "pause" : "start", Player.vod, win.vodPercent())
        onFinished: {
            if (!Player.vodActive) return
            if (Player.vod.kind === "catchup") {
                if (!Player.playNextCatchup()) { toast.show(qsTr("Back to live")); Player.stopVod() }
                return
            }
            Vod.saveProgress(Player.vod, video.duration, video.duration)   // watched
            Trakt.scrobble("stop", Player.vod, 100)
            if (win.playWhatsNext()) return
            win.leaveVod()
        }
    }

    // Automatic playlist refresh, when nothing would be disturbed by the
    // channel list changing underneath.
    Timer {
        interval: 5 * 60 * 1000
        repeat: true
        running: Settings.playlistRefreshHours > 0 && Session.ready
        onTriggered: {
            if (!Session.playlistDue()) return
            if (Player.vodActive || multiView.opened || pip.active || guide.opened || channelPanel.opened || search.opened
                || vodBrowser.opened || recordings.opened || win.showSetup || playlistPicker.opened || pinDialog.opened) return
            Session.reloadQuietly()
        }
    }

    // Remember how far into a movie we are, every 10 seconds.
    Timer {
        interval: 10000
        repeat: true
        running: Player.vodActive && video.playing
        onTriggered: win.saveVodProgress()
    }

    // ---- Casting -------------------------------------------------------------
    // The sound track picked in a movie, so the TV gets the same one (and the
    // next episode too): its place in the list and its labels.
    property var castAudio: ({})
    function addCastAudio(source) {
        if (!Player.vodActive) return source
        if (video.vod) {
            const tracks = video.tracks("audio")
            const i = tracks.findIndex(t => t.selected)
            if (i >= 0) win.castAudio = { index: i, lang: tracks[i].lang, title: tracks[i].title }
        }
        if (win.castAudio.index !== undefined) {
            source.audioIndex = win.castAudio.index
            source.audioLang = win.castAudio.lang
            source.audioTitle = win.castAudio.title
        }
        source.audioPreferred = Settings.audioLanguage
        return source
    }
    function castTo(deviceId) {
        const source = Player.castSource()
        if (!source.url) { toast.show(qsTr("Nothing is playing"), true); return }
        if (Player.vodActive) { source.start = win.vodOnTv ? Cast.position : video.position; win.vodOnTv = true }
        addCastAudio(source)
        Cast.start(deviceId, source)
        // One connection to the provider: the TV's, through Relay (a paused
        // copy here would hold a second one, and the provider drops it).
        video.stop()
    }
    function stopCasting() {
        Cast.stop()
    }
    Connections {
        target: Cast
        function onMessage(text, problem) { toast.show(text, problem) }
        // A movie or episode played to the end on the TV: the next one goes there too.
        function onFinished() {
            if (!Player.vodActive) { Cast.stop(); return }
            if (Player.vod.kind === "movie" || Player.vod.kind === "episode")
                Vod.saveProgress(Player.vod, Math.max(1, Cast.duration), Math.max(1, Cast.duration))   // watched
            if (win.playWhatsNext()) return
            toast.show(qsTr("That was the end"))
            Cast.stop()
        }
        function onEnded(problem) {
            // A movie that went straight to the TV (no copy here): carry on here.
            if (Player.vodActive && win.vodOnTv && !video.vod) {
                win.vodOnTv = false
                video.playVod(Player.vod.url, Math.max(0, Cast.position - 3))
                toast.show(problem ? problem + " · " + qsTr("carrying on here") : qsTr("Casting stopped · carrying on here"), problem.length > 0)
                return
            }
            // Here, it picks up where the TV got to.
            if (Player.vodActive && win.vodOnTv && video.vod && Cast.position > 5) video.seek(Cast.position - 3, false)
            win.vodOnTv = false
            const back = Player.vodActive ? qsTr("Space plays it here") : qsTr("back to watching here")
            toast.show(problem ? problem + " · " + back : qsTr("Casting stopped · %1").arg(back), problem.length > 0)
            if (!Player.vodActive) Player.retry()
        }
    }
    // Browsing while casting: a small panel over whatever is open.
    CastMini {
        id: castMini
        z: 28
        shown: vodBrowser.opened || guide.opened || recordings.opened || channelPanel.opened || search.opened || multiView.opened
        onStopRequested: win.stopCasting()
        onNextRequested: { win.saveVodProgress(); win.playWhatsNext() }
        onBackRequested: {
            vodBrowser.close(); guide.close(); recordings.close(); channelPanel.close(); search.close()
            if (multiView.opened) multiView.close()
            win.returnFocus()
        }
    }
    // Play / pause (a remote, the phone, a keyboard's media key) reaches the TV from anywhere.
    Shortcut {
        sequences: [Qt.Key_MediaTogglePlayPause, Qt.Key_MediaPlay, Qt.Key_MediaPause]
        context: Qt.ApplicationShortcut
        enabled: Cast.active && !win.showSetup
        onActivated: Cast.togglePause()
    }
    CastBar {
        z: 6.5   // over the movie controls and live bar, under the menu bar
        onStopRequested: win.stopCasting()
        onPickRequested: castPicker.open()
        onNextRequested: { win.saveVodProgress(); win.playWhatsNext() }
    }
    CastPicker {
        id: castPicker
        onChosen: (id) => win.castTo(id)
        onStopRequested: win.stopCasting()
        onFinished: keyHandler.forceActiveFocus()
    }
    Connections {
        target: video
        // Changing channels while casting: don't keep a second connection here.
        function onPlaybackStarted() { if (Cast.active && !Player.vodActive) video.stop() }
    }

    Connections {
        target: Trakt
        function onMessage(text) { toast.show(text) }
    }
    Connections {
        target: Updates
        function onMessage(text, problem) { toast.show(text, problem) }
    }
    Connections {
        target: Player
        function onPlayRequested(url, userAgent, referrer) {
            win.playbackError = ""
            video.play(url, userAgent, referrer)
            infoBar.show()
        }
        function onNumberNotFound(number) { toast.show(qsTr("No channel %1").arg(number), true) }
        function onVodRequested(url, start) {
            win.playbackError = ""
            infoBar.hide()
            vodBrowser.close()
            keyHandler.forceActiveFocus()
            // Casting: it goes to the TV (the next episode, something from the queue…).
            if (Cast.active && Cast.deviceId) {
                video.stop()
                win.vodOnTv = true
                const source = Player.castSource()
                source.start = start
                addCastAudio(source)
                Cast.start(Cast.deviceId, source)
                return
            }
            win.vodOnTv = false
            video.playVod(url, start)
        }
        function onStopRequested() { video.stop() }
        // Casting a channel: the next channel goes to the TV too.
        function onCurrentChanged() {
            if (Cast.active && !Player.vodActive) { Cast.start(Cast.deviceId, Player.castSource()); Qt.callLater(video.stop) }
        }
        function onExternalFailed(message) { toast.show(message, true) }
        function onExternalOpened(name) {
            // One connection is all many providers allow: hand the stream over.
            if (Player.vodActive) { if (!video.paused) video.togglePause(); toast.show(qsTr("Opened in %1 · paused here").arg(name)) }
            else { video.stop(); toast.show(qsTr("Opened in %1 · press %2 to watch here again").arg(name).arg(Keymap.label("retry"))) }
        }
    }
    Connections {
        target: Epg
        function onImportFailed(message) { if (!win.showSetup) toast.show(message, true) }
    }
    Connections {
        target: Schedule
        function onReminderDue(item) { reminderPopup.show(item) }
        function onMessage(text) { toast.show(text) }
    }
    Connections {
        target: Session
        function onReadyChanged() {
            if (!Session.ready) video.stop()
            else if (Settings.playlists.length > 1) toast.show(qsTr("Watching %1").arg(Settings.activeName))
        }
        function onLastErrorChanged() {
            if (!Session.lastError) return
            if (!Session.ready) win.showSetup = true          // saved playlist failed at startup
            else if (!win.showSetup) toast.show(Session.lastError, true)
        }
    }

    // Buffering spinner
    BusyIndicator {
        id: spinner
        anchors.centerIn: parent
        width: 64; height: 64
        running: video.buffering && !win.playbackError && !win.showSetup
        visible: running
        palette.dark: Theme.accent
    }
    Rectangle {
        visible: spinner.visible && video.reconnecting
        anchors.top: spinner.bottom
        anchors.topMargin: 14
        anchors.horizontalCenter: parent.horizontalCenter
        width: reconnectText.implicitWidth + 28
        height: 34
        radius: 17
        color: Theme.panel
        Text {
            id: reconnectText
            anchors.centerIn: parent
            text: qsTr("Connection dropped · reconnecting…")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
        }
    }

    // Channel failed to play: hide the stale last frame, explain, offer retry.
    Rectangle {
        anchors.fill: parent
        color: Theme.background
        opacity: 0.88
        visible: win.playbackError.length > 0 && !win.showSetup
    }
    Rectangle {
        anchors.centerIn: parent
        visible: win.playbackError.length > 0 && !win.showSetup
        width: Math.min(560, parent.width - 64)
        height: errCol.implicitHeight + 48
        radius: 12
        color: Theme.panel
        border.width: 1
        border.color: Theme.border
        Column {
            id: errCol
            anchors.centerIn: parent
            width: parent.width - 48
            spacing: 10
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: Player.vodActive ? Player.vod.title : Player.currentName
                color: Theme.text
                font.pixelSize: Theme.fontLarge
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: win.playbackError
                color: Theme.textSecondary
                font.pixelSize: Theme.fontBody
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: Player.vod.kind === "catchup" ? qsTr("R to retry  ·  Esc for live TV")
                      : Player.vodActive ? qsTr("R to retry  ·  Esc to go back")
                                       : qsTr("R to retry  ·  ↑ ↓ to change channel")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
            }
        }
    }

    // Playlist or guide loading over live TV
    Rectangle {
        visible: (Playlists.busy || Epg.busy) && !win.showSetup
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 24
        height: 40
        width: busyText.implicitWidth + 32
        radius: 20
        color: Theme.panel
        Text {
            id: busyText
            anchors.centerIn: parent
            text: Playlists.busy ? Playlists.status : Epg.status
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
        }
    }

    // ---- Keyboard + mouse on the live video ------------------------------
    FocusScope {
        id: keyHandler
        anchors.fill: parent
        focus: !win.showSetup

        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: (win.visibility !== Window.FullScreen || cursorTimer.running)
                         ? Qt.ArrowCursor : Qt.BlankCursor
            onPositionChanged: { cursorTimer.restart(); if (Player.vodActive) vodOsd.show() }
            onClicked: (e) => {
                if (win.sleepWarning) { win.keepWatching(); return }
                if (Player.vodActive) { if (e.button === Qt.RightButton || vodOsd.shown) video.togglePause(); vodOsd.show(); return }
                if (e.button === Qt.RightButton) liveMenu.popup()
                else if (channelPanel.opened) channelPanel.close()
                else channelPanel.open()
            }
            onDoubleClicked: win.toggleFullscreen()
            // Volume by wheel only on the bare live picture; with a list,
            // the guide or settings open the wheel belongs to them.
            onWheel: (w) => {
                w.accepted = true
                if (channelPanel.opened || guide.opened || search.opened || vodBrowser.opened || recordings.opened || multiView.opened
                    || win.showSetup || shortcuts.opened) return
                if (w.angleDelta.y !== 0) win.changeVolume(w.angleDelta.y > 0 ? 5 : -5)
            }
            Timer { id: cursorTimer; interval: 2500; running: true }
        }

        // While a movie or episode plays.
        function vodKey(e) {
            const shift = e.modifiers & Qt.ShiftModifier
            switch (e.key) {
            case Qt.Key_Left: vodOsd.seekBy(shift ? -60 : -10); break
            case Qt.Key_Right: vodOsd.seekBy(shift ? 60 : 10); break
            case Qt.Key_Down: vodOsd.seekBy(-60); break
            case Qt.Key_Up: vodOsd.seekBy(60); break
            case Qt.Key_PageDown: vodOsd.seekBy(-600); break
            case Qt.Key_PageUp: vodOsd.seekBy(600); break
            case Qt.Key_AudioRewind: vodOsd.seekBy(-30); break
            case Qt.Key_AudioForward: vodOsd.seekBy(30); break
            case Qt.Key_Home: vodOsd.seekTo(0); break
            case Qt.Key_End:
                if (Player.vod.kind === "catchup") win.leaveVod()
                else vodOsd.seekTo(vodOsd.dur)
                break
            case Qt.Key_Space:
            case Qt.Key_K:
            case Qt.Key_P:
            case Qt.Key_MediaTogglePlayPause:
            case Qt.Key_MediaPlay:
            case Qt.Key_MediaPause:
                video.togglePause(); vodOsd.show(); break
            case Qt.Key_Return:
            case Qt.Key_Enter:
            case Qt.Key_I:
                vodOsd.toggle(); break
            case Qt.Key_A: toast.show(vodOsd.cycleTrack("audio")); break
            case Qt.Key_C: toast.show(vodOsd.cycleTrack("sub")); break
            case Qt.Key_Z: toast.show(playerOptions.nextShape(1)); break
            case Qt.Key_O: win.openPlayerOptions(); break
            case Qt.Key_N:
            case Qt.Key_MediaNext:
                if (Player.vod.kind === "episode") win.playNextEpisode(); break
            case Qt.Key_Escape:
            case Qt.Key_Backspace:
            case Qt.Key_Back:
            case Qt.Key_MediaStop:
                if (shortcuts.opened) shortcuts.opened = false
                else win.leaveVod()
                break
            case Qt.Key_F:
            case Qt.Key_F11:
                win.toggleFullscreen(); break
            case Qt.Key_M:
            case Qt.Key_VolumeMute:
                video.toggleMute()
                toast.show(video.muted ? qsTr("Muted") : qsTr("Sound on"))
                break
            case Qt.Key_Equal:
            case Qt.Key_Plus:
            case Qt.Key_VolumeUp:
                win.changeVolume(5); break
            case Qt.Key_Minus:
            case Qt.Key_VolumeDown:
                win.changeVolume(-5); break
            case Qt.Key_R:
                win.playbackError = ""
                if (Player.vod.kind === "catchup") { Player.retry(); break }
                Player.playVod(Object.assign({}, Player.vod, { start: video.position > 0 ? video.position : Player.vod.start }))
                break
            case Qt.Key_W:
                if (e.modifiers & Qt.ShiftModifier) { if (pip.active) pip.moveCorner(1) }
                else win.togglePip()
                break
            case Qt.Key_X:
                win.swapPip(); break
            case Qt.Key_H:
            case Qt.Key_Question:
            case Qt.Key_F1:
                shortcuts.open(); break
            default:
                return
            }
            e.accepted = true
        }

        Keys.onPressed: (ev) => {
            if (win.sleepWarning) { win.keepWatching(); ev.accepted = true; return }
            // While casting, Space and Esc work the TV.
            if (Cast.active && !(ev.modifiers & Qt.ControlModifier)) {
                if (ev.key === Qt.Key_Space || ev.key === Qt.Key_MediaTogglePlayPause) { Cast.togglePause(); ev.accepted = true; return }
                // A movie or episode on the TV: skip back and ahead there.
                if (!Cast.live) {
                    const step = ev.key === Qt.Key_Left || ev.key === Qt.Key_AudioRewind ? -10
                               : ev.key === Qt.Key_Right || ev.key === Qt.Key_AudioForward ? 30
                               : ev.key === Qt.Key_Down ? -60 : ev.key === Qt.Key_Up ? 60 : 0
                    if (step) { Cast.seekBy(step); ev.accepted = true; return }
                    if (ev.key === Qt.Key_N && Player.vod.kind === "episode") { win.saveVodProgress(); win.playWhatsNext(); ev.accepted = true; return }
                }
                if (ev.key === Qt.Key_Escape) { win.stopCasting(); ev.accepted = true; return }
            }
            if (ev.key === Keymap.keyOf("cast") && !(ev.modifiers & (Qt.ControlModifier | Qt.AltModifier))) {
                castPicker.open(); ev.accepted = true; return
            }
            if (Player.vodActive) {
                if (ev.key === Keymap.keyOf("multiview") && !(ev.modifiers & (Qt.ControlModifier | Qt.AltModifier))) {
                    win.multiViewFromVod(); ev.accepted = true; return
                }
                if (ev.key === Keymap.keyOf("external") && !(ev.modifiers & Qt.ControlModifier)) { Player.openExternally(); ev.accepted = true; return }
                vodKey(ev)
                return
            }
            // The user's own keys (Settings → Keys) stand in for the usual ones.
            const e = { key: Keymap.translate(ev.key, ev.modifiers), modifiers: ev.modifiers, accepted: false }
            liveKey(e)
            ev.accepted = e.accepted
        }
        function liveKey(e) {
            const ctrl = e.modifiers & Qt.ControlModifier
            // Pause live TV: move within what's been kept.
            const shift = e.modifiers & Qt.ShiftModifier
            const liveSeek = (shift && e.key === Qt.Key_Left) || e.key === Qt.Key_Comma ? -10
                           : (shift && e.key === Qt.Key_Right) || e.key === Qt.Key_Period ? 10
                           : e.key === Qt.Key_AudioRewind ? -30 : e.key === Qt.Key_AudioForward ? 30 : 0
            if (liveSeek) {
                video.seekLive(liveSeek)
                liveBar.flash()
                if (video.liveBuffer < 1 && liveSeek < 0) toast.show(qsTr("Nothing saved to rewind yet"))
                e.accepted = true
                return
            }
            if (e.key === Qt.Key_End) {
                if (video.liveBehind > 0 || video.paused) { video.goLive(); toast.show(qsTr("Live")); liveBar.flash() }
                e.accepted = true
                return
            }
            if (e.key === Qt.Key_B) { win.watchFromStart(); e.accepted = true; return }
            if (ctrl && e.key === Qt.Key_R) { win.toggleRecordNow(); e.accepted = true; return }
            if (reminderPopup.shown && (e.key === Qt.Key_Return || e.key === Qt.Key_Enter)) {
                reminderPopup.watch(); e.accepted = true; return
            }
            if (reminderPopup.shown && e.key === Qt.Key_Escape) { reminderPopup.dismiss(); e.accepted = true; return }
            if (ctrl && e.key === Qt.Key_F) { win.openSearch(); e.accepted = true; return }
            if (e.key >= Qt.Key_0 && e.key <= Qt.Key_9 && !ctrl) {
                Player.typeDigit(e.key - Qt.Key_0)
            } else switch (e.key) {
            case Qt.Key_Return:
            case Qt.Key_Enter:
                if (Player.pendingNumber.length) Player.commitNumber()
                else channelPanel.open()
                break
            case Qt.Key_Left:
                channelPanel.open(); break
            case Qt.Key_Up:
            case Qt.Key_PageUp:
            case Qt.Key_ChannelUp:
                Player.channelUp(); break
            case Qt.Key_Down:
            case Qt.Key_PageDown:
            case Qt.Key_ChannelDown:
                Player.channelDown(); break
            case Qt.Key_Right:
            case Qt.Key_I:
                infoBar.toggle(); break
            case Qt.Key_Backspace:
            case Qt.Key_L:
                if (Player.pendingNumber.length) Player.cancelNumber()
                else if (Player.hasPrevious) Player.previousChannel()
                break
            case Qt.Key_Escape:
                if (Player.pendingNumber.length) Player.cancelNumber()
                else if (shortcuts.opened) shortcuts.opened = false
                else if (infoBar.shown) infoBar.hide()
                // A TV is always full screen, and its remote has few buttons:
                // Back opens Relay's menu (Guide, Movies, Settings, Leave Relay).
                else if (Settings.tv) win.openMenu()
                else if (win.visibility === Window.FullScreen) win.visibility = Window.Windowed
                break
            case Qt.Key_F:
            case Qt.Key_F11:
                win.toggleFullscreen(); break
            case Qt.Key_Space:
            case Qt.Key_MediaTogglePlayPause:
                video.togglePause(); liveBar.flash(); break
            case Qt.Key_M:
            case Qt.Key_VolumeMute:
                video.toggleMute()
                toast.show(video.muted ? qsTr("Muted") : qsTr("Sound on"))
                break
            case Qt.Key_Equal:
            case Qt.Key_Plus:
            case Qt.Key_VolumeUp:
                win.changeVolume(5); break
            case Qt.Key_Minus:
            case Qt.Key_VolumeDown:
                win.changeVolume(-5); break
            case Qt.Key_R:
                Player.retry(); break
            case Qt.Key_S:
                win.openSetupGuarded(); break
            case Qt.Key_G:
            case Qt.Key_Guide:
                win.openGuide(); break
            case Qt.Key_V:
                win.openVod(); break
            case Qt.Key_Q:
                win.openMultiView(); break
            case Qt.Key_C:
                toast.show(vodOsd.cycleTrack("sub")); break
            case Qt.Key_A:
                toast.show(playerOptions.nextAudio(1)); break
            case Qt.Key_Z:
                toast.show(playerOptions.nextShape(1)); break
            case Qt.Key_O:
                win.openPlayerOptions(); break
            case Qt.Key_E:
                Player.openExternally(); break
            case Qt.Key_W:
                if (shift) { if (pip.active) pip.moveCorner(1) }
                else win.togglePip()
                break
            case Qt.Key_X:
                win.swapPip(); break
            case Qt.Key_P:
                if (Settings.playlists.length < 2) toast.show(qsTr("Add another playlist in Settings (S) to switch between them"))
                else { channelPanel.close(); infoBar.hide(); playlistPicker.open() }
                break
            case Qt.Key_Slash:
            case Qt.Key_Search:
                win.openSearch(); break
            case Qt.Key_H:
            case Qt.Key_Question:
            case Qt.Key_F1:
                shortcuts.open(); break
            default:
                return
            }
            e.accepted = true
        }
    }

    // ---- Overlays -----------------------------------------------------------
    InfoBar { id: infoBar; video: video; visible: !Player.vodActive }

    PipView {
        id: pip
        z: 4
        suspended: guide.opened || multiView.opened || vodBrowser.opened || recordings.opened
                   || search.opened || win.showSetup
        // Kept clear of whatever is along the bottom.
        bottomInset: Player.vodActive ? (vodOsd.shown ? 200 : 0)
                                      : (infoBar.shown ? infoBar.height + 40 : 0) + (liveBar.shown ? liveBar.panelHeight : 0)
        onSwapRequested: win.swapPip()
    }

    VodOsd {
        id: vodOsd
        video: video
        z: 5
        onNextRequested: win.playNextEpisode()
        onLiveRequested: win.leaveVod()
        onBackRequested: win.leaveVod()
        onOptionsRequested: win.openPlayerOptions()
        onCastRequested: castPicker.open()
        // Multi-view is live channels: the movie keeps its place, and live TV opens in tiles.
        onMultiViewRequested: win.multiViewFromVod()
        onSourceRequested: win.chooseSource(qsTr("Play from"), -1)
    }

    // Live TV's player controls (bottom; appear with the mouse or after a skip).
    TimeshiftBar {
        id: liveBar
        video: video
        z: 6
        mouseActive: mouse.containsMouse && cursorTimer.running
        allowed: !guide.opened && !channelPanel.opened && !vodBrowser.opened && !recordings.opened && !win.showSetup
                 && !multiView.opened && !search.opened && !shortcuts.opened && !playlistPicker.opened && !pinDialog.opened
                 && !playerOptions.opened
        bottomInset: infoBar.shown ? infoBar.height + 40 : 0
        onMessage: (text) => toast.show(text)
    }

    // The mouse's way around live TV (appears when the mouse moves).
    ControlBar {
        id: controlBar
        z: 7
        wanted: mouse.containsMouse && cursorTimer.running
        allowed: !Player.vodActive && !guide.opened && !vodBrowser.opened && !recordings.opened && !multiView.opened
                 && !search.opened && !win.showSetup && !shortcuts.opened && !playlistPicker.opened && !pinDialog.opened
                 && !channelPanel.opened && !playerOptions.opened
        fullscreen: win.visibility === Window.FullScreen
        muted: video.muted
        pipOn: pip.active
        captionsOn: win.captionsOn
        onChannelsClicked: { infoBar.hide(); channelPanel.open() }
        onGuideClicked: win.openGuide()
        onMoviesClicked: win.openVod()
        onRecordingsClicked: win.openRecordings(0)
        onMultiViewClicked: win.openMultiView()
        onSearchClicked: win.openSearch()
        onPipClicked: win.togglePip()
        onOptionsClicked: win.openPlayerOptions()
        onCaptionsClicked: { toast.show(vodOsd.cycleTrack("sub")); win.refreshCaptions() }
        onMuteClicked: { video.toggleMute(); toast.show(video.muted ? qsTr("Muted") : qsTr("Sound on")) }
        onFullscreenClicked: win.toggleFullscreen()
        onPlaylistsClicked: {
            if (Settings.playlists.length < 2) win.openSetupGuarded()
            else { channelPanel.close(); infoBar.hide(); playlistPicker.open() }
        }
        onSettingsClicked: win.openSetupGuarded()
        onHelpClicked: shortcuts.open()
        onCastClicked: castPicker.open()
        casting: Cast.active
    }
    // Whether captions are on, for the menu bar's button (checked while it shows).
    property bool captionsOn: false
    function refreshCaptions() { captionsOn = video.tracks("sub").some(t => t.selected) }
    Timer { interval: 1500; repeat: true; running: controlBar.shown; triggeredOnStart: true; onTriggered: win.refreshCaptions() }

    Guide {
        id: guide
        anchors.fill: parent
        z: 10
        onClosed: win.returnFocus()
        onMessage: (text) => toast.show(text)
        onSearchRequested: win.openSearch()
    }

    Connections {
        target: Player
        function onPinRequired(name) {
            win.requestPin(qsTr("\"%1\" is locked").arg(name),
                           () => Player.resumePending(), () => Player.cancelPending())
        }
    }

    ChannelPanel {
        id: channelPanel
        onOpenedChanged: if (opened) { infoBar.hide(); shortcuts.opened = false }
        onClosed: win.returnFocus()
        onMessage: (text) => toast.show(text)
        onSearchRequested: win.openSearch()
        onPinRequested: (reason, ok) => win.requestPin(reason, ok)
        onMultiViewRequested: (globalIndex) => { channelPanel.close(); win.openMultiViewWith(globalIndex) }
    }

    // What the phone remote shows at the top.
    Binding {
        target: Phone
        property: "nowShowing"
        value: Player.vodActive ? (Player.vod.title || "")
             : Player.hasChannel ? Player.currentName + (Player.nowProgram.title ? " · " + Player.nowProgram.title : "") : ""
    }
    Connections {
        target: Phone
        function onPhonePaired() { toast.show(qsTr("Phone paired · it's a remote now")) }
    }
    // Remote buttons with a meaning of their own (remotes, controllers, the
    // phone). Most become the key the user has for them.
    Connections {
        target: Remote
        // Play / pause while browsing during a cast: the TV (the browser doesn't use it).
        function onTriggered(name) {
            if (name === "playpause" && Cast.active && castMini.visible) Cast.togglePause()
        }
        function onAction(name) {
            if (name === "menu") { win.toggleMenu(); return }
            // These go straight there from wherever you are (Settings included).
            const jumps = ["home", "guide", "movies", "search", "favorites", "settings"]
            if (jumps.indexOf(name) >= 0) { win.jumpTo(name); return }
            const key = Keymap.keyOf(name)
            if (key) Remote.sendKey(key)
        }
    }
    // Leave Settings and anything open over the picture (not a PIN prompt), then go.
    function jumpTo(name) {
        if (pinDialog.opened) return
        if (win.showSetup) {
            if (!setupScreen.canClose) return       // no playlist yet: nowhere to go
            win.showSetup = false
        }
        liveMenu.close()
        if (name === "settings") { win.openSetupGuarded(); return }
        if (name === "home") { win.goHome(); return }
        if (name === "search" && vodBrowser.opened) { vodBrowser.focusSearch(); return }
        if (name === "movies") {
            if (vodBrowser.opened) { vodBrowser.showHome(); return }
            castPicker.close(); playerOptions.close(); shortcuts.opened = false; recordings.close()
            if (multiView.opened) multiView.close()
            if (Player.vodActive && !win.vodOnTv) { saveVodProgress(); Player.stopVod() }
            win.openVod(Vod.section)
            return
        }
        // Guide, search, favorites belong to live TV.
        castPicker.close(); playerOptions.close(); shortcuts.opened = false; recordings.close()
        if (multiView.opened) multiView.close()
        if (Player.vodActive) { saveVodProgress(); Player.stopVod() }
        vodBrowser.close()
        // Guide again while it's open: its group chooser (All channels…).
        if (name === "guide") { if (!guide.opened) win.openGuide(); else guide.focusGroups() }
        else if (name === "search") win.openSearch()
        else if (name === "favorites") {
            guide.close()
            channelPanel.open()
            Channels.currentGroup = Channels.favoritesGroup
        }
        else win.returnFocus()
    }
    // Menu: opens it, and closes it again.
    function toggleMenu() {
        if (liveMenu.opened) { liveMenu.close(); return }
        if (playerOptions.opened) { playerOptions.close(); return }
        if (win.showSetup) {
            if (pinDialog.opened || !setupScreen.canClose) return
            win.showSetup = false
        }
        win.openMenu()
    }
    // Home: back to live TV, whatever is open.
    function goHome() {
        if (pinDialog.opened || win.showSetup) return
        castPicker.close(); playerOptions.close(); search.close(); shortcuts.opened = false
        guide.close(); channelPanel.close(); recordings.close()
        if (multiView.opened) multiView.close()
        // A movie on the TV keeps playing there: Home is its casting screen.
        if (Player.vodActive && !win.vodOnTv) { saveVodProgress(); Player.stopVod() }
        vodBrowser.close()
        returnFocus()
    }
    // Menu: what right-click offers there.
    function openMenu() {
        if (Player.vodActive && !win.vodOnTv) { vodOsd.show(); openPlayerOptions(); return }
        if (win.showSetup || pinDialog.opened) return
        // Anywhere else: the menu (it's how a controller gets to Movies and Settings).
        liveMenu.popup(win.width / 2 - 130, win.height / 2 - 170)
    }

    // Right-click on the live picture.
    RelayMenu {
        id: liveMenu
        // Back to the picture's keys, unless the choice opened something that has them now.
        onClosed: Qt.callLater(() => {
            if (!multiView.opened && !guide.opened && !channelPanel.opened && !castPicker.opened && !pip.active
                    && !vodBrowser.opened && !recordings.opened && !win.showSetup)
                win.returnFocus()
        })
        RelayMenuItem { text: qsTr("Movies & series"); onTriggered: win.openVod(0) }
        RelayMenuItem { text: qsTr("Recordings"); onTriggered: win.openRecordings(0) }
        RelayMenuItem { text: qsTr("Settings"); onTriggered: win.openSetupGuarded() }
        MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: Theme.border } }
        RelayMenuItem { text: qsTr("Channel info"); onTriggered: infoBar.toggle() }
        RelayMenuItem { text: qsTr("Channel list"); onTriggered: channelPanel.open() }
        RelayMenuItem { text: qsTr("TV guide"); onTriggered: win.openGuide() }
        RelayMenuItem { text: qsTr("Search"); visible: Settings.tv; enabled: visible; onTriggered: win.openSearch() }
        // A TV has one video decoder for apps and nothing to cast to.
        RelayMenuItem { text: qsTr("Multi-view"); visible: !Settings.tv; enabled: visible && Player.hasChannel; onTriggered: win.openMultiView() }
        RelayMenuItem { text: pip.active ? qsTr("Close picture-in-picture") : qsTr("Picture-in-picture"); visible: !Settings.tv; enabled: visible && Player.hasChannel; onTriggered: win.togglePip() }
        RelayMenuItem { text: Cast.active ? qsTr("Casting…") : qsTr("Cast to a TV…"); visible: !Settings.tv; enabled: visible; onTriggered: castPicker.open() }
        RelayMenuItem {
            text: Player.currentIndex >= 0 && Channels.isFavorite(Player.currentIndex) ? qsTr("Remove from Favorites") : qsTr("Add to Favorites")
            enabled: Player.hasChannel
            onTriggered: {
                const fav = Channels.toggleFavorite(Player.currentIndex)
                toast.show(fav ? qsTr("Added to Favorites") : qsTr("Removed from Favorites"))
            }
        }
        MenuSeparator { visible: Settings.tv; height: visible ? implicitHeight : 0; contentItem: Rectangle { implicitHeight: 1; color: Theme.border } }
        RelayMenuItem { text: qsTr("Leave Relay"); visible: Settings.tv; enabled: visible; onTriggered: Qt.quit() }
    }

    // On a TV, the catalogue is let go a while after Movies & series closes
    // (it can take 200 MB that live TV needs); opening it again reads it back
    // from the saved copy in a few seconds.
    Timer {
        interval: 90000
        running: Settings.tv && Vod.loaded && !vodBrowser.opened && !recordings.opened && !Player.vodActive
        onTriggered: Vod.release()
    }

    VodBrowser {
        id: vodBrowser
        anchors.fill: parent
        z: 15
        onClosed: if (!recordings.opened) win.returnFocus()
        onMessage: (text) => toast.show(text)
        onPlayRequested: (d) => Player.playVod(d)
        onMultiViewRequested: (d) => win.multiViewWithVod(d)
        onRecordingsRequested: win.openRecordings(0)
        onPinRequested: (reason, ok) => win.requestPin(reason, ok)
    }

    Recordings {
        id: recordings
        anchors.fill: parent
        z: 15
        onClosed: if (!vodBrowser.opened) win.returnFocus()
        onMessage: (text) => toast.show(text)
        onPlayRequested: (d) => { recordings.close(); Player.playVod(d) }
        onMoviesRequested: (section) => { recordings.close(); win.openVod(section) }
        onPinRequested: (reason, ok) => win.requestPin(reason, ok)
    }

    MultiView {
        id: multiView
        anchors.fill: parent
        z: 18
        mainVideo: video
        onMessage: (text) => toast.show(text)
        onPinRequested: (reason, ok) => win.requestPin(reason, ok)
        onClosed: (gi) => {
            win.returnFocus()
            if (gi >= 0) Player.tune(gi, gi === Player.currentIndex ? Player.zapGroup : -1)
        }
        onVodChosen: (d) => Player.playVod(d)
    }

    PlaylistPicker {
        id: playlistPicker
        z: 26
        onClosed: if (!win.showSetup) win.returnFocus()
        onManageRequested: win.openSetupGuarded()
    }

    ReminderPopup {
        id: reminderPopup
        z: 28
        onWatchRequested: (item) => {
            if (item.playlistId && item.playlistId !== Settings.activeId) {   // from another playlist
                if (Player.vodActive) { win.saveVodProgress(); Player.stopVod() }
                multiView.close(); vodBrowser.close(); recordings.close(); guide.close(); channelPanel.close()
                Session.switchPlaylistAndTune(item.playlistId, item.channelKey)
                return
            }
            if (item.globalIndex < 0) { toast.show(qsTr("That channel isn't in the playlist any more"), true); return }
            if (Player.vodActive) { win.saveVodProgress(); Player.stopVod() }
            vodBrowser.close(); recordings.close(); guide.close(); channelPanel.close()
            if (multiView.opened) { multiView.setTile(multiView.selected, Settings.activeId, Channels.keyAt(item.globalIndex)); return }
            Player.tune(item.globalIndex, -1)
        }
    }

    // Recording indicator
    Rectangle {
        visible: Schedule.recordingCount > 0 && !win.showSetup && !guide.opened && !vodBrowser.opened && !recordings.opened
        z: 27
        x: 24
        y: 24
        height: 30
        width: recText.implicitWidth + 34
        radius: 15
        color: Theme.panel
        Rectangle { x: 12; anchors.verticalCenter: parent.verticalCenter; width: 9; height: 9; radius: 4.5; color: "#E5484D" }
        Text {
            id: recText
            x: 27
            anchors.verticalCenter: parent.verticalCenter
            text: Schedule.recordingCount > 1 ? qsTr("REC %1").arg(Schedule.recordingCount) : qsTr("REC")
            color: Theme.text
            font.pixelSize: Theme.fontSmall
            font.weight: Font.DemiBold
        }
    }

    Search {
        id: search
        anchors.fill: parent
        z: 25
        onClosed: if (!vodBrowser.opened) win.returnFocus()
        onVodRequested: (kind, id, preview) => {
            if (Player.vodActive) { win.saveVodProgress(); Player.stopVod() }
            vodBrowser.openDetails(kind, id, preview)
        }
    }

    NumberEntry {}

    // Picture and sound (O).
    PlayerOptions {
        id: playerOptions
        video: video
        sleepMinutesLeft: win.sleepMinutesLeft
        sleepChoice: win.sleepChoice
        onClosed: win.returnFocus()
        onSleepRequested: (minutes) => win.setSleepTimer(minutes)
    }

    // ---- Sleep timer ---------------------------------------------------------
    property double sleepAt: 0            // when playback stops (ms; 0 = off)
    property int sleepChoice: 0
    property double sleepNow: Date.now()
    readonly property int sleepSecondsLeft: sleepAt > 0 ? Math.max(0, Math.round((sleepAt - sleepNow) / 1000)) : 0
    readonly property int sleepMinutesLeft: Math.ceil(sleepSecondsLeft / 60)
    readonly property bool sleepWarning: sleepAt > 0 && sleepSecondsLeft <= 60
    function setSleepTimer(minutes) {
        sleepChoice = minutes
        sleepNow = Date.now()
        sleepAt = minutes > 0 ? Date.now() + minutes * 60000 : 0
    }
    // Any key or click during the last minute: another 30 minutes.
    function keepWatching() {
        setSleepTimer(30)
        toast.show(qsTr("Sleep timer: 30 more minutes"))
    }
    function sleepNowDue() {
        sleepAt = 0
        sleepChoice = 0
        playerOptions.close(); shortcuts.opened = false
        pip.close()
        if (multiView.opened) multiView.close()
        if (Player.vodActive) {
            win.saveVodProgress()
            if (!video.paused) video.togglePause()
            vodOsd.show()
            toast.show(qsTr("Paused by the sleep timer"))
            return
        }
        video.stop()
        win.playbackError = qsTr("Stopped by the sleep timer.")
    }
    Timer {
        interval: 1000
        repeat: true
        running: win.sleepAt > 0
        onTriggered: {
            win.sleepNow = Date.now()
            if (win.sleepNow >= win.sleepAt) win.sleepNowDue()
        }
    }
    Rectangle {
        visible: win.sleepWarning
        z: 29
        anchors.horizontalCenter: parent.horizontalCenter
        y: 96
        width: sleepRow.implicitWidth + 36
        height: 52
        radius: 26
        color: Theme.panelSolid
        border.width: 1
        border.color: Theme.accent
        Row {
            id: sleepRow
            anchors.centerIn: parent
            spacing: 14
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Sleep timer: stopping in %1 s").arg(win.sleepSecondsLeft)
                color: Theme.text
                font.pixelSize: Theme.fontBody
            }
            BarButton {
                anchors.verticalCenter: parent.verticalCenter
                size: 36
                accent: true
                label: qsTr("Keep watching")
                showShortcut: false
                onClicked: win.keepWatching()
            }
        }
    }

    // Every keyboard shortcut (H, ?, F1 or the ? in the menu bar).
    ShortcutsPanel {
        id: shortcuts
        onClosed: win.returnFocus()
    }

    // Over the guide's small picture: clicking it goes back to full screen.
    Rectangle {
        visible: guide.opened && guide.previewHovered
        z: 21
        x: guide.previewRect.x + (guide.previewRect.width - width) / 2
        y: guide.previewRect.y + guide.previewRect.height - height - 12
        width: hintRow.implicitWidth + 24
        height: 34
        radius: 17
        color: "#CC000000"
        Row {
            id: hintRow
            anchors.centerIn: parent
            spacing: 8
            Icon { anchors.verticalCenter: parent.verticalCenter; width: 16; height: 16; path: Icons.fullscreen }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Watch full screen")
                color: Theme.text
                font.pixelSize: Theme.fontSmall + 1
            }
        }
    }

    // On a TV: what the remote's few buttons do on the live picture, shown
    // with the channel's details.
    Rectangle {
        visible: Settings.tv && infoBar.shown && !win.showSetup && !channelPanel.opened && !guide.opened
                 && !vodBrowser.opened && !liveMenu.opened
        z: 21
        anchors.horizontalCenter: parent.horizontalCenter
        y: 28
        width: tvHints.implicitWidth + 32
        height: 40
        radius: 20
        color: "#CC000000"
        Text {
            id: tvHints
            anchors.centerIn: parent
            text: qsTr("OK  channel list  (hold: guide)   ·   ∧ ∨  channels   ·   →  what's on   ·   Back  menu")
            color: Theme.text
            font.pixelSize: Theme.fontBody
        }
    }

    Toast { id: toast; z: 30 }

    // ---- Several playlists have this movie or episode --------------------------------------
    Connections {
        target: Vod
        function onPlaybackReady(d) { win.playbackError = ""; Player.playVod(d) }
        function onPlaybackProblem(message) { toast.show(message, true) }
    }
    function chooseSource(heading, failed) {
        if (!Player.vodActive || !(Player.vod.sourceCount > 1)) return
        const list = Vod.sources(Player.vod.detailsKind, Player.vod.detailsId)
        if (list.length < 2) return
        vodSourcePicker.open(list, heading, failed >= 0 ? -1 : Player.vod.sourceIndex, failed)
    }
    SourcePicker {
        id: vodSourcePicker
        onChosen: (i) => {
            win.playbackError = ""
            const at = video.position > 0 ? video.position : (Player.vod.start || 0)
            if (Player.vod.kind === "movie" || Player.vod.kind === "episode") win.saveVodProgress()
            Vod.switchSource(Player.vod, i, at)
            win.returnFocus()
        }
        onDismissed: win.returnFocus()
    }

    // A new version (Settings → Updates has the same).
    UpdateBanner { z: 24; suppressed: win.showSetup }

    SetupScreen {
        id: setupScreen
        anchors.fill: parent
        visible: win.showSetup
        focus: win.showSetup
        onCloseRequested: { win.showSetup = false; win.returnFocus() }
        onPinRequested: (mode, reason, ok) => pinDialog.open(mode, reason, ok)
        focusGuard: !pinDialog.opened
    }

    // Esc always gets out of settings, even if keyboard focus went astray.
    Shortcut {
        sequences: ["Esc"]
        context: Qt.WindowShortcut
        enabled: win.showSetup && !pinDialog.opened
        onActivated: setupScreen.goBack()
    }

    PinDialog {
        id: pinDialog
        z: 50
        onFinished: win.restoreFocusAfterPin()
    }
}

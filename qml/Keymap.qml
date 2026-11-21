pragma Singleton
import QtQuick
import Relay.Core

// The keys for watching live TV that can be changed in Settings → Keys.
// Each action has its usual key; Settings.keys holds the ones the user changed.
QtObject {
    readonly property var actions: [
        { id: "guide", label: qsTr("TV guide"), key: Qt.Key_G },
        { id: "movies", label: qsTr("Movies, series and recordings"), key: Qt.Key_V },
        { id: "search", label: qsTr("Search"), key: Qt.Key_Slash },
        { id: "multiview", label: qsTr("Multi-view"), key: Qt.Key_Q },
        { id: "pip", label: qsTr("Picture-in-picture"), key: Qt.Key_W },
        { id: "swap", label: qsTr("Swap picture-in-picture"), key: Qt.Key_X },
        { id: "playlist", label: qsTr("Switch playlist"), key: Qt.Key_P },
        { id: "settings", label: qsTr("Settings"), key: Qt.Key_S },
        { id: "help", label: qsTr("Keyboard shortcuts"), key: Qt.Key_H },
        { id: "info", label: qsTr("What's on"), key: Qt.Key_I },
        { id: "last", label: qsTr("Last channel watched"), key: Qt.Key_L },
        { id: "pause", label: qsTr("Pause / play"), key: Qt.Key_Space },
        { id: "fromStart", label: qsTr("Watch the show from the start"), key: Qt.Key_B },
        { id: "fullscreen", label: qsTr("Full screen"), key: Qt.Key_F },
        { id: "mute", label: qsTr("Mute"), key: Qt.Key_M },
        { id: "captions", label: qsTr("Captions / subtitles"), key: Qt.Key_C },
        { id: "audio", label: qsTr("Audio track"), key: Qt.Key_A },
        { id: "shape", label: qsTr("Picture shape"), key: Qt.Key_Z },
        { id: "options", label: qsTr("Picture and sound"), key: Qt.Key_O },
        { id: "external", label: qsTr("Open in the external player"), key: Qt.Key_E },
        { id: "cast", label: qsTr("Cast to a TV or Chromecast"), key: Qt.Key_T },
        { id: "retry", label: qsTr("Reconnect the channel"), key: Qt.Key_R },
    ]
    readonly property var custom: Settings.keys

    function action(id) { return actions.find(a => a.id === id) }
    function keyOf(id) {
        const a = action(id)
        return a ? (custom[id] || a.key) : 0
    }
    // The key the live-TV handler should act on for the key that was pressed:
    // an action's usual key when its (possibly changed) key was pressed, 0
    // when the key's usual action has moved to another key.
    function translate(key, modifiers) {
        if (modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) return key
        for (const a of actions)
            if ((custom[a.id] || a.key) === key) return a.key
        for (const a of actions)
            if (a.key === key) return 0
        return key
    }
    // Which action already uses this key ("" = free).
    function userOf(key, exceptId) {
        const a = actions.find(x => x.id !== exceptId && (custom[x.id] || x.key) === key)
        return a ? a.label : ""
    }
    // Keys that do something fixed while watching and can't be given away.
    function reserved(key) {
        return [Qt.Key_Escape, Qt.Key_Return, Qt.Key_Enter, Qt.Key_Up, Qt.Key_Down, Qt.Key_Left, Qt.Key_Right,
                Qt.Key_PageUp, Qt.Key_PageDown, Qt.Key_End, Qt.Key_Backspace, Qt.Key_Tab, Qt.Key_Backtab,
                Qt.Key_Shift, Qt.Key_Control, Qt.Key_Alt, Qt.Key_Meta, Qt.Key_AltGr, Qt.Key_CapsLock,
                Qt.Key_Comma, Qt.Key_Period, Qt.Key_Plus, Qt.Key_Minus, Qt.Key_Equal, Qt.Key_F1, Qt.Key_F11,
                Qt.Key_Question].indexOf(key) >= 0 || (key >= Qt.Key_0 && key <= Qt.Key_9)
    }
    function label(id) { return keyName(keyOf(id)) }
    function keyName(key) {
        if (key >= Qt.Key_A && key <= Qt.Key_Z) return String.fromCharCode(key)
        if (key >= Qt.Key_F1 && key <= Qt.Key_F35) return "F" + (key - Qt.Key_F1 + 1)
        switch (key) {
        case Qt.Key_Space: return "Space"
        case Qt.Key_Slash: return "/"
        case Qt.Key_Backslash: return "\\"
        case Qt.Key_Semicolon: return ";"
        case Qt.Key_Apostrophe: return "'"
        case Qt.Key_BracketLeft: return "["
        case Qt.Key_BracketRight: return "]"
        case Qt.Key_QuoteLeft: return "`"
        case Qt.Key_Insert: return "Insert"
        case Qt.Key_Delete: return "Delete"
        case Qt.Key_Home: return "Home"
        case Qt.Key_Asterisk: return "*"
        case Qt.Key_MediaPlay: case Qt.Key_MediaTogglePlayPause: return "Play/Pause"
        }
        return key > 32 && key < 127 ? String.fromCharCode(key) : qsTr("Key %1").arg(key)
    }
}

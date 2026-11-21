import QtQuick
import Relay.Core

// Four-digit PIN entry. Modes:
//  verify  – enter the PIN to unlock (onOk runs after it's right)
//  set     – choose a new PIN (twice)
//  change  – current PIN, then the new one twice
//  remove  – current PIN, then the PIN is removed
// Digits type, Backspace deletes, Esc cancels. The fourth digit submits.
FocusScope {
    id: root
    property bool opened: false
    property string mode: "verify"
    property string reason
    property int step: 0            // which entry within the mode
    property string current         // entered so far
    property string oldPin
    property string firstNew
    property string error
    property var onOk: null
    property var onCancel: null
    signal finished()

    function open(m, why, ok, cancel) {
        mode = m
        reason = why || ""
        onOk = ok || null
        onCancel = cancel || null
        step = 0
        current = oldPin = firstNew = error = ""
        opened = true
        forceActiveFocus()
    }
    function close(success) {
        if (!opened) return
        opened = false
        const cb = success ? onOk : onCancel
        onOk = onCancel = null
        finished()
        if (cb) cb()
    }
    function prompt() {
        if (mode === "verify") return qsTr("Enter your PIN")
        if (mode === "remove") return qsTr("Enter your PIN to remove it")
        if (mode === "change" && step === 0) return qsTr("Enter your current PIN")
        const s = mode === "change" ? step - 1 : step
        return s === 0 ? qsTr("Choose a new 4-digit PIN") : qsTr("Enter the new PIN again")
    }
    function fail(text) {
        error = text
        current = ""
        shake.restart()
    }
    function submit() {
        const pin = current
        if (mode === "verify") {
            if (Parental.enterPin(pin)) close(true)
            else fail(Parental.retryWait() > 0 ? qsTr("Too many tries. Wait %1 s.").arg(Parental.retryWait())
                                               : qsTr("Wrong PIN"))
            return
        }
        if (mode === "remove") {
            if (Parental.removePin(pin)) close(true)
            else fail(Parental.retryWait() > 0 ? qsTr("Too many tries. Wait %1 s.").arg(Parental.retryWait())
                                               : qsTr("Wrong PIN"))
            return
        }
        if (mode === "change" && step === 0) {
            if (!Parental.enterPin(pin)) {
                fail(Parental.retryWait() > 0 ? qsTr("Too many tries. Wait %1 s.").arg(Parental.retryWait()) : qsTr("Wrong PIN"))
                return
            }
            oldPin = pin; current = ""; error = ""; step = 1
            return
        }
        const s = mode === "change" ? step - 1 : step
        if (s === 0) { firstNew = pin; current = ""; error = ""; step++; return }
        if (pin !== firstNew) { step = mode === "change" ? 1 : 0; firstNew = ""; fail(qsTr("The PINs didn't match. Try again.")); return }
        if (Parental.setPin(oldPin, pin)) close(true)
        else fail(qsTr("Couldn't save the PIN"))
    }
    function digit(d) {
        if (current.length >= 4) return
        error = ""
        current += d
        if (current.length === 4) Qt.callLater(submit)
    }

    visible: opened
    anchors.fill: parent

    Keys.onPressed: (e) => {
        if (e.key >= Qt.Key_0 && e.key <= Qt.Key_9) root.digit(String(e.key - Qt.Key_0))
        else if (e.key === Qt.Key_Backspace) root.current = root.current.slice(0, -1)
        else if (e.key === Qt.Key_Escape || e.key === Qt.Key_Back) root.close(false)
        else if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter) { if (root.current.length === 4) root.submit() }
        e.accepted = true   // keep keys from reaching the screen underneath
    }

    Rectangle {
        anchors.fill: parent
        color: "#C0000000"
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = true }
    }

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: 380
        height: col.implicitHeight + 48
        radius: 14
        color: Theme.panelSolid
        border.width: 1
        border.color: Theme.border

        SequentialAnimation {
            id: shake
            NumberAnimation { target: card; property: "anchors.horizontalCenterOffset"; to: -12; duration: 50 }
            NumberAnimation { target: card; property: "anchors.horizontalCenterOffset"; to: 12; duration: 70 }
            NumberAnimation { target: card; property: "anchors.horizontalCenterOffset"; to: 0; duration: 50 }
        }

        Column {
            id: col
            x: 24; y: 24
            width: parent.width - 48
            spacing: 16

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: "🔒"
                font.pixelSize: 28
                color: Theme.accent
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: root.prompt()
                color: Theme.text
                font.pixelSize: Theme.fontLarge
                font.weight: Font.DemiBold
            }
            Text {
                width: parent.width
                visible: text.length > 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: root.reason
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall + 1
            }
            Row {   // four dots
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 16
                Repeater {
                    model: 4
                    delegate: Rectangle {
                        width: 18; height: 18; radius: 9
                        color: index < root.current.length ? Theme.accent : "transparent"
                        border.width: 2
                        border.color: index < root.current.length ? Theme.accent : Theme.textMuted
                    }
                }
            }
            Text {
                width: parent.width
                height: 18
                horizontalAlignment: Text.AlignHCenter
                text: root.error
                color: Theme.danger
                font.pixelSize: Theme.fontSmall + 1
            }
            // On-screen keypad for mouse users
            Grid {
                anchors.horizontalCenter: parent.horizontalCenter
                columns: 3
                spacing: 8
                Repeater {
                    model: ["1", "2", "3", "4", "5", "6", "7", "8", "9", "⌫", "0", "✕"]
                    delegate: Rectangle {
                        width: 72; height: 44; radius: 8
                        color: keyMouse.pressed ? Theme.focus : Theme.raised
                        Text {
                            anchors.centerIn: parent
                            text: modelData
                            color: modelData === "✕" ? Theme.textSecondary : Theme.text
                            font.pixelSize: Theme.fontLarge
                        }
                        MouseArea {
                            id: keyMouse
                            anchors.fill: parent
                            onClicked: {
                                if (modelData === "⌫") root.current = root.current.slice(0, -1)
                                else if (modelData === "✕") root.close(false)
                                else root.digit(modelData)
                                root.forceActiveFocus()
                            }
                        }
                    }
                }
            }
        }
    }
}

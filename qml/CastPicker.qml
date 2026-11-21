import QtQuick
import QtQuick.Controls.Basic
import Relay.Core

// Cast to…: Chromecasts and smart TVs on the network. ↑ ↓ pick, Enter casts,
// A adds one by address, Esc closes.
FocusScope {
    id: root
    property bool opened: false
    property bool adding: false
    signal chosen(string deviceId)
    signal stopRequested()
    signal finished()

    function open() {
        opened = true
        adding = false
        Cast.scan()
        list.currentIndex = 0
        list.forceActiveFocus()
    }
    function close() {
        if (!opened) return
        opened = false
        finished()
    }
    function pick(i) {
        if (i === list.count - 1) { startAdding(); return }
        const d = Cast.devices[i]
        if (!d) return
        close()
        chosen(d.id)
    }
    function startAdding() {
        adding = true
        address.text = ""
        address.field.forceActiveFocus()
    }
    function add() {
        if (address.text.trim().length) Cast.addDevice(address.text)
        adding = false
        list.forceActiveFocus()
    }

    visible: opened
    anchors.fill: parent
    z: 26
    Keys.onEscapePressed: adding ? (adding = false, list.forceActiveFocus()) : close()

    Rectangle {
        anchors.fill: parent
        color: "#A0000000"
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onClicked: root.close(); onWheel: (w) => w.accepted = true }
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(520, parent.width - 48)
        height: col.implicitHeight + 48
        radius: 12
        color: Theme.panelSolid
        border.width: 1
        border.color: Theme.border
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }

        Column {
            id: col
            x: 24; y: 24
            width: parent.width - 48
            spacing: 14

            Item {
                width: parent.width
                height: 44
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 12
                    Icon { anchors.verticalCenter: parent.verticalCenter; width: 24; height: 24; path: Icons.cast; color: Theme.accent }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Cast to…")
                        color: Theme.text
                        font.pixelSize: Theme.fontLarge
                        font.weight: Font.DemiBold
                    }
                }
                CloseButton { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; onClicked: root.close() }
            }

            Rectangle {   // casting now
                visible: Cast.active
                width: parent.width
                height: 52
                radius: 8
                color: "#1FF2A33A"
                border.width: 1
                border.color: Theme.accent
                Text {
                    x: 14
                    width: parent.width - stopButton.width - 40
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Casting to %1").arg(Cast.deviceName)
                    color: Theme.text
                    font.pixelSize: Theme.fontBody - 1
                    elide: Text.ElideRight
                }
                ActionButton {
                    id: stopButton
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Stop casting")
                    onClicked: { root.close(); root.stopRequested() }
                }
            }

            ListView {
                id: list
                width: parent.width
                height: Math.min(contentHeight, 52 * 6)
                clip: true
                model: Cast.devices.length + 1
                boundsBehavior: Flickable.StopAtBounds
                highlightMoveDuration: 0
                Keys.onReturnPressed: root.pick(currentIndex)
                Keys.onEnterPressed: root.pick(currentIndex)
                Keys.onPressed: (e) => {
                    if (e.key === Qt.Key_A) { root.startAdding(); e.accepted = true }
                    else if (e.key === Qt.Key_Delete && currentIndex < Cast.devices.length) { Cast.forgetDevice(Cast.devices[currentIndex].id); e.accepted = true }
                    else if (e.key === Qt.Key_F5) { Cast.scan(); e.accepted = true }
                }
                delegate: Item {
                    width: list.width
                    height: 52
                    readonly property bool isAdd: index === Cast.devices.length
                    readonly property var dev: isAdd ? null : Cast.devices[index]
                    readonly property bool selected: ListView.isCurrentItem && list.activeFocus
                    Rectangle {
                        anchors.fill: parent
                        radius: 6
                        color: selected ? Theme.focus : (rowMouse.containsMouse ? Theme.raised : "transparent")
                    }
                    Icon {
                        x: 14
                        anchors.verticalCenter: parent.verticalCenter
                        width: 20; height: 20
                        path: isAdd ? Icons.plus : (dev.kind === "chromecast" ? Icons.cast : Icons.tv)
                        color: isAdd ? Theme.accent : (Cast.deviceId === (dev ? dev.id : "") && Cast.active ? Theme.accent : Theme.text)
                    }
                    Column {
                        x: 48
                        width: parent.width - 60
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        Text {
                            width: parent.width
                            text: isAdd ? qsTr("Add a device by address…") : dev.name
                            color: isAdd ? Theme.accent : Theme.text
                            font.pixelSize: Theme.fontBody
                            elide: Text.ElideRight
                        }
                        Text {
                            visible: !isAdd
                            width: parent.width
                            text: dev ? (dev.kind === "chromecast" ? qsTr("Chromecast") : qsTr("Smart TV / DLNA"))
                                        + (dev.model && dev.model !== "Chromecast" ? " · " + dev.model : "") + " · " + dev.address : ""
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            elide: Text.ElideRight
                        }
                    }
                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { list.currentIndex = index; list.forceActiveFocus(); root.pick(index) }
                    }
                }
            }

            Text {
                visible: !root.adding
                width: parent.width
                wrapMode: Text.Wrap
                text: Cast.scanning ? qsTr("Looking for Chromecasts and smart TVs on your network…")
                      : Cast.devices.length === 0
                        ? qsTr("Nothing found. The TV or Chromecast must be on and on the same network as this computer. If Windows asked whether Relay may use the network, allow private networks.")
                        : qsTr("F5 looks again · A adds a device by address · Delete takes one off the list")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
            }

            FieldInput {
                id: address
                visible: root.adding
                width: parent.width
                label: qsTr("Chromecast IP address, or a TV's description address")
                placeholder: "192.168.1.20"
                onAccepted: root.add()
            }
            Row {
                visible: root.adding
                spacing: 10
                ActionButton { text: qsTr("Add"); primary: true; onClicked: root.add() }
                ActionButton { text: qsTr("Cancel"); onClicked: { root.adding = false; list.forceActiveFocus() } }
            }
        }
    }
}

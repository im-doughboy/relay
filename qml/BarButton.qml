import QtQuick
import QtQuick.Window

// A button for the mouse: icon, optional label, and the keyboard shortcut
// that does the same thing (shown as a key, so the keys can be learned).
// Without a label, hovering shows the name and key in a tip.
Rectangle {
    id: root
    property string icon
    property bool iconFilled: false
    property string label
    property string shortcut
    property bool showLabel: true
    property bool showShortcut: true
    property bool active: false          // on (captions on, full screen…)
    property bool accent: false          // the main action of a group
    property int size: 40
    signal clicked()

    readonly property bool hovered: mouse.containsMouse

    implicitHeight: size
    implicitWidth: labelText.visible ? content.implicitWidth + 20 : size
    radius: size / 2
    color: accent ? (mouse.pressed ? Qt.darker(Theme.accent, 1.2) : (hovered ? Qt.lighter(Theme.accent, 1.08) : Theme.accent))
                  : mouse.pressed ? Theme.focus : (hovered ? "#2EFFFFFF" : (active ? "#1FF2A33A" : "transparent"))
    border.width: !accent && (hovered || active) ? 1 : 0
    border.color: active ? Theme.accent : "#33FFFFFF"
    Behavior on color { ColorAnimation { duration: Theme.animFast } }
    // Not available right now (e.g. forward while already live).
    opacity: enabled ? 1 : 0.35

    Row {
        id: content
        anchors.centerIn: parent
        spacing: 8
        Icon {
            id: icon_
            anchors.verticalCenter: parent.verticalCenter
            width: root.size >= 40 ? 20 : 18
            height: width
            path: root.icon
            filled: root.iconFilled
            visible: root.icon.length > 0
            color: root.accent ? Theme.accentText : (root.active ? Theme.accent : Theme.text)
        }
        Text {
            id: labelText
            anchors.verticalCenter: parent.verticalCenter
            visible: root.showLabel && root.label.length > 0
            text: root.label
            color: root.accent ? Theme.accentText : Theme.text
            font.pixelSize: Theme.fontBody - 1
            font.weight: root.accent ? Font.DemiBold : Font.Medium
        }
        KeyCap {
            anchors.verticalCenter: parent.verticalCenter
            visible: root.showLabel && root.showShortcut && root.shortcut.length > 0 && root.label.length > 0
            text: root.shortcut
            dark: root.accent
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        acceptedButtons: Qt.LeftButton
        onClicked: root.clicked()
    }

    // Name and key for icon-only buttons.
    Rectangle {
        id: tip
        visible: tipTimer.done && root.hovered && !labelText.visible && (root.label.length > 0)
        z: 100
        // Centred under the button, but kept inside the window.
        readonly property real buttonX: visible ? root.mapToItem(null, 0, 0).x : 0
        x: Math.max(8 - buttonX, Math.min((root.width - width) / 2, root.Window.width - buttonX - width - 8))
        y: parent.height + 8
        width: tipRow.implicitWidth + 20
        height: 32
        radius: 6
        color: Theme.panelSolid
        border.width: 1
        border.color: Theme.border
        Row {
            id: tipRow
            anchors.centerIn: parent
            spacing: 8
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.label
                color: Theme.text
                font.pixelSize: Theme.fontSmall + 1
            }
            KeyCap {
                anchors.verticalCenter: parent.verticalCenter
                visible: root.shortcut.length > 0
                text: root.shortcut
            }
        }
    }
    Timer {
        id: tipTimer
        property bool done: false
        interval: 350
        running: root.hovered
        onRunningChanged: if (running) done = false
        onTriggered: done = true
    }
}

import QtQuick

// A rounded button for the Movies & series pages: light when it has the
// remote's or keyboard's focus (`lit`), quiet glass otherwise. Optional
// icon; `progress` (0-1) draws how much is watched along its bottom.
Rectangle {
    id: root
    property string icon
    property bool iconFilled: false
    property string text
    property bool lit: false             // selected by keys / controller / phone
    property real progress: -1
    property int fontSize: Theme.fontBody
    property bool wide: false            // fill the width given (a list of actions), text on the left
    signal clicked()

    readonly property bool hovered: mouse.containsMouse
    readonly property bool bright: lit || (hovered && !wide)
    implicitHeight: Math.round(fontSize * 2.5)
    implicitWidth: content.implicitWidth + (text.length ? 2 * Math.round(fontSize * 1.4) : implicitHeight)
    radius: height / 2
    color: bright ? Theme.text : (wide ? (hovered ? "#22FFFFFF" : "transparent") : "#3DFFFFFF")
    Behavior on color { ColorAnimation { duration: Theme.animFast } }
    scale: mouse.pressed ? 0.97 : 1

    Row {
        id: content
        x: root.wide ? Math.round(root.fontSize * 1.1) : Math.round((root.width - implicitWidth) / 2)
        anchors.verticalCenter: parent.verticalCenter
        spacing: Math.round(root.fontSize * 0.7)
        Icon {
            visible: root.icon.length > 0
            anchors.verticalCenter: parent.verticalCenter
            width: Math.round(root.fontSize * 1.25)
            height: width
            path: root.icon
            filled: root.iconFilled
            color: root.bright ? Theme.background : Theme.text
        }
        Text {
            visible: root.text.length > 0
            anchors.verticalCenter: parent.verticalCenter
            text: root.text
            color: root.bright ? Theme.background : (root.enabled ? Theme.text : Theme.textMuted)
            font.pixelSize: root.fontSize
            font.weight: root.bright ? Font.DemiBold : Font.Medium
        }
    }
    // Watched so far (Resume).
    Rectangle {
        visible: root.progress > 0
        anchors.right: parent.right
        anchors.rightMargin: Math.round(root.fontSize * 1.2)
        anchors.verticalCenter: parent.verticalCenter
        width: Math.round(root.fontSize * 3.5)
        height: 4
        radius: 2
        color: root.bright ? "#33000000" : "#40FFFFFF"
        Rectangle { width: parent.width * Math.min(1, root.progress); height: parent.height; radius: 2; color: Theme.accent }
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: if (root.enabled) root.clicked()
    }
}

import QtQuick

// Short-lived message at the top of the screen (volume, errors, hints).
Rectangle {
    id: root
    property string message
    property bool warning: false

    function show(msg, isWarning) {
        message = msg
        warning = !!isWarning
        opacity = 1
        hideTimer.restart()
    }

    anchors.horizontalCenter: parent.horizontalCenter
    y: 28
    width: label.implicitWidth + 40
    height: 44
    radius: 22
    color: Theme.panel
    border.width: 1
    border.color: warning ? Theme.danger : Theme.border
    opacity: 0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: Theme.animNormal } }

    Text {
        id: label
        anchors.centerIn: parent
        text: root.message
        color: root.warning ? Theme.danger : Theme.text
        font.pixelSize: Theme.fontBody
    }

    Timer { id: hideTimer; interval: 2600; onTriggered: root.opacity = 0 }
}

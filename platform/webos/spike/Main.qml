import QtQuick
import QtQuick.Controls.Basic

// The spike's test screen: Qt Quick drawn over the TV's own video. Keys are
// the ones Relay handles on Windows: Back arrives as Escape, OK as Return.
FocusScope {
    id: root

    Keys.onEscapePressed: spike.quit()

    readonly property color panel: "#d9101418"
    readonly property color accent: "#4f8cff"

    Rectangle {
        x: 60; y: 50
        width: title.implicitWidth + 56; height: 76
        radius: 18
        color: root.panel
        Text {
            id: title
            anchors.centerIn: parent
            text: "Relay · Qt Quick over the TV's video"
            color: "white"
            font.pixelSize: 34
            font.weight: Font.DemiBold
        }
    }

    // Font check: Inter for Latin, the TV's own fonts for everything else.
    Rectangle {
        x: 660; y: 170
        width: fontCheck.implicitWidth + 48; height: fontCheck.implicitHeight + 32
        radius: 22
        color: root.panel
        Column {
            id: fontCheck
            x: 24; y: 16
            spacing: 6
            Text { text: "Inter Regular · Medium · Bold"; color: "white"; font.pixelSize: 30 }
            Text { text: "Ελληνικά · Русский · العربية · 日本語 · 한국어 · हिन्दी"; color: "white"; font.pixelSize: 30 }
            Text { text: spike.fontInfo; color: "#c0ffffff"; font.pixelSize: 22 }
        }
    }

    // Turns smoothly when frames arrive on time.
    Rectangle {
        anchors { right: parent.right; top: parent.top; margins: 60 }
        width: 72; height: 72; radius: 16
        color: root.accent
        RotationAnimation on rotation { from: 0; to: 360; duration: 2000; loops: Animation.Infinite }
    }

    Rectangle {
        x: 60; y: 170
        width: 560; height: list.contentHeight + 40
        radius: 22
        color: root.panel

        ListView {
            id: list
            anchors { fill: parent; margins: 20 }
            focus: true
            interactive: false            // no dragging with the pointer...
            keyNavigationEnabled: true    // ...but the remote's arrows still move it
            spacing: 8
            model: ["Animal Planet", "Discovery Channel", "News", "Sports"]
            highlight: Rectangle { radius: 14; color: root.accent }
            highlightMoveDuration: 120
            Keys.onPressed: (event) => { console.log("list got key", event.key); event.accepted = false }
            onCurrentIndexChanged: console.log("list current", currentIndex)
            delegate: Item {
                id: row
                required property string modelData
                required property int index
                width: ListView.view.width
                height: 84
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    x: 26
                    text: row.modelData
                    color: "white"
                    font.pixelSize: 36
                }
                // The pointer: pointing moves the highlight, clicking picks.
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    onEntered: list.currentIndex = row.index
                    onClicked: { list.currentIndex = row.index; console.log("clicked", row.modelData) }
                }
            }
        }
    }

    Button {
        x: 60; y: 640
        text: "Controls: Basic style"
        font.pixelSize: 28
        focusPolicy: Qt.NoFocus
    }

    Rectangle {
        x: 60
        width: parent.width - 120; height: 96
        y: parent.height - height - 50
        radius: 22
        color: spike.state === 2 ? "#d91e7a46" : spike.state === 3 ? "#d9b03030" : "#d9283c8c"
        Behavior on color { ColorAnimation { duration: 200 } }

        Text {
            anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 30; right: fps.left; rightMargin: 30 }
            text: spike.status + (spike.detail.length ? "  ·  " + spike.detail : "")
            color: "white"
            font.pixelSize: 32
            elide: Text.ElideRight
        }
        Text {
            id: fps
            anchors { verticalCenter: parent.verticalCenter; right: parent.right; rightMargin: 30 }
            text: Math.round(spike.fps) + " fps"
            color: "#e0ffffff"
            font.pixelSize: 28
        }
    }
}

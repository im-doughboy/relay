import QtQuick
import Relay.Core

// A movie or series poster (2:3) with a lettered fallback, progress bar,
// watched tick and favorite star.
Item {
    id: root
    property string source
    property string title
    property real progress: -1     // 0..1, or -1 for none
    property bool watched: false
    property bool favorite: false
    property bool selected: false

    Rectangle {
        id: frame
        anchors.fill: parent
        radius: 8
        color: Theme.raised
        clip: true
        border.width: root.selected ? 3 : 0
        border.color: Theme.accent

        // Lettered fallback while loading or when there's no poster.
        Column {
            anchors.centerIn: parent
            width: parent.width - 20
            spacing: 6
            visible: img.status !== Image.Ready
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: root.title.replace(/^[A-Z]{2,3}\s*[-|:]\s*/, "").substring(0, 1).toUpperCase()
                color: Theme.textMuted
                font.pixelSize: Math.round(root.height * 0.22)
                font.weight: Font.DemiBold
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                maximumLineCount: 3
                elide: Text.ElideRight
                text: root.title
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
            }
        }

        Image {
            id: img
            anchors.fill: parent
            anchors.margins: root.selected ? 3 : 0
            // On a TV, at the size shown (TMDB has posters at 220 and 300
            // wide too): a fraction of the download and the decoding.
            source: Settings.tv ? Vod.picture(root.source, root.width > 220 ? 300 : 220) : root.source
            asynchronous: true
            cache: true
            fillMode: Image.PreserveAspectCrop
            sourceSize.width: 300
            sourceSize.height: 450
            smooth: true
        }

        Rectangle {   // progress
            visible: root.progress >= 0
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: root.selected ? 3 : 0
            height: 5
            color: "#B0000000"
            Rectangle {
                width: parent.width * Math.max(0.03, root.progress)
                height: parent.height
                color: Theme.accent
            }
        }

        Rectangle {   // watched
            visible: root.watched
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 8
            width: 26; height: 26
            radius: 13
            color: Theme.accent
            Text {
                anchors.centerIn: parent
                text: "✓"
                color: Theme.accentText
                font.pixelSize: 15
                font.weight: Font.Bold
            }
        }

        Rectangle {   // favorite
            visible: root.favorite
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.margins: 8
            width: 26; height: 26
            radius: 13
            color: "#C0000000"
            Text {
                anchors.centerIn: parent
                text: "★"
                color: Theme.accent
                font.pixelSize: 15
            }
        }
    }

    scale: root.selected ? 1.04 : 1
    Behavior on scale { NumberAnimation { duration: Theme.animFast } }
}

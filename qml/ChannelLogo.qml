import QtQuick

// Channel logo with a lettered fallback when there's no logo or it fails to load.
Rectangle {
    id: root
    property string source
    property string name

    radius: 6
    color: Theme.raised
    clip: true

    Image {
        id: img
        anchors.fill: parent
        anchors.margins: 4
        source: root.source
        asynchronous: true
        cache: true
        fillMode: Image.PreserveAspectFit
        sourceSize.width: 160
        sourceSize.height: 160
        smooth: true
    }

    Text {
        anchors.centerIn: parent
        visible: img.status !== Image.Ready
        text: {
            const words = root.name.replace(/[^A-Za-z0-9 ]/g, " ").trim().split(/\s+/)
            if (!words.length || !words[0]) return "TV"
            return (words.length > 1 ? words[0][0] + words[1][0] : words[0].substring(0, 2)).toUpperCase()
        }
        color: Theme.textSecondary
        font.pixelSize: Math.round(root.height * 0.36)
        font.weight: Font.DemiBold
    }
}

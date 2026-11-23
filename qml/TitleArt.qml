import QtQuick

// A title as on a poster: its logo when TMDB has one, else its name in large type.
Item {
    id: root
    property var item: null
    property var info: null
    property real maxWidth: 400
    property real maxHeight: 120
    property int fontSize: 40
    readonly property string logo: info && info.logo ? info.logo : ""
    readonly property bool showLogo: logoImage.status === Image.Ready
    implicitWidth: showLogo ? logoImage.paintedWidth : Math.min(maxWidth, name.implicitWidth)
    implicitHeight: showLogo ? logoImage.paintedHeight : name.implicitHeight
    width: implicitWidth
    height: implicitHeight

    Image {
        id: logoImage
        width: root.maxWidth
        height: root.maxHeight
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        source: root.logo
        fillMode: Image.PreserveAspectFit
        horizontalAlignment: Image.AlignLeft
        verticalAlignment: Image.AlignBottom
        asynchronous: true
        sourceSize.width: 600
        opacity: status === Image.Ready ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 250 } }
    }
    Text {
        id: name
        visible: !root.showLogo
        width: root.maxWidth
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        text: root.item ? root.item.title || "" : (root.info ? root.info.title || "" : "")
        color: Theme.text
        font.pixelSize: root.fontSize
        font.weight: Font.Black
        font.letterSpacing: -0.5
        lineHeight: 0.92
        wrapMode: Text.Wrap
        maximumLineCount: 2
        elide: Text.ElideRight
        style: Text.Raised
        styleColor: "#70000000"
    }
}

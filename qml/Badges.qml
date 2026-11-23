import QtQuick

// Small labels on a picture: "Recently added", "Top rated"…
Row {
    id: root
    property var list: []
    property int fontSize: 13
    spacing: Math.round(fontSize * 0.6)
    layoutDirection: Qt.RightToLeft
    Repeater {
        model: root.list
        delegate: Rectangle {
            required property var modelData
            height: Math.round(root.fontSize * 2.1)
            width: label.implicitWidth + Math.round(root.fontSize * 1.4)
            radius: 6
            color: "#B0101010"
            Text {
                id: label
                anchors.centerIn: parent
                text: parent.modelData
                color: Theme.text
                font.pixelSize: root.fontSize
                font.weight: Font.DemiBold
            }
        }
    }
}

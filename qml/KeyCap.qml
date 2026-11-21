import QtQuick

// A keyboard key, e.g. "G" or "Esc", for showing shortcuts.
Rectangle {
    id: root
    property string text
    property bool dark: false     // on an amber button
    implicitWidth: Math.max(22, label.implicitWidth + 12)
    implicitHeight: 22
    radius: 5
    color: dark ? "#26000000" : "#14FFFFFF"
    border.width: 1
    border.color: dark ? "#40000000" : "#33FFFFFF"
    Text {
        id: label
        anchors.centerIn: parent
        text: root.text
        color: root.dark ? Theme.accentText : Theme.textSecondary
        font.pixelSize: Theme.fontSmall - 1
        font.weight: Font.DemiBold
    }
}

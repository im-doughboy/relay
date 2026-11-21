import QtQuick
import QtQuick.Controls.Basic

// Labeled text field in Relay's style.
Column {
    id: root
    property alias text: field.text
    property alias placeholder: field.placeholderText
    property alias echoMode: field.echoMode
    property alias field: field
    property string label
    signal accepted()
    // Every key, before the text box uses it (set e.accepted to keep it).
    signal keyPressed(var event)

    spacing: 6
    width: parent ? parent.width : 400

    Text {
        text: root.label
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSmall
        font.weight: Font.Medium
    }

    TextField {
        id: field
        width: root.width
        height: 46
        color: Theme.text
        placeholderTextColor: Theme.textMuted
        selectionColor: Theme.accent
        selectedTextColor: Theme.accentText
        font.pixelSize: Theme.fontBody
        leftPadding: 14
        rightPadding: 14
        selectByMouse: true
        onAccepted: root.accepted()
        Keys.onPressed: (e) => root.keyPressed(e)
        background: Rectangle {
            radius: Theme.radius
            color: Theme.raised
            border.width: field.activeFocus ? 2 : 1
            border.color: field.activeFocus ? Theme.accent : Theme.border
        }
    }
}

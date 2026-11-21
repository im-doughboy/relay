import QtQuick
import QtQuick.Controls.Basic

// Primary (amber) or quiet button; keyboard focus shows the same ring as fields.
Button {
    id: control
    property bool primary: false
    property bool busy: false

    implicitHeight: 46
    implicitWidth: Math.max(120, contentItem.implicitWidth + 36)
    font.pixelSize: Theme.fontBody
    font.weight: Font.Medium
    focusPolicy: Qt.StrongFocus

    Keys.onReturnPressed: clicked()
    Keys.onEnterPressed: clicked()

    contentItem: Text {
        text: control.busy ? qsTr("Loading…") : control.text
        font: control.font
        color: control.primary ? Theme.accentText
                               : (control.enabled ? Theme.text : Theme.textMuted)
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        radius: Theme.radius
        color: control.primary
               ? (control.down ? Qt.darker(Theme.accent, 1.15) : Theme.accent)
               : (control.down || control.hovered ? Theme.focus : Theme.raised)
        opacity: control.enabled ? 1 : 0.5
        border.width: control.activeFocus ? 2 : (control.primary ? 0 : 1)
        border.color: control.activeFocus ? (control.primary ? Theme.text : Theme.accent) : Theme.border
    }
}

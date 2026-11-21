import QtQuick
import Relay.Core

// Small "rename" box. Enter saves, Esc cancels; saving an empty name
// restores the original one.
FocusScope {
    id: root
    property bool opened: false
    property string title
    property string original
    // Text under the field; empty = the usual "Original: … · leave empty to restore it".
    property string note
    signal saved(string name)
    signal finished()

    function open(heading, current, originalName, noteText) {
        title = heading
        original = originalName
        note = noteText || ""
        input.text = current
        opened = true
        input.field.forceActiveFocus()
        input.field.selectAll()
    }
    function close() {
        if (!opened) return
        opened = false
        finished()
    }
    function save() {
        saved(input.text.trim())
        close()
    }

    visible: opened
    anchors.fill: parent
    Keys.onEscapePressed: close()

    Rectangle {
        anchors.fill: parent
        color: "#A0000000"
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onClicked: root.close()
            onWheel: (w) => w.accepted = true
        }
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(520, parent.width - 48)
        height: col.implicitHeight + 48
        radius: 12
        color: Theme.panelSolid
        border.width: 1
        border.color: Theme.border
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }

        Column {
            id: col
            x: 24
            y: 24
            width: parent.width - 48
            spacing: 16

            Text {
                width: parent.width
                text: root.title
                color: Theme.text
                font.pixelSize: Theme.fontLarge
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            FieldInput {
                id: input
                width: parent.width
                label: qsTr("Name")
                placeholder: root.original
                onAccepted: root.save()
            }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                visible: text.trim().length > 0
                text: root.note.length ? root.note : qsTr("Original: %1 · leave empty to restore it").arg(root.original)
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
            }
            Row {
                spacing: 10
                ActionButton { text: qsTr("Save"); primary: true; onClicked: root.save() }
                ActionButton { text: qsTr("Cancel"); onClicked: root.close() }
            }
        }
    }
}

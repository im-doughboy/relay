import QtQuick
import Relay.Core

// The phone remote's address as a QR code, with the pairing code beside it
// (Settings, and the first screen on a TV).
Row {
    id: root
    // Shown under the code: what to allow, where to look.
    property string note
    spacing: 20
    topPadding: 6

    // Scan with the phone's camera.
    QrCode {
        visible: Phone.url.length > 0
        text: Phone.url
    }
    Column {
        spacing: 6
        width: root.width - 172
        anchors.verticalCenter: parent.verticalCenter
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: Phone.url.length ? qsTr("Scan the code with the phone's camera, or open %1 in its browser.").arg(Phone.url)
                                   : Settings.tv ? qsTr("This TV isn't on a home network right now.")
                                                 : qsTr("This computer isn't on a home network right now.")
            color: Theme.text
            font.pixelSize: Theme.fontBody - 1
        }
        Text {
            text: qsTr("Pairing code")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
        }
        Text {
            text: Phone.code.split("").join(" ")
            color: Theme.accent
            font.pixelSize: Theme.fontLarge + 8
            font.weight: Font.DemiBold
            font.family: "monospace"
        }
        Text {
            visible: root.note.length > 0
            width: parent.width
            wrapMode: Text.Wrap
            text: root.note
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
        }
    }
}

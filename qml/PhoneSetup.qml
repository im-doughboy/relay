import QtQuick
import Relay.Core

// Before the first playlist: set Relay up from a phone. The QR code opens
// Relay's page on the phone already paired (its key works once, for 15
// minutes); a computer opens the address and enters the code.
Column {
    id: root
    spacing: 14

    Text {
        width: parent.width
        wrapMode: Text.Wrap
        text: qsTr("Set it up from your phone")
        color: Theme.text
        font.pixelSize: Theme.fontLarge
        font.weight: Font.DemiBold
    }
    Text {
        width: parent.width
        wrapMode: Text.Wrap
        text: Phone.setupUrl.length
              ? qsTr("Scan the code with your phone's camera. Relay's page opens on the phone, already paired: what you type there goes into the box with the orange border.")
              : Settings.tv ? qsTr("This TV isn't on a home network right now.")
                            : qsTr("This computer isn't on a home network right now.")
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSmall
        lineHeight: 1.15
    }
    QrCode {
        visible: Phone.setupUrl.length > 0
        width: Math.min(root.width, 220); height: width
        text: Phone.setupUrl
    }
    Text {
        visible: Phone.url.length > 0
        width: parent.width
        wrapMode: Text.Wrap
        textFormat: Text.StyledText
        text: qsTr("On a computer, open <b>%1</b> and enter the code <b>%2</b>.").arg(Phone.url).arg(Phone.code)
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSmall
        lineHeight: 1.15
    }
    Text {
        visible: Phone.url.length > 0
        width: parent.width
        wrapMode: Text.Wrap
        text: qsTr("Only devices on your home network can open it. The QR code pairs one phone and changes every 15 minutes.")
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall - 1
        lineHeight: 1.15
    }
}

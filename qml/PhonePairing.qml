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
    Rectangle {
        width: 152; height: 152
        radius: 8
        color: "white"
        visible: Phone.url.length > 0
        Canvas {
            anchors.fill: parent
            anchors.margins: 10
            property var code: Phone.url.length ? Phone.qr(Phone.url) : null
            onCodeChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                if (!code) return
                const n = code.size, cell = width / n
                ctx.fillStyle = "#000000"
                for (let y = 0; y < n; ++y)
                    for (let x = 0; x < n; ++x)
                        if (code.modules[y * n + x] === "1")
                            ctx.fillRect(Math.floor(x * cell), Math.floor(y * cell), Math.ceil(cell), Math.ceil(cell))
            }
        }
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

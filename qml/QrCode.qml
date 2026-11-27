import QtQuick
import Relay.Core

// A QR code for `text`, black on a white card with a quiet border.
Rectangle {
    id: root
    property string text
    width: 152; height: 152
    radius: 8
    color: "white"
    Canvas {
        anchors.fill: parent
        anchors.margins: Math.round(root.width * 0.065)
        property var code: root.text.length ? Phone.qr(root.text) : null
        onCodeChanged: requestPaint()
        onWidthChanged: requestPaint()
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

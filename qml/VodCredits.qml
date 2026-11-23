import QtQuick
import Relay.Core

// Credits & more info for a title: the sections on the left (Director,
// Writers, Cast, Genres, Age rating…), all of them on the right; the one
// selected on the left is bright. ↑ ↓ move, Esc / Back closes.
FocusScope {
    id: root
    property bool opened: false
    property var item: null
    property var info: ({})
    signal closed()

    readonly property var sections: {
        const i = info || ({})
        const out = []
        const list = (s) => String(s || "").split(",").map(x => x.trim()).filter(x => x.length)
        if (i.director) out.push({ title: item && item.kind === "series" ? qsTr("Created by") : qsTr("Director"), lines: list(i.director) })
        if (i.writer) out.push({ title: qsTr("Writers"), lines: list(i.writer) })
        if (i.cast) out.push({ title: qsTr("Cast"), lines: list(i.cast) })
        if (i.genre) out.push({ title: qsTr("Genres"), lines: list(i.genre) })
        if (i.certification) out.push({ title: qsTr("Age rating"), lines: [i.certification] })
        const facts = []
        if (i.year) facts.push(qsTr("Year: %1").arg(i.year))
        if (i.duration) facts.push(qsTr("Length: %1").arg(i.duration))
        if (i.rating > 0) facts.push(qsTr("Rating: ★ %1").arg(Number(i.rating).toFixed(1)))
        if (facts.length) out.push({ title: qsTr("About"), lines: facts })
        return out
    }
    property int at: 0

    function open(it, inf) {
        item = it
        info = inf || ({})
        at = 0
        opened = true
        forceActiveFocus()
    }
    function close() {
        if (!opened) return
        opened = false
        closed()
    }

    visible: opened
    Keys.onPressed: (e) => {
        switch (e.key) {
        case Qt.Key_Up: at = Math.max(0, at - 1); break
        case Qt.Key_Down: at = Math.min(sections.length - 1, at + 1); break
        case Qt.Key_Escape:
        case Qt.Key_Back:
        case Qt.Key_Backspace:
        case Qt.Key_Left: close(); break
        default: return
        }
        e.accepted = true
    }

    Rectangle {
        anchors.fill: parent
        color: "black"
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = true }
    }

    readonly property real side: Math.round(Math.max(32, width * 0.035))
    readonly property int font1: Math.round(Math.max(Theme.fontBody, Math.min(24, height * 0.026)))

    Column {
        id: sideCol
        x: root.side
        y: Math.round(root.height * 0.08)
        width: Math.min(root.width * 0.3, 420)
        spacing: Math.round(root.font1 * 0.6)
        TitleArt {
            item: root.item
            info: root.info
            maxWidth: sideCol.width * 0.8
            maxHeight: root.height * 0.14
            fontSize: Math.round(root.font1 * 2)
        }
        Text {
            text: [root.info.year, root.info.duration, root.info.certification].filter(x => x).join("  •  ")
            color: Theme.textSecondary
            font.pixelSize: root.font1 - 2
        }
        Item { width: 1; height: root.font1 * 1.5 }
        Repeater {
            model: root.sections
            delegate: PillButton {
                required property var modelData
                required property int index
                width: sideCol.width * 0.85
                wide: true
                text: modelData.title
                fontSize: root.font1 - 1
                lit: root.activeFocus && root.at === index
                onClicked: { root.at = index; root.forceActiveFocus() }
            }
        }
    }

    Flickable {
        id: right
        anchors.left: sideCol.right
        anchors.leftMargin: root.side * 2
        anchors.right: parent.right
        anchors.rightMargin: root.side
        y: sideCol.y
        height: root.height - y - root.side
        contentHeight: content.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        Behavior on contentY { NumberAnimation { duration: 200 } }
        Column {
            id: content
            width: right.width
            spacing: Math.round(root.font1 * 1.3)
            Repeater {
                id: blocks
                model: root.sections
                delegate: Column {
                    required property var modelData
                    required property int index
                    width: content.width
                    spacing: 6
                    opacity: index === root.at ? 1 : 0.55
                    Behavior on opacity { NumberAnimation { duration: 150 } }
                    onYChanged: if (index === root.at) right.follow(this)
                    Text {
                        text: modelData.title
                        color: Theme.textSecondary
                        font.pixelSize: root.font1 - 2
                        font.weight: Font.DemiBold
                    }
                    Repeater {
                        model: modelData.lines
                        delegate: Text {
                            required property var modelData
                            width: content.width
                            text: modelData
                            color: Theme.text
                            font.pixelSize: root.font1
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }
        }
        function follow(block) {
            if (!block) return
            const top = block.y, bottom = block.y + block.height
            if (top < contentY) contentY = top
            else if (bottom > contentY + height) contentY = Math.min(contentHeight - height, bottom - height)
        }
    }
    onAtChanged: right.follow(blocks.itemAt(at))

    CloseButton {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 24
        label: qsTr("Back")
        onClicked: root.close()
    }
}

import QtQuick
import QtQuick.Controls.Basic
import Relay.Core

// Slide-in channel list over the video: groups on the left, channels on the
// right. Arrow keys move, Left/Right switch columns, Enter tunes, Esc closes.
// F toggles a favorite, H hides, E renames and Ctrl+↑/↓ moves the selected
// channel (or group, in the groups column); right-click offers the same as a
// menu. / opens search.
FocusScope {
    id: root
    property bool opened: false
    property bool syncingGroups: false
    signal closed()
    signal message(string text)
    signal searchRequested()
    signal multiViewRequested(int globalIndex)
    signal pinRequested(string reason, var onOk)

    function open() {
        Channels.refreshRecent()
        opened = true
        groupList.currentIndex = Channels.currentGroup
        groupList.positionViewAtIndex(groupList.currentIndex, ListView.Contain)
        syncChannelRow()
        channelList.forceActiveFocus()
    }
    function close() {
        if (!opened) return
        opened = false
        closed()
    }
    function syncChannelRow() {
        const row = Channels.rowOf(Player.currentIndex)
        channelList.currentIndex = row >= 0 ? row : 0
        channelList.positionViewAtIndex(channelList.currentIndex, ListView.Center)
    }
    function tuneRow(row) {
        const idx = Channels.globalIndexAt(row)
        if (idx < 0) return
        Player.tune(idx, Channels.currentGroup)
        close()
    }
    // Selecting a group is always explicit (keys or mouse). The list itself
    // resets its selection whenever the group list changes (after hiding,
    // renaming or moving a group), so that must never switch groups.
    function selectGroup(i) {
        if (i < 0 || i >= groupList.count) return
        syncingGroups = true
        groupList.currentIndex = i
        syncingGroups = false
        groupList.positionViewAtIndex(i, ListView.Contain)
        Channels.currentGroup = i
    }
    function syncGroupList() {
        syncingGroups = true
        groupList.currentIndex = Channels.currentGroup
        groupList.positionViewAtIndex(groupList.currentIndex, ListView.Contain)
        syncingGroups = false
    }
    function toggleFavoriteRow(row) {
        const idx = Channels.globalIndexAt(row)
        if (idx < 0) return
        const fav = Channels.toggleFavorite(idx)
        if (Channels.currentGroup === Channels.favoritesGroup)
            channelList.currentIndex = Math.min(row, channelList.count - 1)
        message(fav ? qsTr("Added to Favorites") : qsTr("Removed from Favorites"))
    }
    function hideRow(row) {
        const idx = Channels.globalIndexAt(row)
        if (idx < 0) return
        Channels.hideChannel(idx)
        channelList.currentIndex = Math.min(row, channelList.count - 1)
        channelList.positionViewAtIndex(channelList.currentIndex, ListView.Contain)
        message(qsTr("Channel hidden · unhide it in Playlist settings (S)"))
    }
    // The user's own favorites lists.
    function listsRow(row) {
        const idx = Channels.globalIndexAt(row)
        if (idx < 0) return
        listPicker.open(idx, Channels.info(idx).name)
    }
    function removeFromListRow(row) {
        if (!Channels.isListGroup(Channels.currentGroup)) return
        const idx = Channels.globalIndexAt(row)
        if (idx < 0) return
        const label = Channels.groupLabel(Channels.currentGroup)
        Channels.setInList(idx, Channels.groupName(Channels.currentGroup), false)
        channelList.currentIndex = Math.max(0, Math.min(row, channelList.count - 1))
        message(qsTr("Removed from %1").arg(label))
    }
    function newList() {
        renameTarget = { kind: "newlist" }
        renameDialog.open(qsTr("New favorites list"), "", qsTr("Sports, Kids, News…"), qsTr("Add channels to it with Shift+F, or right-click a channel."))
    }
    property int deleteArmed: -1
    Timer { id: deleteTimer; interval: 4000; onTriggered: root.deleteArmed = -1 }
    function deleteListAt(index) {
        if (!Channels.isListGroup(index)) { message(qsTr("Only your own lists can be deleted · H hides a playlist group")); return }
        const label = Channels.groupLabel(index)
        if (deleteArmed !== index) {
            deleteArmed = index
            deleteTimer.restart()
            message(qsTr("Press Delete again to delete the list \"%1\" (its channels stay in the playlist)").arg(label))
            return
        }
        deleteArmed = -1
        Channels.deleteList(index)
        message(qsTr("List \"%1\" deleted").arg(label))
    }
    function hideGroupAt(index) {
        if (index <= Channels.allGroup) return   // Favorites and All channels can't be hidden
        if (Channels.isListGroup(index)) { message(qsTr("Press Delete to delete one of your lists")); return }
        const label = Channels.groupLabel(index)
        Channels.hideGroup(Channels.groupName(index))
        message(qsTr("Group \"%1\" hidden · unhide it in Playlist settings (S)").arg(label))
    }
    property var renameTarget: null
    function renameRow(row) {
        const idx = Channels.globalIndexAt(row)
        if (idx < 0) return
        renameTarget = { kind: "channel", index: idx, row: row }
        renameDialog.open(qsTr("Rename channel"), channelList.currentItem ? channelList.currentItem.channelName : "",
                          Channels.originalChannelName(idx))
    }
    function guideRow(row) {
        const idx = Channels.globalIndexAt(row)
        if (idx < 0) return
        guidePicker.open(idx, Channels.info(idx).name)
    }
    function renameGroupAt(index) {
        if (!Channels.canEditGroup(index)) {
            message(qsTr("Favorites and All channels can't be renamed"))
            return
        }
        renameTarget = { kind: "group", index: index }
        if (Channels.isListGroup(index))
            renameDialog.open(qsTr("Rename list"), Channels.groupLabel(index), Channels.groupLabel(index), " ")
        else
            renameDialog.open(qsTr("Rename group"), Channels.groupLabel(index), Channels.groupName(index))
    }
    function moveRow(row, delta) {
        if (!Channels.canMoveChannels()) {
            message(Channels.currentGroup === Channels.recentGroup
                    ? qsTr("Recently watched is in the order you watched")
                    : qsTr("All channels keeps the playlist's order · move channels inside a group or Favorites"))
            return
        }
        const n = Channels.moveChannel(row, delta)
        channelList.currentIndex = n
        channelList.positionViewAtIndex(n, ListView.Contain)
    }
    function moveGroupAt(index, delta) {
        if (!Channels.canEditGroup(index)) {
            message(qsTr("Favorites and All channels always stay at the top"))
            return
        }
        Channels.moveGroup(index, delta)
    }
    // Parental lock (K): needs a PIN, and the PIN entered to change locks.
    function withUnlocked(fn) {
        if (!Parental.hasPin) { message(qsTr("Set a parental PIN in Settings (S) first")); return }
        if (Parental.unlocked) fn()
        else pinRequested(qsTr("Enter the PIN to change locks"), fn)
    }
    function toggleLockRow(row) {
        const idx = Channels.globalIndexAt(row)
        if (idx < 0) return
        withUnlocked(() => {
            const locked = Parental.toggleChannelLock(idx)
            message(locked ? qsTr("Channel locked · it needs the PIN to play") : qsTr("Channel unlocked"))
        })
    }
    function toggleLockGroup(index) {
        if (index <= Channels.allGroup || Channels.isListGroup(index)) { message(qsTr("Lock channels or playlist groups")); return }
        withUnlocked(() => {
            const locked = Parental.toggleGroupLock(index)
            message(locked ? qsTr("Group locked · its channels need the PIN to play") : qsTr("Group unlocked"))
        })
    }
    function pageMove(list, delta) {
        list.currentIndex = Math.max(0, Math.min(list.count - 1, list.currentIndex + delta))
    }

    width: Math.min(820, parent.width * 0.68)
    height: parent.height
    x: opened ? 0 : -width - 24
    visible: x > -width - 24
    Behavior on x { NumberAnimation { duration: Theme.animNormal; easing.type: Easing.OutCubic } }

    Keys.onEscapePressed: close()
    Keys.onBackPressed: close()

    Connections {
        target: Channels
        function onCurrentGroupChanged() { root.syncGroupList(); root.syncChannelRow() }
        function onGroupsChanged() { Qt.callLater(root.syncGroupList) }
    }

    RelayMenu {
        id: channelMenu
        property int row: -1
        RelayMenuItem {
            text: channelMenu.row >= 0 && Channels.isFavorite(Channels.globalIndexAt(channelMenu.row))
                  ? qsTr("Remove from Favorites") : qsTr("Add to Favorites")
            onTriggered: root.toggleFavoriteRow(channelMenu.row)
        }
        RelayMenuItem {
            text: qsTr("Favorites lists…")
            onTriggered: root.listsRow(channelMenu.row)
        }
        RelayMenuItem {
            visible: Channels.isListGroup(Channels.currentGroup)
            height: visible ? implicitHeight : 0
            text: qsTr("Remove from this list")
            onTriggered: root.removeFromListRow(channelMenu.row)
        }
        RelayMenuItem {
            text: qsTr("Hide channel")
            onTriggered: root.hideRow(channelMenu.row)
        }
        RelayMenuItem {
            text: (Parental.revision, channelMenu.row >= 0 && Parental.isChannelLocked(Channels.globalIndexAt(channelMenu.row)))
                  ? qsTr("Unlock channel") : qsTr("Lock channel (PIN)")
            onTriggered: root.toggleLockRow(channelMenu.row)
        }
        RelayMenuItem {
            text: qsTr("Rename…")
            onTriggered: root.renameRow(channelMenu.row)
        }
        RelayMenuItem {
            text: qsTr("Guide listings…")
            onTriggered: root.guideRow(channelMenu.row)
        }
        RelayMenuItem {
            enabled: Channels.currentGroup !== Channels.allGroup && Channels.currentGroup !== Channels.recentGroup && channelMenu.row > 0
            text: qsTr("Move up")
            onTriggered: root.moveRow(channelMenu.row, -1)
        }
        RelayMenuItem {
            enabled: Channels.currentGroup !== Channels.allGroup && Channels.currentGroup !== Channels.recentGroup && channelMenu.row < channelList.count - 1
            text: qsTr("Move down")
            onTriggered: root.moveRow(channelMenu.row, 1)
        }
        RelayMenuItem {
            text: qsTr("Watch")
            onTriggered: root.tuneRow(channelMenu.row)
        }
        RelayMenuItem {
            text: qsTr("Watch in multi-view")
            onTriggered: root.multiViewRequested(Channels.globalIndexAt(channelMenu.row))
        }
    }
    RelayMenu {
        id: groupMenu
        property int group: -1
        readonly property bool isList: (Channels.groups, Channels.isListGroup(group))
        RelayMenuItem {
            text: qsTr("New favorites list…")
            onTriggered: root.newList()
        }
        RelayMenuItem {
            enabled: groupMenu.group > Channels.allGroup
            text: groupMenu.isList ? qsTr("Delete list") : qsTr("Hide group")
            onTriggered: groupMenu.isList ? root.deleteListAt(groupMenu.group) : root.hideGroupAt(groupMenu.group)
        }
        RelayMenuItem {
            enabled: groupMenu.group > Channels.allGroup
            text: qsTr("Rename…")
            onTriggered: root.renameGroupAt(groupMenu.group)
        }
        RelayMenuItem {
            enabled: groupMenu.group > Channels.allGroup && !groupMenu.isList
            text: (Parental.revision, Channels.isGroupLocked(groupMenu.group)) ? qsTr("Unlock group") : qsTr("Lock group (PIN)")
            onTriggered: root.toggleLockGroup(groupMenu.group)
        }
        RelayMenuItem {
            enabled: groupMenu.group > Channels.allGroup + 1
            text: qsTr("Move up")
            onTriggered: root.moveGroupAt(groupMenu.group, -1)
        }
        RelayMenuItem {
            enabled: groupMenu.group > Channels.allGroup && groupMenu.group < Channels.groups.length - 1
            text: qsTr("Move down")
            onTriggered: root.moveGroupAt(groupMenu.group, 1)
        }
        RelayMenuItem {
            enabled: groupMenu.group > Channels.allGroup && !groupMenu.isList
            text: qsTr("Sort channels A–Z")
            onTriggered: Channels.sortGroup(groupMenu.group)
        }
        RelayMenuItem {
            enabled: groupMenu.group > Channels.allGroup && !groupMenu.isList
            text: qsTr("Reset channel order")
            onTriggered: Channels.resetGroupOrder(groupMenu.group)
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.panel
        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border }
        MouseArea {   // keep clicks on empty panel space from reaching the video behind
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: (w) => w.accepted = true
        }
    }

    // ---- Groups column ----
    Item {
        id: groupsCol
        width: 270
        height: parent.height

        Text {
            id: groupsTitle
            x: 24; y: 28
            text: qsTr("Groups")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            font.weight: Font.Medium
            font.capitalization: Font.AllUppercase
            font.letterSpacing: 1.2
        }

        ListView {
            id: groupList
            anchors.top: groupsTitle.bottom
            anchors.topMargin: 14
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 16
            width: parent.width
            clip: true
            model: Channels.groups
            highlightMoveDuration: 0
            boundsBehavior: Flickable.StopAtBounds
            keyNavigationEnabled: false
            onModelChanged: Qt.callLater(root.syncGroupList)

            Keys.onRightPressed: channelList.forceActiveFocus()
            Keys.onReturnPressed: channelList.forceActiveFocus()
            Keys.onEnterPressed: channelList.forceActiveFocus()
            Keys.onPressed: (e) => {
                const ctrl = e.modifiers & Qt.ControlModifier
                if (!ctrl && e.key === Qt.Key_Down) { root.selectGroup(groupList.currentIndex + 1); e.accepted = true }
                else if (!ctrl && e.key === Qt.Key_Up) { root.selectGroup(groupList.currentIndex - 1); e.accepted = true }
                else if (e.key === Qt.Key_PageDown) { root.selectGroup(Math.min(groupList.count - 1, groupList.currentIndex + 8)); e.accepted = true }
                else if (e.key === Qt.Key_PageUp) { root.selectGroup(Math.max(0, groupList.currentIndex - 8)); e.accepted = true }
                else if (e.key === Qt.Key_Backspace) { root.close(); e.accepted = true }
                else if (e.key === Qt.Key_H) { root.hideGroupAt(groupList.currentIndex); e.accepted = true }
                else if (e.key === Qt.Key_Delete) { root.deleteListAt(groupList.currentIndex); e.accepted = true }
                else if (e.key === Qt.Key_N) { root.newList(); e.accepted = true }
                else if (e.key === Qt.Key_K) { root.toggleLockGroup(groupList.currentIndex); e.accepted = true }
                else if (e.key === Qt.Key_E) { root.renameGroupAt(groupList.currentIndex); e.accepted = true }
                else if (e.key === Qt.Key_Slash) { root.searchRequested(); e.accepted = true }
                else if ((e.modifiers & Qt.ControlModifier) && (e.key === Qt.Key_Up || e.key === Qt.Key_Down)) {
                    root.moveGroupAt(groupList.currentIndex, e.key === Qt.Key_Up ? -1 : 1)
                    e.accepted = true
                }
            }

            delegate: Item {
                width: groupList.width
                height: 48
                readonly property bool selected: ListView.isCurrentItem

                Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    radius: 6
                    color: selected ? (groupList.activeFocus ? Theme.focus : Theme.raised) : "transparent"
                    Rectangle {
                        visible: selected && groupList.activeFocus
                        width: 3; height: parent.height - 16
                        anchors.verticalCenter: parent.verticalCenter
                        radius: 2
                        color: Theme.accent
                    }
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 28
                    anchors.right: count.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: (modelData.locked ? "🔒 " : "") + (modelData.list ? "★ " : "") + modelData.name
                    elide: Text.ElideRight
                    color: selected ? Theme.text : Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                    font.weight: selected ? Font.Medium : Font.Normal
                }
                Text {
                    id: count
                    anchors.right: parent.right
                    anchors.rightMargin: 26
                    anchors.verticalCenter: parent.verticalCenter
                    text: Channels.groupCounts[index] !== undefined ? Channels.groupCounts[index] : ""
                    color: index === Channels.favoritesGroup ? Theme.accent : Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                }
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: (m) => {
                        root.selectGroup(index)
                        if (m.button === Qt.RightButton) { groupMenu.group = index; groupMenu.popup() }
                        else channelList.forceActiveFocus()
                    }
                }
            }
        }

        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border }
    }

    // ---- Channels column ----
    Item {
        anchors.left: groupsCol.right
        anchors.right: parent.right
        height: parent.height

        Text {
            id: channelsTitle
            x: 24; y: 28
            width: parent.width - 48
            text: {
                const g = Channels.groups[Channels.currentGroup]
                return g ? g.name : ""
            }
            elide: Text.ElideRight
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            font.weight: Font.Medium
            font.capitalization: Font.AllUppercase
            font.letterSpacing: 1.2
        }

        ListView {
            id: channelList
            anchors.top: channelsTitle.bottom
            anchors.topMargin: 14
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 16
            width: parent.width
            clip: true
            model: Channels
            highlightMoveDuration: 0
            boundsBehavior: Flickable.StopAtBounds
            reuseItems: true
            cacheBuffer: 600

            Keys.onLeftPressed: groupList.forceActiveFocus()
            Keys.onReturnPressed: root.tuneRow(currentIndex)
            Keys.onEnterPressed: root.tuneRow(currentIndex)
            Keys.onPressed: (e) => {
                if (e.key === Qt.Key_PageDown) { root.pageMove(channelList, 8); e.accepted = true }
                else if (e.key === Qt.Key_PageUp) { root.pageMove(channelList, -8); e.accepted = true }
                else if (e.key === Qt.Key_Backspace) { root.close(); e.accepted = true }
                else if (e.key === Qt.Key_F && (e.modifiers & Qt.ShiftModifier)) { root.listsRow(currentIndex); e.accepted = true }
                else if (e.key === Qt.Key_F) { root.toggleFavoriteRow(currentIndex); e.accepted = true }
                else if (e.key === Qt.Key_Delete && Channels.isListGroup(Channels.currentGroup)) { root.removeFromListRow(currentIndex); e.accepted = true }
                else if (e.key === Qt.Key_H) { root.hideRow(currentIndex); e.accepted = true }
                else if (e.key === Qt.Key_K) { root.toggleLockRow(currentIndex); e.accepted = true }
                else if (e.key === Qt.Key_E) { root.renameRow(currentIndex); e.accepted = true }
                else if (e.key === Qt.Key_U) { root.guideRow(currentIndex); e.accepted = true }
                else if (e.key === Qt.Key_Slash) { root.searchRequested(); e.accepted = true }
                else if ((e.modifiers & Qt.ControlModifier) && (e.key === Qt.Key_Up || e.key === Qt.Key_Down)) {
                    root.moveRow(currentIndex, e.key === Qt.Key_Up ? -1 : 1)
                    e.accepted = true
                }
            }

            delegate: Item {
                width: channelList.width
                height: Theme.rowHeight + 8
                readonly property string channelName: model.name
                readonly property bool selected: ListView.isCurrentItem
                readonly property bool playing: model.globalIndex === Player.currentIndex

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 4
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    radius: 6
                    color: selected ? (channelList.activeFocus ? Theme.focus : Theme.raised) : "transparent"
                    Rectangle {
                        visible: selected && channelList.activeFocus
                        width: 3; height: parent.height - 16
                        anchors.verticalCenter: parent.verticalCenter
                        radius: 2
                        color: Theme.accent
                    }
                }

                Row {
                    anchors.left: parent.left
                    anchors.leftMargin: 26
                    anchors.right: parent.right
                    anchors.rightMargin: 26
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 16

                    Text {
                        width: 46
                        anchors.verticalCenter: parent.verticalCenter
                        horizontalAlignment: Text.AlignRight
                        text: model.number
                        color: playing ? Theme.accent : Theme.textMuted
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.Medium
                    }
                    ChannelLogo {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 64
                        height: 42
                        source: model.logo
                        name: model.name
                    }
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 46 - 64 - 16 * 3 - 20
                        spacing: 3

                        Text {
                            width: parent.width
                            text: (model.locked ? "🔒 " : "") + model.name
                            elide: Text.ElideRight
                            color: selected || playing ? Theme.text : Theme.textSecondary
                            font.pixelSize: model.nowTitle ? Theme.fontBody + 2 : Theme.fontLarge
                            font.weight: selected ? Font.Medium : Font.Normal
                        }
                        Text {
                            visible: text.length > 0
                            width: parent.width
                            text: model.nowTitle
                            elide: Text.ElideRight
                            color: selected ? Theme.textSecondary : Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                        }
                        Rectangle {
                            visible: model.nowProgress >= 0
                            width: Math.min(parent.width, 180)
                            height: 3
                            radius: 1.5
                            color: Theme.track
                            Rectangle {
                                width: parent.width * Math.max(0, model.nowProgress)
                                height: parent.height
                                radius: 1.5
                                color: selected ? Theme.accent : Theme.textMuted
                            }
                        }
                    }
                    Item {
                        width: 20
                        height: 20
                        anchors.verticalCenter: parent.verticalCenter
                        Text {
                            anchors.centerIn: parent
                            visible: model.favorite
                            text: "★"
                            color: Theme.accent
                            font.pixelSize: 16
                        }
                        Rectangle {
                            visible: playing && !model.favorite
                            anchors.centerIn: parent
                            width: 8; height: 8; radius: 4
                            color: Theme.accent
                        }
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: (m) => {
                        if (m.button === Qt.RightButton) {
                            channelList.currentIndex = index
                            channelList.forceActiveFocus()
                            channelMenu.row = -1      // re-evaluate the favorite label
                            channelMenu.row = index
                            channelMenu.popup()
                        } else if (channelList.currentIndex === index && channelList.activeFocus) root.tuneRow(index)
                        else { channelList.currentIndex = index; channelList.forceActiveFocus() }
                    }
                    onDoubleClicked: root.tuneRow(index)
                }
            }

            Text {
                anchors.centerIn: parent
                width: parent.width - 80
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                visible: channelList.count === 0
                text: Channels.currentGroup === Channels.favoritesGroup
                      ? qsTr("No favorites yet.\nSelect a channel and press F, or right-click it.")
                      : Channels.currentGroup === Channels.recentGroup
                      ? qsTr("Channels you watch show up here, most recent first.")
                      : qsTr("No channels in this group")
                color: Theme.textMuted
                font.pixelSize: Theme.fontBody
                lineHeight: 1.3
            }
        }
    }

    // Shortcut hint (keyboard keys: not on a TV)
    Text {
        visible: !Settings.tv
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 10
        anchors.rightMargin: 20
        text: groupList.activeFocus ? qsTr("N new list  ·  E rename  ·  Ctrl+↑↓ move  ·  H hide  ·  / search  ·  right-click for more")
                                    : qsTr("F favorite  ·  Shift+F lists  ·  E rename  ·  Ctrl+↑↓ move  ·  H hide  ·  K lock  ·  / search")
        color: Theme.textMuted
        font.pixelSize: 11
    }

    GuidePicker {
        id: guidePicker
        onMessage: (text) => root.message(text)
        onFinished: channelList.forceActiveFocus()
    }

    ListPicker {
        id: listPicker
        onMessage: (text) => root.message(text)
        onFinished: channelList.forceActiveFocus()
    }

    RenameDialog {
        id: renameDialog
        onSaved: (name) => {
            const t = root.renameTarget
            if (!t) return
            if (t.kind === "newlist") {
                if (!name.length) return
                const g = Channels.createList(name)
                if (g >= 0) { root.selectGroup(g); root.message(qsTr("List \"%1\" made · add channels with Shift+F").arg(name)) }
            } else if (t.kind === "group" && Channels.isListGroup(t.index)) {
                if (name.length) { Channels.renameGroup(t.index, name); root.message(qsTr("List renamed to \"%1\"").arg(name)) }
            } else if (t.kind === "channel") {
                Channels.renameChannel(t.index, name)
                root.message(name.length ? qsTr("Renamed to \"%1\"").arg(name) : qsTr("Original name restored"))
            } else {
                Channels.renameGroup(t.index, name)
                root.message(name.length ? qsTr("Group renamed to \"%1\"").arg(name) : qsTr("Original group name restored"))
            }
        }
        onFinished: {
            const t = root.renameTarget
            root.renameTarget = null
            if (t && (t.kind === "group" || t.kind === "newlist")) groupList.forceActiveFocus()
            else channelList.forceActiveFocus()
        }
    }
}

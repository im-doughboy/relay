import QtQuick

// The round ✕ in the top-right corner of full-screen views: the mouse's way
// back (same as Esc).
BarButton {
    icon: Icons.close
    label: qsTr("Close")
    shortcut: "Esc"
    showLabel: false
    size: 44
    color: hovered ? "#3DFFFFFF" : "#26FFFFFF"
    z: 50
}

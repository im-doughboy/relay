pragma Singleton
import QtQuick

// Relay's visual identity: near-black surfaces, high-contrast type sized for
// viewing from the couch, and a single warm amber accent for focus.
QtObject {
    // Surfaces
    readonly property color background: "#0A0C10"
    readonly property color panel: "#EB10141B"        // translucent over video
    readonly property color panelSolid: "#12161E"
    readonly property color raised: "#1B212C"
    readonly property color focus: "#262E3C"
    readonly property color border: "#252C38"
    readonly property color track: "#3A4352"       // unfilled part of progress bars

    // Text
    readonly property color text: "#F3F5F8"
    readonly property color textSecondary: "#A3ACBB"
    readonly property color textMuted: "#6C7686"

    // Accent
    readonly property color accent: "#F2A33A"
    readonly property color accentText: "#1E1404"
    readonly property color accentSoft: "#33F2A33A"
    readonly property color danger: "#FF7A6B"

    // Type scale (pixels)
    readonly property int fontSmall: 13
    readonly property int fontBody: 16
    readonly property int fontLarge: 20
    readonly property int fontTitle: 28
    readonly property int fontDisplay: 44

    readonly property int radius: 8
    readonly property int rowHeight: 56
    readonly property int animFast: 140
    readonly property int animNormal: 220
}

pragma Singleton
import QtQuick

// Relay's line icons: SVG path data on a 24×24 grid, drawn by Icon.qml with a
// round 2 px stroke (the ones marked "filled" are drawn solid).
QtObject {
    readonly property string heart: "M12 20.5s-7.5-4.6-7.5-10.1A4.4 4.4 0 0 1 12 7.6a4.4 4.4 0 0 1 7.5 2.8c0 5.5-7.5 10.1-7.5 10.1z"
    readonly property string info: "M12 21a9 9 0 1 0 0-18a9 9 0 0 0 0 18z M12 11v6 M12 7.5v.5"
    readonly property string download: "M12 4v11 M7 10.5l5 5l5-5 M5 19.5h14"
    readonly property string channels: "M4 6h16M4 12h16M4 18h10"
    readonly property string guide: "M3 5h18v14H3z M3 10h18 M9 10v9 M15 10v9"
    readonly property string movies: "M4 5h16v14H4z M10 9l5 3l-5 3z"
    readonly property string search: "M10.5 4a6.5 6.5 0 1 0 0.01 0z M15.5 15.5L20 20"
    readonly property string multiview: "M4 4h7v7H4z M13 4h7v7h-7z M4 13h7v7H4z M13 13h7v7h-7z"
    readonly property string recordings: "M12 4a8 8 0 1 0 0.01 0z M12 9a3 3 0 1 0 0.01 0z"
    readonly property string pip: "M3 5h18v14H3z M12 12h6v4h-6z"
    readonly property string captions: "M3 6h18v12H3z M10.5 10h-3v4h3 M17 10h-3v4h3"
    readonly property string volume: "M4 9h4l5-4v14l-5-4H4z M16.5 8.5a5 5 0 0 1 0 7 M19 6a8.5 8.5 0 0 1 0 12"
    readonly property string muted: "M4 9h4l5-4v14l-5-4H4z M16 9.5l5 5 M21 9.5l-5 5"
    readonly property string fullscreen: "M4 9V4h5 M15 4h5v5 M20 15v5h-5 M9 20H4v-5"
    readonly property string exitFullscreen: "M9 4v5H4 M20 9h-5V4 M15 20v-5h5 M4 15h5v5"
    readonly property string settings: "M4 7h9 M17 7h3 M4 17h3 M11 17h9 M15 5v4 M9 15v4"
    readonly property string help: "M12 3a9 9 0 1 0 0.01 0z M9.6 9.4a2.5 2.5 0 1 1 3.6 2.3c-0.8 0.4-1.2 0.9-1.2 1.8v0.4 M12 17v0.2"
    readonly property string close: "M6 6l12 12 M18 6L6 18"
    readonly property string back: "M15 5l-7 7l7 7"
    readonly property string play: "M8 5v14l11-7z"                       // filled
    readonly property string pause: "M7 5h3.5v14H7z M13.5 5H17v14h-3.5z"    // filled
    readonly property string rewind: "M11.5 6.5v11l-8-5.5z M20.5 6.5v11l-8-5.5z"      // filled
    readonly property string forward: "M3.5 6.5v11l8-5.5z M12.5 6.5v11l8-5.5z"        // filled
    readonly property string live: "M12 8a4 4 0 1 0 0.01 0z"             // filled
    readonly property string toLive: "M5 6l7 6l-7 6z M13 6l7 6l-7 6z M20 6v12"  // filled-ish
    readonly property string record: "M12 6a6 6 0 1 0 0.01 0z"           // filled
    readonly property string recordSeries: "M9 7a5 5 0 1 0 0.01 0z M14.5 7.3a5 5 0 0 1 0 9.4 M18.5 7.3a5 5 0 0 1 0 9.4"
    readonly property string bell: "M6 16v-5a6 6 0 0 1 12 0v5l2 2H4z M10 20.5a2 2 0 0 0 4 0"
    readonly property string now: "M12 4a8 8 0 1 0 0.01 0z M12 8v4l3 2"
    readonly property string fromStart: "M6 5v14 M19 5l-9 7l9 7z"
    readonly property string plus: "M12 5v14 M5 12h14"
    readonly property string remove: "M5 7h14 M10 7V4.5h4V7 M7 7l1 12.5h8L17 7"
    readonly property string swap: "M6 8h12l-3-3 M18 16H6l3 3"
    readonly property string layout: "M4 4h16v16H4z M4 12h16 M12 12v8"
    readonly property string edit: "M5 19h4L19 9l-4-4L5 15z M13 7l4 4"
    readonly property string playlists: "M4 6h12 M4 11h12 M4 16h7 M17 14v6 M14 17h6"
    readonly property string options: "M3 5h18v12H3z M8 21h8 M12 17v4"
    readonly property string cast: "M3 8V6h18v12h-6 M3 18h.01 M3 14a4 4 0 0 1 4 4 M3 10.5a7.5 7.5 0 0 1 7.5 7.5"
    readonly property string tv: "M3 6h18v11H3z M8 21h8 M12 17v4"
    readonly property string keyboard: "M3 6h18v12H3z M7 10h0.01 M11 10h0.01 M15 10h0.01 M17 14H7"
}

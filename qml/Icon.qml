import QtQuick
import QtQuick.Shapes
import Relay.Core

// One of Relay's line icons (see Icons.qml), drawn crisp at any size.
Item {
    id: root
    property string path
    property color color: Theme.text
    property bool filled: false
    implicitWidth: 20
    implicitHeight: 20

    Shape {
        id: shape
        width: 24
        height: 24
        anchors.centerIn: parent
        scale: Math.min(root.width, root.height) / 24
        // Drawn into a larger texture with multisampling, then scaled: smooth
        // edges on every Qt 6 version and graphics driver. Not on a TV: its
        // graphics have no multisampling, and an offscreen texture per icon
        // costs it a lot; Qt's curve renderer smooths the edges instead.
        layer.enabled: !Settings.tv
        layer.samples: 4
        layer.smooth: true
        layer.textureSize: Qt.size(96, 96)
        Component.onCompleted: {
            // (From script: the property is Qt 6.6+, and Relay also builds with older Qt.)
            if (Settings.tv) try { shape.preferredRendererType = Shape.CurveRenderer } catch (e) {}
        }
        ShapePath {
            strokeColor: root.filled ? "transparent" : root.color
            strokeWidth: 2
            fillColor: root.filled ? root.color : "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: root.path }
        }
    }
}

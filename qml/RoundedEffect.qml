import QtQuick
import QtQuick.Effects

// Rounded corners for a layer (the spotlight card, the selected card, the
// trailer). Loaded at run time: on a Qt without QtQuick.Effects (before 6.5)
// it doesn't load and corners stay square.
MultiEffect {
    property real cornerRadius: 12
    maskEnabled: true
    maskSource: mask
    maskThresholdMin: 0.5
    maskSpreadAtMin: 1.0
    Item {
        id: mask
        width: parent.width
        height: parent.height
        layer.enabled: true
        visible: false
        Rectangle { anchors.fill: parent; radius: parent.parent.cornerRadius; antialiasing: true }
    }
}

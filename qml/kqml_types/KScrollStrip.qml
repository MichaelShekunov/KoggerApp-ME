import QtQuick 2.15

// A row that scrolls sideways once it is wider than the room it has (owner 2026-10-01: the main menu's device chain
// no longer fit and was cut): by finger, by the wheel; a cut edge fades into fadeColor ("transparent": no fade). The
// content is one item, laid out at its own size; the strip is as wide as it, up to maxWidth.
Item {
    id: strip

    property real maxWidth: Number.POSITIVE_INFINITY
    property color fadeColor: "transparent"
    property real fadeWidth: 16
    default property alias content: holder.data

    readonly property real contentWidth: holder.childrenRect.width
    readonly property bool overflows: contentWidth > maxWidth + 0.5
    readonly property alias contentX: flick.contentX
    readonly property bool atStart: flick.contentX <= 0.5
    readonly property bool atEnd: flick.contentX >= flick.contentWidth - flick.width - 0.5

    implicitWidth: Math.min(contentWidth, maxWidth)
    implicitHeight: holder.childrenRect.height
    width: implicitWidth
    height: implicitHeight

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: strip.contentWidth
        contentHeight: height
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        interactive: strip.overflows
        clip: strip.overflows

        Item {
            id: holder
            width: childrenRect.width
            height: childrenRect.height
        }
    }

    // the wheel scrolls it sideways (either axis of the wheel)
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.NoButton
        enabled: strip.overflows
        onWheel: function(wheel) {
            var d = wheel.angleDelta.x !== 0 ? wheel.angleDelta.x : wheel.angleDelta.y
            flick.contentX = Math.max(0, Math.min(flick.contentWidth - flick.width, flick.contentX - d))
            wheel.accepted = true
        }
    }

    Rectangle {
        objectName: "stripFadeStart"
        visible: strip.overflows && !strip.atStart && strip.fadeColor.a > 0
        anchors.left: parent.left
        width: Math.min(strip.fadeWidth, parent.width / 4)
        height: parent.height
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: strip.fadeColor }
            GradientStop { position: 1.0; color: Qt.rgba(strip.fadeColor.r, strip.fadeColor.g, strip.fadeColor.b, 0) }
        }
    }
    Rectangle {
        objectName: "stripFadeEnd"
        visible: strip.overflows && !strip.atEnd && strip.fadeColor.a > 0
        anchors.right: parent.right
        width: Math.min(strip.fadeWidth, parent.width / 4)
        height: parent.height
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: Qt.rgba(strip.fadeColor.r, strip.fadeColor.g, strip.fadeColor.b, 0) }
            GradientStop { position: 1.0; color: strip.fadeColor }
        }
    }
}

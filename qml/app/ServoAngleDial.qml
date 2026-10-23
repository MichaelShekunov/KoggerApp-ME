import QtQuick 2.15
import kqml_types 1.0

// The servo's manual angle as a knob: 0 at the top, positive clockwise, the servo's range as the drawn sector.
// A drag turns it by as much as the pointer turns around the centre (Shift: a tenth of that), so pressing does
// not make the servo jump to where the pointer is; the wheel steps 1 degree (Ctrl: 0.1).
Item {
    id: dial

    property real angle: 0.0
    property real limit: 90.0          // the angle stays within +-limit
    property real size: Math.round(120 * AppPalette.scale)
    property color lineColor: AppPalette.textStrong
    property color knobColor: AppPalette.accent
    property color faceColor: AppPalette.card
    property color faceBorder: AppPalette.border
    property color sectorColor: AppPalette.accent
    property color tickColor: AppPalette.textMuted

    signal pressed()
    signal moved(real degrees)
    signal released(real degrees)

    width: size
    height: size

    readonly property real _r: size / 2
    readonly property real _rad: angle * Math.PI / 180.0

    function _clamp(deg) {
        var l = Math.max(0, dial.limit)
        return Math.round(Math.max(-l, Math.min(l, deg)) * 100) / 100
    }
    function _pointerDeg(x, y) {
        return Math.atan2(x - dial._r, -(y - dial._r)) * 180.0 / Math.PI
    }

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: dial.faceColor
        border.color: area.pressed ? AppPalette.borderFocus : dial.faceBorder
        border.width: 1
    }

    Canvas {
        id: sector
        anchors.fill: parent
        readonly property real lim: Math.min(180, Math.max(0, dial.limit))
        readonly property color col: dial.sectorColor
        onLimChanged: requestPaint()
        onColChanged: requestPaint()
        onWidthChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            var w = Math.max(3, Math.round(4 * AppPalette.scale))
            var rr = dial._r - w / 2 - 1
            ctx.lineWidth = w
            ctx.lineCap = "round"
            ctx.globalAlpha = 0.35
            ctx.strokeStyle = col
            ctx.beginPath()
            // Canvas angles start at 3 o'clock; the dial's 0 is at 12.
            ctx.arc(dial._r, dial._r, rr, (-90 - lim) * Math.PI / 180, (-90 + lim) * Math.PI / 180, false)
            ctx.stroke()
        }
    }

    Repeater {
        model: 12
        delegate: Rectangle {
            required property int index
            readonly property bool major: index % 3 === 0
            readonly property real a: index * 30 * Math.PI / 180.0
            readonly property real dist: dial._r - Math.round(9 * AppPalette.scale) - height / 2
            width: Math.max(1, Math.round((major ? 2 : 1) * AppPalette.scale))
            height: Math.round((major ? 8 : 4) * AppPalette.scale)
            radius: width / 2
            color: dial.tickColor
            x: dial._r + Math.sin(a) * dist - width / 2
            y: dial._r - Math.cos(a) * dist - height / 2
            rotation: index * 30
        }
    }

    Rectangle {
        id: line
        width: Math.max(2, Math.round(2 * AppPalette.scale))
        height: dial.size / 2 - Math.round(16 * AppPalette.scale)
        radius: width / 2
        color: dial.lineColor
        x: dial._r - width / 2
        y: dial._r - height
        transformOrigin: Item.Bottom
        rotation: dial.angle
    }

    Rectangle {
        id: knob
        readonly property real dist: line.height
        width: Math.round(18 * AppPalette.scale)
        height: width
        radius: width / 2
        color: dial.knobColor
        border.color: AppPalette.accentBorder
        border.width: 1
        x: dial._r + Math.sin(dial._rad) * dist - width / 2
        y: dial._r - Math.cos(dial._rad) * dist - height / 2
        scale: area.pressed ? 1.15 : 1.0
        Behavior on scale { NumberAnimation { duration: Anim.controlMs; easing.type: Anim.controlEasing } }
    }

    Text {
        anchors.centerIn: parent
        anchors.verticalCenterOffset: Math.round(dial.size * 0.22)
        text: dial.angle.toFixed(2) + "°"
        color: dial.lineColor
        font.pixelSize: Tokens.fontSm
        font.bold: true
        Rectangle {
            anchors.centerIn: parent
            width: parent.implicitWidth + Tokens.spaceSm
            height: parent.implicitHeight + Tokens.spaceXxs
            radius: Tokens.radiusSm
            color: dial.faceColor
            z: -1
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        preventStealing: true
        cursorShape: Qt.PointingHandCursor
        property real _lastPointer: 0
        property real _value: 0
        onPressed: function(mouse) {
            _lastPointer = dial._pointerDeg(mouse.x, mouse.y)
            _value = dial.angle
            dial.pressed()
        }
        onPositionChanged: function(mouse) {
            if (!pressed)
                return
            var p = dial._pointerDeg(mouse.x, mouse.y)
            var d = p - _lastPointer
            if (d > 180) d -= 360
            else if (d < -180) d += 360
            _lastPointer = p
            if (mouse.modifiers & Qt.ShiftModifier)
                d /= 10
            var next = dial._clamp(_value + d)
            // Past an end the pointer keeps turning but the value does not, so turning back moves it at once.
            _value = Math.max(-dial.limit, Math.min(dial.limit, _value + d))
            if (next !== dial.angle)
                dial.moved(next)
        }
        onReleased: dial.released(dial._clamp(_value))
        onCanceled: dial.released(dial.angle)
        onWheel: function(wheel) {
            var notches = wheel.angleDelta.y / 120
            if (notches === 0)
                return
            var step = (wheel.modifiers & Qt.ControlModifier) ? 0.1 : 1.0
            dial.released(dial._clamp(dial.angle + notches * step))
        }
    }
}

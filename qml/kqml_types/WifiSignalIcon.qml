import QtQuick 2.15

// Wi-Fi symbol (dot + three arcs, the geometry of icons/ui/wifi.svg) lit by signal level: `level` is
// DevQProperty.wifiSignalLevel (thresholds in WifiSignal, src/device/wifi_ops.h): -1 no link, everything dim;
// 0 only the dot; 1..3 lit arcs. The colours of the levels are chosen here only.
Item {
    id: icon

    property int level: -1
    property real size: Math.round(22 * AppPalette.scale)

    // level -> colour of the lit parts: 3 green, 2 yellow-green, 1 orange, 0 red; readable on the green/neutral
    // badge fills of either theme
    readonly property color litColor: level >= 3 ? (AppPalette.isDark ? "#22C55E" : "#15803D")
                                    : level === 2 ? (AppPalette.isDark ? "#A3E635" : "#4D7C0F")
                                    : level === 1 ? (AppPalette.isDark ? "#FB923C" : "#C2410C")
                                    : (AppPalette.isDark ? "#F87171" : "#B91C1C")
    readonly property color dimColor: Qt.rgba(AppPalette.textStrong.r, AppPalette.textStrong.g, AppPalette.textStrong.b, 0.24)

    width: size
    height: size

    onLevelChanged: canvas.requestPaint()
    onLitColorChanged: canvas.requestPaint()
    onDimColorChanged: canvas.requestPaint()

    Canvas {
        id: canvas
        anchors.fill: parent
        antialiasing: true
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        Component.onCompleted: requestPaint()

        onPaint: {
            var ctx = getContext("2d")
            if (!ctx)
                return
            ctx.clearRect(0, 0, width, height)
            var s = Math.min(width, height) / 24          // the svg's 24-unit box
            var cx = width / 2
            var cy = height / 2 + 6 * s                   // (12, 18) in the svg
            ctx.lineCap = "round"
            ctx.lineWidth = Math.max(1.5, 2 * s)
            for (var i = 1; i <= 3; ++i) {                // radius 4, 8, 12: arc i lit from level i
                ctx.strokeStyle = (icon.level >= i) ? icon.litColor : icon.dimColor
                ctx.beginPath()
                ctx.arc(cx, cy, 4 * i * s, -0.75 * Math.PI, -0.25 * Math.PI, false)
                ctx.stroke()
            }
            ctx.fillStyle = (icon.level >= 0) ? icon.litColor : icon.dimColor
            ctx.beginPath()
            ctx.arc(cx, cy, Math.max(1.5, 1.8 * s), 0, 2 * Math.PI, false)   // a bit above the svg's dot: alone at level 0
            ctx.fill()
        }
    }
}

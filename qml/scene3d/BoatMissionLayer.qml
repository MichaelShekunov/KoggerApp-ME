import QtQuick
import QtQuick.Shapes
import kqml_types 1.0

// The boat mission drawn over the 3D scene: the planned route, numbered waypoints (draggable while
// editing), the home position, the leg from the boat to the point it heads for and the way back
// home. Everything is placed with view.geoToScreen() on the z = 0 plane and re-placed on
// view.cameraChanged, on plan / telemetry changes and by a slow timer that covers camera
// animations. Only the markers take input; the rest lets touches through to the scene.
Item {
    id: layer

    property var view: null          // GraphicsScene3dView
    property var vehicle: null       // MavVehicle (context property "boat")
    property bool editable: false    // markers can be dragged
    property int selectedIndex: -1
    signal pointClicked(int index)

    readonly property real _s: AppPalette.scale
    readonly property real markerSize: Math.round(30 * _s)
    readonly property real hitSize: Math.round(46 * _s)

    property int _tick: 0
    function refresh() { _tick++ }

    // A marker being dragged is drawn at the finger, and the line follows it, until release commits it.
    property int _dragIndex: -1
    property point _dragPos: Qt.point(0, 0)

    Connections {
        target: layer.view
        ignoreUnknownSignals: true
        function onCameraChanged() { layer.refresh() }
    }
    Connections {
        target: layer.vehicle
        ignoreUnknownSignals: true
        function onPlanChanged() { layer.refresh() }
        function onPositionChanged() { layer.refresh() }
        function onHomeChanged() { layer.refresh() }
        function onMissionStateChanged() { layer.refresh() }
    }
    Timer {
        interval: 250
        repeat: true
        running: layer.visible
        onTriggered: layer.refresh()
    }
    onWidthChanged: refresh()
    onHeightChanged: refresh()

    function _project(lat, lon) {
        if (!view || typeof view.geoToScreen !== "function")
            return { x: 0, y: 0, ok: false }
        var p = view.geoToScreen(lat, lon)
        return { x: p.x, y: p.y, ok: isFinite(p.x) && isFinite(p.y) }
    }

    readonly property var _pts: {
        _tick
        var out = []
        if (!vehicle)
            return out
        var plan = vehicle.plan
        for (var i = 0; i < plan.length; ++i) {
            var p = (i === _dragIndex) ? { x: _dragPos.x, y: _dragPos.y, ok: true } : _project(plan[i].lat, plan[i].lon)
            out.push(p)
        }
        return out
    }
    readonly property var _home: {
        _tick
        return (vehicle && vehicle.homeValid) ? _project(vehicle.homeLatitude, vehicle.homeLongitude) : { ok: false }
    }
    readonly property var _boat: {
        _tick
        return (vehicle && vehicle.positionValid) ? _project(vehicle.latitude, vehicle.longitude) : { ok: false }
    }
    readonly property int _target: vehicle ? vehicle.currentPlanIndex : -1
    readonly property bool _running: !!vehicle && vehicle.armed && vehicle.mode === 10 && _target >= 0

    function _polyline(points) {
        var out = []
        for (var i = 0; i < points.length; ++i)
            if (points[i].ok)
                out.push(Qt.point(points[i].x, points[i].y))
        return out
    }

    // planned route
    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        visible: layer._pts.length >= 2
        ShapePath {
            strokeColor: "#E6FFFFFF"
            strokeWidth: Math.max(4, Math.round(5 * layer._s))
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathPolyline { path: layer._polyline(layer._pts) }
        }
        ShapePath {
            strokeColor: AppPalette.accent
            strokeWidth: Math.max(2, Math.round(3 * layer._s))
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathPolyline { path: layer._polyline(layer._pts) }
        }
    }

    // boat -> the point it heads for (or -> the first point while not running)
    Shape {
        id: legShape
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        readonly property var from: layer._boat
        readonly property var to: layer._pts.length > 0 ? layer._pts[Math.max(0, layer._target)] : null
        visible: from.ok && to !== null && to.ok
        ShapePath {
            strokeColor: layer._running ? AppPalette.accent : AppPalette.textSecond
            strokeWidth: Math.max(2, Math.round(2 * layer._s))
            strokeStyle: ShapePath.DashLine
            dashPattern: [3, 3]
            fillColor: "transparent"
            PathPolyline {
                path: legShape.visible ? [Qt.point(legShape.from.x, legShape.from.y),
                                          Qt.point(legShape.to.x, legShape.to.y)] : []
            }
        }
    }

    // last point -> home, when the mission ends with a return
    Shape {
        id: returnShape
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        readonly property var from: layer._pts.length > 0 ? layer._pts[layer._pts.length - 1] : null
        readonly property var to: layer._home
        visible: !!layer.vehicle && layer.vehicle.planReturnHome && from !== null && from.ok && to.ok
        ShapePath {
            strokeColor: AppPalette.textSecond
            strokeWidth: Math.max(2, Math.round(2 * layer._s))
            strokeStyle: ShapePath.DashLine
            dashPattern: [1, 3]
            fillColor: "transparent"
            PathPolyline {
                path: returnShape.visible ? [Qt.point(returnShape.from.x, returnShape.from.y),
                                             Qt.point(returnShape.to.x, returnShape.to.y)] : []
            }
        }
    }

    // home
    Rectangle {
        visible: layer._home.ok
        width: Math.round(24 * layer._s)
        height: width
        radius: Math.round(5 * layer._s)
        x: layer._home.ok ? Math.round(layer._home.x - width / 2) : 0
        y: layer._home.ok ? Math.round(layer._home.y - height / 2) : 0
        color: "#CC1B1F24"
        border.width: Math.max(1, Math.round(2 * layer._s))
        border.color: "#FFFFFF"
        Text {
            anchors.centerIn: parent
            text: "H"
            color: "#FFFFFF"
            font.pixelSize: Tokens.fontMd
            font.bold: true
        }
    }

    // waypoints
    Repeater {
        model: layer._pts.length
        delegate: Item {
            id: wp
            required property int index
            readonly property var pt: index < layer._pts.length ? layer._pts[index] : null
            readonly property bool isTarget: layer._running && index === layer._target
            readonly property bool isSelected: index === layer.selectedIndex
            readonly property bool done: layer._running && index < layer._target
            visible: pt !== null && pt.ok
            width: layer.hitSize
            height: layer.hitSize
            x: (pt && pt.ok) ? Math.round(pt.x - width / 2) : 0
            y: (pt && pt.ok) ? Math.round(pt.y - height / 2) : 0
            z: isSelected || isTarget ? 2 : 1

            Rectangle {
                anchors.centerIn: parent
                width: wp.isSelected || wp.isTarget ? Math.round(layer.markerSize * 1.15) : layer.markerSize
                height: width
                radius: width / 2
                color: wp.isTarget ? AppPalette.accent : (wp.done ? "#CC5B6470" : "#E61B1F24")
                border.width: Math.max(2, Math.round((wp.isSelected ? 3 : 2) * layer._s))
                border.color: wp.isSelected ? AppPalette.accent : "#FFFFFF"
                Behavior on width { NumberAnimation { duration: 120 } }
                Text {
                    anchors.centerIn: parent
                    text: String(wp.index + 1)
                    color: "#FFFFFF"
                    font.pixelSize: Tokens.fontBase
                    font.bold: true
                }
            }

            MouseArea {
                id: wpMouse
                anchors.fill: parent
                enabled: true
                cursorShape: layer.editable ? Qt.OpenHandCursor : Qt.PointingHandCursor
                preventStealing: true
                property point pressPos
                property bool dragging: false
                onPressed: function(mouse) {
                    pressPos = Qt.point(mouse.x, mouse.y)
                    dragging = false
                }
                onPositionChanged: function(mouse) {
                    if (!layer.editable)
                        return
                    var dx = mouse.x - pressPos.x, dy = mouse.y - pressPos.y
                    if (!dragging && dx * dx + dy * dy > 64)
                        dragging = true
                    if (dragging) {
                        var p = mapToItem(layer, mouse.x, mouse.y)
                        layer._dragIndex = wp.index
                        layer._dragPos = p
                        layer.refresh()
                    }
                }
                onReleased: function(mouse) {
                    if (dragging && layer.view && layer.vehicle) {
                        var p = mapToItem(layer, mouse.x, mouse.y)
                        var g = layer.view.screenToGeo(p.x, p.y)
                        if (g.valid)
                            layer.vehicle.movePoint(wp.index, g.lat, g.lon)
                    } else {
                        layer.pointClicked(wp.index)
                    }
                    dragging = false
                    layer._dragIndex = -1
                    layer.refresh()
                }
                onCanceled: {
                    dragging = false
                    layer._dragIndex = -1
                    layer.refresh()
                }
            }
        }
    }
}

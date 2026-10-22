import QtQuick
import QtQuick.Shapes
import SceneGraphRendering 1.0
import kqml_types 1.0

// The boat mission drawn over the 3D scene: the planned route, numbered waypoints (draggable while
// editing), the home position, the leg from the boat to the point it heads for and the way back
// home; under them the boat's track by link quality and, while the link is lost, a faint boat where it should be
// now. Everything is placed with view.geoToScreen() on the z = 0 plane and re-placed on
// view.cameraChanged, on plan / telemetry changes and by a slow timer that covers camera
// animations; the track by its own C++ item (LinkTrackItem). Only the markers take input; the rest lets touches
// through to the scene.
Item {
    id: missionLayer
    // Not "layer": every Item has a layer property, and inside the waypoint delegate that property
    // is found before this id.

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
        target: missionLayer.view
        ignoreUnknownSignals: true
        function onCameraChanged() { missionLayer.refresh() }
    }
    Connections {
        target: missionLayer.vehicle
        ignoreUnknownSignals: true
        function onPlanChanged() { missionLayer.refresh() }
        function onPositionChanged() { missionLayer.refresh() }
        function onHomeChanged() { missionLayer.refresh() }
        function onMissionStateChanged() { missionLayer.refresh() }
        function onGhostChanged() { missionLayer.refresh() }
    }
    Timer {
        interval: 250
        repeat: true
        running: missionLayer.visible
        onTriggered: missionLayer.refresh()
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

    // The track by link quality (vehicle.linkTrack*): each stretch in the colour of the link on it
    // (AppPalette.linkQualityColors), a stretch over a loss dashed. One C++ item that reads the track when it changes
    // and places it again as it is drawn after a camera move: a Repeater of Shapes made anew in JavaScript at every
    // move held a drag of the view with a long session's track (2026-10-22).
    LinkTrackItem {
        id: linkTrack
        objectName: "linkTrack"
        anchors.fill: parent
        view: missionLayer.view
        vehicle: missionLayer.vehicle
        colors: AppPalette.linkQualityColors
        lineWidth: Math.max(3, Math.round(4 * missionLayer._s))
    }

    // planned route
    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        visible: missionLayer._pts.length >= 2
        ShapePath {
            strokeColor: "#E6FFFFFF"
            strokeWidth: Math.max(4, Math.round(5 * missionLayer._s))
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathPolyline { path: missionLayer._polyline(missionLayer._pts) }
        }
        ShapePath {
            strokeColor: AppPalette.accent
            strokeWidth: Math.max(2, Math.round(3 * missionLayer._s))
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathPolyline { path: missionLayer._polyline(missionLayer._pts) }
        }
    }

    // boat -> the point it heads for (or -> the first point while not running)
    Shape {
        id: legShape
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        readonly property var from: missionLayer._boat
        readonly property var to: missionLayer._pts.length > 0 ? missionLayer._pts[Math.max(0, missionLayer._target)] : null
        visible: from.ok && to !== null && to.ok
        ShapePath {
            strokeColor: missionLayer._running ? AppPalette.accent : AppPalette.textSecond
            strokeWidth: Math.max(2, Math.round(2 * missionLayer._s))
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
        readonly property var from: missionLayer._pts.length > 0 ? missionLayer._pts[missionLayer._pts.length - 1] : null
        readonly property var to: missionLayer._home
        visible: !!missionLayer.vehicle && missionLayer.vehicle.planReturnHome && from !== null && from.ok && to.ok
        ShapePath {
            strokeColor: AppPalette.textSecond
            strokeWidth: Math.max(2, Math.round(2 * missionLayer._s))
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
        visible: missionLayer._home.ok
        width: Math.round(24 * missionLayer._s)
        height: width
        radius: Math.round(5 * missionLayer._s)
        x: missionLayer._home.ok ? Math.round(missionLayer._home.x - width / 2) : 0
        y: missionLayer._home.ok ? Math.round(missionLayer._home.y - height / 2) : 0
        color: "#CC1B1F24"
        border.width: Math.max(1, Math.round(2 * missionLayer._s))
        border.color: "#FFFFFF"
        Text {
            anchors.centerIn: parent
            text: "H"
            color: "#FFFFFF"
            font.pixelSize: Tokens.fontMd
            font.bold: true
        }
    }

    // While the link is lost: the boat where it should be now (vehicle.ghost*, MavVehicle::updateGhost), faint and
    // pulsing, a dashed line from where it was last seen, and how long the link has been gone.
    readonly property var _ghost: {
        _tick
        return (vehicle && vehicle.ghostValid) ? _project(vehicle.ghostLatitude, vehicle.ghostLongitude) : { ok: false }
    }
    Shape {
        id: ghostLine
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        visible: missionLayer._ghost.ok && missionLayer._boat.ok && !!missionLayer.vehicle && missionLayer.vehicle.ghostMoving
        ShapePath {
            strokeColor: "#FFFF6B6B"
            strokeWidth: Math.max(2, Math.round(2 * missionLayer._s))
            strokeStyle: ShapePath.DashLine
            dashPattern: [2, 3]
            fillColor: "transparent"
            PathPolyline {
                path: ghostLine.visible ? [Qt.point(missionLayer._boat.x, missionLayer._boat.y),
                                           Qt.point(missionLayer._ghost.x, missionLayer._ghost.y)] : []
            }
        }
    }
    Item {
        id: ghostMark
        visible: missionLayer._ghost.ok
        width: missionLayer.markerSize
        height: width
        x: visible ? Math.round(missionLayer._ghost.x - width / 2) : 0
        y: visible ? Math.round(missionLayer._ghost.y - height / 2) : 0
        z: 3
        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: "#40FFFFFF"
            border.width: Math.max(2, Math.round(2 * missionLayer._s))
            border.color: "#FFFF6B6B"
            SequentialAnimation on opacity {
                loops: Animation.Infinite
                running: ghostMark.visible
                NumberAnimation { to: 0.4; duration: 900 }
                NumberAnimation { to: 1.0; duration: 900 }
            }
        }
        Image {
            anchors.centerIn: parent
            source: "qrc:/icons/ui/speedboat.svg"
            width: Math.round(parent.width * 0.6)
            height: width
            sourceSize.width: width * 2
            sourceSize.height: height * 2
            opacity: 0.8
        }
        Text {
            anchors.top: parent.bottom
            anchors.topMargin: Math.round(2 * missionLayer._s)
            anchors.horizontalCenter: parent.horizontalCenter
            text: !missionLayer.vehicle ? ""
                  : missionLayer.vehicle.ghostMoving ? qsTr("no link %1 s").arg(missionLayer.vehicle.ghostAgeS)
                  : qsTr("no link %1 s, last seen here").arg(missionLayer.vehicle.ghostAgeS)
            color: "#FFFFFF"
            style: Text.Outline
            styleColor: "#CC000000"
            font.pixelSize: Tokens.fontSm
            font.bold: true
        }
    }

    // waypoints
    Repeater {
        model: missionLayer._pts.length
        delegate: Item {
            id: wp
            required property int index
            readonly property var pt: index < missionLayer._pts.length ? missionLayer._pts[index] : null
            readonly property bool isTarget: missionLayer._running && index === missionLayer._target
            readonly property bool isSelected: index === missionLayer.selectedIndex
            readonly property bool done: missionLayer._running && index < missionLayer._target
            visible: pt !== null && pt.ok
            width: missionLayer.hitSize
            height: missionLayer.hitSize
            x: (pt && pt.ok) ? Math.round(pt.x - width / 2) : 0
            y: (pt && pt.ok) ? Math.round(pt.y - height / 2) : 0
            z: isSelected || isTarget ? 2 : 1

            Rectangle {
                anchors.centerIn: parent
                width: wp.isSelected || wp.isTarget ? Math.round(missionLayer.markerSize * 1.15) : missionLayer.markerSize
                height: width
                radius: width / 2
                color: wp.isTarget ? AppPalette.accent : (wp.done ? "#CC5B6470" : "#E61B1F24")
                border.width: Math.max(2, Math.round((wp.isSelected ? 3 : 2) * missionLayer._s))
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
                cursorShape: missionLayer.editable ? Qt.OpenHandCursor : Qt.PointingHandCursor
                preventStealing: true
                property point pressPos
                property bool dragging: false
                onPressed: function(mouse) {
                    pressPos = Qt.point(mouse.x, mouse.y)
                    dragging = false
                }
                onPositionChanged: function(mouse) {
                    if (!missionLayer.editable)
                        return
                    var dx = mouse.x - pressPos.x, dy = mouse.y - pressPos.y
                    if (!dragging && dx * dx + dy * dy > 64)
                        dragging = true
                    if (dragging) {
                        var p = mapToItem(missionLayer, mouse.x, mouse.y)
                        missionLayer._dragIndex = wp.index
                        missionLayer._dragPos = p
                        missionLayer.refresh()
                    }
                }
                onReleased: function(mouse) {
                    if (dragging && missionLayer.view && missionLayer.vehicle) {
                        var p = mapToItem(missionLayer, mouse.x, mouse.y)
                        var g = missionLayer.view.screenToGeo(p.x, p.y)
                        if (g.valid)
                            missionLayer.vehicle.movePoint(wp.index, g.lat, g.lon)
                    } else {
                        missionLayer.pointClicked(wp.index)
                    }
                    dragging = false
                    missionLayer._dragIndex = -1
                    missionLayer.refresh()
                }
                onCanceled: {
                    dragging = false
                    missionLayer._dragIndex = -1
                    missionLayer.refresh()
                }
            }
        }
    }
}

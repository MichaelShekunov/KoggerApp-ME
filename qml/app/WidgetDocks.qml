import QtQuick 2.15
import kqml_types 1.0
import "DockLayout.js" as DockLayout

// The docks of WorkspaceStore.dockLayout (docs/widget-docking.ru.md): a solid strip under each dock's widgets, the
// line along its inner edge and between its widgets, and the tab on that edge. A tap on the tab collapses the dock
// (the workspace takes its room, the tab stays at the window's edge) and brings it back; the tab dragged across the
// edge sets the dock's thickness, one of its scale steps, shown by a ghost line and applied when let go. The widgets
// draw themselves over the strip (BasePanePopup.edgeDocked). While a widget is dragged to an edge, the ghost of the
// dock as it would be (WorkspaceStore.dockPreview), red when it would leave the workspace less than half the window.
Item {
    id: docksRoot

    required property var store

    readonly property int tabDepth: Math.round(22 * AppPalette.scale)
    readonly property int tabLength: Math.round(64 * AppPalette.scale)
    readonly property int tabRadius: Math.round(6 * AppPalette.scale)
    // Finger room around the tab: a touch a little off it still lands.
    readonly property int tabGrab: Math.round(12 * AppPalette.scale)
    // How far a finger goes on the tab before it is a drag, not a tap.
    readonly property int dragStart: Math.round(10 * AppPalette.scale)

    // The tab being dragged for a thickness: its side and the inner edge the dock would have.
    property string thickSide: ""
    property real thickEdge: 0

    Repeater {
        model: DockLayout.SIDES.length

        delegate: Item {
            id: dock

            required property int index
            readonly property string side: DockLayout.SIDES[index]
            readonly property bool vertical: DockLayout.isVertical(side)
            readonly property bool atStart: side === "left" || side === "top"   // the workspace after it
            readonly property var geom: docksRoot.store ? docksRoot.store.dockLayout.docks[side] : null
            readonly property bool used: !!geom && geom.ids.length > 0
            readonly property bool open: used && !geom.collapsed && geom.thickness > 0
            readonly property int edgeLine: docksRoot.store ? docksRoot.store.dockLines.edge : 1

            objectName: "dock_" + side
            anchors.fill: parent
            visible: used

            Rectangle {
                id: strip
                visible: dock.open
                x: dock.geom ? dock.geom.x : 0
                y: dock.geom ? dock.geom.y : 0
                width: dock.geom ? dock.geom.w : 0
                height: dock.geom ? dock.geom.h : 0
                color: AppPalette.bg

                // Along the inner edge, as the border between two panes.
                Rectangle {
                    color: AppPalette.borderFocus
                    x: dock.vertical ? (dock.atStart ? parent.width - width : 0) : 0
                    y: dock.vertical ? 0 : (dock.atStart ? parent.height - height : 0)
                    width: dock.vertical ? dock.edgeLine : parent.width
                    height: dock.vertical ? parent.height : dock.edgeLine
                }

                // Between two widgets: the line in the gap before each but the first.
                Repeater {
                    model: dock.open ? dock.geom.ids.length - 1 : 0
                    delegate: Rectangle {
                        required property int index
                        readonly property var next: docksRoot.store.dockLayout.widgets[dock.geom.ids[index + 1]]
                        readonly property int gap: docksRoot.store.dockLines.gap
                        color: AppPalette.border
                        x: dock.vertical ? 0 : (next ? next.x - gap - strip.x : 0)
                        y: dock.vertical ? (next ? next.y - gap - strip.y : 0) : 0
                        width: dock.vertical ? strip.width : gap
                        height: dock.vertical ? gap : strip.height
                    }
                }
            }

            // The tab: in the middle of the inner edge, out into the workspace; at the window's edge when collapsed.
            Item {
                id: tab
                objectName: "dockTab_" + dock.side
                readonly property real along: dock.geom ? Math.round((dock.geom.from + dock.geom.to) / 2) : 0
                readonly property real edge: dock.geom ? dock.geom.edge : 0
                width: dock.vertical ? docksRoot.tabDepth : docksRoot.tabLength
                height: dock.vertical ? docksRoot.tabLength : docksRoot.tabDepth
                x: dock.vertical ? (dock.atStart ? edge : edge - width) : along - width / 2
                y: dock.vertical ? along - height / 2 : (dock.atStart ? edge : edge - height)

                // Rounded on the workspace's side only: the rest runs on under the dock's edge, cut off here.
                Item {
                    anchors.fill: parent
                    clip: true

                    Rectangle {
                        readonly property int r: docksRoot.tabRadius
                        x: dock.vertical && dock.atStart ? -r : 0
                        y: !dock.vertical && dock.atStart ? -r : 0
                        width: parent.width + (dock.vertical ? r : 0)
                        height: parent.height + (dock.vertical ? 0 : r)
                        radius: r
                        color: tabArea.pressed ? AppPalette.cardHover : AppPalette.bg
                        border.width: dock.edgeLine
                        border.color: tabArea.dragging ? "#93C5FD" : AppPalette.borderFocus
                    }
                }

                // Where a tap sends the dock: to its edge when open, out of it when collapsed.
                Text {
                    anchors.centerIn: parent
                    text: dock.atStart !== (dock.used && dock.geom.collapsed) ? "‹" : "›"
                    rotation: dock.vertical ? 0 : 90
                    color: AppPalette.textSecond
                    font.pixelSize: Math.round(22 * AppPalette.scale)
                }

                MouseArea {
                    id: tabArea
                    objectName: "dockTabArea_" + dock.side
                    anchors.fill: parent
                    anchors.margins: -docksRoot.tabGrab
                    preventStealing: true

                    property bool dragging: false
                    property point start: Qt.point(0, 0)
                    property var steps: []
                    property real startThickness: 0
                    property int pick: -1   // the step the ghost shows

                    function edgeFor(thickness) {
                        return dock.atStart ? thickness : (dock.vertical ? docksRoot.width : docksRoot.height) - thickness
                    }

                    onPressed: function(mouse) {
                        start = mapToItem(docksRoot, mouse.x, mouse.y)
                        dragging = false
                        pick = -1
                        steps = docksRoot.store.dockScaleSteps(dock.side)
                        startThickness = dock.open ? dock.geom.thickness : 0
                    }
                    onPositionChanged: function(mouse) {
                        var p = mapToItem(docksRoot, mouse.x, mouse.y)
                        var moved = dock.vertical ? p.x - start.x : p.y - start.y
                        if (!dragging && Math.abs(moved) < docksRoot.dragStart)
                            return
                        dragging = true
                        var want = startThickness + (dock.atStart ? moved : -moved)
                        var best = -1, bestD = Infinity
                        for (var i = 0; i < steps.length; ++i) {
                            var d = Math.abs(steps[i].thickness - want)
                            if (d < bestD) { bestD = d; best = i }
                        }
                        pick = best
                        if (best >= 0) {
                            docksRoot.thickSide = dock.side
                            docksRoot.thickEdge = edgeFor(steps[best].thickness)
                        }
                    }
                    onReleased: {
                        if (dragging && pick >= 0)
                            docksRoot.store.setDockScale(dock.side, steps[pick].scale, true)
                        else if (!dragging)
                            docksRoot.store.toggleDockCollapsed(dock.side)
                        dragging = false
                        pick = -1
                        docksRoot.thickSide = ""
                    }
                    onCanceled: {
                        dragging = false
                        pick = -1
                        docksRoot.thickSide = ""
                    }
                }
            }
        }
    }

    // The ghosts go over the docked widgets, so they are the widgets' siblings: a drag's under the widget dragged,
    // the thickness line over all of them.
    Item {
        id: ghosts
        parent: docksRoot.parent
        anchors.fill: parent
        readonly property var pv: docksRoot.store ? docksRoot.store.dockPreview : null
        readonly property var pvDock: pv && pv.layout ? pv.layout.docks[pv.side] : null
        readonly property var pvSlot: pv && pv.layout ? pv.layout.widgets[pv.id] : null
        readonly property color tint: pv && !pv.fits ? "#F87171" : "#93C5FD"
        z: pv ? ZOrder.widgetPopup + docksRoot.store.widgetStackRank(pv.id) - 0.5
              : ZOrder.widgetPopup + (docksRoot.store ? docksRoot.store.widgetStackSlots : 0)
        visible: (!!pvDock && pvDock.thickness > 0) || docksRoot.thickSide !== ""

        // The dock as it would be, the dragged widget's place in it brighter.
        Rectangle {
            objectName: "dockGhost"
            visible: !!ghosts.pvDock && ghosts.pvDock.thickness > 0
            x: ghosts.pvDock ? ghosts.pvDock.x : 0
            y: ghosts.pvDock ? ghosts.pvDock.y : 0
            width: ghosts.pvDock ? ghosts.pvDock.w : 0
            height: ghosts.pvDock ? ghosts.pvDock.h : 0
            color: Qt.rgba(ghosts.tint.r, ghosts.tint.g, ghosts.tint.b, 0.14)
            border.color: ghosts.tint
            border.width: 2
        }
        Rectangle {
            visible: !!ghosts.pvSlot && ghosts.pvSlot.w > 0 && ghosts.pvSlot.h > 0
            x: ghosts.pvSlot ? ghosts.pvSlot.x : 0
            y: ghosts.pvSlot ? ghosts.pvSlot.y : 0
            width: ghosts.pvSlot ? ghosts.pvSlot.w : 0
            height: ghosts.pvSlot ? ghosts.pvSlot.h : 0
            color: Qt.rgba(ghosts.tint.r, ghosts.tint.g, ghosts.tint.b, 0.28)
            border.color: ghosts.tint
            border.width: 2
        }

        // The inner edge a dragged tab would give its dock.
        Rectangle {
            objectName: "dockThicknessGhost"
            readonly property bool vertical: DockLayout.isVertical(docksRoot.thickSide)
            readonly property var geom: docksRoot.thickSide !== "" && docksRoot.store
                                        ? docksRoot.store.dockLayout.docks[docksRoot.thickSide] : null
            readonly property int band: 4
            visible: !!geom
            color: "#93C5FD"
            opacity: 0.85
            radius: 2
            x: vertical ? docksRoot.thickEdge - band / 2 : (geom ? geom.from : 0)
            y: vertical ? (geom ? geom.from : 0) : docksRoot.thickEdge - band / 2
            width: vertical ? band : (geom ? geom.to - geom.from : 0)
            height: vertical ? (geom ? geom.to - geom.from : 0) : band
        }
    }
}

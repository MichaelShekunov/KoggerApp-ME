import QtQuick 2.15
import kqml_types 1.0
import "DockLayout.js" as DockLayout

// The docks of WorkspaceStore.dockLayout (docs/widget-docking.ru.md): a solid strip under each dock's widgets, the
// line along its inner edge and between its widgets, and the tab on that edge. A tap on the tab collapses the dock
// (the workspace takes its room, the tab stays at the window's edge) and brings it back. The widgets draw themselves
// over the strip (BasePanePopup.edgeDocked).
Item {
    id: docksRoot

    required property var store

    readonly property int tabDepth: Math.round(22 * AppPalette.scale)
    readonly property int tabLength: Math.round(64 * AppPalette.scale)
    readonly property int tabRadius: Math.round(6 * AppPalette.scale)
    // Finger room around the tab: a touch a little off it still lands.
    readonly property int tabGrab: Math.round(12 * AppPalette.scale)

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
                        color: AppPalette.bg
                        border.width: dock.edgeLine
                        border.color: AppPalette.borderFocus
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
                    objectName: "dockTabArea_" + dock.side
                    anchors.fill: parent
                    anchors.margins: -docksRoot.tabGrab
                    preventStealing: true
                    onClicked: docksRoot.store.toggleDockCollapsed(dock.side)
                }
            }
        }
    }
}

import QtQuick 2.15

Item {
    id: root

    property var paneData
    property int leafId: -1
    property bool rotateEnabled: false
    property bool active: true
    property var workspaceRoot: null
    property string videoContentIdOverride: ""

    readonly property string paneMode: paneData && paneData.mode === "3D" ? "3D"
                                     : paneData && paneData.mode === "Video" ? "Video"
                                     : paneData && paneData.mode === "2/3D" ? "2/3D"
                                     : "2D"

    Loader {
        anchors.fill: parent
        active: root.active
        // "2/3D" reuses Pane3DWindow rather than duplicating it: both drive the
        // same single Scene3DView, and the only difference is which layers it
        // shows. A separate window file would be four hundred lines of copy
        // waiting to drift.
        sourceComponent: root.paneMode === "3D" ? pane3DComponent
                       : root.paneMode === "2/3D" ? pane23DComponent
                       : root.paneMode === "Video" ? paneVideoComponent
                       : pane2DComponent
    }

    Component {
        id: pane2DComponent

        Pane2DWindow {
            rotateEnabled: root.rotateEnabled
            workspaceRoot: root.workspaceRoot
            leafId: root.leafId
            paneData: root.paneData
        }
    }

    Component {
        id: pane3DComponent

        Pane3DWindow {
            rotateEnabled: root.rotateEnabled
            workspaceRoot: root.workspaceRoot
            leafId: root.leafId
            paneData: root.paneData
        }
    }

    Component {
        id: pane23DComponent

        Pane3DWindow {
            rotateEnabled: root.rotateEnabled
            workspaceRoot: root.workspaceRoot
            leafId: root.leafId
            paneData: root.paneData
            reliefOnly: true
        }
    }

    Component {
        id: paneVideoComponent

        PaneVideoWindow {
            rotateEnabled: root.rotateEnabled
            workspaceRoot: root.workspaceRoot
            leafId: root.leafId
            paneData: root.paneData
            videoContentIdOverride: root.videoContentIdOverride
        }
    }
}

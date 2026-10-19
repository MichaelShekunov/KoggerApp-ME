import QtQuick 2.15
import kqml_types 1.0

// Floating echogram-edit palette. Reuses BasePanePopup's drag / snap /
// collision / position-persistence, but renders as a pill of three columns
// (transparent frame; no resize / collapse / fullscreen). Header KDragBar is
// the drag handle. Left column: close and the bottom-line tools; middle: the
// history, the ping tools and the trim; right: the brushes and the
// whole-recording filters (EchogramEditor, "echoEdit"; docs/echogram-editing.md).
BasePanePopup {
    id: root

    required property var store

    readonly property real _s: AppPalette.appScale
    readonly property int _controlH: Math.round(36 * _s)
    readonly property int _sidePad: Math.round(3 * _s)
    readonly property int _gap: Math.round(6 * _s)

    readonly property var btTools: [
        { tool: 0, icon: "qrc:/icons/ui/direction_arrows.svg",  tip: qsTr("Navigate") },
        { tool: 1, icon: "qrc:/icons/ui/pencil.svg",            tip: qsTr("Draw bottom track") },
        { tool: 2, icon: "qrc:/icons/ui/arrow_bar_to_up.svg",   tip: qsTr("Raise bottom track") },
        { tool: 3, icon: "qrc:/icons/ui/arrow_bar_to_down.svg", tip: qsTr("Lower bottom track") },
        { tool: 4, icon: "qrc:/icons/ui/eraser.svg",            tip: qsTr("Erase bottom track") }
    ]
    readonly property var pingTools: [
        { tool: 5, icon: "qrc:/icons/ui/ban.svg",             tip: qsTr("Mark bad pings: they are left out of the mosaic, the bottom surface and the export") },
        { tool: 6, icon: "qrc:/icons/ui/circle-check.svg",    tip: qsTr("Unmark pings") },
        { tool: 7, icon: "qrc:/icons/ui/arrow-bar-right.svg", tip: qsTr("The recording starts here: what is before is left out") },
        { tool: 8, icon: "qrc:/icons/ui/arrow-bar-left.svg",  tip: qsTr("The recording ends here: what is after is left out") }
    ]
    readonly property var brushTools: [
        { tool: 9,  icon: "qrc:/icons/ui/wave-sine.svg",     tip: qsTr("Smooth the bottom: each pass gives a ping the median depth of its neighbours") },
        { tool: 10, icon: "qrc:/icons/ui/vector-spline.svg", tip: qsTr("Fill the gaps of the bottom: a straight line between the depths around them") },
        { tool: 11, icon: "qrc:/icons/ui/map-off.svg",       tip: qsTr("Leave the side under the cursor out of the mosaic; its depths stay (\"Unmark pings\" puts it back)") }
    ]
    readonly property int _btTool: (typeof core !== "undefined" && core) ? core.bottomTrackEditTool : 0
    readonly property var _ed: (typeof echoEdit !== "undefined" && echoEdit) ? echoEdit : null

    // Left: close, 5 tools, the magnet; middle: undo, redo, 4 tools, trim off, reset;
    // right: 3 brushes, the spikes, the turns.
    readonly property int _rows: Math.max(btTools.length + 2, pingTools.length + 4, brushTools.length + 2)
    readonly property real _pillW: _controlH * 3 + _gap * 2 + _sidePad * 2
    readonly property real _pillH: _rows * _controlH + (_rows - 1) * _gap + _sidePad * 2
    readonly property real _wantW: _pillW + contentPadding * 2
    readonly property real _wantH: headerHeight + _pillH + contentPadding

    // What a step of the history was, for the undo and redo tooltips (echoedit::Command::Kind).
    function kindName(kind) {
        switch (kind) {
        case 0:  return qsTr("pencil")
        case 1:  return qsTr("eraser")
        case 2:  return qsTr("raise")
        case 3:  return qsTr("lower")
        case 4:  return qsTr("pings marked bad")
        case 5:  return qsTr("pings unmarked")
        case 6:  return qsTr("points deleted in 3D")
        case 7:  return qsTr("recording start")
        case 8:  return qsTr("recording end")
        case 9:  return qsTr("trim removed")
        case 10: return qsTr("all edits reset")
        case 11: return qsTr("smoothing")
        case 12: return qsTr("gaps filled")
        case 13: return qsTr("sides left out of the mosaic")
        case 14: return qsTr("spikes removed")
        case 15: return qsTr("turns left out of the mosaic")
        }
        return ""
    }

    popupVisible: store.bottomTrackEditorOpen
    dragEnabled: true
    resizeEnabled: false
    collapseButtonVisible: false
    fullscreenMode: false
    panelColor: "transparent"
    panelBorderColor: "transparent"
    ghostFollowsContent: true
    ghostGripAndContent: true
    ghostRadius: _controlH / 2 + _sidePad
    headerDragBarLength: Math.max(Math.round(24 * _s), _pillW - _sidePad * 2)
    siblingSnapAlignTop: true
    snapEdgeCenters: true

    function _applySize() {
        expandedWidth = _wantW
        expandedHeight = _wantH
    }

    property bool _synced: false

    function syncFromStore() {
        if (!popupVisible)
            return
        suspendSignals = true
        var p = store.btEditPopupPosition(popupWidth, popupHeight)
        panelX = clampX(p.x)
        panelY = clampY(p.y)
        suspendSignals = false
        _synced = true
    }

    on_WantWChanged: _applySize()
    on_WantHChanged: _applySize()

    Component.onCompleted: {
        _applySize()
        syncFromStore()
        Qt.callLater(syncFromStore)
        Qt.callLater(resolveOverlapWithSibling)
    }

    onPopupVisibleChanged: {
        if (popupVisible) {
            _applySize()
            syncFromStore()
            Qt.callLater(syncFromStore)
            Qt.callLater(resolveOverlapWithSibling)
        } else if (typeof core !== "undefined" && core) {
            core.setBottomTrackEditTool(0)   // closing resets to navigation
        }
    }

    onPositionCommitted: function(x, y, w, h) {
        if (_synced)
            store.setBtEditPopupPosition(x, y, w, h)
    }

    dockState: store ? store.popupDock(popupId) : null
    onDockCommitted: function(targetId, side, gap, crossOffset) {
        store.setPopupDock(popupId, { targetId: targetId, side: side, gap: gap, cross: crossOffset })
    }

    onCloseRequested: store.bottomTrackEditorOpen = false

    // "Reset all edits" asks twice: the first press arms it for a few seconds.
    property bool _resetArmed: false
    Timer {
        id: resetDisarm
        interval: 4000
        onTriggered: root._resetArmed = false
    }

    component ToolButton: KCircleIconButton {
        property bool selected: false
        property bool danger: false
        width: root._controlH
        height: root._controlH
        iconTintColor: danger ? AppPalette.dangerText : AppPalette.text
        fillColor:        danger ? AppPalette.dangerBg : (selected ? AppPalette.accentBgStrong : AppPalette.card)
        fillHoverColor:   danger ? AppPalette.dangerHover : (selected ? AppPalette.accentBorder : AppPalette.cardHover)
        fillPressedColor: AppPalette.bgDeep
        borderColor:      danger ? AppPalette.dangerBorder : (selected ? AppPalette.accentBorder : AppPalette.border)
        borderHoverColor: danger ? AppPalette.dangerBorder : (selected ? AppPalette.accentBorder : AppPalette.borderHover)
        borderWidth: danger ? Math.max(1, Tokens.cardBorderWidth) : Tokens.cardBorderWidth
    }

    // ── Content pill (fills the content area below the header drag bar) ──
    Rectangle {
        id: pill
        anchors.fill: parent
        radius: root._controlH / 2 + root._sidePad
        color: AppPalette.bg
        border.width: 0

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            y: root._sidePad
            spacing: root._gap

            Column {
                spacing: root._gap

                ToolButton {
                    iconSource: "qrc:/icons/ui/x.svg"
                    onClicked: root.store.bottomTrackEditorOpen = false
                }

                Repeater {
                    model: root.btTools
                    delegate: ToolButton {
                        required property var modelData
                        selected: root._btTool === modelData.tool
                        iconSource: modelData.icon
                        toolTipText: modelData.tip
                        onClicked: {
                            if (typeof core !== "undefined" && core)
                                core.setBottomTrackEditTool(modelData.tool)
                        }
                    }
                }

                ToolButton {
                    objectName: "echoEditMagnet"
                    selected: !!root._ed && root._ed.magnet
                    iconSource: "qrc:/icons/ui/magnet.svg"
                    toolTipText: qsTr("Magnet: the pencil snaps to the bottom the bottom track finds near it")
                    onClicked: if (root._ed) root._ed.magnet = !root._ed.magnet
                }
            }

            Column {
                spacing: root._gap

                ToolButton {
                    objectName: "echoEditUndo"
                    enabled: !!root._ed && root._ed.canUndo
                    iconSource: "qrc:/icons/ui/arrow-back-up.svg"
                    toolTipText: (root._ed && root._ed.undoKind >= 0)
                                 ? qsTr("Undo: %1 (Ctrl+Z)").arg(root.kindName(root._ed.undoKind))
                                 : qsTr("Undo (Ctrl+Z)")
                    onClicked: if (root._ed) root._ed.undo()
                }

                ToolButton {
                    objectName: "echoEditRedo"
                    enabled: !!root._ed && root._ed.canRedo
                    iconSource: "qrc:/icons/ui/arrow-forward-up.svg"
                    toolTipText: (root._ed && root._ed.redoKind >= 0)
                                 ? qsTr("Redo: %1 (Ctrl+Y)").arg(root.kindName(root._ed.redoKind))
                                 : qsTr("Redo (Ctrl+Y)")
                    onClicked: if (root._ed) root._ed.redo()
                }

                Repeater {
                    model: root.pingTools
                    delegate: ToolButton {
                        required property var modelData
                        selected: root._btTool === modelData.tool
                        iconSource: modelData.icon
                        toolTipText: modelData.tip
                        onClicked: {
                            if (typeof core !== "undefined" && core)
                                core.setBottomTrackEditTool(modelData.tool)
                        }
                    }
                }

                ToolButton {
                    objectName: "echoEditClearTrim"
                    enabled: !!root._ed && root._ed.trimmed
                    iconSource: "qrc:/icons/ui/arrows-horizontal.svg"
                    toolTipText: qsTr("The whole recording again: the trim removed")
                    onClicked: if (root._ed) root._ed.clearTrim()
                }

                ToolButton {
                    objectName: "echoEditReset"
                    enabled: !!root._ed && root._ed.hasEdits
                    danger: root._resetArmed
                    iconSource: "qrc:/icons/ui/trash.svg"
                    toolTipText: root._resetArmed
                                 ? qsTr("Press again to reset every edit of this recording (Undo brings them back)")
                                 : qsTr("Reset every edit of this recording")
                    onClicked: {
                        if (!root._ed)
                            return
                        if (!root._resetArmed) {
                            root._resetArmed = true
                            resetDisarm.restart()
                            return
                        }
                        root._resetArmed = false
                        resetDisarm.stop()
                        root._ed.resetAll()
                    }
                }
            }

            Column {
                spacing: root._gap

                Repeater {
                    model: root.brushTools
                    delegate: ToolButton {
                        required property var modelData
                        selected: root._btTool === modelData.tool
                        iconSource: modelData.icon
                        toolTipText: modelData.tip
                        onClicked: {
                            if (typeof core !== "undefined" && core)
                                core.setBottomTrackEditTool(modelData.tool)
                        }
                    }
                }

                ToolButton {
                    objectName: "echoEditDespike"
                    enabled: !!root._ed
                    iconSource: "qrc:/icons/ui/activity.svg"
                    toolTipText: qsTr("Remove the spikes of the bottom over the whole recording (one step to undo)")
                    onClicked: if (root._ed) root._ed.removeSpikes()
                }

                ToolButton {
                    objectName: "echoEditTurns"
                    enabled: !!root._ed
                    iconSource: "qrc:/icons/ui/s-turn-right.svg"
                    toolTipText: qsTr("Leave the turns out of the mosaic over the whole recording (one step to undo)")
                    onClicked: if (root._ed) root._ed.turnsOutOfMosaic()
                }
            }
        }
    }
}

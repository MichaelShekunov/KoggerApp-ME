import QtQuick 2.15
import kqml_types 1.0

BasePanePopup {
    id: root

    required property var store
    required property var def
    property real widgetScale: 1.0

    readonly property var _ds: (typeof dataset !== "undefined") ? dataset : null
    readonly property var _dmw: (typeof deviceManagerWrapper !== "undefined") ? deviceManagerWrapper : null
    readonly property var _sysbat: (typeof systemBattery !== "undefined") ? systemBattery : null
    readonly property var _sysmon: (typeof systemMonitor !== "undefined") ? systemMonitor : null
    readonly property var _boat: (typeof boat !== "undefined") ? boat : null

    readonly property real _bgAlpha: {
        var t = (def && typeof def.transparency === "number") ? def.transparency : 0
        return Math.max(0, Math.min(1, 1 - t / 100))
    }

    readonly property real _appScale: AppPalette.appScale
    readonly property real _k: _appScale * widgetScale

    readonly property int _cols: def ? def.cols : 1
    readonly property int _rows: def ? def.rows : 1

    readonly property real _cellW: Math.round(84 * _k)
    readonly property real _cellH: Math.round(84 * _k)
    readonly property real _gap:   Math.round(4 * _k)
    readonly property real _pad:   Math.round(8 * _k)

    readonly property real _gridW: _cols * _cellW + (_cols - 1) * _gap
    readonly property real _gridH: _rows * _cellH + (_rows - 1) * _gap

    readonly property real _baseCellW: Math.round(84 * _appScale)
    readonly property real _baseCellH: Math.round(84 * _appScale)
    readonly property real _baseGap:   Math.round(4 * _appScale)
    readonly property real _basePad:   Math.round(8 * _appScale)
    readonly property real _baseW: _cols * _baseCellW + (_cols - 1) * _baseGap + _basePad * 2 + contentPadding * 2
    readonly property real _baseH: _rows * _baseCellH + (_rows - 1) * _baseGap + _basePad * 2 + contentPadding * 2

    // Docked to a window edge, the grid fills the slot the dock gives it, at the dock's scale (WorkspaceStore.dockLayout);
    // lifted out of it by a drag, it is the floating grid again.
    readonly property var _slot: (store && def && def.id) ? store.widgetDockSlot(def.id) : null
    readonly property var _shownSlot: _inSlot ? _slot : null
    readonly property real _kShown: _shownSlot ? _appScale * _shownSlot.scale : _k
    readonly property real _gapShown: _shownSlot ? Math.round(4 * _kShown) : _gap
    readonly property real _padShown: _shownSlot ? Math.round(8 * _kShown) : _pad
    readonly property real _gridShownW: _shownSlot ? Math.max(0, _shownSlot.w - 2 * (_padShown + contentPadding)) : _gridW
    readonly property real _gridShownH: _shownSlot ? Math.max(0, _shownSlot.h - 2 * (_padShown + contentPadding)) : _gridH
    readonly property real _cellShownW: _shownSlot ? Math.max(0, (_gridShownW - (_cols - 1) * _gapShown) / _cols) : _cellW
    readonly property real _cellShownH: _shownSlot ? Math.max(0, (_gridShownH - (_rows - 1) * _gapShown) / _rows) : _cellH

    edgeDocked: !!_slot
    edgeCollapsed: !!_slot && _slot.collapsed === true
    edgeX: _slot ? _slot.x : 0
    edgeY: _slot ? _slot.y : 0
    edgeW: _slot ? _slot.w : 0
    edgeH: _slot ? _slot.h : 0

    function _reportNaturalSize() {
        if (store && def && def.id)
            store.reportWidgetNaturalSize(def.id, _baseW, _baseH, true)
    }
    on_BaseWChanged: _reportNaturalSize()
    on_BaseHChanged: _reportNaturalSize()

    readonly property bool _beingEdited: !!(store && store.widgetEditorActive
        && store.widgetEditIndex >= 0 && store.widgetEditIndex < store.widgets.length
        && store.widgets[store.widgetEditIndex] && def
        && store.widgets[store.widgetEditIndex].id === def.id)

    popupVisible: !_beingEdited
    // Docked, it is dragged out in the layout edit mode, or after a long press (BasePanePopup).
    dragEnabled: !(store && store.widgetEditorActive) && (!edgeDocked || (store && store.editableMode))
    edgeDockable: !(store && store.widgetEditorActive)
    onEdgeDragMoved: function(x, y) { if (store && def && def.id) edgeTarget = store.dockTargetAt(def.id, x, y) }
    onEdgeTargetChanged: if (store) store.setDockPreview(edgeTarget)
    onEdgeDockCommitted: function(side, index) { if (store && def && def.id) store.dockWidget(def.id, side, index) }
    onEdgeUndockCommitted: if (store && def && def.id) store.undockWidget(def.id)
    dragAnywhere: true
    headerReserved: false
    resizeEnabled: false
    collapseButtonVisible: false
    fullscreenMode: false
    panelColor: "transparent"
    panelBorderColor: "transparent"
    panelRadius: Tokens.radiusLg
    ghostFollowsContent: true
    ghostRadius: Tokens.radiusLg
    headerDragBarLength: 0
    snapEdgeCenters: true

    readonly property var _scaleSteps: [0.75, 1.0, 1.25, 1.5, 2.0, 2.5]
    resizeSnapSizes: {
        var out = []
        for (var i = 0; i < _scaleSteps.length; ++i)
            out.push(Qt.size(Math.round(_baseW * _scaleSteps[i]), Math.round(_baseH * _scaleSteps[i])))
        return out
    }

    function _applyScale() {
        expandedWidth  = Math.round(_gridW + _pad * 2 + contentPadding * 2)
        expandedHeight = Math.round(_gridH + _pad * 2 + contentPadding * 2)
    }

    property bool _synced: false

    function syncFromStore() {
        if (!def || !def.id)
            return
        widgetScale = store.widgetScale(def.id)
        _applyScale()
        var p = store.widgetPosition(def.id, popupWidth, popupHeight)
        var rb = store.widgetRevealBounds(popupWidth, popupHeight)
        var nx = Math.max(rb.minX, Math.min(rb.maxX, p.x))
        var ny = Math.max(rb.minY, Math.min(rb.maxY, p.y))
        suspendSignals = true
        panelX = clampX(nx)
        panelY = clampY(ny)
        suspendSignals = false
        _synced = true
    }

    onWidgetScaleChanged: _applyScale()
    on_GridWChanged: _applyScale()
    on_GridHChanged: _applyScale()
    onDefChanged: { _applyScale(); _reportNaturalSize(); Qt.callLater(syncFromStore) }

    Component.onCompleted: {
        _reportNaturalSize()
        syncFromStore()
        Qt.callLater(syncFromStore)
        Qt.callLater(resolveOverlapWithSibling)
    }

    onPositionCommitted: function(x, y, w, h) {
        if (_synced && def && def.id)
            store.setWidgetPosition(def.id, x, y, w, h)
    }

    onSizeCommitted: function(w, h) {
        if (!def || !def.id)
            return
        widgetScale = Math.max(0.75, Math.min(2.5, w / _baseW))
        store.setWidgetScale(def.id, widgetScale)
    }

    onInteractionStarted: if (store && def && def.id) store.widgetBringToFront(def.id)

    dockState: (store && def && def.id) ? store.popupDock(popupId) : null
    onDockCommitted: function(targetId, side, gap, crossOffset) {
        store.setPopupDock(popupId, { targetId: targetId, side: side, gap: gap, cross: crossOffset })
    }

    Rectangle {
        anchors.fill: parent
        radius: root._inSlot ? 0 : root.panelRadius
        color: Qt.rgba(AppPalette.bg.r, AppPalette.bg.g, AppPalette.bg.b, root._bgAlpha)
        border.width: 0

        Item {
            width: root._gridShownW
            height: root._gridShownH
            anchors.centerIn: parent

            Repeater {
                model: root.def ? root.def.cells : []
                delegate: Item {
                    id: cell
                    required property var modelData
                    readonly property var _sp: root.store._cellSpan(modelData.rep, modelData.big === true)
                    readonly property string _field: modelData.field
                    // At whole pixels: a docked cell's size is a share of the dock, not a multiple of a pixel.
                    readonly property real _stepX: root._cellShownW + root._gapShown
                    readonly property real _stepY: root._cellShownH + root._gapShown

                    x: Math.round(modelData.col * _stepX)
                    y: Math.round(modelData.row * _stepY)
                    width: Math.round((modelData.col + _sp.sc) * _stepX - root._gapShown) - x
                    height: Math.round((modelData.row + _sp.sr) * _stepY - root._gapShown) - y

                    WidgetCellContent {
                        anchors.fill: parent
                        rep: cell.modelData.rep
                        label: DataFieldCatalog.label(cell._field)
                        value: DataFieldCatalog.formatValue(cell._field, root._ds, root._dmw, root.store, root._sysbat,
                                                            undefined, root._boat, root._sysmon)
                        k: root._kShown
                        gap: root._gapShown
                        bars: DataFieldCatalog.bars(cell._field, root._dmw, root._sysmon)
                        field: cell._field
                        sys: root._sysmon
                    }
                }
            }
        }
    }
}

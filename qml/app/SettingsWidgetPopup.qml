import QtQuick 2.15
import kqml_types 1.0

// A settings widget: settings pinned from the settings pane, each a row with the setting's own controls
// (SettingMirrorRow). Pin mode makes and fills them (WorkspaceStore.togglePinnedSetting); the pencil here opens
// pin mode on this widget, and in pin mode every row has a cross to take it out. It is one of store.widgets
// (kind "settings") and shares their machinery: position, z-rank, docking, the shown map, the list.
//
// SCALE IS FIXED AT 1, as the stand panel's: it takes input, and a spin box scaled by a corner grip stops being
// usable long before a text card stops being readable.
BasePanePopup {
    id: root

    required property var store
    required property var def

    readonly property real widgetScale: 1.0

    readonly property real _bgAlpha: {
        var t = (def && typeof def.transparency === "number") ? def.transparency : 0
        return Math.max(0.15, Math.min(1, 1 - t / 100))
    }

    readonly property real _s: AppPalette.scale
    readonly property real _barH: headerHeight
    readonly property real _barPad: contentPadding
    readonly property real _barBtn: Math.max(Math.round(18 * AppPalette.scale), _barH - _barPad * 2)
    readonly property real _pad: Math.round(9 * _s)
    readonly property bool _editing: PinRegistry.pinMode
    readonly property var _items: (def && Array.isArray(def.items)) ? def.items : []

    // As wide as the widest row wants, within bounds: a row that does not fit puts its label above its controls.
    property real _naturalMax: 0
    readonly property real _contentW: Math.round(Math.max(220 * _s, Math.min(460 * _s, _naturalMax)))
    readonly property real _contentH: _barH + _pad + rowsCol.implicitHeight

    function _remeasure() {
        var m = 0
        for (var i = 0; i < rowsRepeater.count; ++i) {
            var r = rowsRepeater.itemAt(i)
            if (r)
                m = Math.max(m, r.naturalWidth)
        }
        _naturalMax = m
    }

    popupVisible: true
    dragEnabled: true
    dragAnywhere: false
    // The title row is part of the content: BasePanePopup masks its content to rounded corners, which cuts a
    // title bar placed above the content, in the header strip.
    headerReserved: false
    dragHandleOpacity: 0
    resizeEnabled: false
    collapseButtonVisible: false
    fullscreenMode: false
    panelColor: Qt.rgba(AppPalette.bg.r, AppPalette.bg.g, AppPalette.bg.b, _bgAlpha)
    panelBorderColor: AppPalette.border
    panelRadius: Tokens.radiusLg
    ghostFollowsContent: true
    ghostRadius: Tokens.radiusLg
    snapEdgeCenters: true

    // Imperative, as in the other panel kinds: BasePanePopup writes into expandedWidth/Height itself, so a
    // binding here is broken the first time it does.
    function _applyScale() {
        expandedWidth  = Math.round(_contentW + _pad * 2 + contentPadding * 2)
        expandedHeight = Math.round(_contentH + _pad + _contentTopMargin + contentPadding)
    }

    property bool _synced: false

    function syncFromStore() {
        if (!def || !def.id)
            return
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

    on_ContentHChanged: _applyScale()
    on_ContentWChanged: _applyScale()
    onDefChanged: { Qt.callLater(_remeasure); _applyScale(); Qt.callLater(syncFromStore) }

    Component.onCompleted: {
        syncFromStore()
        Qt.callLater(syncFromStore)
        Qt.callLater(resolveOverlapWithSibling)
    }

    onPositionCommitted: function(x, y, w, h) {
        if (_synced && def && def.id)
            store.setWidgetPosition(def.id, x, y, w, h)
    }

    onInteractionStarted: if (store && def && def.id) store.widgetBringToFront(def.id)

    dockState: (store && def && def.id) ? store.popupDock(popupId) : null
    onDockCommitted: function(targetId, side, gap, crossOffset) {
        store.setPopupDock(popupId, { targetId: targetId, side: side, gap: gap, cross: crossOffset })
    }

    Item {
        id: titleBar
        x: root._pad
        width: root._contentW
        height: root._barH
        z: 10

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            cursorShape: Qt.OpenHandCursor
            onPressed: function(mouse) { mouse.accepted = true }
        }

        DragHandler {
            target: null
            enabled: root.dragEnabled && !root.collapsed && !root.fullscreenMode
            xAxis.enabled: true
            yAxis.enabled: true
            onActiveChanged: active ? root._beginDrag() : root._endDrag()
            onTranslationChanged: if (active) root._updateDrag(translation.x, translation.y)
        }

        Text {
            anchors.left: parent.left
            anchors.right: editBtn.left
            anchors.rightMargin: root._barPad
            anchors.verticalCenter: parent.verticalCenter
            text: root.def && root.def.name ? root.def.name : qsTr("Settings")
            color: AppPalette.textStrong
            font.pixelSize: Tokens.fontBase
            elide: Text.ElideRight
        }

        KCircleIconButton {
            id: closeBtn
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: root._barPad
            width: root._barBtn
            height: root._barBtn
            rounded: false
            cornerRadius: Tokens.radiusLg
            z: 1
            iconSource: ""
            glyph: "×"
            showGlyphWithIcon: true
            glyphPixelSize: Tokens.iconSm
            glyphColor: AppPalette.textSecond
            fillColor: AppPalette.controlRaised
            fillHoverColor: Qt.lighter(AppPalette.controlRaised, 1.2)
            fillPressedColor: AppPalette.bgDeep
            borderColor: AppPalette.border
            borderHoverColor: AppPalette.borderHover
            toolTipText: qsTr("Hide panel")
            onClicked: if (root.def && root.def.id) root.store.setWidgetShown(root.def.id, false)
        }

        KCircleIconButton {
            id: editBtn
            anchors.right: closeBtn.left
            anchors.rightMargin: root._barPad
            anchors.top: closeBtn.top
            width: root._barBtn
            height: root._barBtn
            rounded: false
            cornerRadius: Tokens.radiusLg
            z: 1
            iconSource: "qrc:/icons/ui/pencil.svg"
            iconTintColor: AppPalette.textSecond
            fillColor: AppPalette.controlRaised
            fillHoverColor: Qt.lighter(AppPalette.controlRaised, 1.2)
            fillPressedColor: AppPalette.bgDeep
            borderColor: AppPalette.border
            borderHoverColor: AppPalette.borderHover
            toolTipText: qsTr("Add or take out settings")
            onClicked: if (root.def && root.def.id) root.store.openSettingsPinMode(root.def.id)
        }
    }

    Item {
        x: root._pad
        y: root._barH + root._pad
        width: root._contentW
        height: rowsCol.implicitHeight

        Column {
            id: rowsCol
            width: parent.width
            spacing: Tokens.spaceSm

            Repeater {
                id: rowsRepeater
                model: root._items

                delegate: SettingMirrorRow {
                    required property var modelData
                    store: root.store
                    item: modelData
                    availableWidth: root._contentW
                    removable: root._editing
                    onNaturalWidthChanged: Qt.callLater(root._remeasure)
                    Component.onCompleted: Qt.callLater(root._remeasure)
                    onRemoveRequested: if (root.def && root.def.id) root.store.removePinnedSetting(root.def.id, modelData.key)
                }
            }
        }
    }
}

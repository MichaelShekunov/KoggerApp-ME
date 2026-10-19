import QtQuick 2.15

// The pins of pin mode over a settings page: one by every KPinTag whose row lies under `root` and can be seen --
// the row shown and not folded, its settings group open. Put it in the page's Flickable beside the content, so
// that the pins scroll with it, and leave a strip `pinX .. pinX + pinSize` free left of the content. The rows are
// found again a few times a second while pin mode is on: groups open and close, rows fold, with animations.
Item {
    id: overlay

    property Item root: null
    property real pinX: 0
    readonly property real pinSize: Tokens.controlHSm

    property int _tick: 0

    function _under(item) {
        for (var p = item; p; p = p.parent)
            if (p === overlay.root)
                return true
        return false
    }

    // A settings group keeps its body laid out while it is closed, only clipped and disabled: a row in it has
    // to be skipped by the group's own state, not by its visibility.
    function _groupOpen(item) {
        for (var p = item; p && p !== overlay.root; p = p.parent)
            if (typeof p.stateKey === "string" && typeof p._bodyShown === "boolean")
                return p._bodyShown
        return true
    }

    // Where the pin of a tag goes, in this item's coordinates; null when its row is not to be seen.
    function spotOf(tag) {
        if (!tag || !tag.key || !overlay.root)
            return null
        var b = tag.block
        if (!b || !b.visible || b.height < 1 || !_under(b) || !_groupOpen(b))
            return null
        var p = b.mapToItem(overlay, 0, 0)
        var lineH = Math.min(b.height, Tokens.rowH)
        return { x: overlay.pinX, y: Math.round(p.y + (lineH - overlay.pinSize) / 2) }
    }

    Timer {
        interval: 150
        repeat: true
        running: PinRegistry.pinMode && overlay.visible
        onTriggered: ++overlay._tick
    }

    Repeater {
        model: PinRegistry.pinMode ? PinRegistry.tags : []

        delegate: KCircleIconButton {
            id: pin
            required property var modelData

            readonly property var _spot: { overlay._tick; return overlay.spotOf(pin.modelData) }
            readonly property bool pinned: PinRegistry.isPinned(pin.modelData ? pin.modelData.key : "")

            visible: _spot !== null
            x: _spot ? _spot.x : 0
            y: _spot ? _spot.y : 0
            width: overlay.pinSize
            height: overlay.pinSize
            rounded: false
            cornerRadius: Tokens.radiusMd
            borderWidth: 0
            iconSource: "qrc:/icons/ui/pin.svg"
            iconPixelSize: Math.round(overlay.pinSize * 0.66)
            iconTintColor: pinned ? AppPalette.accentText : AppPalette.textSecond
            fillColor: pinned ? AppPalette.accentBgStrong : AppPalette.chipRaised
            fillHoverColor: pinned ? AppPalette.accentBgStrong : AppPalette.chipRaisedHover
            toolTipText: pinned ? qsTr("Remove from the widget") : qsTr("Pin as a widget")
            onClicked: if (pin.modelData) PinRegistry.toggleRequested(pin.modelData.key, pin.modelData.shownLabel)
        }
    }
}

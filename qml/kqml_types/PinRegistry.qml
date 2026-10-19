pragma Singleton
import QtQuick 2.15

// Settings that can be taken out of the settings pane as widgets ("pinned"). A KPinTag inside a settings row
// registers the row here under a stable key; KPinOverlay draws a pin by every registered row while pinMode is
// on; the app's store answers toggleRequested and keeps pinnedMap -- which keys are pinned, and where. Kept in
// kqml_types so that the rows need no reference to the app's store.
QtObject {
    id: reg

    property bool pinMode: false
    // key -> the id of the widget it is pinned to; filled by the store from its widget definitions.
    property var pinnedMap: ({})
    // The live tags, in the order they registered.
    property var tags: []
    // Bumped on every registration and unregistration, for bindings that look a tag up by key.
    property int revision: 0

    signal toggleRequested(string key, string label)

    function isPinned(key) { return !!(key && pinnedMap && pinnedMap[key]) }

    function register(tag) {
        if (!tag || tags.indexOf(tag) >= 0)
            return
        var next = tags.slice(0)
        next.push(tag)
        tags = next
        ++revision
    }

    function unregister(tag) {
        var i = tags.indexOf(tag)
        if (i < 0)
            return
        var next = tags.slice(0)
        next.splice(i, 1)
        tags = next
        ++revision
    }

    // The live tag of a key; the last one registered when there are several (a page built again).
    function tagOf(key) {
        for (var i = tags.length - 1; i >= 0; --i)
            if (tags[i] && tags[i].key === key)
                return tags[i]
        return null
    }

    // The controls of a row a widget mirrors: every item under it (or the row itself) that names its
    // controlKind -- KSwitch, KSpinBox, KCombo, KSlider, KTabBar -- in the order they are laid out.
    function controlsOf(block) {
        var out = []
        if (!block)
            return out
        if (typeof block.controlKind === "string" && block.controlKind.length > 0) {
            out.push(block)
            return out
        }
        var walk = function(item) {
            var kids = item.children
            for (var i = 0; i < kids.length; ++i) {
                var c = kids[i]
                if (!c)
                    continue
                if (typeof c.controlKind === "string" && c.controlKind.length > 0)
                    out.push(c)
                else
                    walk(c)
            }
        }
        walk(block)
        return out
    }
}

import QtQuick 2.15

// Marks a settings row as one that can be pinned as a widget: put it inside the row with a key (stable, never
// translated; "app:<group>.<name>") and, unless the row is a KIslandRow or a KSwitch with a text, the label the
// widget shows. It takes no room in any layout -- it is never visible -- and the row it marks is the KIslandRow
// it sits in, else its parent. KPinOverlay draws its pin; the widget mirrors the row's controls.
Item {
    id: tag

    property string key: ""
    property string label: ""

    readonly property Item block: {
        var p = parent
        for (var i = 0; p && i < 3; ++i) {
            if (p.isKIslandRow === true)
                return p
            p = p.parent
        }
        return parent
    }

    // The label without the colon a settings row ends it with.
    readonly property string shownLabel: {
        var t = key
        var b = block
        if (label.length > 0)
            t = label
        else if (b && typeof b.label === "string" && b.label.length > 0)
            t = b.label
        else if (b && typeof b.text === "string" && b.text.length > 0)
            t = b.text
        return t.replace(/\s*:\s*$/, "")
    }

    // The settings group the row is in: a widget names it under the label, as "Size" alone says little.
    readonly property string groupTitle: {
        for (var p = parent; p; p = p.parent)
            if (typeof p.stateKey === "string" && typeof p.title === "string")
                return p.title
        return ""
    }

    visible: false
    width: 0
    height: 0

    Component.onCompleted: PinRegistry.register(tag)
    Component.onDestruction: PinRegistry.unregister(tag)
}

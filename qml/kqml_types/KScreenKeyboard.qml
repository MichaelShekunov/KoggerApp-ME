import QtQuick 2.15
import kqml_types 1.0

// On-screen keyboard panel, driven by the C++ `screenKeyboard` context property (ScreenKeyboard).
// It appears when an editable text input has focus (touch mode, or always, per the setting),
// opens under the input when there is room and above it otherwise, and delivers keys through
// screenKeyboard.sendText / sendKey. Layouts: Latin, Cyrillic, symbols; numeric pad for number
// fields. Nothing here takes keyboard focus, so the edited field keeps it.
Item {
    id: root

    readonly property var kb: (typeof screenKeyboard !== "undefined" && screenKeyboard) ? screenKeyboard : null
    readonly property bool active: kb ? kb.visible : false
    readonly property real s: AppPalette.scale
    readonly property int viewportW: parent ? parent.width : 0
    readonly property int viewportH: parent ? parent.height : 0

    readonly property int gap: Math.round(5 * s)
    readonly property int pad: Math.round(8 * s)
    readonly property int margin: Tokens.spaceLg

    property string layout: "en"        // en | ru | sym | num
    property string alphaLayout: "en"   // last alphabetic layout, restored when leaving "sym"
    property int shift: 0               // 0 off, 1 next key only, 2 caps lock
    property double _lastShiftMs: 0
    property rect _lastFocusRect: Qt.rect(0, 0, 0, 0)

    readonly property var rows: ({
        "en": [["q","w","e","r","t","y","u","i","o","p"],
               ["a","s","d","f","g","h","j","k","l"],
               ["z","x","c","v","b","n","m"]],
        "ru": [["й","ц","у","к","е","н","г","ш","щ","з","х","ъ"],
               ["ф","ы","в","а","п","р","о","л","д","ж","э"],
               ["я","ч","с","м","и","т","ь","б","ю"]],
        "sym": [["1","2","3","4","5","6","7","8","9","0"],
                ["@","#","$","%","^","&","*","-","+","="],
                ["(",")","[","]","{","}","<",">","/","\\"],
                ["!","?",":",";","'","\"","_","~","`","|"]],
        "num": [["1","2","3"],
                ["4","5","6"],
                ["7","8","9"],
                [".","0","-"]]
    })

    width: layout === "num" ? Math.round(230 * s) : Math.min(viewportW - 2 * margin, Math.round(640 * s))
    height: panel.height
    z: ZOrder.screenKeyboard
    visible: opacity > 0.01
    enabled: active
    opacity: active ? 1.0 : 0.0
    Behavior on opacity { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }

    x: _placeX()
    y: _placeY()

    function _focusRect() {
        return kb ? kb.focusRect : Qt.rect(0, 0, 0, 0)
    }
    function _placeX() {
        var r = _focusRect()
        var cx = (r && r.width > 0) ? r.x + r.width / 2 : viewportW / 2
        return Math.round(Math.max(margin, Math.min(cx - width / 2, viewportW - width - margin)))
    }
    function _placeY() {
        var r = _focusRect()
        if (!r || r.height <= 0)
            return Math.round(viewportH - height - margin)
        var below = r.y + r.height + gap
        if (below + height + margin <= viewportH)
            return Math.round(below)
        var above = r.y - gap - height
        if (above >= margin)
            return Math.round(above)
        return Math.round(viewportH - height - margin)
    }

    Connections {
        target: root.kb
        function onStateChanged() {
            if (!root.kb)
                return
            var r = root.kb.focusRect
            var moved = r.x !== root._lastFocusRect.x || r.y !== root._lastFocusRect.y
                     || r.width !== root._lastFocusRect.width || r.height !== root._lastFocusRect.height
            if (moved) {
                root._lastFocusRect = r
                root.shift = 0
                root.layout = root.kb.numericMode ? "num" : root.alphaLayout
            }
        }
    }

    function isAlpha() { return layout === "en" || layout === "ru" }
    function pressChar(ch) {
        var out = ch
        if (isAlpha() && shift > 0)
            out = ch.toUpperCase()
        if (shift === 1)
            shift = 0
        if (kb)
            kb.sendText(out)
    }
    function pressShift() {
        var now = Date.now()
        if (shift === 2)
            shift = 0
        else if (shift === 1 && now - _lastShiftMs < 350)
            shift = 2
        else
            shift = (shift === 1) ? 0 : 1
        _lastShiftMs = now
    }
    function pressBackspace() { if (kb) kb.sendKey(Qt.Key_Backspace) }
    function pressDone() { if (kb) { kb.sendKey(Qt.Key_Return); kb.hide() } }
    function pressHide() { if (kb) kb.hide() }
    function toggleSym() { layout = (layout === "sym") ? alphaLayout : "sym"; shift = 0 }
    function toggleLang() { alphaLayout = (alphaLayout === "en") ? "ru" : "en"; layout = alphaLayout; shift = 0 }

    // One key. `weight` is its width relative to a plain letter key; `unit` is the letter-key
    // width for its row. Self-contained: it only uses the singletons and its own properties.
    component Key: Item {
        id: key
        property string label: ""
        property bool special: false
        property bool highlighted: false
        property bool repeatable: false
        property real weight: 1.0
        property real unit: 0
        property var action: null
        readonly property bool down: keyMouse.pressed
        readonly property int keyGap: Math.round(5 * AppPalette.scale)

        width: Math.round(unit * weight + (weight - 1) * keyGap)
        height: Math.round(38 * AppPalette.scale)

        Rectangle {
            anchors.fill: parent
            radius: Tokens.radiusMd
            color: key.down ? AppPalette.accentBgStrong
                 : key.highlighted ? AppPalette.accentBg
                 : key.special ? AppPalette.bgHover : AppPalette.card
            border.width: 1
            border.color: (key.down || key.highlighted) ? AppPalette.accentBorder : AppPalette.border
        }
        Text {
            anchors.centerIn: parent
            text: key.label
            color: (key.down || key.highlighted) ? AppPalette.accentText : AppPalette.textStrong
            font.pixelSize: key.special ? Tokens.fontMd : Tokens.fontLg
            font.bold: true
        }
        MouseArea {
            id: keyMouse
            anchors.fill: parent
            preventStealing: true
            onClicked: if (!repeatTimer.fired) key.fire()
            onPressAndHold: if (key.repeatable) { repeatTimer.fired = true; repeatTimer.start() }
            onReleased: { repeatTimer.stop(); repeatTimer.fired = false }
            onCanceled: { repeatTimer.stop(); repeatTimer.fired = false }
        }
        Timer {
            id: repeatTimer
            property bool fired: false
            interval: 55
            repeat: true
            onTriggered: key.fire()
        }
        function fire() {
            if (key.action)
                key.action()
        }
    }

    Rectangle {
        id: panel
        width: root.width
        height: content.height + 2 * root.pad
        radius: Tokens.radiusLg
        color: AppPalette.bgDeep
        border.width: 1
        border.color: AppPalette.borderHover
        readonly property int innerWidth: width - 2 * root.pad

        Column {
            id: content
            x: root.pad
            y: root.pad
            width: panel.innerWidth
            spacing: root.gap

            Repeater {
                model: root.rows[root.layout]
                delegate: Row {
                    id: charRow
                    required property var modelData
                    required property int index
                    readonly property var keys: modelData
                    readonly property bool lastAlphaRow: root.isAlpha() && index === root.rows[root.layout].length - 1
                    readonly property int extraKeys: lastAlphaRow ? 2 : 0
                    readonly property real weightSum: keys.length + (lastAlphaRow ? 3.0 : 0)
                    readonly property real unit: (panel.innerWidth - (keys.length + extraKeys - 1) * root.gap) / weightSum
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: root.gap

                    Key {
                        visible: charRow.lastAlphaRow
                        unit: charRow.unit
                        weight: 1.5
                        special: true
                        highlighted: root.shift > 0
                        label: root.shift === 2 ? "AA" : (root.shift === 1 ? "Aa" : "aA")
                        action: function() { root.pressShift() }
                    }
                    Repeater {
                        model: charRow.keys
                        delegate: Key {
                            required property string modelData
                            unit: charRow.unit
                            label: (root.isAlpha() && root.shift > 0) ? modelData.toUpperCase() : modelData
                            action: function() { root.pressChar(modelData) }
                        }
                    }
                    Key {
                        visible: charRow.lastAlphaRow
                        unit: charRow.unit
                        weight: 1.5
                        special: true
                        repeatable: true
                        label: "←"
                        action: function() { root.pressBackspace() }
                    }
                }
            }

            Row {
                id: bottomRow
                readonly property bool numeric: root.layout === "num"
                readonly property real weightSum: numeric ? 3.0 : 10.0
                readonly property int nKeys: numeric ? 2 : 6
                readonly property real unit: (panel.innerWidth - (nKeys - 1) * root.gap) / weightSum
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: root.gap

                Key { visible: !bottomRow.numeric; unit: bottomRow.unit; weight: 1.5; special: true
                      label: root.layout === "sym" ? (root.alphaLayout === "ru" ? "абв" : "abc") : "?123"
                      action: function() { root.toggleSym() } }
                Key { visible: !bottomRow.numeric; unit: bottomRow.unit; weight: 1.2; special: true
                      label: root.alphaLayout === "ru" ? "EN" : "RU"
                      action: function() { root.toggleLang() } }
                Key { visible: !bottomRow.numeric; unit: bottomRow.unit; weight: 1.0; label: ","
                      action: function() { root.pressChar(",") } }
                Key { visible: !bottomRow.numeric; unit: bottomRow.unit; weight: 3.3; label: ""
                      action: function() { root.pressChar(" ") } }
                Key { visible: !bottomRow.numeric; unit: bottomRow.unit; weight: 1.0; label: "."
                      action: function() { root.pressChar(".") } }
                Key { visible: bottomRow.numeric; unit: bottomRow.unit; weight: 1.5; special: true; repeatable: true
                      label: "←"; action: function() { root.pressBackspace() } }
                Key { unit: bottomRow.unit; weight: bottomRow.numeric ? 1.5 : 2.0; special: true; highlighted: true
                      label: qsTr("Done"); action: function() { root.pressDone() } }
            }
        }

        KCircleIconButton {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Math.round(2 * root.s)
            width: Math.round(22 * root.s)
            height: width
            iconSource: "qrc:/icons/ui/chevron-down.svg"
            iconTintColor: AppPalette.textSecond
            toolTipText: qsTr("Hide keyboard")
            fillColor: "transparent"
            borderWidth: 0
            onClicked: root.pressHide()
        }
    }
}

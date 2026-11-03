import QtQuick 2.15
import kqml_types 1.0

// One pinned setting of a settings widget (SettingsWidgetPopup): its label and its controls.
//
// AN "app:" SETTING IS A ROW OF THE SETTINGS PAGE, MIRRORED. The page is made once and lives as long as the app;
// its row registered itself (KPinTag), and every control here stands for one of the row's: it shows the row's
// value and, when used, acts on the row's control the way a hand on it would -- KSwitch toggle() + toggled(),
// KSpinBox applyValue(), KCombo currentIndex + activated(), KSlider valueModified(), KTabBar valueSelected().
// So the page keeps doing what a change does, applying it and saving it, and the two always show the same.
// toggle() and not click(): a setting's group closed disables its rows, and click() does nothing on a disabled
// switch. A slider's value is written only if valueModified did not move it: writing a bound value cuts the
// binding, which a hand on the slider does not.
//
// A "dev:<device type>:<param>" SETTING IS A DEVICE PARAMETER (DeviceParamCatalog), read from and written to the
// first connected device of its type (WorkspaceStore.settingsDeviceFor).
Item {
    id: mrow

    required property var store
    property var item: null                // { key, label }
    property real availableWidth: 0
    property bool removable: false         // pin mode: a cross to take the setting out

    signal removeRequested()

    readonly property real _s: AppPalette.scale
    readonly property real _gap: Tokens.spaceSm
    readonly property string key: item ? item.key : ""
    readonly property bool isDevice: key.indexOf("dev:") === 0

    // An app setting: the row's tag, looked up again as tags come and go (the page loads after the widgets).
    readonly property var tag: { PinRegistry.revision; return (!isDevice && key.length > 0) ? PinRegistry.tagOf(key) : null }
    readonly property var targets: tag ? PinRegistry.controlsOf(tag.block) : []

    // A device parameter.
    readonly property var _parts: isDevice ? key.split(":") : []
    readonly property string devName: _parts.length === 3 ? _parts[1] : ""
    readonly property string paramKey: _parts.length === 3 ? _parts[2] : ""
    readonly property var meta: isDevice ? DeviceParamCatalog.meta(paramKey) : null
    readonly property var dev: (isDevice && store) ? store.settingsDeviceFor(devName) : null
    readonly property bool devReady: !!dev && !!meta && DeviceParamCatalog.supported(dev, paramKey, null)
    readonly property var devValue: devReady ? DeviceParamCatalog.read(dev, paramKey) : 0

    readonly property string labelText: tag ? tag.shownLabel
                                      : isDevice ? (meta ? meta.label : (item ? item.label : ""))
                                      : (item && item.label.length > 0 ? item.label : key)
    // Under the label: where the setting is from (its settings group, its device), or why it does nothing.
    readonly property string note: isDevice ? (dev ? (devReady ? devName : qsTr("not supported by %1").arg(devName))
                                                   : qsTr("%1 is not connected").arg(devName))
                                            : (tag ? tag.groupTitle : qsTr("not available"))
    readonly property bool _noteIsTrouble: isDevice ? !devReady : !tag

    // The width the row wants on one line; the widget takes the widest of its rows.
    readonly property real naturalWidth: Math.ceil(labelMetrics.advanceWidth) + _gap + controlsRow.implicitWidth
                                         + (removable ? removeButton.width + _gap : 0)
    readonly property bool _stacked: naturalWidth > availableWidth + 0.5

    width: availableWidth
    height: _stacked ? labelCol.height + _gap + controlsRow.height
                     : Math.max(labelCol.height, controlsRow.height, Tokens.controlHMd)

    TextMetrics {
        id: labelMetrics
        font.pixelSize: Tokens.fontBase
        text: mrow.labelText
    }

    Column {
        id: labelCol
        width: mrow._stacked ? mrow.width
                             : Math.max(0, mrow.width - controlsRow.width - mrow._gap
                                           - (mrow.removable ? removeButton.width + mrow._gap : 0))
        y: mrow._stacked ? 0 : Math.round((mrow.height - height) / 2)
        spacing: 0

        Text {
            width: parent.width
            text: mrow.labelText
            color: AppPalette.textStrong
            font.pixelSize: Tokens.fontBase
            elide: Text.ElideRight
        }
        Text {
            width: parent.width
            visible: text.length > 0
            text: mrow.note
            color: mrow._noteIsTrouble ? AppPalette.dangerText : AppPalette.textMuted
            font.pixelSize: Tokens.fontXs
            elide: Text.ElideRight
        }
    }

    KCircleIconButton {
        id: removeButton
        visible: mrow.removable
        anchors.right: parent.right
        y: mrow._stacked ? 0 : Math.round((mrow.height - height) / 2)
        width: visible ? Tokens.controlHSm : 0
        height: Tokens.controlHSm
        rounded: false
        cornerRadius: Tokens.radiusMd
        iconSource: ""
        glyph: "×"
        showGlyphWithIcon: true
        glyphPixelSize: Tokens.iconSm
        glyphColor: AppPalette.textSecond
        fillColor: AppPalette.controlRaised
        fillHoverColor: Qt.lighter(AppPalette.controlRaised, 1.2)
        borderWidth: 0
        toolTipText: qsTr("Take out of the widget")
        onClicked: mrow.removeRequested()
    }

    Row {
        id: controlsRow
        anchors.right: mrow.removable ? removeButton.left : parent.right
        anchors.rightMargin: mrow.removable ? mrow._gap : 0
        y: mrow._stacked ? labelCol.height + mrow._gap : Math.round((mrow.height - height) / 2)
        spacing: mrow._gap
        enabled: !mrow.isDevice || mrow.devReady

        Repeater {
            model: mrow.isDevice ? (mrow.meta ? [mrow.meta.kind] : []) : mrow.targets

            delegate: Loader {
                id: ld
                required property var modelData
                readonly property string kind: mrow.isDevice ? String(modelData) : (modelData ? modelData.controlKind : "")
                // A control the row hides, the mirror hides, as long as the row itself can be seen.
                visible: mrow.isDevice || !modelData || modelData.visible || !mrow.tag || !mrow.tag.block.visible
                sourceComponent: mrow.isDevice
                    ? (kind === "spin" ? devSpin : kind === "switch" ? devSwitch : kind === "tabs" ? devTabs : null)
                    : (kind === "switch" ? mirrorSwitch : kind === "spin" ? mirrorSpin : kind === "combo" ? mirrorCombo
                       : kind === "slider" ? mirrorSlider : kind === "tabs" ? mirrorTabs : null)
                onLoaded: if (!mrow.isDevice) item.t = Qt.binding(function() { return ld.modelData })
            }
        }
    }

    component MirrorSwitch: KSwitch {
        property var t: null
        readonly property bool want: !!(t && t.checked)
        property bool _g: false
        function _sync() { if (checked !== want) { _g = true; checked = want; _g = false } }
        flat: true
        onWantChanged: _sync()
        onTChanged: _sync()
        onToggled: {
            if (_g || !t)
                return
            if (t.checked !== checked) {
                t.toggle()
                t.toggled()
            }
            _sync()
        }
    }

    component MirrorSpin: KSpinBox {
        property var t: null
        readonly property int want: t ? t.value : 0
        property bool _in: false
        function _sync() { if (value !== want) { _in = true; value = want; _in = false } }
        from: t ? t.from : 0
        to: t ? t.to : 100
        stepSize: t ? t.stepSize : 1
        stepValues: t ? t.stepValues : []
        divisor: t ? t.divisor : 1
        decimals: t ? t.decimals : 0
        trimZeros: t ? t.trimZeros : false
        textFromValue: t ? t.textFromValue : null
        editable: t ? t.editable : true
        applyWhileTyping: t ? t.applyWhileTyping : true
        fontPixelSize: Tokens.fontBase
        width: t ? Math.max(Math.round(96 * mrow._s), Math.min(Math.round(140 * mrow._s), t.width)) : implicitWidth
        onWantChanged: _sync()
        onTChanged: _sync()
        onValueModified: function(v) {
            if (!_in && t)
                t.applyValue(v)
            _sync()
        }
    }

    component MirrorCombo: KCombo {
        property var t: null
        readonly property int want: t ? t.currentIndex : 0
        function _sync() { if (currentIndex !== want) currentIndex = want }
        model: t ? t.model : []
        swatchFor: t ? t.swatchFor : null
        displayTextOverride: t ? t.displayTextOverride : ""
        fontPixelSize: Tokens.fontBase
        width: t ? Math.max(Math.round(110 * mrow._s), Math.min(Math.round(180 * mrow._s), t.width)) : implicitWidth
        height: Tokens.controlHMd
        onWantChanged: _sync()
        onTChanged: _sync()
        onModelChanged: Qt.callLater(_sync)
        onActivated: function(index) {
            if (t) {
                if (t.currentIndex !== index)
                    t.currentIndex = index
                t.activated(index)
            }
            _sync()
        }
    }

    component MirrorSlider: KSlider {
        property var t: null
        readonly property real want: t ? t.value : 0
        property bool _in: false
        function _sync() { if (!pressed && value !== want) { _in = true; value = want; _in = false } }
        from: t ? t.from : 0
        to: t ? t.to : 1
        stepSize: t ? t.stepSize : 0
        valueDecimals: t ? t.valueDecimals : 0
        valueDivisor: t ? t.valueDivisor : 1
        valueSuffix: t ? t.valueSuffix : ""
        width: Math.round(150 * mrow._s)
        onWantChanged: _sync()
        onTChanged: _sync()
        // The range comes with the row, after the value may have been clamped to the empty one.
        onFromChanged: _sync()
        onToChanged: _sync()
        onPressedChanged: if (!pressed) _sync()
        onValueModified: function(v) {
            if (_in || !t)
                return
            t.valueModified(v)
            if (t.value !== v)
                t.value = v
        }
    }

    component MirrorTabs: KTabBar {
        property var t: null
        readonly property var want: t ? t.currentValue : undefined
        function _sync() { if (currentValue !== want) currentValue = want }
        options: t ? t.options : []
        fontPixelSize: Tokens.fontSm
        width: Math.min(Math.round(320 * mrow._s), Math.max(Math.round(160 * mrow._s), optionCount * Math.round(64 * mrow._s)))
        onWantChanged: _sync()
        onTChanged: _sync()
        onValueSelected: function(v) {
            if (t)
                t.valueSelected(v)
            _sync()
        }
    }

    Component { id: mirrorSwitch; MirrorSwitch {} }
    Component { id: mirrorSpin; MirrorSpin {} }
    Component { id: mirrorCombo; MirrorCombo {} }
    Component { id: mirrorSlider; MirrorSlider {} }
    Component { id: mirrorTabs; MirrorTabs {} }

    Component {
        id: devSpin
        KSpinBox {
            readonly property int want: Number(mrow.devValue) || 0
            property bool _in: false
            function _sync() { if (value !== want) { _in = true; value = want; _in = false } }
            from: mrow.meta && typeof mrow.meta.from === "number" ? mrow.meta.from : 0
            to: mrow.meta && typeof mrow.meta.to === "number" ? mrow.meta.to : 100
            stepSize: mrow.meta && typeof mrow.meta.step === "number" ? mrow.meta.step : 1
            applyWhileTyping: false   // the device gets the number typed, not each digit of it
            fontPixelSize: Tokens.fontBase
            width: Math.round(120 * mrow._s)
            onWantChanged: _sync()
            Component.onCompleted: _sync()
            onValueModified: function(v) { if (!_in && mrow.dev) DeviceParamCatalog.write(mrow.dev, mrow.paramKey, v) }
        }
    }

    Component {
        id: devSwitch
        KSwitch {
            flat: true
            readonly property bool want: !!mrow.devValue
            property bool _g: false
            function _sync() { if (checked !== want) { _g = true; checked = want; _g = false } }
            onWantChanged: _sync()
            Component.onCompleted: _sync()
            onToggled: { if (!_g && mrow.dev) DeviceParamCatalog.write(mrow.dev, mrow.paramKey, checked) }
        }
    }

    Component {
        id: devTabs
        KTabBar {
            options: mrow.meta && mrow.meta.options ? mrow.meta.options : []
            fontPixelSize: Tokens.fontSm
            width: Math.min(Math.round(320 * mrow._s), Math.max(Math.round(160 * mrow._s), optionCount * Math.round(64 * mrow._s)))
            readonly property var want: mrow.devValue
            property bool _g: false
            function _sync() { if (currentValue !== want) { _g = true; currentValue = want; _g = false } }
            onWantChanged: _sync()
            Component.onCompleted: _sync()
            onValueSelected: function(v) { if (!_g && mrow.dev) DeviceParamCatalog.write(mrow.dev, mrow.paramKey, v) }
        }
    }
}

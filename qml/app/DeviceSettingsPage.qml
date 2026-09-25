import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Dialogs
import QtCore
import kqml_types 1.0
import "RecorderStatus.js" as RecorderStatus

Column {
    id: root

    property var dev: null
    property var store: null
    // The USBL plan and the interrogation loop are NOT owned here. This page is a settings
    // sub-page: its loader destroys it the moment the operator navigates away, which used to
    // take the schedule down with it. Both now live in MainWindow for the session and arrive
    // as properties. See UsblEngine.qml.
    property var usblPlan: null
    property var usblEngine: null
    property real scrollTopInset: 0

    property bool favEditMode: false
    readonly property string devTypeKey: dev ? (dev.devName || "") : ""

    readonly property var compactParamKeys: {
        if (!_isBasicSonar)
            return []
        var keys = ["dataset.period", "dataset.chart", "dataset.dist",
                    "chart.resolution", "chart.samples", "dist.confidence"]
        if (_isNanoSSS)
            keys.push("trans.freq")
        return keys
    }

    function isPinnable(paramKey) {
        if (paramKey.indexOf("action.") === 0)
            return _isBasicSonar
        return DeviceParamCatalog.supported(dev, paramKey)
               && compactParamKeys.indexOf(paramKey) < 0
    }

    readonly property var baudrateOptions: [9600, 19200, 38400, 57600, 115200,
                                            230400, 460800, 921600, 1000000, 1200000, 1500000, 2000000, 3000000, 4000000]
    property int baudrateIndex: baudrateOptions.indexOf(115200)

    function applyBaudrate() {
        if (!dev)
            return
        var b = baudrateOptions[baudrateIndex]
        var uuid = _linkUuidOfDev()
        dev.baudrate = b
        notifications.info(qsTr("Baudrate set: %1").arg(b))
        if (uuid.length > 0)
            Qt.callLater(_applyLinkBaudrate, uuid, b)
    }

    function openImportSettingsFile() {
        importXmlDialog.currentFolder = devSettingsGroup.importFolder
        importXmlDialog.open()
    }

    function openExportSettingsFile() {
        exportXmlDialog.currentFolder = devSettingsGroup.exportFolder
        exportXmlDialog.open()
    }

    readonly property bool hasPinnableParams: {
        var list = DeviceParamCatalog.params
        var found = false
        for (var i = 0; i < list.length; ++i)
            if (isPinnable(list[i].key))
                found = true
        return found
    }
    onHasPinnableParamsChanged: if (!hasPinnableParams) favEditMode = false

    readonly property var favoriteKeys: {
        var map = store ? store.deviceFavoriteParams : null
        var key = devTypeKey
        if (!map || key.length === 0)
            return []
        var list = map[key]
        if (!Array.isArray(list))
            return []
        var out = []
        for (var i = 0; i < list.length; ++i)
            if (isPinnable(list[i]))
                out.push(list[i])
        return out
    }

    function _hitTest(item, pos) {
        if (!item || !item.visible)
            return false
        var local = root.mapToItem(item, pos.x, pos.y)
        return local.x >= 0 && local.y >= 0 && local.x <= item.width && local.y <= item.height
    }

    function _pressedOutsideAdvanced(pos) {
        if (!favEditMode)
            return
        if (_hitTest(advancedPanel, pos) || _hitTest(favoritesReveal, pos))
            return
        favEditMode = false
    }

    on_EngExpandedChanged: if (!_engExpanded) favEditMode = false

    TapHandler {
        acceptedButtons: Qt.AllButtons
        gesturePolicy: TapHandler.DragThreshold
        onPressedChanged: if (pressed) root._pressedOutsideAdvanced(point.position)
    }

    onVisibleChanged: if (!visible) favEditMode = false

    Connections {
        target: root.store
        ignoreUnknownSignals: true
        function onSettingsPanelOpenChanged() {
            if (root.store && !root.store.settingsPanelOpen)
                root.favEditMode = false
        }
        function onSettingsSubPageKindChanged() { root.favEditMode = false }
        function onAnySettingsSubPageActiveChanged() { root.favEditMode = false }
        function onActiveDeviceChanged() { root.favEditMode = false }
    }

    function toggleFavorite(paramKey) {
        if (!store || devTypeKey.length === 0)
            return
        if (!store.isFavoriteParam(devTypeKey, paramKey) && store.favoriteParamsFull(devTypeKey)) {
            notifications.info(qsTr("Favourites are full: up to %1 settings").arg(store.deviceFavoriteParamLimit))
            return
        }
        store.toggleFavoriteParam(devTypeKey, paramKey)
    }

    // Per-device UI memory. This page is one instance whose `dev` changes on switch,
    // so disclosure state is kept per device: on switch, snapshot the leaving device's
    // group expansions + advanced-cut state, restore the entering device's (all
    // collapsed if unseen). Entries for disconnected devices are pruned — a reconnect
    // is a new object → fresh defaults.
    property var _groupStates: []   // [{ dev, map: { stateKey: bool }, eng: bool }]
    property var _prevDev: null

    onDevChanged: {
        if (_prevDev) {
            var prev = _groupStateFor(_prevDev)
            if (prev) { prev.map = _snapshotGroups(); prev.eng = _engExpanded }
            else _groupStates.push({ dev: _prevDev, map: _snapshotGroups(), eng: _engExpanded })
        }
        _pruneGroupStates()
        _applyForCurrentDev()
        favEditMode = false
        _prevDev = dev
    }

    Component.onCompleted: _applyForCurrentDev()

    function _applyForCurrentDev() {
        var cur = dev ? _groupStateFor(dev) : null
        _applyGroups(cur ? cur.map : null)
        var eng = cur ? !!cur.eng : false
        if (eng !== _engExpanded) {
            advReveal._animReady = false   // snap the cut on switch, no reveal animation
            _engExpanded = eng
            Qt.callLater(function() { advReveal._animReady = true })
        }
    }

    function _groupStateFor(d) {
        for (var i = 0; i < _groupStates.length; ++i)
            if (_groupStates[i].dev === d) return _groupStates[i]
        return null
    }

    function _collectGroups(item, out) {
        var kids = item ? item.children : null
        if (!kids) return
        for (var i = 0; i < kids.length; ++i) {
            var g = kids[i]
            if (!g) continue
            if (g.stateKey !== undefined && typeof g.expanded === "boolean")
                out.push(g)
            else
                _collectGroups(g, out)
        }
    }

    function _snapshotGroups() {
        var m = {}
        var groups = []
        _collectGroups(root, groups)
        for (var i = 0; i < groups.length; ++i)
            m[groups[i].stateKey] = groups[i].expanded
        return m
    }

    function _applyGroups(map) {
        var groups = []
        _collectGroups(root, groups)
        for (var i = 0; i < groups.length; ++i) {
            var g = groups[i]
            if (g.bodyAnimated !== undefined)
                g.bodyAnimated = false   // programmatic: snap, no expand/collapse flicker
            g.expanded = (map && map[g.stateKey] !== undefined) ? map[g.stateKey] : !g.collapsedByDefault
        }
        Qt.callLater(_reenableGroupAnim)
    }

    function _reenableGroupAnim() {
        var groups = []
        _collectGroups(root, groups)
        for (var i = 0; i < groups.length; ++i)
            if (groups[i].bodyAnimated !== undefined)
                groups[i].bodyAnimated = true
    }

    function _pruneGroupStates() {
        var ds = (typeof deviceManagerWrapper !== "undefined" && deviceManagerWrapper) ? deviceManagerWrapper.devs : []
        var kept = []
        for (var i = 0; i < _groupStates.length; ++i) {
            var e = _groupStates[i], present = false
            for (var j = 0; j < ds.length; ++j)
                if (ds[j] === e.dev) { present = true; break }
            if (present)
                kept.push(e)
        }
        _groupStates = kept
    }

    readonly property bool _isBasic2D: !!(dev && dev.devName === "Basic2D")
    readonly property bool _isNanoSSS: !!(dev && dev.devName === "NanoSSS")
    readonly property bool _isBasicSonar: _isBasic2D || _isNanoSSS
    readonly property bool _isRecorder: !!(dev && dev.isRecorder)
    readonly property bool _isUsbl: !!(dev && (dev.isUSBL || dev.isUSBLBeacon))
    readonly property bool _isWifi: !!(dev && dev.isWifi)
    // Without a cut the panel has no header and advReveal.open is permanently true, so
    // Actions / Settings file / Upgrade sit open below the operating groups and the
    // "Advanced settings" row is a zero-height, unclickable ghost. USBL gets the cut for
    // the same reason the basic sonars do: those three are firmware and maintenance
    // actions, not operating controls.
    // The Wi-Fi module gets the cut too: its island is the operating control, the rest is upkeep.
    readonly property bool _hasCut: _isBasicSonar || _isRecorder || _isUsbl || _isWifi
    property bool _engExpanded: false

    function _linkUuidOfDev() {
        if (!dev || typeof deviceTopology === "undefined" || !deviceTopology)
            return ""
        var groups = deviceTopology.groups || []
        for (var gi = 0; gi < groups.length; ++gi) {
            var members = groups[gi].members || []
            for (var mi = 0; mi < members.length; ++mi)
                if (members[mi].device === dev)
                    return groups[gi].linkUuid || ""
        }
        return ""
    }

    function _applyLinkBaudrate(uuid, baudrate) {
        if (typeof linkManagerWrapper === "undefined" || !linkManagerWrapper)
            return
        linkManagerWrapper.updateBaudrateFor(uuid, baudrate)
    }

    readonly property var _warnings: {
        var w = []
        if (!dev || !_isBasicSonar) return w
        var tpl = qsTr('Group "%1" settings were not applied')
        if (dev.chartSetupState === false) w.push(tpl.arg(qsTr("Echogram")))
        if (dev.distSetupState === false)  w.push(tpl.arg(qsTr("Rangefinder")))
        if (dev.transcState === false)     w.push(tpl.arg(qsTr("Transducer")))
        if (dev.dspState === false || dev.soundState === false) w.push(tpl.arg(qsTr("DSP")))
        if (dev.datasetState === false)    w.push(tpl.arg(qsTr("Dataset")))
        if (dev.uartState === false)       w.push(tpl.arg(qsTr("Actions")))
        return w
    }

    property int warningCount: _warnings.length
    property bool warningShown: false
    onWarningCountChanged: {
        if (warningCount > 0) {
            if (!warningShown) warnDelayTimer.restart()
        } else {
            warnDelayTimer.stop()
            warningShown = false
        }
    }
    Timer { id: warnDelayTimer; interval: 1000; onTriggered: root.warningShown = true }

    function _advFlick() {
        var item = root.parent
        while (item) {
            if (item.contentY !== undefined && item.contentHeight !== undefined && item.flickableDirection !== undefined)
                return item
            item = item.parent
        }
        return null
    }
    function _scrollAdvancedIntoView() {
        var flick = _advFlick()
        if (!flick) return
        var topInContent = advancedPanel.mapToItem(flick.contentItem, 0, 0).y
        var bottomInContent = topInContent + advancedPanel.height
        var vpH = flick.height
        var inset = Math.max(0, root.scrollTopInset)
        var cy = flick.contentY
        if (topInContent >= cy + inset - 0.5 && bottomInContent <= cy + vpH + 0.5)
            return
        var target = bottomInContent - vpH + Tokens.spaceLg
        target = Math.min(target, topInContent - inset)
        target = Math.max(0, Math.min(target, flick.contentHeight - vpH))
        if (Math.abs(target - cy) < 0.5) return
        advScrollAnim.target = flick
        advScrollAnim.from = cy
        advScrollAnim.to = target
        advScrollAnim.restart()
    }
    NumberAnimation { id: advScrollAnim; property: "contentY"; duration: 240; easing.type: Easing.OutCubic }
    Timer { id: advScrollTimer; interval: 260; onTriggered: root._scrollAdvancedIntoView() }

    readonly property real groupWidth: Math.max(0, width)
    readonly property real spinW: Math.round(115 * AppPalette.scale)
    readonly property real toggleW: Math.round(44 * AppPalette.scale)
    readonly property real spinToggleSlotW: spinW + Tokens.spaceMd + toggleW

    readonly property int chartSamplesMin: 100
    readonly property int chartSamplesMax: 15000
    readonly property var distanceStepsM: [1, 2, 3, 4, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 60, 70, 80, 90, 100, 125, 150]
    readonly property var periodStepsMs: [10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 125, 150, 175, 200, 250, 300, 350, 400, 450, 500, 750, 1000, 1500, 2000]

    width: parent ? parent.width : implicitWidth
    spacing: Tokens.spaceLg

    // KSpinBox wrapper that syncs with a C++ device property
    component DevSpin: Item {
        id: ds
        property int devValue: 0
        property int from: 0
        property int to: 100
        property int stepSize: 1
        property var stepValues: []
        property real divisor: 1.0
        property int decimals: 0
        property bool trimZeros: false
        property var writeBack: null  // function(v) called on user interaction

        implicitWidth: Math.round(115 * AppPalette.scale); implicitHeight: Tokens.controlHMd
        property bool _in: false

        onDevValueChanged: {
            if (spin.value !== devValue) {
                _in = true; spin.value = devValue; _in = false
            }
        }
        Component.onCompleted: { _in = true; spin.value = devValue; _in = false }

        KSpinBox {
            id: spin
            anchors.fill: parent
            from: ds.from; to: ds.to; stepSize: ds.stepSize; stepValues: ds.stepValues
            divisor: ds.divisor; decimals: ds.decimals; trimZeros: ds.trimZeros
            onValueModified: function(v) { if (!ds._in && ds.writeBack) ds.writeBack(v) }
        }
    }

    component DevIsland: KIsland {
        property bool unconfirmed: false
        fillColor: unconfirmed ? AppPalette.dangerBg : AppPalette.card
        borderColor: unconfirmed ? AppPalette.dangerBorder : AppPalette.border
    }

    component DevStackedRow: KIslandRow {
        stacked: true
        verticalPadding: Tokens.spaceMd
    }

    component DevIslandTitle: KIslandRow {
        labelBold: true
        labelPixelSize: Math.max(Math.round(16 * AppPalette.scale), 13)
        minHeight: 0
        verticalPadding: Tokens.spaceSm
    }

    component DevButton: KButton {
        normalBg: AppPalette.controlRaised
        hoverBg: Qt.lighter(AppPalette.controlRaised, 1.2)
        dangerBg: AppPalette.controlRaised
        dangerHoverBg: Qt.lighter(AppPalette.controlRaised, 1.2)
        borderWidth: danger ? Math.max(1, Math.round(1.5 * AppPalette.scale)) : Tokens.cardBorderWidth
    }

    component DevInfoRow: KIslandRow {
        id: infoRow
        property string value: ""
        visible: value.length > 0
        Text {
            width: infoRow.stacked ? parent.width
                                   : Math.min(implicitWidth, Math.round(infoRow.innerWidth * 0.75))
            text: infoRow.value
            color: AppPalette.textSecond
            font.pixelSize: Tokens.fontMd
            elide: Text.ElideRight
        }
    }

    // Signal-strength glyph: four bars lit by RSSI (dBm), the way phones draw it.
    component WifiBars: Item {
        id: bars
        property int rssi: -100
        property real size: Tokens.iconMd
        readonly property int level: rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi >= -90 ? 1 : 0
        width: size
        height: size
        Row {
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: Math.max(1, Math.round(bars.size * 0.09))
            Repeater {
                model: 4
                Rectangle {
                    width: Math.max(2, Math.round(bars.size * 0.16))
                    height: Math.round(bars.size * (0.34 + 0.22 * index))
                    anchors.bottom: parent.bottom
                    radius: Math.max(1, Math.round(AppPalette.scale))
                    color: index < bars.level ? AppPalette.accent : AppPalette.trackOff
                }
            }
        }
    }

    // One-line text field in the island style. echoMode is what the on-screen keyboard reads to
    // switch to its password layout, so the eye button flips it rather than masking by hand.
    component WifiField: Rectangle {
        id: wf
        property alias text: wfInput.text
        property string placeholder: ""
        property bool password: false
        property bool revealed: false
        signal accepted()
        function takeFocus() { wfInput.forceActiveFocus() }
        height: Tokens.controlHMd
        radius: Tokens.radiusMd
        color: AppPalette.bg
        border.width: wfInput.activeFocus ? 1 : Tokens.cardBorderWidth
        border.color: wfInput.activeFocus ? AppPalette.accentBorder : AppPalette.border
        TextInput {
            id: wfInput
            anchors.fill: parent
            anchors.leftMargin: Tokens.spaceMd
            anchors.rightMargin: wf.password ? Tokens.controlHMd + Tokens.spaceXs : Tokens.spaceMd
            verticalAlignment: TextInput.AlignVCenter
            color: AppPalette.text
            font.pixelSize: Tokens.fontBase
            clip: true
            activeFocusOnTab: true
            selectByMouse: true
            echoMode: (wf.password && !wf.revealed) ? TextInput.Password : TextInput.Normal
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText | (wf.password ? Qt.ImhSensitiveData : Qt.ImhNone)
            onAccepted: wf.accepted()
        }
        Text {
            anchors.left: parent.left
            anchors.leftMargin: Tokens.spaceMd
            anchors.verticalCenter: parent.verticalCenter
            visible: wfInput.text.length === 0
            text: wf.placeholder
            color: AppPalette.textMuted
            font.pixelSize: Tokens.fontBase
        }
        KCircleIconButton {
            visible: wf.password
            anchors.right: parent.right
            anchors.rightMargin: Tokens.spaceXs
            anchors.verticalCenter: parent.verticalCenter
            width: Tokens.controlHMd - 2 * Tokens.spaceXs
            height: width
            rounded: false
            cornerRadius: Tokens.radiusMd
            borderWidth: 0
            fillColor: "transparent"
            fillHoverColor: AppPalette.bgHover
            fillPressedColor: AppPalette.bgHover
            iconSource: wf.revealed ? "qrc:/icons/ui/eye-off.svg" : "qrc:/icons/ui/eye.svg"
            iconTintColor: AppPalette.textSecond
            toolTipText: wf.revealed ? qsTr("Hide password") : qsTr("Show password")
            onClicked: wf.revealed = !wf.revealed
        }
    }

    component FlashButton: DevButton {
        height: Tokens.controlHMd
        fontPixelSize: Tokens.fontMd
        text: qsTr("Flash settings")
        toolTipText: qsTr("Write current settings to device memory")
        onClicked: {
            if (!root.dev)
                return
            root.dev.flashSettings()
            notifications.info(qsTr("Settings written to device: %1").arg(root.dev.devName))
        }
    }

    component Reveal: Item {
        id: rv
        property bool open: false
        property real contentHeight: Tokens.controlHMd
        default property alias content: _rvInner.data
        width: parent ? parent.width : 0
        clip: true
        height: open ? contentHeight : 0
        Behavior on height { NumberAnimation { duration: 170; easing.type: Easing.OutCubic } }
        Item {
            id: _rvInner
            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
            height: rv.contentHeight
            opacity: rv.open ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
        }
    }

    // ── Recorder ──────────────────────────────────────────────────────────
    // Status snapshot + log archive with per-log batched download. Data comes from
    // dev.recorder* (ID_RECORDER_STATUS) and deviceManagerWrapper.streamsList; download
    // is driven by deviceManagerWrapper.startStreamDownload(id).
    DeviceSettingsGroup {
        id: recorderGroup
        width: root.groupWidth; preferredWidth: root.groupWidth
        title: qsTr("Recorder"); titlePixelSize: 13
        stateKey: "dev.recorder"; collapsedByDefault: false
        visible: !!(dev && dev.isRecorder)

        // Input ports live in the HEADER — visible even when the group is collapsed.
        // Compact: dot + short label (S1/S2/Nav). gray=never · green=live · red=lost.
        headerActions: Row {
            id: portsRow
            readonly property int gap: Tokens.spaceSm
            spacing: gap
            rightPadding: gap
            visible: !!(dev && dev.recorderStatusValid)
            readonly property int dotSize: Math.round(8 * AppPalette.scale)
            readonly property int innerGap: Math.round(6 * AppPalette.scale)
            Repeater {
                model: recorderGroup._sourceChips()
                delegate: Rectangle {
                    readonly property color accent: modelData.state === "live" ? AppPalette.linkOkBorder
                                                  : modelData.state === "lost" ? AppPalette.linkDownBorder
                                                  : AppPalette.textMuted
                    readonly property bool off: modelData.state === "off"
                    implicitWidth: chipContent.implicitWidth + Tokens.spaceLg * 2
                    width: implicitWidth
                    height: recorderGroup.headerActionSize - portsRow.gap * 2; radius: height / 2
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    color: "transparent"
                    border.width: Math.max(1, Math.round(1.5 * AppPalette.scale))
                    border.color: off ? AppPalette.border : accent
                    Row {
                        id: chipContent
                        anchors.centerIn: parent; spacing: portsRow.innerGap
                        Rectangle {
                            width: portsRow.dotSize; height: width; radius: width / 2
                            anchors.verticalCenter: parent.verticalCenter
                            color: off ? "transparent" : accent
                            border.width: off ? Math.max(1, Math.round(1.5 * AppPalette.scale)) : 0
                            border.color: AppPalette.textMuted
                        }
                        Text { text: modelData.short; color: AppPalette.textStrong
                               font.pixelSize: Tokens.fontBase; font.bold: true
                               anchors.verticalCenter: parent.verticalCenter }
                    }
                }
            }
        }

        // real, not int: free/recorded bytes exceed 2^31 (QML int is 32-bit) and would wrap.
        // recorded_size is 64 KiB units (2^16); free_space is 1 MB = 10^6-byte units (matches
        // the firmware's freeSpaceBytes()/1000000).
        readonly property real recordedBytes: dev ? (dev.recorderRecordedSize64k || 0) * 65536.0 : 0
        readonly property real freeBytes:     dev ? (dev.recorderFreeSpace1m || 0) * 1000000.0 : 0

        function _fmtSize(b) { return RecorderStatus.fmtSize(b) }
        function _elapsed(s) { return RecorderStatus.elapsed(s) }
        function _logDuration(s) {
            if (!s || s <= 0) return "0" + qsTr("s")
            var h = Math.floor(s / 3600)
            var m = Math.floor((s % 3600) / 60)
            var sec = Math.floor(s % 60)
            var pad = function(n) { return (n < 10 ? "0" : "") + n }
            if (h > 0)   return h + qsTr("h") + " " + pad(m) + qsTr("m")
            if (m > 0)   return m + qsTr("m") + " " + pad(sec) + qsTr("s")
            return sec + qsTr("s")
        }

        readonly property int _curId: dev ? (dev.recorderCurrentLogId || 0) : 0
        property real _curSize: 0
        property real _curDone: 0
        property int  _curUpload: 1
        property int  _hiddenCount: 0
        readonly property int _otherLogsCount: Math.max(0, logList.count - _hiddenCount)

        function _recount() {
            var n = streamScan.count
            var hidden = 0, sz = 0, dn = 0, up = 1
            for (var i = 0; i < n; ++i) {
                var it = streamScan.itemAt(i)
                if (!it) continue
                if (it._hidden) hidden++
                if (it._isCur) { sz = it.rSize; dn = it.rDone; up = it.rUpload }
            }
            _hiddenCount = hidden
            _curSize = sz; _curDone = dn; _curUpload = up
        }

        readonly property int kStallS: 10

        // Join source names for flag-word bits 0/1/2 (Sonar1=1, Sonar2=2, Nav=4).
        function _sources(v) {
            var a = []
            if (v & 1) a.push(qsTr("Sonar 1"))
            if (v & 2) a.push(qsTr("Sonar 2"))
            if (v & 4) a.push(qsTr("Nav"))
            if (!a.length) return ""
            return a.length === 1 ? a[0] : a.slice(0, -1).join(", ") + qsTr(" & ") + a[a.length - 1]
        }

        // Semantic severity accent (from the app palette; distinct from the blue accent).
        function _sevColor(s) {
            if (s === "good")  return AppPalette.linkOkBorder
            if (s === "warn")  return AppPalette.linkIdleBorder
            if (s === "crit")  return AppPalette.linkDownBorder
            if (s === "stale") return AppPalette.textMuted
            return AppPalette.accentBar
        }
        function _sevText(s) {
            if (s === "good")  return AppPalette.linkOkText
            if (s === "warn")  return AppPalette.linkIdleText
            if (s === "crit")  return AppPalette.linkDownText
            if (s === "stale") return AppPalette.textMuted
            return AppPalette.accentBar
        }

        // Worst-state-wins hero: { sev, word, sub, pulse }. Mirrors the UX concept: the one
        // line that answers "is my data being saved right now, and if not, what broke?".
        readonly property var hero: _hero()
        function _hero() {
            var sev = (!dev || !dev.recorderStatusValid) ? "idle"
                    : stale ? "stale"
                    : RecorderStatus.severity(dev)
            if (!dev || !dev.recorderStatusValid)
                return { sev: sev, word: qsTr("Connecting…"), sub: qsTr("Waiting for the recorder."), pulse: false }
            if (stale)
                return { sev: sev, word: qsTr("No response"),
                         sub: qsTr("No update for %1 — showing last known.").arg(_elapsed(staleAgeS)), pulse: false }
            var st = dev.recorderRecordingState
            if (st === 3 || dev.recorderCriticalFlags)
                return { sev: sev,
                         word: (dev.recorderCriticalFlags & 1) ? qsTr("Storage lost") : qsTr("Recording failed"),
                         sub: qsTr("Recording stopped — check the recorder."), pulse: false }
            if (st === 4)
                return { sev: sev, word: qsTr("Recording off"), sub: qsTr("Recording is disabled."), pulse: false }
            if (st === 2) {
                var active = dev.recorderStatusFlags || 0
                var silentBits = (dev.recorderDegradedFlags || 0) & 0x7
                // Not writing: no data landed for a while, OR every seen source has gone
                // silent. Still green if another source is feeding (that rides as an alert).
                if (dev.recorderSecondsSinceLastWrite > kStallS || (silentBits && !active)) {
                    var sil = _sources(dev.recorderDegradedFlags || 0)
                    var gap = _elapsed(dev.recorderSecondsSinceLastWrite)
                    return { sev: sev, word: qsTr("Recording stopped"),
                             sub: sil.length ? qsTr("No data from %1 · nothing recorded for %2").arg(sil).arg(gap)
                                             : qsTr("Nothing recorded for %1 — source silent.").arg(gap),
                             pulse: false }
                }
                var lostBits = silentBits & ~active
                if (lostBits) {
                    var seen = active | lostBits
                    var defs = [{ b: 1, n: qsTr("Sonar 1") }, { b: 2, n: qsTr("Sonar 2") }, { b: 4, n: qsTr("Nav") }]
                    var parts = []
                    for (var i = 0; i < defs.length; i++)
                        if (seen & defs[i].b)
                            parts.push((active & defs[i].b ? qsTr("%1 connected") : qsTr("%1 disconnected")).arg(defs[i].n))
                    var detail = parts.join(", ")
                    var logId2 = dev.recorderCurrentLogId || 0
                    return { sev: sev, word: qsTr("Recording"),
                             sub: logId2 > 0 ? qsTr("Saving to log #%1 — %2").arg(logId2).arg(detail)
                                             : qsTr("Saving — %1").arg(detail),
                             pulse: true }
                }
                var live = _sources(active)
                var liveN = (active & 1 ? 1 : 0) + (active & 2 ? 1 : 0) + (active & 4 ? 1 : 0)
                var logId = dev.recorderCurrentLogId || 0
                var sub
                if (logId > 0)
                    sub = live.length ? (liveN > 1 ? qsTr("Saving to log #%1 — %2 are connected.").arg(logId).arg(live)
                                                   : qsTr("Saving to log #%1 — %2 connected.").arg(logId).arg(live))
                                      : qsTr("Saving to log #%1.").arg(logId)
                else
                    sub = live.length ? (liveN > 1 ? qsTr("Saving — %1 are connected.").arg(live)
                                                   : qsTr("Saving — %1 connected.").arg(live))
                                      : qsTr("Saving.")
                return { sev: sev, word: qsTr("Recording"), sub: sub, pulse: true }
            }
            if (st === 1)
                return { sev: sev, word: qsTr("Idle"), sub: qsTr("Armed — waiting for data."), pulse: false }
            return { sev: sev, word: qsTr("Starting…"), sub: qsTr("Recorder is coming up."), pulse: false }
        }

        // Input ports — ALL THREE always shown. Per-port state: "live" (status_flags,
        // green), "lost" (degraded_flags, red), "off" (neither seen, gray hollow).
        function _sourceChips() {
            if (!dev || !dev.recorderStatusValid) return []
            var sf = dev.recorderStatusFlags || 0, df = dev.recorderDegradedFlags || 0
            var defs = [{ b: 1, n: qsTr("Sonar 1"), s: qsTr("S1") },
                        { b: 2, n: qsTr("Sonar 2"), s: qsTr("S2") },
                        { b: 4, n: qsTr("Nav"), s: qsTr("Nav") }]
            var out = []
            for (var i = 0; i < defs.length; i++)
                out.push({ name: defs[i].n, short: defs[i].s,
                           state: (sf & defs[i].b) ? "live" : ((df & defs[i].b) ? "lost" : "off") })
            return out
        }

        // Severity tint for the banner background (low-alpha sev colour; idle = none).
        function _sevBg(s) {
            if (s === "idle") return "transparent"
            var c = _sevColor(s)
            return Qt.rgba(c.r, c.g, c.b, 0.12)
        }
        // Free-space "error" (blink): storage fault, or free space critically low (<2 GB).
        function _freeError() {
            if (!dev || !dev.recorderStatusValid) return false
            if (dev.recorderCriticalFlags) return true
            return recorderGroup.freeBytes > 0 && recorderGroup.freeBytes < 2000000000
        }

        // A vital-number tile for the banner: label + value. keyBox = green (good), errorBox
        // = red border + blink, alertBox = red static (e.g. dropping frames).
        component StatBox: Rectangle {
            property string blabel: ""
            property string bvalue: ""
            property bool keyBox: false
            property bool errorBox: false
            property bool alertBox: false
            height: Math.round(42 * AppPalette.scale); radius: Tokens.radiusMd
            implicitWidth: sbCol.implicitWidth + Tokens.spaceMd * 2; width: implicitWidth
            color: AppPalette.card
            border.width: 1
            border.color: (errorBox || alertBox) ? AppPalette.linkDownBorder
                        : keyBox ? Qt.rgba(AppPalette.linkOkBorder.r, AppPalette.linkOkBorder.g, AppPalette.linkOkBorder.b, 0.45)
                        : AppPalette.border
            Column {
                id: sbCol
                anchors.centerIn: parent; spacing: Math.round(1 * AppPalette.scale)
                Text { visible: blabel.length > 0; text: blabel; color: AppPalette.textMuted
                       font.pixelSize: Tokens.fontXs }
                Text { text: bvalue; font.pixelSize: Tokens.fontSm; font.bold: true
                       color: (errorBox || alertBox) ? AppPalette.linkDownText
                            : keyBox ? AppPalette.linkOkText : AppPalette.text }
            }
            Rectangle {   // blink glow when errorBox
                anchors.fill: parent; radius: parent.radius; color: "transparent"
                border.width: Math.round(2 * AppPalette.scale); border.color: AppPalette.linkDownBorder
                visible: errorBox
                SequentialAnimation on opacity {
                    running: errorBox; loops: Animation.Infinite
                    NumberAnimation { from: 1.0; to: 0.12; duration: 560; easing.type: Easing.InOutSine }
                    NumberAnimation { from: 0.12; to: 1.0; duration: 560; easing.type: Easing.InOutSine }
                }
            }
        }

        // Freshness: recorderStatusChanged fires every poll (~3s). If it stops (e.g. the
        // telem link is pulled — the serial port stays "open", so link state won't notice)
        // the last snapshot is stale and the device's real state is unknown. Track the age
        // of the last status and flag it, so the readout isn't mistaken for live truth.
        property double _lastStatusMs: 0
        property double _nowMs: 0
        readonly property bool stale: _lastStatusMs > 0 && (_nowMs - _lastStatusMs) > 7000
        readonly property int staleAgeS: _lastStatusMs > 0 ? Math.floor((_nowMs - _lastStatusMs) / 1000) : 0

        Timer { interval: 1000; repeat: true; running: true; onTriggered: recorderGroup._nowMs = Date.now() }
        Connections {
            target: dev
            ignoreUnknownSignals: true
            function onRecorderStatusChanged() { recorderGroup._lastStatusMs = Date.now(); recorderGroup._nowMs = Date.now(); Qt.callLater(recorderGroup._recount) }
        }

        // ── Banner: severity-tinted; hero + vital boxes. Ports are in the header. ──
        Rectangle {
            id: banner
            width: parent.width; radius: Tokens.radiusMd
            color: recorderGroup._sevBg(recorderGroup.hero.sev)
            border.width: recorderGroup.hero.sev === "idle" ? 1 : 0
            border.color: AppPalette.border
            implicitHeight: bannerCol.implicitHeight + Tokens.spaceMd * 2; height: implicitHeight
            opacity: recorderGroup.stale ? 0.6 : 1.0

            Rectangle {   // left severity stripe
                anchors.left: parent.left; anchors.leftMargin: Math.round(2 * AppPalette.scale)
                anchors.top: parent.top; anchors.bottom: parent.bottom
                anchors.topMargin: parent.radius; anchors.bottomMargin: parent.radius
                width: Math.round(3 * AppPalette.scale); radius: width / 2
                color: recorderGroup._sevColor(recorderGroup.hero.sev)
                visible: recorderGroup.hero.sev !== "idle"
            }

            Column {
                id: bannerCol
                anchors.left: parent.left; anchors.right: parent.right
                anchors.leftMargin: Tokens.spaceMd + Math.round(4 * AppPalette.scale)
                anchors.rightMargin: Tokens.spaceMd
                anchors.verticalCenter: parent.verticalCenter
                spacing: Tokens.spaceMd

                Row {
                    id: heroRow
                    width: parent.width; spacing: Tokens.spaceMd

                    Column {
                        id: heroTextBlock
                        anchors.verticalCenter: parent.verticalCenter
                        width: heroRow.width - (freeBadge.visible ? freeBadge.width + heroRow.spacing : 0)
                        spacing: Math.round(2 * AppPalette.scale)

                        Row {
                            width: parent.width; spacing: Tokens.spaceMd
                            Item {
                                id: heroDisc
                                width: Math.round(16 * AppPalette.scale); height: width
                                anchors.verticalCenter: parent.verticalCenter
                                Rectangle {   // pulse ring while actively writing
                                    id: pulseRing
                                    anchors.centerIn: parent; width: parent.width; height: parent.height; radius: width / 2
                                    color: "transparent"; border.width: Math.max(1, Math.round(1.5 * AppPalette.scale))
                                    border.color: recorderGroup._sevColor(recorderGroup.hero.sev)
                                    visible: recorderGroup.hero.pulse
                                    ParallelAnimation {
                                        running: recorderGroup.hero.pulse; loops: Animation.Infinite
                                        NumberAnimation { target: pulseRing; property: "scale"; from: 1.0; to: 2.4; duration: 1500; easing.type: Easing.OutQuad }
                                        NumberAnimation { target: pulseRing; property: "opacity"; from: 0.5; to: 0.0; duration: 1500; easing.type: Easing.OutQuad }
                                    }
                                }
                                Rectangle {   // the severity disc
                                    anchors.centerIn: parent; width: parent.width; height: parent.height; radius: width / 2
                                    color: recorderGroup._sevColor(recorderGroup.hero.sev)
                                }
                            }
                            Text {
                                id: heroWord
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - heroDisc.width - parent.spacing
                                text: recorderGroup.hero.word; color: recorderGroup._sevText(recorderGroup.hero.sev)
                                font.pixelSize: Tokens.fontXxl; font.bold: true
                                elide: Text.ElideRight
                            }
                        }

                        Text {
                            width: parent.width
                            visible: recorderGroup.hero.sub.length > 0
                            text: recorderGroup.hero.sub; color: AppPalette.textSecond; font.pixelSize: Tokens.fontSm
                            wrapMode: Text.WordWrap
                        }
                    }

                    Rectangle {
                        id: freeBadge
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !!(dev && dev.recorderStatusValid)
                        radius: Tokens.radiusMd
                        implicitWidth: freeBadgeCol.implicitWidth + Tokens.spaceMd * 2; width: implicitWidth
                        height: heroTextBlock.implicitHeight
                        color: AppPalette.card
                        border.width: 1
                        border.color: recorderGroup._freeError() ? AppPalette.linkDownBorder
                                    : (recorderGroup.hero.sev === "good" || recorderGroup.hero.sev === "idle")
                                      ? Qt.rgba(AppPalette.linkOkBorder.r, AppPalette.linkOkBorder.g, AppPalette.linkOkBorder.b, 0.45)
                                      : AppPalette.border
                        Column {
                            id: freeBadgeCol
                            anchors.centerIn: parent; spacing: Math.round(2 * AppPalette.scale)
                            Text {
                                text: qsTr("Free space"); color: AppPalette.textMuted; font.pixelSize: Tokens.fontXs
                                anchors.horizontalCenter: parent.horizontalCenter
                            }
                            Text {
                                text: (dev && (dev.recorderCriticalFlags & 1)) ? "—" : recorderGroup._fmtSize(recorderGroup.freeBytes)
                                color: recorderGroup._freeError() ? AppPalette.linkDownText
                                     : (recorderGroup.hero.sev === "good" || recorderGroup.hero.sev === "idle") ? AppPalette.linkOkText
                                     : AppPalette.text
                                font.pixelSize: Tokens.fontSm; font.bold: true
                                anchors.horizontalCenter: parent.horizontalCenter
                            }
                        }
                        Rectangle {   // blink glow when free space is critical
                            anchors.fill: parent; radius: parent.radius; color: "transparent"
                            border.width: Math.round(2 * AppPalette.scale); border.color: AppPalette.linkDownBorder
                            visible: recorderGroup._freeError()
                            SequentialAnimation on opacity {
                                running: recorderGroup._freeError(); loops: Animation.Infinite
                                NumberAnimation { from: 1.0; to: 0.12; duration: 560; easing.type: Easing.InOutSine }
                                NumberAnimation { from: 0.12; to: 1.0; duration: 560; easing.type: Easing.InOutSine }
                            }
                        }
                    }
                }

                Flow {
                    width: parent.width; spacing: Tokens.spaceSm
                    visible: !!(dev && dev.recorderStatusValid && (dev.recorderDegradedFlags & 0x8))
                    StatBox {
                        visible: !!(dev && dev.recorderStatusValid && (dev.recorderDegradedFlags & 0x8))
                        alertBox: true
                        bvalue: qsTr("⚠ Dropping frames")
                    }
                }
            }
        }

        // Stale note — the banner already shows "No response"; this labels the frozen values.
        Text {
            visible: recorderGroup.stale
            width: parent.width
            text: qsTr("Values may be outdated.")
            color: AppPalette.textMuted; font.pixelSize: Tokens.fontXs
        }

        Item {
            visible: false
            Repeater {
                id: streamScan
                model: (typeof deviceManagerWrapper !== "undefined" && deviceManagerWrapper) ? deviceManagerWrapper.streamsList : null
                onCountChanged: Qt.callLater(recorderGroup._recount)
                delegate: Item {
                    readonly property int  rId: id
                    readonly property int  rRec: recordState
                    readonly property real rSize: size
                    readonly property real rDone: doneSize
                    readonly property int  rUpload: uploadState
                    readonly property bool _isCur: recorderGroup._curId > 0 && rId === recorderGroup._curId
                    readonly property bool _hidden: _isCur || rRec === 3
                    onRIdChanged: Qt.callLater(recorderGroup._recount)
                    onRRecChanged: Qt.callLater(recorderGroup._recount)
                    onRSizeChanged: Qt.callLater(recorderGroup._recount)
                    onRDoneChanged: Qt.callLater(recorderGroup._recount)
                    onRUploadChanged: Qt.callLater(recorderGroup._recount)
                    Component.onCompleted: Qt.callLater(recorderGroup._recount)
                }
            }
        }

        Row {
            width: parent.width; height: Tokens.controlHMd; spacing: Tokens.spaceSm
            Text {
                text: qsTr("Logs (%1)").arg(Math.max(logList.count, recorderGroup._curId > 0 ? 1 : 0))
                color: AppPalette.textSecond; font.pixelSize: Tokens.fontMd; font.bold: true
                width: parent.width - refreshBtn.width - parent.spacing
                anchors.verticalCenter: parent.verticalCenter; elide: Text.ElideRight
            }
            DevButton {
                id: refreshBtn
                width: Math.round(112 * AppPalette.scale); height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                text: qsTr("Refresh")
                onClicked: if (typeof deviceManagerWrapper !== "undefined" && deviceManagerWrapper) deviceManagerWrapper.refreshStreamList()
            }
        }

        Text {
            visible: logList.count === 0 && recorderGroup._curId === 0
            width: parent.width
            text: qsTr("No logs listed yet — tap Refresh to enumerate the recorder's archive.")
            color: AppPalette.textMuted; font.pixelSize: Tokens.fontSm; wrapMode: Text.WordWrap
        }

        // Current log + archive kept tight together (small gap) so the plate reads as the
        // list's pinned first entry.
        Column {
            id: logsBlock
            width: parent.width
            spacing: Math.round(3 * AppPalette.scale)

        // ── Current log plate: pinned directly above the archive list, reads as its first
        // entry. Live/last log + download; hidden when there is no current log. ──
        Rectangle {
            id: curLogPlate
            width: parent.width; radius: Tokens.radiusMd
            color: AppPalette.card
            border.width: 1
            // frame mirrors the recorder state (same severity colour as the status banner)
            border.color: recorderGroup.hero.sev === "idle"
                          ? AppPalette.border
                          : recorderGroup._sevColor(recorderGroup.hero.sev)
            height: Tokens.controlHMd + Tokens.spaceMd * 2
            visible: curLogPlate._hasLog

            readonly property bool _hasLog: recorderGroup._curId > 0
            readonly property bool _recording: !!(dev && dev.recorderStatusValid && dev.recorderRecordingState === 2)
            readonly property bool _uploading: recorderGroup._curUpload === 3
            readonly property bool _doneState: recorderGroup._curUpload === 1 && recorderGroup._curDone > 0
            readonly property real _pct: recorderGroup._curSize > 0 ? Math.max(0, Math.min(1, recorderGroup._curDone / recorderGroup._curSize)) : 0
            readonly property bool _active: _uploading || _revealDone
            readonly property int _shift: Math.round(7 * AppPalette.scale)
            readonly property string _sizeText: recorderGroup._fmtSize(curLogPlate._recording ? recorderGroup.recordedBytes : recorderGroup._curSize)
            property bool _initiated: false
            property bool _revealDone: false

            on_DoneStateChanged: {
                if (_doneState && _initiated) {
                    _initiated = false
                    _revealDone = true
                    plateHideTimer.restart()
                    if (typeof notifications !== "undefined" && notifications)
                        notifications.info(qsTr("Saved \"%1\" to \"%2\"").arg("recorder_log_" + recorderGroup._curId + ".klf").arg(recorderGroup._downloadDir))
                }
            }
            on_UploadingChanged: if (_uploading) { _revealDone = false; plateHideTimer.stop() }
            Timer { id: plateHideTimer; interval: 4000; onTriggered: curLogPlate._revealDone = false }

            DevButton {
                id: dlBtnPlate
                anchors.right: parent.right; anchors.rightMargin: Tokens.spaceLg
                anchors.verticalCenter: parent.verticalCenter
                width: Math.round(112 * AppPalette.scale); height: Tokens.controlHMd
                fontPixelSize: Tokens.fontMd
                danger: curLogPlate._uploading
                text: curLogPlate._uploading ? qsTr("Cancel") : (curLogPlate._revealDone ? qsTr("Re-download") : qsTr("Download"))
                onClicked: {
                    if (typeof deviceManagerWrapper === "undefined" || !deviceManagerWrapper || !curLogPlate._hasLog)
                        return
                    if (curLogPlate._uploading)
                        deviceManagerWrapper.cancelStreamDownload(recorderGroup._curId)
                    else {
                        curLogPlate._initiated = true
                        deviceManagerWrapper.startStreamDownload(recorderGroup._curId)
                    }
                }
            }

            Row {
                id: plateTextRow
                anchors.left: parent.left; anchors.leftMargin: Tokens.spaceLg
                anchors.right: dlBtnPlate.left; anchors.rightMargin: Tokens.spaceMd
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: curLogPlate._active ? -curLogPlate._shift : 0
                Behavior on anchors.verticalCenterOffset { NumberAnimation { duration: Anim.fadeMs; easing.type: Easing.OutCubic } }
                spacing: Tokens.spaceSm

                Text {
                    text: "#" + recorderGroup._curId; color: AppPalette.text; font.pixelSize: Tokens.fontMd; font.bold: true
                    width: Math.round(52 * AppPalette.scale); anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                }
                Text {
                    text: curLogPlate._sizeText; color: AppPalette.textSecond; font.pixelSize: Tokens.fontSm
                    width: Math.round(84 * AppPalette.scale); anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    text: curLogPlate._uploading ? qsTr("Downloading… %1%").arg(Math.round(curLogPlate._pct * 100))
                                         : (curLogPlate._revealDone ? qsTr("Saved")
                                            : (curLogPlate._recording ? recorderGroup._logDuration(dev.recorderDurationSeconds) : ""))
                    color: curLogPlate._revealDone ? AppPalette.accentBar : AppPalette.textMuted
                    font.pixelSize: Tokens.fontSm
                    width: plateTextRow.width - Math.round(52 * AppPalette.scale) - Math.round(84 * AppPalette.scale) - 2 * plateTextRow.spacing
                    anchors.verticalCenter: parent.verticalCenter; elide: Text.ElideRight
                }
            }

            Rectangle {
                id: plateProgress
                anchors.left: parent.left; anchors.leftMargin: Tokens.spaceLg
                anchors.right: dlBtnPlate.left; anchors.rightMargin: Tokens.spaceMd
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: Math.round(9 * AppPalette.scale)
                height: Math.round(4 * AppPalette.scale); radius: height / 2
                color: AppPalette.trackOff
                opacity: curLogPlate._active ? 1 : 0
                visible: opacity > 0.01
                Behavior on opacity { NumberAnimation { duration: Anim.fadeMs } }
                Rectangle {
                    height: parent.height; radius: parent.radius
                    width: parent.width * curLogPlate._pct
                    color: AppPalette.accentBar
                    Behavior on width { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                }
            }
        }

        Rectangle {
            width: parent.width
            visible: recorderGroup._otherLogsCount > 0
            readonly property int rowH: Tokens.controlHMd + Tokens.spaceMd * 2
            height: Math.min(recorderGroup._otherLogsCount * rowH, Math.round(rowH * 3.5)) + 2
            color: AppPalette.bg; radius: Tokens.radiusMd
            border.width: Tokens.cardBorderWidth; border.color: AppPalette.border
            clip: true

            ListView {
                id: logList
                anchors.fill: parent; anchors.margins: 1
                clip: true
                model: (typeof deviceManagerWrapper !== "undefined" && deviceManagerWrapper) ? deviceManagerWrapper.streamsList : null
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {
                    id: logScroll
                    policy: ScrollBar.AsNeeded
                    implicitWidth: Tokens.spaceLg
                    readonly property int _handleW: Math.max(6, Math.round(7 * AppPalette.scale))
                    leftPadding: (implicitWidth - _handleW) / 2
                    rightPadding: leftPadding
                    topPadding: Math.round(2 * AppPalette.scale)
                    bottomPadding: topPadding

                    property bool _shown: false
                    onActiveChanged: { if (active) { _shown = true; hideTimer.stop() } else hideTimer.restart() }
                    Timer { id: hideTimer; interval: 1100; onTriggered: logScroll._shown = false }

                    contentItem: Rectangle {
                        radius: width / 2
                        color: logScroll.pressed ? AppPalette.borderHover : AppPalette.border
                        opacity: logScroll._shown ? 1.0 : 0.0
                        Behavior on opacity { NumberAnimation { duration: Anim.fadeMs } }
                    }
                }

                delegate: Item {
                    id: rowItem
                    readonly property bool _hidden: (recorderGroup._curId > 0 && id === recorderGroup._curId) || recordState === 3
                    width: ListView.view ? ListView.view.width : 0
                    height: _hidden ? 0 : (Tokens.controlHMd + Tokens.spaceMd * 2)
                    visible: !_hidden

                    readonly property bool _uploading: uploadState === 3
                    readonly property bool _done: uploadState === 1 && doneSize > 0
                    readonly property real _pct: size > 0 ? Math.max(0, Math.min(1, doneSize / size)) : 0
                    // active = downloading, or the brief reveal after a user-initiated download
                    // finishes; drives the progress bar + text-lift, then auto-reverts.
                    property bool _initiated: false
                    property bool _revealDone: false
                    readonly property bool _active: _uploading || _revealDone
                    readonly property int _shift: Math.round(7 * AppPalette.scale)

                    on_DoneChanged: {
                        if (_done && _initiated) {
                            _initiated = false
                            _revealDone = true
                            hideTimer.restart()
                            if (typeof notifications !== "undefined" && notifications)
                                notifications.info(qsTr("Saved \"%1\" to \"%2\"").arg("recorder_log_" + id + ".klf").arg(recorderGroup._downloadDir))
                        }
                    }
                    on_UploadingChanged: if (_uploading) { _revealDone = false; hideTimer.stop() }

                    Timer { id: hideTimer; interval: 4000; onTriggered: rowItem._revealDone = false }

                    DevButton {
                        id: dlBtn
                        anchors.right: parent.right; anchors.rightMargin: Tokens.spaceLg
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.round(112 * AppPalette.scale); height: Tokens.controlHMd
                        fontPixelSize: Tokens.fontMd
                        danger: rowItem._uploading
                        text: rowItem._uploading ? qsTr("Cancel") : (rowItem._revealDone ? qsTr("Re-download") : qsTr("Download"))
                        onClicked: {
                            if (typeof deviceManagerWrapper === "undefined" || !deviceManagerWrapper)
                                return
                            if (rowItem._uploading)
                                deviceManagerWrapper.cancelStreamDownload(id)
                            else {
                                rowItem._initiated = true
                                deviceManagerWrapper.startStreamDownload(id)
                            }
                        }
                    }

                    Row {
                        id: textRow
                        anchors.left: parent.left; anchors.leftMargin: Tokens.spaceLg
                        anchors.right: dlBtn.left; anchors.rightMargin: Tokens.spaceMd
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.verticalCenterOffset: rowItem._active ? -rowItem._shift : 0
                        Behavior on anchors.verticalCenterOffset { NumberAnimation { duration: Anim.fadeMs; easing.type: Easing.OutCubic } }
                        spacing: Tokens.spaceSm

                        Text {
                            text: "#" + id; color: AppPalette.text; font.pixelSize: Tokens.fontMd; font.bold: true
                            width: Math.round(52 * AppPalette.scale); anchors.verticalCenter: parent.verticalCenter
                            elide: Text.ElideRight
                        }
                        Text {
                            text: recorderGroup._fmtSize(size); color: AppPalette.textSecond; font.pixelSize: Tokens.fontSm
                            width: Math.round(84 * AppPalette.scale); anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            id: statusText
                            text: rowItem._uploading ? qsTr("Downloading… %1%").arg(Math.round(rowItem._pct * 100))
                                             : (rowItem._revealDone ? qsTr("Saved") : "")
                            color: rowItem._revealDone ? AppPalette.accentBar : AppPalette.textMuted
                            font.pixelSize: Tokens.fontSm
                            width: textRow.width - Math.round(52 * AppPalette.scale) - Math.round(84 * AppPalette.scale) - 2 * textRow.spacing
                            anchors.verticalCenter: parent.verticalCenter; elide: Text.ElideRight
                        }
                    }

                    Rectangle {
                        id: progress
                        anchors.left: parent.left; anchors.leftMargin: Tokens.spaceLg
                        anchors.right: dlBtn.left; anchors.rightMargin: Tokens.spaceMd
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.verticalCenterOffset: Math.round(9 * AppPalette.scale)
                        height: Math.round(4 * AppPalette.scale); radius: height / 2
                        color: AppPalette.trackOff
                        opacity: rowItem._active ? 1 : 0
                        visible: opacity > 0.01
                        Behavior on opacity { NumberAnimation { duration: Anim.fadeMs } }
                        Rectangle {
                            height: parent.height; radius: parent.radius
                            width: parent.width * rowItem._pct
                            color: AppPalette.accentBar
                            Behavior on width { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                        }
                    }

                    Rectangle {
                        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                        height: 1; color: AppPalette.border; opacity: 0.5
                        visible: !rowItem._hidden && index < logList.count - 1
                    }
                }
            }
        }
        }

        // Mirrors stream_list.cpp: QStandardPaths::DocumentsLocation + "/KoggerApp/recorder".
        readonly property string _downloadDir: {
            var s = String(StandardPaths.writableLocation(StandardPaths.DocumentsLocation))
            if (s.indexOf("file:///") === 0)      s = (Qt.platform.os === "windows" ? s.slice(8) : s.slice(7))
            else if (s.indexOf("file://") === 0)  s = s.slice(7)
            try { s = decodeURIComponent(s) } catch (e) {}
            return s + "/KoggerApp/recorder"
        }

        Text {
            width: parent.width
            text: qsTr("Downloads are saved to \"%1\"").arg(recorderGroup._downloadDir)
            color: AppPalette.textSecond; font.pixelSize: Tokens.fontSm; wrapMode: Text.WordWrap
        }
    }

    // ── Basic2D user view ─────────────────────────────────────────────────

    Column {
        id: basicView
        visible: root._isBasicSonar
        width: root.groupWidth
        spacing: Tokens.spaceLg

        Reveal {
            open: root.warningShown
            contentHeight: warnBanner.implicitHeight
            Rectangle {
                id: warnBanner
                width: parent.width
                radius: Tokens.radiusMd
                readonly property color _accent: "#EAB308"
                color: Qt.rgba(_accent.r, _accent.g, _accent.b, AppPalette.isDark ? 0.14 : 0.16)
                border.width: Math.max(1, Math.round(AppPalette.scale))
                border.color: _accent
                implicitHeight: warnRow.implicitHeight + 2 * Tokens.spaceMd
                Row {
                    id: warnRow
                    x: Tokens.spaceMd; y: Tokens.spaceMd
                    width: parent.width - 2 * Tokens.spaceMd
                    spacing: Tokens.spaceMd
                    readonly property int badge: Math.round(20 * AppPalette.scale)
                    Rectangle {
                        width: warnRow.badge; height: width; radius: width / 2
                        color: warnBanner._accent
                        Text { anchors.centerIn: parent; text: "!"; color: "#10171F"
                               font.bold: true; font.pixelSize: Math.round(13 * AppPalette.scale) }
                    }
                    Column {
                        width: parent.width - parent.spacing - warnRow.badge
                        spacing: Tokens.spaceXxs
                        Text { text: qsTr("Not applied to the device"); color: AppPalette.textStrong
                               font.pixelSize: Tokens.fontBase; font.bold: true }
                        Repeater {
                            model: root._warnings
                            delegate: Text {
                                width: parent.width
                                text: "• " + modelData
                                color: AppPalette.textSecond
                                font.pixelSize: Tokens.fontSm
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }
            }
        }

        KIsland {
            slotWidth: root.spinToggleSlotW

            KIslandRow {
                id: periodRow
                label: qsTr("Period, ms")

                property int _periodRestore: 100
                readonly property bool wantChecked: !!(dev && dev.ch1Period > 0)
                onWantCheckedChanged: periodSwitch.pushFromDev()
                Component.onCompleted: periodSwitch.pushFromDev()

                Item {
                    width: parent.width
                    height: Tokens.controlHMd

                    DevSpin {
                        width: root.spinW
                        height: parent.height
                        enabled: periodSwitch.checked
                        opacity: enabled ? 1.0 : 0.45
                        from: 10; to: 2000; divisor: 1; decimals: 0
                        stepValues: root.periodStepsMs
                        devValue: dev ? (dev.ch1Period || 0) : 0
                        writeBack: function(v) { if (dev) dev.ch1Period = v }
                    }

                    KSwitch {
                        id: periodSwitch
                        flat: true
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter

                        property bool _g: false
                        function pushFromDev() {
                            if (checked === periodRow.wantChecked) return
                            _g = true; checked = periodRow.wantChecked; _g = false
                        }
                        onToggled: {
                            if (_g || !dev) return
                            if (checked) {
                                dev.ch1Period = periodRow._periodRestore
                            } else {
                                if (dev.ch1Period > 0) periodRow._periodRestore = dev.ch1Period
                                dev.ch1Period = 0
                            }
                        }
                    }
                }
            }

            KIslandRow {
                label: qsTr("Distance, m")
                slotWidth: root.spinW
                DevSpin {
                    from: 1; to: 150; divisor: 1; decimals: 0
                    stepValues: root.distanceStepsM
                    devValue: (dev && dev.chartResolution > 0) ? Math.round(dev.chartResolution * dev.chartSamples / 1000) : 0
                    writeBack: function(v) {
                        if (dev && dev.chartResolution > 0)
                            dev.chartSamples = Math.max(root.chartSamplesMin, Math.min(root.chartSamplesMax, Math.round(v * 1000 / dev.chartResolution)))
                    }
                }
            }

            KIslandRow {
                label: qsTr("Frequency, kHz")
                visible: root._isNanoSSS
                slotWidth: root.spinW
                DevSpin { from: 40; to: 6000; stepSize: 5; divisor: 1; decimals: 0; devValue: dev ? (dev.transFreq || 0) : 0; writeBack: function(v) { if (dev) dev.transFreq = v } }
            }

            KIslandSection { label: qsTr("Echogram") }

            DevStackedRow {
                KTabBar {
                    id: b2ChartTab; width: parent.width
                    trackColor: AppPalette.bgDeep
                    // Compressed is a third state, not a second channel. The two
                    // formats are mutually exclusive: asking for both puts two
                    // pictures of the same ping into one plot, and since the codec
                    // is lossy they do not even agree.
                    // IQ and RAW are not echogram formats but taps: the device keeps
                    // ranging, sends no echogram on this link, and the second core
                    // streams the baseband (IQ) or the raw ADC samples (RAW) over its
                    // own USB port, which UsbTap picks up and records.
                    options: [{ label: qsTr("Off"), value: 0 }, { label: qsTr("8-bit"), value: 1 }, { label: qsTr("Compressed"), value: 2 }, { label: qsTr("IQ"), value: 3 }, { label: qsTr("RAW"), value: 4 }]
                    property int chartModel: dev ? (dev.datasetTapRaw === 1 ? 4 : (dev.datasetTapIq === 1 ? 3 : (dev.datasetChartZ === 1 ? 2 : (dev.datasetChart === 1 ? 1 : 0)))) : 0
                    property bool _g: false
                    onChartModelChanged: {
                        if (currentValue !== chartModel) { _g = true; currentValue = chartModel; _g = false }
                        // The device may come up already in a tap mode (the mask is kept in its flash).
                        if (chartModel >= 3) usbTap.setMode(chartModel - 2, core.logDirectory())
                    }
                    Component.onCompleted: { _g = true; currentValue = chartModel; _g = false }
                    onValueSelected: function(v) {
                        if (_g || !dev) return
                        dev.datasetChart  = (v === 1) ? 1 : 0
                        dev.datasetChartZ = (v === 2) ? 1 : 0
                        dev.datasetTapIq  = (v === 3) ? 1 : 0
                        dev.datasetTapRaw = (v === 4) ? 1 : 0
                        usbTap.setMode(v >= 3 ? v - 2 : 0, core.logDirectory())
                    }
                }
                Text {
                    visible: !!(dev && (dev.datasetTapIq === 1 || dev.datasetTapRaw === 1))
                    width: parent.width; wrapMode: Text.Wrap
                    color: AppPalette.textSecond; font.pixelSize: Tokens.fontMd
                    text: usbTap.statusText
                }
            }

            KIslandRow {
                label: qsTr("Resolution, cm")
                open: b2ChartTab.currentValue !== 0
                slotWidth: root.spinW
                DevSpin {
                    from: 10; to: 100; stepSize: 10; divisor: 10; decimals: 1; trimZeros: true
                    devValue: dev ? (dev.chartResolution || 0) : 0
                    writeBack: function(v) {
                        if (!dev || v <= 0) return
                        var distCm = (dev.chartResolution > 0) ? Math.round(dev.chartResolution * dev.chartSamples / 10) : 0
                        dev.chartResolution = v
                        if (distCm > 0)
                            dev.chartSamples = Math.max(root.chartSamplesMin, Math.min(root.chartSamplesMax, Math.round(distCm * 10 / v)))
                    }
                }
            }

            KIslandSection { label: qsTr("Rangefinder") }

            DevStackedRow {
                KTabBar {
                    id: b2DistTab; width: parent.width
                    trackColor: AppPalette.bgDeep
                    options: [{ label: qsTr("Off"), value: 0 }, { label: qsTr("On"), value: 1 }, { label: qsTr("NMEA"), value: 2 }]
                    property int distModel: dev ? (dev.datasetDist === 1 ? 1 : (dev.datasetSDDBT === 1 ? 2 : 0)) : 0
                    property bool _g: false
                    onDistModelChanged: { if (currentValue !== distModel) { _g = true; currentValue = distModel; _g = false } }
                    Component.onCompleted: { _g = true; currentValue = distModel; _g = false }
                    onValueSelected: function(v) {
                        if (_g || !dev) return
                        if (v === 1)      { dev.datasetDist = 1 }
                        else if (v === 2) { dev.datasetSDDBT = 1 }
                        else              { dev.datasetDist = 0; dev.datasetSDDBT = 0 }
                    }
                }
            }

            KIslandRow {
                label: qsTr("Confidence threshold, %")
                open: b2DistTab.currentValue !== 0
                slotWidth: root.spinW
                DevSpin { from: 0; to: 100; stepSize: 1; devValue: dev ? (dev.distConfidence || 0) : 0; writeBack: function(v) { if (dev) dev.distConfidence = v } }
            }

            DevStackedRow {
                forceSeparator: true
                FlashButton { width: parent.width }
            }
        }
    }

    // ── USBL ──────────────────────────────────────────────────────────────
    // Three groups ordered by how often they are touched: operating (nodes + schedule)
    // stays open, the command plan and the response gating start collapsed. The plan model
    // is shared by the first two and persists independently of the device.
    UsblGroup {
        width: root.groupWidth; preferredWidth: root.groupWidth
        dev: root.dev
        plan: root.usblPlan
        engine: root.usblEngine
    }

    UsblPlanGroup {
        width: root.groupWidth; preferredWidth: root.groupWidth
        dev: root.dev
        plan: root.usblPlan
    }

    UsblResponseGroup {
        width: root.groupWidth; preferredWidth: root.groupWidth
        dev: root.dev
    }

    Reveal {
        id: favoritesReveal
        open: root.hasPinnableParams && (root.favoriteKeys.length > 0 || root.favEditMode)
        contentHeight: favoritesIsland.implicitHeight
        visible: height > 0.5

        DevIsland {
            id: favoritesIsland
            title: qsTr("Favourites:")
            titleColor: AppPalette.textSecond
            titleInset: Tokens.spaceXxs

            Repeater {
                model: root.favoriteKeys
                delegate: DeviceParamRow {
                    page: root
                    paramKey: modelData
                    favorite: true
                }
            }

            KIslandRow {
                open: root.favEditMode && root.favoriteKeys.length === 0
                minHeight: 0
                verticalPadding: Tokens.spaceMd
                label: qsTr("Tap a star next to a setting to pin it here")
                labelPixelSize: Tokens.fontSm
                labelColor: AppPalette.textMuted
            }
        }
    }

    // ── Wi‑Fi module ───────────────────────────────────────────────────────
    // Board 87 is a Wi‑Fi client on the head unit's own UART: it joins the boat's access point and
    // reports the link. The island mirrors dev.wifi* (DevQProperty) and sends the ID_WIFI commands;
    // contract in docs/WIFI_MODULE.md. The password field brings up the on-screen keyboard on a
    // touch screen like any other input.
    DevIsland {
        id: wifiIsland
        width: root.groupWidth
        visible: root._isWifi
        rowMinHeight: Tokens.controlHMd + Tokens.spaceXs

        readonly property bool live: root._isWifi && !!dev.wifiStatusValid
        readonly property int wifiState: live ? dev.wifiState : 0
        readonly property bool connected: live && wifiState === 3
        readonly property bool busy: live && (wifiState === 1 || wifiState === 2)
        readonly property string ssid: live ? dev.wifiSsid : ""
        readonly property int rssi: live ? dev.wifiRssi : 0
        readonly property int channel: live ? dev.wifiChannel : 0
        readonly property int security: live ? dev.wifiSecurity : 0
        readonly property int reason: live ? dev.wifiReason : 0
        readonly property string ip: live ? dev.wifiIp : ""
        readonly property int rxBps: live ? dev.wifiRxBps : 0
        readonly property int txBps: live ? dev.wifiTxBps : 0
        readonly property real rxTotal: live ? dev.wifiRxTotal : 0
        readonly property real txTotal: live ? dev.wifiTxTotal : 0
        readonly property int uptime: live ? dev.wifiUptime : 0
        readonly property bool autoConnect: live && !!dev.wifiAutoConnect
        readonly property bool currentSaved: live && !!dev.wifiCurrentSaved
        readonly property bool scanning: root._isWifi && !!dev.wifiScanning
        readonly property var networks: root._isWifi ? dev.wifiNetworks : []
        readonly property var savedList: root._isWifi ? dev.wifiSavedNetworks : []

        // The network the operator tapped; the join rows stay open while it is set. The module
        // never reveals a stored password and joins with exactly what it is sent, so a saved
        // network is joined by its auto-connect or by typing the password once more.
        property string pickSsid: ""
        property int pickSecurity: 0
        property bool pickSaved: false
        property bool pickHidden: false
        readonly property bool pickActive: pickSsid.length > 0 || pickHidden
        // A join was sent for this SSID; the module's next status frames carry the verdict.
        property string awaitingSsid: ""

        function stateText(st) {
            switch (st) {
            case 3: return qsTr("Connected")
            case 2: return qsTr("Connecting…")
            case 1: return qsTr("Searching…")
            }
            return qsTr("Not connected")
        }
        function securityText(sec) {
            switch (sec) {
            case 0: return qsTr("Open")
            case 1: return "WEP"
            case 2: return "WPA"
            case 3: return "WPA2"
            case 4: return "WPA/WPA2"
            case 5: return "WPA3"
            case 6: return "WPA2/WPA3"
            case 7: return qsTr("Enterprise")
            case 8: return "OWE"
            }
            return qsTr("Secured")
        }
        function reasonText(r) {
            switch (r) {
            case 1: return qsTr("network not found")
            case 2: return qsTr("wrong password")
            case 3: return qsTr("association failed")
            case 4: return qsTr("connection lost")
            case 5: return qsTr("disconnected")
            }
            return ""
        }
        function rateText(bps) {
            if (bps >= 1048576) return (bps / 1048576).toFixed(2) + " " + qsTr("MB/s")
            if (bps >= 1024) return (bps / 1024).toFixed(1) + " " + qsTr("KB/s")
            return bps + " " + qsTr("B/s")
        }
        function sizeText(bytes) {
            if (bytes >= 1073741824) return (bytes / 1073741824).toFixed(2) + " " + qsTr("GB")
            if (bytes >= 1048576) return (bytes / 1048576).toFixed(1) + " " + qsTr("MB")
            if (bytes >= 1024) return Math.round(bytes / 1024) + " " + qsTr("KB")
            return bytes + " " + qsTr("B")
        }
        function uptimeText(sec) {
            var h = Math.floor(sec / 3600)
            var m = Math.floor((sec % 3600) / 60)
            return h > 0 ? qsTr("%1 h %2 min").arg(h).arg(m) : qsTr("%1 min").arg(m)
        }
        function pick(ssid, security, saved, hidden) {
            pickSsid = ssid
            pickSecurity = security
            pickSaved = saved
            pickHidden = hidden
            wifiPassField.text = ""
            wifiSsidField.text = hidden ? "" : ssid
            wifiRemember.checked = !saved
            if (hidden)
                wifiSsidField.takeFocus()
            else if (security !== 0)
                wifiPassField.takeFocus()
        }
        function closePick() {
            pickSsid = ""
            pickHidden = false
            wifiPassField.text = ""
            root.forceActiveFocus()
        }
        function doConnect() {
            if (!dev)
                return
            var ssid = pickHidden ? wifiSsidField.text : pickSsid
            if (!ssid.length) {
                notifications.warning(qsTr("Enter the network name"))
                return
            }
            var pass = pickSecurity !== 0 ? wifiPassField.text : ""
            if (dev.wifiConnect(ssid, pass, wifiRemember.checked)) {
                awaitingSsid = ssid
                notifications.info(qsTr("Connecting to %1…").arg(ssid))
                closePick()
            } else {
                notifications.warning(qsTr("Network name or password is too long"))
            }
        }

        onVisibleChanged: if (visible && dev) dev.wifiRefresh()
        Component.onCompleted: if (visible && dev) dev.wifiRefresh()

        Connections {
            target: root._isWifi ? root.dev : null
            function onWifiStatusChanged() {
                if (!wifiIsland.awaitingSsid.length || !wifiIsland.live)
                    return
                if (wifiIsland.wifiState === 3 && wifiIsland.ssid === wifiIsland.awaitingSsid) {
                    notifications.info(qsTr("Wi‑Fi connected: %1").arg(wifiIsland.ssid))
                    wifiIsland.awaitingSsid = ""
                } else if (wifiIsland.wifiState === 0 && wifiIsland.reason !== 0) {
                    notifications.warning(qsTr("Wi‑Fi: %1 (%2)").arg(wifiIsland.awaitingSsid).arg(wifiIsland.reasonText(wifiIsland.reason)))
                    wifiIsland.awaitingSsid = ""
                }
            }
            function onWifiCommandResult(version, resp) {
                if (resp === 1 || version === 0)
                    return
                var why = resp === 8 ? qsTr("the module is busy, try again in a moment")
                        : resp === 3 ? qsTr("the module rejected the request")
                                     : qsTr("response code %1").arg(resp)
                notifications.warning(qsTr("Wi‑Fi command failed: %1").arg(why))
                if (version === 3)
                    wifiIsland.awaitingSsid = ""
            }
        }

        DevIslandTitle {
            label: "Wi‑Fi"
            minHeight: Tokens.rowH
            verticalPadding: 0

            Rectangle {
                id: wifiChip
                readonly property color fg: wifiIsland.connected ? AppPalette.linkOkText
                                          : (wifiIsland.busy ? AppPalette.linkIdleText : AppPalette.linkDownText)
                height: Math.round(24 * AppPalette.scale)
                width: wifiChipText.implicitWidth + 2 * Tokens.spaceMd
                radius: height / 2
                color: wifiIsland.connected ? AppPalette.linkOkBg
                     : (wifiIsland.busy ? AppPalette.linkIdleBg : AppPalette.linkDownBg)
                border.width: Tokens.cardBorderWidth
                border.color: wifiIsland.connected ? AppPalette.linkOkBorder
                            : (wifiIsland.busy ? AppPalette.linkIdleBorder : AppPalette.linkDownBorder)
                Text {
                    id: wifiChipText
                    anchors.centerIn: parent
                    text: wifiIsland.live ? wifiIsland.stateText(wifiIsland.wifiState) : qsTr("No data")
                    color: wifiChip.fg
                    font.pixelSize: Tokens.fontSm
                    font.bold: true
                }
            }
        }

        // Current network
        KIslandRow {
            label: (wifiIsland.connected || wifiIsland.wifiState === 2) ? wifiIsland.ssid : qsTr("Not connected")
            labelBold: wifiIsland.connected
            caption: {
                if (!wifiIsland.live)
                    return qsTr("Waiting for the module…")
                if (wifiIsland.connected)
                    return wifiIsland.rssi + " " + qsTr("dBm") + " · " + qsTr("ch %1").arg(wifiIsland.channel)
                         + " · " + wifiIsland.securityText(wifiIsland.security)
                         + (wifiIsland.ip.length ? " · " + wifiIsland.ip : "")
                if (wifiIsland.wifiState === 2)
                    return qsTr("Connecting…")
                if (wifiIsland.wifiState === 1)
                    return qsTr("Looking for a saved network…")
                var r = wifiIsland.reasonText(wifiIsland.reason)
                if (r.length)
                    return qsTr("Last attempt: %1").arg(r)
                return wifiIsland.savedList.length ? qsTr("Auto-connect is off") : qsTr("No saved networks yet")
            }
            captionColor: (wifiIsland.live && wifiIsland.wifiState === 0 && wifiIsland.reason !== 0)
                          ? AppPalette.dangerText : AppPalette.textSecond
            leading: WifiBars { rssi: wifiIsland.connected ? wifiIsland.rssi : -200 }

            Column {
                visible: wifiIsland.connected
                spacing: Tokens.spaceXxs
                Text {
                    anchors.right: parent.right
                    text: "↓ " + wifiIsland.rateText(wifiIsland.rxBps)
                    color: AppPalette.textSecond
                    font.pixelSize: Tokens.fontSm
                }
                Text {
                    anchors.right: parent.right
                    text: "↑ " + wifiIsland.rateText(wifiIsland.txBps)
                    color: AppPalette.textSecond
                    font.pixelSize: Tokens.fontSm
                }
            }
        }

        DevInfoRow {
            label: qsTr("Traffic")
            value: wifiIsland.connected
                   ? qsTr("%1 received, %2 sent").arg(wifiIsland.sizeText(wifiIsland.rxTotal)).arg(wifiIsland.sizeText(wifiIsland.txTotal))
                   : ""
            caption: wifiIsland.connected ? qsTr("Module up %1").arg(wifiIsland.uptimeText(wifiIsland.uptime)) : ""
        }

        KIslandRow {
            open: wifiIsland.live
            label: qsTr("Auto-connect")
            caption: qsTr("Join the strongest saved network at power-up and after a loss")
            KSwitch {
                id: wifiAutoSwitch
                flat: true
                property bool wantChecked: wifiIsland.autoConnect
                property bool _g: false
                onWantCheckedChanged: { if (checked !== wantChecked) { _g = true; checked = wantChecked; _g = false } }
                Component.onCompleted: { _g = true; checked = wantChecked; _g = false }
                onToggled: {
                    if (_g || !dev)
                        return
                    dev.wifiSetAutoConnect(checked)
                    if (!checked)
                        notifications.info(qsTr("Auto-connect off: the module leaves the network"))
                }
            }
        }

        DevStackedRow {
            open: wifiIsland.live
            Row {
                width: parent.width
                spacing: Tokens.spaceMd
                readonly property real bw: (width - 2 * Tokens.spaceMd) / 3
                DevButton {
                    width: parent.bw; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                    text: wifiIsland.scanning ? qsTr("Scanning…") : qsTr("Scan")
                    enabled: !wifiIsland.scanning
                    toolTipText: qsTr("Look for networks nearby (takes a few seconds)")
                    onClicked: if (dev) dev.wifiScan()
                }
                DevButton {
                    width: parent.bw; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                    text: qsTr("Disconnect")
                    enabled: wifiIsland.connected || wifiIsland.busy
                    toolTipText: qsTr("Leave the network and switch auto-connect off")
                    onClicked: {
                        if (!dev)
                            return
                        dev.wifiDisconnect()
                        notifications.info(qsTr("Wi‑Fi disconnected, auto-connect off"))
                    }
                }
                DevButton {
                    width: parent.bw; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                    text: qsTr("Other…")
                    toolTipText: qsTr("Join a network that hides its name")
                    onClicked: wifiIsland.pick("", 3, false, true)
                }
            }
        }

        // Join rows: open for the tapped network or a hidden one
        KIslandSection {
            open: wifiIsland.pickActive
            label: wifiIsland.pickHidden ? qsTr("Join a hidden network") : qsTr("Join %1").arg(wifiIsland.pickSsid)
        }
        DevStackedRow {
            open: wifiIsland.pickActive && wifiIsland.pickHidden
            label: qsTr("Network name (SSID)")
            WifiField {
                id: wifiSsidField
                width: parent.width
                placeholder: qsTr("SSID")
                onAccepted: wifiPassField.takeFocus()
            }
        }
        DevStackedRow {
            open: wifiIsland.pickActive && wifiIsland.pickSecurity !== 0
            label: qsTr("Password")
            caption: wifiIsland.pickSaved ? qsTr("Saved network: the module keeps its password to itself, type it once more")
                                          : (wifiIsland.pickHidden ? qsTr("Leave empty for an open network") : "")
            WifiField {
                id: wifiPassField
                width: parent.width
                password: true
                placeholder: qsTr("Password")
                onAccepted: wifiIsland.doConnect()
            }
        }
        KIslandRow {
            open: wifiIsland.pickActive
            label: qsTr("Remember this network")
            caption: qsTr("Stored in the module once an address is obtained")
            KSwitch { id: wifiRemember; flat: true; checked: true }
        }
        DevStackedRow {
            open: wifiIsland.pickActive
            Row {
                width: parent.width
                spacing: Tokens.spaceMd
                readonly property real bw: (width - Tokens.spaceMd) / 2
                DevButton {
                    width: parent.bw; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                    text: qsTr("Connect")
                    normalBg: AppPalette.accentBg
                    hoverBg: AppPalette.accentBgHover
                    normalBorder: AppPalette.accentBorder
                    hoverBorder: AppPalette.accentBorder
                    textColor: AppPalette.accentText
                    onClicked: wifiIsland.doConnect()
                }
                DevButton {
                    width: parent.bw; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                    text: qsTr("Cancel")
                    onClicked: wifiIsland.closePick()
                }
            }
        }

        // Scan result
        KIslandSection {
            open: wifiIsland.live
            label: wifiIsland.scanning ? qsTr("Networks nearby (searching…)") : qsTr("Networks nearby")
        }
        Repeater {
            model: wifiIsland.networks
            delegate: KIslandRow {
                interactive: true
                chevron: true
                label: modelData.ssid.length ? modelData.ssid : qsTr("(hidden network)")
                labelBold: modelData.current === true
                caption: wifiIsland.securityText(modelData.security) + " · " + qsTr("ch %1").arg(modelData.channel)
                         + (modelData.current ? " · " + qsTr("connected") : (modelData.saved ? " · " + qsTr("saved") : ""))
                leading: WifiBars { rssi: modelData.rssi }
                Text {
                    text: modelData.rssi + " " + qsTr("dBm")
                    color: AppPalette.textMuted
                    font.pixelSize: Tokens.fontSm
                }
                onClicked: wifiIsland.pick(modelData.ssid, modelData.security, modelData.saved === true, false)
            }
        }
        KIslandRow {
            open: wifiIsland.live && !wifiIsland.scanning && wifiIsland.networks.length === 0
            minHeight: 0
            verticalPadding: Tokens.spaceMd
            label: qsTr("Nothing found yet. Tap Scan.")
            labelPixelSize: Tokens.fontSm
            labelColor: AppPalette.textMuted
        }

        // Saved networks
        KIslandSection {
            open: wifiIsland.savedList.length > 0
            label: qsTr("Saved networks")
        }
        Repeater {
            model: wifiIsland.savedList
            delegate: KIslandRow {
                label: modelData
                labelBold: wifiIsland.connected && modelData === wifiIsland.ssid
                caption: (wifiIsland.connected && modelData === wifiIsland.ssid) ? qsTr("connected") : ""
                KCircleIconButton {
                    implicitWidth: Tokens.controlHMd
                    implicitHeight: Tokens.controlHMd
                    rounded: false
                    cornerRadius: Tokens.radiusMd
                    iconSource: "qrc:/icons/ui/x.svg"
                    iconTintColor: AppPalette.dangerText
                    fillColor: AppPalette.controlRaised
                    fillHoverColor: Qt.lighter(AppPalette.controlRaised, 1.2)
                    fillPressedColor: AppPalette.controlRaised
                    borderWidth: Tokens.cardBorderWidth
                    borderColor: AppPalette.border
                    borderHoverColor: AppPalette.borderHover
                    toolTipText: qsTr("Forget")
                    onClicked: {
                        if (!dev)
                            return
                        dev.wifiForget(modelData)
                        notifications.info(qsTr("Forgotten: %1").arg(modelData))
                    }
                }
            }
        }
    }

    // ── Advanced settings ("Расширенные настройки") ────────────────────────
    Rectangle {
        id: advancedPanel
        width: root.groupWidth
        radius: Tokens.radiusLg
        clip: true

        readonly property bool _panel: root._hasCut
        readonly property real pad: _panel ? Tokens.spaceMd : 0
        readonly property real headerH: _panel ? Math.round(36 * AppPalette.scale) : 0
        readonly property color _headerColor: !_panel ? "transparent"
                                              : (cutMouse.containsMouse ? AppPalette.cardHover : AppPalette.card)
        readonly property color _bodyColor: (_panel && root._engExpanded) ? AppPalette.bgDeep : _headerColor

        implicitHeight: headerH + advReveal.height

        readonly property real _seamStart: Math.min(1, headerH / Math.max(1, height))
        readonly property real _seamEnd: Math.min(1, (headerH + Tokens.spaceMd) / Math.max(1, height))
        readonly property real _seamSpan: _seamEnd - _seamStart
        function _mix(a, b, t) {
            return Qt.rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                           a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t)
        }
        gradient: Gradient {
            GradientStop { position: 0.0; color: advancedPanel._headerColor }
            GradientStop { position: advancedPanel._seamStart; color: advancedPanel._headerColor }
            GradientStop { position: advancedPanel._seamStart + advancedPanel._seamSpan * 0.25
                           color: advancedPanel._mix(advancedPanel._headerColor, advancedPanel._bodyColor, 0.58) }
            GradientStop { position: advancedPanel._seamStart + advancedPanel._seamSpan * 0.55
                           color: advancedPanel._mix(advancedPanel._headerColor, advancedPanel._bodyColor, 0.91) }
            GradientStop { position: advancedPanel._seamEnd; color: advancedPanel._bodyColor }
            GradientStop { position: 1.0; color: advancedPanel._bodyColor }
        }

        Rectangle {
            z: 10
            anchors.fill: parent
            color: "transparent"
            radius: advancedPanel.radius
            border.width: (advancedPanel._panel && root._engExpanded) ? 1 : 0
            border.color: AppPalette.groupBorder
        }

        Item {
            id: cutHeader
            visible: advancedPanel._panel
            x: 0; y: 0
            width: parent.width
            height: advancedPanel.headerH

            readonly property real lockSlot: advancedPanel.headerH

            Row {
                anchors.fill: parent
                anchors.leftMargin: Tokens.spaceMd
                anchors.rightMargin: Tokens.spaceLg
                spacing: Tokens.spaceSm
                DisclosureIndicator {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.round(10 * AppPalette.scale)
                    height: Math.round(10 * AppPalette.scale)
                    expanded: root._engExpanded
                    indicatorColor: AppPalette.textSecond
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(0, parent.width - Math.round(10 * AppPalette.scale) - parent.spacing
                                       - (root.hasPinnableParams ? cutHeader.lockSlot + Tokens.spaceMd : 0))
                    text: qsTr("Advanced settings")
                    color: AppPalette.text
                    font.pixelSize: Math.max(Math.round(16 * AppPalette.scale), 13)
                    font.bold: true
                    elide: Text.ElideRight
                }
            }
            MouseArea {
                id: cutMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    root._engExpanded = !root._engExpanded
                    advScrollTimer.restart()
                }
            }
            HeaderActionChip {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                visible: root.hasPinnableParams
                size: cutHeader.lockSlot
                active: root.favEditMode
                iconSource: root.favEditMode ? "qrc:/icons/ui/lock-open.svg" : "qrc:/icons/ui/lock.svg"
                toolTipText: root.favEditMode ? qsTr("Finish pinning settings")
                                              : qsTr("Pin settings to favourites")
                onClicked: root.favEditMode = !root.favEditMode
            }
        }

        Item {
            id: advReveal
            x: advancedPanel.pad
            y: advancedPanel.headerH
            width: advancedPanel.width - 2 * advancedPanel.pad
            clip: root._hasCut
            property bool open: !root._hasCut || root._engExpanded
            property bool _animReady: false
            readonly property real _topGap: root._hasCut ? Tokens.spaceLg : 0
            readonly property real _botPad: advancedPanel.pad
            enabled: open
            Component.onCompleted: Qt.callLater(function() { advReveal._animReady = true })

            states: [
                State {
                    name: "open"; when: advReveal.open
                    PropertyChanges { target: advReveal; height: advGroups.implicitHeight + advReveal._topGap + advReveal._botPad }
                },
                State {
                    name: "closed"; when: !advReveal.open
                    PropertyChanges { target: advReveal; height: 0 }
                }
            ]
            transitions: Transition {
                enabled: advReveal._animReady
                NumberAnimation { property: "height"; duration: 220; easing.type: Easing.OutCubic }
            }

                Column {
                    id: advGroups
                    y: advReveal._topGap
                    width: parent.width
                    spacing: Tokens.spaceLg

    // ── Эхограмма ─────────────────────────────────────────────────────────

    DevIsland {
        visible: !!(dev && dev.isChartSupport)
        unconfirmed: !!(dev && dev.chartSetupState === false)

        DevIslandTitle { label: qsTr("Echogram") }

        DeviceParamRow { page: root; paramKey: "chart.resolution" }
        DeviceParamRow { page: root; paramKey: "chart.samples" }
        DeviceParamRow { page: root; paramKey: "chart.offset" }
    }

    // ── Дальномер ─────────────────────────────────────────────────────────

    DevIsland {
        visible: !!(dev && dev.isDistSupport)
        unconfirmed: !!(dev && dev.distSetupState === false)

        DevIslandTitle { label: qsTr("Rangefinder") }

        DeviceParamRow { page: root; paramKey: "dist.max" }
        DeviceParamRow { page: root; paramKey: "dist.deadZone" }
        DeviceParamRow { page: root; paramKey: "dist.confidence" }
    }

    // ── Преобразователь ───────────────────────────────────────────────────

    DevIsland {
        visible: !!(dev && dev.isTransducerSupport)
        unconfirmed: !!(dev && dev.transcState === false)

        DevIslandTitle { label: qsTr("Transducer") }

        DeviceParamRow { page: root; paramKey: "trans.pulse" }
        DeviceParamRow { page: root; paramKey: "trans.freq" }
        DeviceParamRow { page: root; paramKey: "trans.boost" }
    }

    // ── DSP ───────────────────────────────────────────────────────────────

    DevIsland {
        visible: !!(dev && dev.isDSPSupport)
        unconfirmed: !!(dev && (dev.dspState === false || dev.soundState === false))

        DevIslandTitle { label: qsTr("DSP") }

        DeviceParamRow { page: root; paramKey: "dsp.horSmooth" }
        DeviceParamRow { page: root; paramKey: "dsp.soundSpeed" }
    }

    // ── Датасет ───────────────────────────────────────────────────────────

    DevIsland {
        visible: !!(dev && dev.isDatasetSupport)
        unconfirmed: !!(dev && dev.datasetState === false)

        DevIslandTitle { label: qsTr("Dataset") }

        DeviceParamRow { page: root; paramKey: "dataset.period" }
        Column {
            width: parent.width; spacing: Tokens.spaceSm
            Text { text: qsTr("Echogram"); color: AppPalette.textStrong; font.pixelSize: Tokens.fontLg }
            KTabBar {
                id: datasetChartTab; width: parent.width
                // Compressed is a third state, not a second channel. The two
                // formats are mutually exclusive: asking for both puts two
                // pictures of the same ping into one plot, and since the codec
                // is lossy they do not even agree.
                // IQ and RAW are not echogram formats but taps: the device keeps
                // ranging, sends no echogram on this link, and the second core
                // streams the baseband (IQ) or the raw ADC samples (RAW) over its
                // own USB port, which UsbTap picks up and records.
                options: [{ label: qsTr("Off"), value: 0 }, { label: qsTr("8-bit"), value: 1 }, { label: qsTr("Compressed"), value: 2 }, { label: qsTr("IQ"), value: 3 }, { label: qsTr("RAW"), value: 4 }]
                property int chartModel: dev ? (dev.datasetTapRaw === 1 ? 4 : (dev.datasetTapIq === 1 ? 3 : (dev.datasetChartZ === 1 ? 2 : (dev.datasetChart === 1 ? 1 : 0)))) : 0
                property bool _g: false
                onChartModelChanged: {
                    if (currentValue !== chartModel) { _g = true; currentValue = chartModel; _g = false }
                    // The device may come up already in a tap mode (the mask is kept in its flash).
                    if (chartModel >= 3) usbTap.setMode(chartModel - 2, core.logDirectory())
                }
                Component.onCompleted: { _g = true; currentValue = chartModel; _g = false }
                onValueSelected: function(v) {
                    if (_g || !dev) return
                    dev.datasetChart  = (v === 1) ? 1 : 0
                    dev.datasetChartZ = (v === 2) ? 1 : 0
                    dev.datasetTapIq  = (v === 3) ? 1 : 0
                    dev.datasetTapRaw = (v === 4) ? 1 : 0
                    usbTap.setMode(v >= 3 ? v - 2 : 0, core.logDirectory())
                }
            }
            Text {
                visible: !!(dev && (dev.datasetTapIq === 1 || dev.datasetTapRaw === 1))
                width: parent.width; wrapMode: Text.Wrap
                color: AppPalette.textSecond; font.pixelSize: Tokens.fontMd
                text: usbTap.statusText
            }
        }
        DeviceParamRow { page: root; paramKey: "dataset.dist" }
        DeviceParamRow { page: root; paramKey: "dataset.ahrs" }
        DeviceParamRow { page: root; paramKey: "dataset.temperature" }
        DeviceParamRow { page: root; paramKey: "dataset.timestamp" }
    }

    DevIsland {
        unconfirmed: !!(dev && dev.uartState === false)

        DevIslandTitle { label: qsTr("Actions") }

        DevStackedRow {
            Row {
                width: parent.width; spacing: Tokens.spaceMd
                readonly property real bw: (width - 2 * Tokens.spaceMd) / 3
                FlashButton { width: parent.bw }
                DevButton {
                    width: parent.bw; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                    text: qsTr("Erase settings"); danger: true
                    toolTipText: qsTr("Erase device settings (reset)")
                    onClicked: { if (dev) { dev.resetSettings(); notifications.info(qsTr("Settings erased on device: %1").arg(dev.devName)) } }
                }
                DevButton {
                    width: parent.bw; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                    text: qsTr("Reboot")
                    toolTipText: qsTr("Reboot the device")
                    onClicked: { if (dev) { dev.reboot(); notifications.info(qsTr("Reboot command sent: %1").arg(dev.devName)) } }
                }
            }
        }

        DeviceParamRow { page: root; paramKey: "action.baudrate" }
    }

    // ── Сеть прибора ──────────────────────────────────────────────────────

    DeviceSettingsGroup {
        id: devNetGroup
        width: advGroups.width; preferredWidth: advGroups.width
        title: qsTr("Network (USB)"); titlePixelSize: 13
        stateKey: "dev.net"; collapsedByDefault: true
        visible: !!(dev && dev.isNetSupport)

        // Поля показывают значения прибора, пока их не правят: иначе ответ на
        // применение затирал бы то, что человек печатает.
        readonly property bool editing: ipField.activeFocus || maskField.activeFocus || hostField.activeFocus
        readonly property string devIp:   (dev && dev.netValid) ? dev.netIp   : "192.168.7.1"
        readonly property string devMask: (dev && dev.netValid) ? dev.netMask : "255.255.255.0"
        readonly property string devHost: (dev && dev.netValid) ? dev.netHost : "192.168.7.2"

        // force обходит защиту от затирания того, что человек печатает: им
        // пользуется только показ ответа прибора после «Применить», иначе
        // отвергнутая тройка так и висела бы в полях как принятая.
        function _sync(force) {
            if (editing && !force) return
            if (ipField.text !== devIp) ipField.text = devIp
            if (maskField.text !== devMask) maskField.text = devMask
            if (hostField.text !== devHost) hostField.text = devHost
        }
        onDevIpChanged: _sync(false)
        onDevMaskChanged: _sync(false)
        onDevHostChanged: _sync(false)
        onVisibleChanged: if (visible && dev) dev.requestNet()
        Component.onCompleted: { _sync(true); if (visible && dev) dev.requestNet() }

        // Потеря фокуса НЕ возвращает поле к значению прибора: щелчок по
        // «Применить» её и вызывает, и кнопка прочитала бы уже восстановленное.
        Timer { id: netReadBack; interval: 700; onTriggered: devNetGroup._sync(true) }

        Row {
            width: parent.width; height: Tokens.controlHMd; spacing: Tokens.spaceMd
            Text {
                text: qsTr("Device address"); color: AppPalette.textStrong; font.pixelSize: Tokens.fontLg
                width: Math.max(0, parent.width - parent.spacing - root.spinW)
                anchors.verticalCenter: parent.verticalCenter; elide: Text.ElideRight
            }
            Rectangle {
                width: root.spinW; height: Tokens.controlHMd; radius: Tokens.radiusMd
                anchors.verticalCenter: parent.verticalCenter
                color: AppPalette.bg
                border.width: ipField.activeFocus ? 1 : Tokens.cardBorderWidth
                border.color: ipField.acceptableInput
                              ? (ipField.activeFocus ? AppPalette.accentBorder : AppPalette.border)
                              : AppPalette.dangerBorder
                TextInput {
                    id: ipField
                    activeFocusOnTab: true
                    anchors.fill: parent
                    anchors.leftMargin: Tokens.spaceMd; anchors.rightMargin: Tokens.spaceMd
                    verticalAlignment: TextInput.AlignVCenter
                    horizontalAlignment: TextInput.AlignRight
                    color: AppPalette.text; font.pixelSize: Tokens.fontBase
                    clip: true
                    // Четыре октета 0..255. Прибор проверяет тройку ещё раз у
                    // себя и отвергает ту, что не может работать.
                    validator: RegularExpressionValidator { regularExpression: /((25[0-5]|2[0-4]\d|1?\d?\d)\.){3}(25[0-5]|2[0-4]\d|1?\d?\d)/ }
                    TapHandler { acceptedButtons: Qt.LeftButton; onDoubleTapped: ipField.selectAll() }
                }
            }
        }
        Row {
            width: parent.width; height: Tokens.controlHMd; spacing: Tokens.spaceMd
            Text {
                text: qsTr("Subnet mask"); color: AppPalette.textStrong; font.pixelSize: Tokens.fontLg
                width: Math.max(0, parent.width - parent.spacing - root.spinW)
                anchors.verticalCenter: parent.verticalCenter; elide: Text.ElideRight
            }
            Rectangle {
                width: root.spinW; height: Tokens.controlHMd; radius: Tokens.radiusMd
                anchors.verticalCenter: parent.verticalCenter
                color: AppPalette.bg
                border.width: maskField.activeFocus ? 1 : Tokens.cardBorderWidth
                border.color: maskField.acceptableInput
                              ? (maskField.activeFocus ? AppPalette.accentBorder : AppPalette.border)
                              : AppPalette.dangerBorder
                TextInput {
                    id: maskField
                    activeFocusOnTab: true
                    anchors.fill: parent
                    anchors.leftMargin: Tokens.spaceMd; anchors.rightMargin: Tokens.spaceMd
                    verticalAlignment: TextInput.AlignVCenter
                    horizontalAlignment: TextInput.AlignRight
                    color: AppPalette.text; font.pixelSize: Tokens.fontBase
                    clip: true
                    // Четыре октета 0..255. Прибор проверяет тройку ещё раз у
                    // себя и отвергает ту, что не может работать.
                    validator: RegularExpressionValidator { regularExpression: /((25[0-5]|2[0-4]\d|1?\d?\d)\.){3}(25[0-5]|2[0-4]\d|1?\d?\d)/ }
                    TapHandler { acceptedButtons: Qt.LeftButton; onDoubleTapped: maskField.selectAll() }
                }
            }
        }
        Row {
            width: parent.width; height: Tokens.controlHMd; spacing: Tokens.spaceMd
            Text {
                text: qsTr("Host address"); color: AppPalette.textStrong; font.pixelSize: Tokens.fontLg
                width: Math.max(0, parent.width - parent.spacing - root.spinW)
                anchors.verticalCenter: parent.verticalCenter; elide: Text.ElideRight
            }
            Rectangle {
                width: root.spinW; height: Tokens.controlHMd; radius: Tokens.radiusMd
                anchors.verticalCenter: parent.verticalCenter
                color: AppPalette.bg
                border.width: hostField.activeFocus ? 1 : Tokens.cardBorderWidth
                border.color: hostField.acceptableInput
                              ? (hostField.activeFocus ? AppPalette.accentBorder : AppPalette.border)
                              : AppPalette.dangerBorder
                TextInput {
                    id: hostField
                    activeFocusOnTab: true
                    anchors.fill: parent
                    anchors.leftMargin: Tokens.spaceMd; anchors.rightMargin: Tokens.spaceMd
                    verticalAlignment: TextInput.AlignVCenter
                    horizontalAlignment: TextInput.AlignRight
                    color: AppPalette.text; font.pixelSize: Tokens.fontBase
                    clip: true
                    // Четыре октета 0..255. Прибор проверяет тройку ещё раз у
                    // себя и отвергает ту, что не может работать.
                    validator: RegularExpressionValidator { regularExpression: /((25[0-5]|2[0-4]\d|1?\d?\d)\.){3}(25[0-5]|2[0-4]\d|1?\d?\d)/ }
                    TapHandler { acceptedButtons: Qt.LeftButton; onDoubleTapped: hostField.selectAll() }
                }
            }
        }

        KSwitch {
            id: dhcpSwitch
            width: parent.width; text: qsTr("Hand the host its address (DHCP)")
            property bool wantChecked: !!(dev && dev.netDhcp)
            property bool _g: false
            onWantCheckedChanged: { if (checked !== wantChecked) { _g = true; checked = wantChecked; _g = false } }
            Component.onCompleted: { _g = true; checked = wantChecked; _g = false }
        }

        Row {
            width: parent.width; spacing: Tokens.spaceSm
            readonly property real bw: (width - Tokens.spaceSm) / 2
            DevButton {
                width: parent.bw; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                text: qsTr("Apply")
                toolTipText: qsTr("Send the addresses to the device. They take effect at once; write them with Flash settings to survive a reboot.")
                onClicked: {
                    if (!dev) return
                    if (!dev.applyNet(ipField.text, maskField.text, hostField.text, dhcpSwitch.checked)) {
                        notifications.info(qsTr("Address, mask or host address is not four octets"))
                        return
                    }
                    notifications.info(qsTr("Network settings sent: %1").arg(ipField.text))
                    netReadBack.restart()       // покажем то, что прибор принял на деле
                }
            }
            DevButton {
                width: parent.bw; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                text: qsTr("Default")
                toolTipText: qsTr("Put the factory addresses in the fields; Apply sends them")
                onClicked: { ipField.text = "192.168.7.1"; maskField.text = "255.255.255.0"; hostField.text = "192.168.7.2" }
            }
        }

        Text {
            width: parent.width; wrapMode: Text.Wrap
            color: AppPalette.textSecond; font.pixelSize: Tokens.fontMd
            text: {
                if (!dev || !dev.netValid) return qsTr("Reading the device settings...")
                var mac = dev.netMac
                var s = (mac && mac !== "00:00:00:00:00:00") ? qsTr("Adapter MAC %1.").arg(mac) : ""
                s += (s.length ? " " : "") + qsTr("The device answers a discovery request on UDP port 18020, broadcast included, so it can be found and put back even when the addresses do not match. That port is fixed.")
                if (dev.netLive) s += " " + qsTr("These settings are not in the device memory: a reboot brings back the stored ones.")
                return s
            }
        }
    }

    DeviceSettingsGroup {
        id: devActionsGroup
        width: advGroups.width; preferredWidth: advGroups.width
        title: qsTr("Actions"); titlePixelSize: 13
        stateKey: "dev.actions"; collapsedByDefault: root._hasCut
        confirmed: !(dev && dev.uartState === false)

        readonly property var baudrateOptions: [9600, 19200, 38400, 57600, 115200,
                                                230400, 460800, 921600, 1000000, 1200000, 1500000, 2000000, 3000000, 4000000]

        Row {
            width: parent.width; spacing: Tokens.spaceSm
            readonly property real bw: (width - 2 * Tokens.spaceSm) / 3
            DevButton {
                width: parent.bw; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                text: qsTr("Flash settings")
                toolTipText: qsTr("Write current settings to device memory")
                onClicked: { if (dev) { dev.flashSettings(); notifications.info(qsTr("Settings written to device: %1").arg(dev.devName)) } }
            }
            DevButton {
                width: parent.bw; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                text: qsTr("Erase settings"); danger: true
                toolTipText: qsTr("Erase device settings (reset)")
                onClicked: { if (dev) { dev.resetSettings(); notifications.info(qsTr("Settings erased on device: %1").arg(dev.devName)) } }
            }
            DevButton {
                width: parent.bw; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                text: qsTr("Reboot")
                toolTipText: qsTr("Reboot the device")
                onClicked: { if (dev) { dev.reboot(); notifications.info(qsTr("Reboot command sent: %1").arg(dev.devName)) } }
            }
        }
        Row {
            width: parent.width; spacing: Tokens.spaceSm
            readonly property real setW: Math.round(120 * AppPalette.scale)
            KCombo {
                id: baudrateCombo
                width: parent.width - parent.setW - Tokens.spaceSm
                height: Tokens.controlHMd
                model: devActionsGroup.baudrateOptions
                currentIndex: devActionsGroup.baudrateOptions.indexOf(115200)
            }
            DevButton {
                width: parent.setW; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                text: qsTr("Set baudrate")
                toolTipText: qsTr("Apply the selected baud rate")
                onClicked: {
                    if (dev) {
                        var b = devActionsGroup.baudrateOptions[baudrateCombo.currentIndex]
                        dev.baudrate = b
                        notifications.info(qsTr("Baudrate set: %1").arg(b))
                    }
                }
            }
        }
    }

    DevIsland {
        id: devSettingsGroup

        DevIslandTitle { label: qsTr("Settings file") }

        property var importFolder: StandardPaths.writableLocation(StandardPaths.HomeLocation)
        property var exportFolder: StandardPaths.writableLocation(StandardPaths.HomeLocation)

        Settings {
            category: "main/devices"
            property alias devImportFolder: devSettingsGroup.importFolder
            property alias devExportFolder: devSettingsGroup.exportFolder
        }

        function _localPath(u) {
            if (!u) return ""
            if (typeof u !== "string" && u.toLocalFile) {
                var lp = u.toLocalFile()
                if (lp && lp.length) return lp
            }
            var s = String(u)
            if (s.indexOf("file:///") === 0)
                return Qt.platform.os === "windows" ? s.slice(8) : s.slice(7)
            if (s.indexOf("file://") === 0)
                return s.slice(7)
            return s
        }

        FileDialog {
            id: importXmlDialog
            title: qsTr("Open settings file")
            fileMode: FileDialog.OpenFile
            nameFilters: ["XML files (*.xml)"]
            onCurrentFolderChanged: devSettingsGroup.importFolder = currentFolder
            onAccepted: {
                devSettingsGroup.importFolder = importXmlDialog.currentFolder
                var lp = devSettingsGroup._localPath(importXmlDialog.selectedFile)
                if (dev && lp.length) dev.importSettingsFromXML(lp)
            }
        }

        FileDialog {
            id: exportXmlDialog
            title: qsTr("Save settings file")
            fileMode: FileDialog.SaveFile
            nameFilters: ["XML files (*.xml)"]
            defaultSuffix: "xml"
            onCurrentFolderChanged: devSettingsGroup.exportFolder = currentFolder
            onAccepted: {
                devSettingsGroup.exportFolder = exportXmlDialog.currentFolder
                var lp = devSettingsGroup._localPath(exportXmlDialog.selectedFile)
                if (dev && lp.length) dev.exportSettingsToXML(lp)
            }
        }

        DeviceParamRow { page: root; paramKey: "action.settingsFile" }
    }

    DevIsland {
        id: devUpgradeGroup
        visible: !!(dev && dev.isUpgradeSupport)

        DevIslandTitle { label: qsTr("Upgrade") }

        property var upgradeFolder: StandardPaths.writableLocation(StandardPaths.HomeLocation)
        property string selectedUpgradePathSource: ""

        Settings { category: "main/devices"; property alias devUpgradeFolder: devUpgradeGroup.upgradeFolder }

        function _src(value) {
            if (!value) return ""
            if (typeof value === "string") {
                if (value.startsWith("file:///")) return Qt.platform.os === "windows" ? value.slice(8) : value.slice(7)
                if (value.startsWith("file://")) return value.slice(7)
                return value
            }
            var lp = value.toLocalFile ? value.toLocalFile() : ""
            return lp && lp.length ? lp : value.toString()
        }
        function _disp(value) {
            var s = devUpgradeGroup._src(value)
            if (!s.length) return ""
            try { return decodeURIComponent(s) } catch (e) { return s }
        }
        function currentUpgradePath() {
            var d = upgradePathInput.text
            if (!d || !d.length) return ""
            if (devUpgradeGroup.selectedUpgradePathSource
                    && d === devUpgradeGroup._disp(devUpgradeGroup.selectedUpgradePathSource))
                return devUpgradeGroup.selectedUpgradePathSource
            return d
        }
        function setUpgradePath(path) {
            devUpgradeGroup.selectedUpgradePathSource = devUpgradeGroup._src(path)
            upgradePathInput.text = devUpgradeGroup._disp(devUpgradeGroup.selectedUpgradePathSource)
        }

        readonly property int _fwOk: 101
        property string _activeTag: ""
        property string _activeLabel: ""
        property string _activeFw: ""
        function _devLabel() {
            if (!dev) return ""
            var n = dev.devName ? dev.devName : ""
            return dev.devSN ? (n + " (SN " + dev.devSN + ")") : n
        }
        function _baseName(p) {
            if (!p) return ""
            var s = String(p).replace(/\\/g, "/")
            var i = s.lastIndexOf("/")
            return i >= 0 ? s.slice(i + 1) : s
        }

        FileDialog {
            id: upgradeFileDialog
            title: qsTr("Please choose a file")
            currentFolder: devUpgradeGroup.upgradeFolder
            // *.ufwb — Baikal-sonar, *.ufww — the Wi-Fi module; the device tells them
            // apart by the marker in the first four bytes, so one filter serves all.
            nameFilters: ["Upgrade files (*.ufw *.ufwb *.ufww)"]
            onCurrentFolderChanged: devUpgradeGroup.upgradeFolder = currentFolder
            onAccepted: {
                devUpgradeGroup.upgradeFolder = upgradeFileDialog.currentFolder
                devUpgradeGroup.setUpgradePath(upgradeFileDialog.selectedFile)
            }
        }

        KIslandRow {
            stacked: true
            minHeight: 0
            open: upgradeProgress.pct > 0 && upgradeProgress.pct < 100
            Rectangle {
                id: upgradeProgress
                width: parent.width; height: Math.round(4 * AppPalette.scale); radius: height / 2
                color: AppPalette.trackOff
                readonly property int pct: dev && dev.upgradeFWStatus !== undefined
                                           ? Math.max(0, Math.min(100, dev.upgradeFWStatus)) : 0
                Rectangle {
                    height: parent.height; radius: parent.radius; color: AppPalette.accentBar
                    width: parent.width * upgradeProgress.pct / 100
                }
            }
        }

        DevStackedRow {
            Row {
                width: parent.width; spacing: Tokens.spaceMd
                readonly property real browseW: Tokens.controlHMd
                Rectangle {
                    width: parent.width - parent.browseW - Tokens.spaceMd
                    height: Tokens.controlHMd; radius: Tokens.radiusMd
                    color: AppPalette.bg
                    border.width: upgradePathInput.activeFocus ? 1 : Tokens.cardBorderWidth
                    border.color: upgradePathInput.activeFocus ? AppPalette.accentBorder : AppPalette.border
                    TextInput {
                        id: upgradePathInput
                        activeFocusOnTab: true
                        anchors.fill: parent; anchors.leftMargin: Tokens.spaceMd; anchors.rightMargin: Tokens.spaceMd
                        verticalAlignment: TextInput.AlignVCenter
                        color: AppPalette.text; font.pixelSize: Tokens.fontSm; clip: true
                        TapHandler { acceptedButtons: Qt.LeftButton; onDoubleTapped: upgradePathInput.selectAll() }
                        Text {
                            visible: !upgradePathInput.text.length; text: qsTr("Enter path")
                            color: AppPalette.textMuted; font.pixelSize: Tokens.fontSm
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                }
                DevButton {
                    width: Tokens.controlHMd; height: Tokens.controlHMd; text: "..."
                    fontPixelSize: Tokens.fontLg; bold: false
                    horizontalPadding: 0; verticalPadding: 0
                    toolTipText: qsTr("Choose firmware")
                    onClicked: { upgradeFileDialog.currentFolder = devUpgradeGroup.upgradeFolder; upgradeFileDialog.open() }
                }
            }
        }

        DevStackedRow {
            open: upgradePathInput.text !== ""
            DevButton {
                width: parent.width; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                text: qsTr("UPGRADE")
                onClicked: {
                    if (!dev) return
                    var path = devUpgradeGroup.currentUpgradePath()
                    var fw = devUpgradeGroup._baseName(path)
                    var label = devUpgradeGroup._devLabel()
                    var tag = "fw-upgrade-" + (dev.devSN || 0)
                    if (core.upgradeFW(path, dev)) {
                        devUpgradeGroup._activeTag = tag
                        devUpgradeGroup._activeLabel = label
                        devUpgradeGroup._activeFw = fw
                        notifications.warning(qsTr("Flashing device %1 with file %2").arg(label).arg(fw), tag)
                    } else {
                        notifications.warning(qsTr("Failed to open firmware file: %1").arg(fw))
                    }
                }
            }
        }

        Connections {
            target: dev
            ignoreUnknownSignals: true
            function onUpgradingFirmwareDone() {
                if (!devUpgradeGroup._activeTag.length) return
                notifications.dismiss(devUpgradeGroup._activeTag)
                if (dev && dev.upgradeFWStatus === devUpgradeGroup._fwOk)
                    notifications.info(qsTr("Device %1 successfully flashed with file %2")
                                       .arg(devUpgradeGroup._activeLabel).arg(devUpgradeGroup._activeFw))
                else
                    notifications.warning(qsTr("Failed to flash device %1 with file %2 (error code %3)")
                                          .arg(devUpgradeGroup._activeLabel).arg(devUpgradeGroup._activeFw)
                                          .arg(dev ? dev.upgradeFWStatus : -1))
                devUpgradeGroup._activeTag = ""
            }
        }
    }

                }
            }
        }

    // ── Информация ────────────────────────────────────────────────────────

    DevIsland {
        id: devInfoGroup
        width: root.groupWidth
        visible: !!(dev && dev.isBoardInited)
        rowMinHeight: Tokens.controlHMd + Tokens.spaceXs

        readonly property bool appAnswering: !!(dev && (dev.uartState === true
                                                        || dev.recorderStatusValid === true))

        readonly property var linkMeta: {
            if (!dev || typeof deviceTopology === "undefined" || !deviceTopology)
                return null
            var groups = deviceTopology.groups
            if (!groups || !groups.length)
                return null
            var meta = deviceTopology.groupForDevice(dev)
            return (meta && meta.linkPresent) ? meta : null
        }

        readonly property string connectionText: {
            var m = devInfoGroup.linkMeta
            if (!m)
                return ""
            switch (m.linkType) {
            case 1: return m.baudrate > 0 ? (m.portName + " · " + m.baudrate) : m.portName
            case 2: return "UDP " + m.address + ":" + m.destinationPort
            case 3: return "TCP " + m.address + ":" + m.destinationPort
            case 4: return "RTSP " + m.address
            }
            return ""
        }

        function infoText() {
            var lines = []
            var items = devInfoGroup.rows
            for (var i = 0; i < items.length; ++i) {
                var it = items[i]
                if (it && it.value !== undefined && it.visible && String(it.value).length)
                    lines.push(it.label + ": " + it.value)
            }
            return lines.join("\n")
        }

        DevIslandTitle {
            label: qsTr("Information")
            minHeight: Tokens.rowH
            verticalPadding: 0

            KCircleIconButton {
                implicitWidth: Tokens.controlHMd
                implicitHeight: Tokens.controlHMd
                rounded: false
                cornerRadius: Tokens.radiusMd
                iconSource: "qrc:/icons/ui/copy.svg"
                iconTintColor: AppPalette.text
                fillColor: AppPalette.controlRaised
                fillHoverColor: Qt.lighter(AppPalette.controlRaised, 1.2)
                fillPressedColor: AppPalette.controlRaised
                borderWidth: Tokens.cardBorderWidth
                borderColor: AppPalette.border
                borderHoverColor: AppPalette.borderHover
                toolTipText: qsTr("Copy")
                onClicked: {
                    var text = devInfoGroup.infoText()
                    if (!text.length)
                        return
                    core.copyToClipboard(text)
                    notifications.info(qsTr("Device info copied to clipboard"))
                }
            }
        }

        DevInfoRow {
            label: qsTr("Model")
            value: dev ? (dev.devName || "") : ""
        }

        DevInfoRow {
            label: qsTr("Board type")
            value: dev ? (dev.devType + "." + dev.devTypeMinor) : ""
        }

        DevInfoRow {
            label: qsTr("Serial number")
            value: {
                if (!dev)
                    return ""
                var sn = dev.devSN >>> 0
                if (sn === 0)
                    return ""
                return sn === 0xFFFFFFFF ? qsTr("not set") : String(sn)
            }
        }

        DevInfoRow {
            label: qsTr("Firmware version")
            value: dev ? (dev.fwVersion || "") : ""
        }

        DevInfoRow {
            label: qsTr("Bootloader version")
            value: dev ? (dev.bootVersion || "") : ""
        }

        DevInfoRow {
            label: qsTr("Running")
            value: {
                if (!dev || dev.bootMode < 0)
                    return ""
                if (dev.bootMode === 1)
                    return devInfoGroup.appAnswering ? "" : qsTr("Bootloader")
                return qsTr("Firmware")
            }
        }

        DevInfoRow {
            label: qsTr("Bus address")
            value: (dev && dev.uartState === true) ? String(dev.devAddress) : ""
        }

        DevInfoRow {
            label: qsTr("Connection")
            value: devInfoGroup.connectionText
        }

        DevInfoRow {
            label: qsTr("Chip UID")
            value: dev ? (dev.devUID || "") : ""
        }
    }
}

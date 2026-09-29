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

    // Sections that fold (owner 2026-09-29: the Wi-Fi page grew long): folded by default; the open ones are kept by
    // key in QSettings, each on its own (not the settings groups' accordion).
    Settings { id: foldPrefs; category: "deviceSettingsPage"; property string openSections: "" }
    function sectionOpen(key) { return ("," + foldPrefs.openSections + ",").indexOf("," + key + ",") >= 0 }
    function setSectionOpen(key, open) {
        var keys = foldPrefs.openSections.length ? foldPrefs.openSections.split(",") : []
        var i = keys.indexOf(key)
        if (open && i < 0)
            keys.push(key)
        else if (!open && i >= 0)
            keys.splice(i, 1)
        foldPrefs.openSections = keys.join(",")
    }

    // ── Apply and save: the Devices page's pinned bar (field test 2026-09-29) ──
    // An island's Apply sends that island's edits; the bar's "Apply and save" does what every pending Apply on this
    // page would, then writes the device's settings to its memory (what "Flash settings" does). A port change runs in
    // the driver and the next step waits for it (wifiPortBusy); the radio goes after the save, since the module
    // reconnects; the module's own address last, since it then answers as a new device. The bar reads
    // pendingApplyCount, applying and applyProgress from here (store.devicePage).
    property int pendingApplyCount: 0
    property bool applying: false
    property string applyProgress: ""
    property var _applyQueue: []
    property int _applyAt: 0
    property int _applyTotal: 0
    property var _applyDone: []
    property var _applyFailed: []
    property bool _applySaved: false
    property real _applyWaitSince: 0

    // What waits for Apply now. phase 0: before the save; 2: after it (the radio); 3: last (the own address).
    function pendingSteps() {
        var s = []
        if (!dev)
            return s
        function add(ok, name, phase, fn) {
            if (ok)
                s.push({ name: name, phase: phase, run: fn })
        }
        add(relayIsland.pendingApply, qsTr("Boat link"), 0, function() { return relayIsland.apply() })
        add(linesIsland.pendingApply, qsTr("Lines"), 0, function() { return linesIsland.apply() })
        add(apIsland.rolePending, qsTr("Role"), 0, function() { return apIsland.saveRole() })
        add(apIsland.apPending, qsTr("Access point"), 0, function() { return apIsland.applyAp() })
        add(ipIsland.pendingApply, qsTr("Address and DHCP"), 0, function() { return ipIsland.apply() })
        add(devNetGroup.pendingApply, qsTr("Network (USB)"), 0, function() { return devNetGroup.apply() })
        for (var i = 0; i < portsRepeater.count; ++i) {
            var c = portsRepeater.itemAt(i)
            if (!c)
                continue
            add(c.netPending, qsTr("%1 line").arg(portsIsland.portName(i)), 0,
                (function(card) { return function() { return card.applyNet() } })(c))
            add(c.ratePending, qsTr("%1 rate").arg(portsIsland.portName(i)), 0,
                (function(card) { return function() { return card.applyRate() } })(c))
        }
        add(portIsland.pendingApply, qsTr("Port speed"), 0, function() { return portIsland.apply() })
        add(radioIsland.pendingApply, qsTr("Radio"), 2, function() { return radioIsland.apply() })
        add(portsIsland.ownAddrPending, qsTr("Module address"), 3, function() { return portsIsland.applyOwnAddr() })
        add(linesIsland.ownAddrPending, qsTr("Module address"), 3, function() { return linesIsland.applyOwnAddr() })
        return s
    }

    function recountPending() {
        if (!applying)
            pendingApplyCount = pendingSteps().length
    }

    function applyAllAndSave() {
        if (applying || !dev)
            return
        var steps = pendingSteps()
        var q = []
        for (var phase = 0; phase <= 3; ++phase) {
            if (phase === 1)
                q.push({ name: "", phase: 1, save: true })
            for (var i = 0; i < steps.length; ++i)
                if (steps[i].phase === phase)
                    q.push(steps[i])
        }
        _applyQueue = q
        _applyAt = 0
        _applyTotal = steps.length
        _applyDone = []
        _applyFailed = []
        _applySaved = false
        _applyWaitSince = 0
        applying = true
        _applyNext()
    }

    function _applyNext() {
        if (!dev) {
            _finishApply()
            return
        }
        // A port change still runs in the driver: the next step waits for it.
        if (dev.wifiPortBusy) {
            if (_applyWaitSince === 0)
                _applyWaitSince = Date.now()
            if (Date.now() - _applyWaitSince < 45000) {
                applyStepTimer.restart()
                return
            }
            _applyFailed = _applyFailed.concat([qsTr("the rest (a port change did not finish)")])
            _finishApply()
            return
        }
        _applyWaitSince = 0
        if (_applyAt >= _applyQueue.length) {
            _finishApply()
            return
        }
        var step = _applyQueue[_applyAt]
        _applyAt += 1
        if (step.save) {
            dev.flashSettings()
            _applySaved = true
        } else {
            var ok = false
            try {
                ok = step.run() === true
            } catch (err) {
                console.warn("apply and save:", step.name, err)
            }
            if (ok)
                _applyDone = _applyDone.concat([step.name])
            else
                _applyFailed = _applyFailed.concat([step.name])
        }
        var n = _applyDone.length + _applyFailed.length
        applyProgress = _applyTotal > 0 && n < _applyTotal ? qsTr("Applying %1 of %2…").arg(n + 1).arg(_applyTotal)
                                                           : qsTr("Saving…")
        applyStepTimer.restart()
    }

    function _finishApply() {
        applying = false
        applyProgress = ""
        var name = dev ? dev.devName : ""
        if (_applyFailed.length > 0)
            notifications.warning(qsTr("Not applied: %1; see those islands.").arg(_applyFailed.join(", "))
                                  + (_applyDone.length > 0 ? " " + qsTr("Applied: %1.").arg(_applyDone.join(", ")) : "")
                                  + (_applySaved ? " " + qsTr("Settings written to device: %1").arg(name) : ""))
        else if (_applyDone.length > 0)
            notifications.info(qsTr("Applied: %1. Settings written to device: %2").arg(_applyDone.join(", ")).arg(name))
        else if (_applySaved)
            notifications.info(qsTr("Settings written to device: %1").arg(name))
        recountPending()
    }

    Timer { id: applyStepTimer; interval: 300; repeat: false; onTriggered: root._applyNext() }
    // The bar's count follows edits anywhere on the page (fields, menus, selectors): read twice a second while shown.
    Timer {
        interval: 500; repeat: true; triggeredOnStart: true
        running: root.visible && !root.applying
        onTriggered: root.recountPending()
    }

    function _registerWithStore() {
        if (store)
            store.devicePage = root
    }
    onStoreChanged: _registerWithStore()
    Component.onDestruction: if (store && store.devicePage === root) store.devicePage = null
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

    Component.onCompleted: { _applyForCurrentDev(); _registerWithStore() }

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
    // Firmware 0.12 reports its physical ports (ID_WIFI_NET v7): the Ports island replaces the fixed
    // "line 0 is this port" wording; 0.11 and older keep the islands as they were.
    readonly property bool _wifiPorts: _isWifi && dev.wifiNetSupport === 1 && dev.wifiNetPortSupport === 1
    // The role the module RUNS in (ID_WIFI_NET v0 "running", firmware 0.10+): 0 station, 1 access point, -1 not known
    // (older firmware, or not read yet). Only a known role hides the other role's settings; before 0.10 the page
    // stays as it was (the access-point state of ID_WIFI v0 still hides the station controls, as before).
    readonly property int _wifiRole: (_isWifi && dev.wifiNetSupport === 1 && !!dev.wifiNetRole && dev.wifiNetRole.valid === true)
                                     ? dev.wifiNetRole.running : -1
    readonly property bool _wifiAp: _wifiRole === 1 || (_wifiRole < 0 && _isWifi && !!dev.wifiStatusValid && dev.wifiState === 4)
    // Without a cut the panel has no header and advReveal.open is permanently true, so
    // Actions / Settings file / Upgrade sit open below the operating groups and the
    // "Advanced settings" row is a zero-height, unclickable ghost. USBL gets the cut for
    // the same reason the basic sonars do: those three are firmware and maintenance
    // actions, not operating controls.
    // The Wi-Fi module gets the cut too: its island is the operating control, the rest is upkeep.
    readonly property bool _hasCut: _isBasicSonar || _isRecorder || _isUsbl || _isWifi
    property bool _engExpanded: false

    // The device's topology group (one per link) and its own entry there, or null.
    function _topologyEntryOfDev() {
        if (!dev || typeof deviceTopology === "undefined" || !deviceTopology)
            return null
        var groups = deviceTopology.groups || []
        for (var gi = 0; gi < groups.length; ++gi) {
            var members = groups[gi].members || []
            for (var mi = 0; mi < members.length; ++mi)
                if (members[mi].device === dev)
                    return { group: groups[gi], member: members[mi] }
        }
        return null
    }

    function _linkUuidOfDev() {
        var e = _topologyEntryOfDev()
        return e ? (e.group.linkUuid || "") : ""
    }

    // Behind a Wi-Fi module (a bridge, role 3, in the device's group): the boat's logger and sonar on the head
    // unit (every Wi-Fi module is a bridge itself, hidden as one). Their link is the head unit's own port, so
    // "Set baudrate" is hidden for them (DeviceParamCatalog).
    readonly property bool behindModule: {
        var e = _topologyEntryOfDev()
        if (!e || e.member.role === 3)
            return false
        var members = e.group.members || []
        for (var i = 0; i < members.length; ++i)
            if (members[i].role === 3)
                return true
        return false
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
        // A section that folds: its key in foldPrefs; its DevIslandTitle opens and closes it.
        property string foldKey: ""
        folded: foldKey.length > 0 && !root.sectionOpen(foldKey)
        function toggleFold() { if (foldKey.length > 0) root.setSectionOpen(foldKey, folded) }
        fillColor: unconfirmed ? AppPalette.dangerBg : AppPalette.card
        borderColor: unconfirmed ? AppPalette.dangerBorder : AppPalette.border
    }

    component DevStackedRow: KIslandRow {
        stacked: true
        verticalPadding: Tokens.spaceMd
    }

    component DevIslandTitle: KIslandRow {
        id: titleRow
        // The island it opens and closes (DevIsland.foldKey); none: a plain title.
        property var foldIsland: null
        labelBold: true
        labelPixelSize: Math.max(Math.round(16 * AppPalette.scale), 13)
        minHeight: 0
        verticalPadding: Tokens.spaceSm
        interactive: !!foldIsland
        leadingSpacing: Tokens.spaceSm
        onClicked: if (foldIsland) foldIsland.toggleFold()
        // the triangle of the app's other fold headers (Advanced settings, the settings groups)
        leading: DisclosureIndicator {
            readonly property real side: titleRow.foldIsland ? Math.round(10 * AppPalette.scale) : 0
            width: side
            height: side
            visible: side > 0
            expanded: !!titleRow.foldIsland && !titleRow.foldIsland.folded
            indicatorColor: AppPalette.textSecond
        }
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

    // Small status chip for the Wi‑Fi module's port cards. tone: 0 neutral, 1 good (accent), 2 waiting, 3 bad.
    component WifiBadge: Rectangle {
        id: wb
        property string text: ""
        property int tone: 0
        readonly property color fg: tone === 1 ? AppPalette.linkOkText : tone === 2 ? AppPalette.linkIdleText
                                  : tone === 3 ? AppPalette.linkDownText : AppPalette.textSecond
        height: Math.round(20 * AppPalette.scale)
        width: wbText.implicitWidth + 2 * Tokens.spaceSm
        radius: height / 2
        color: tone === 1 ? AppPalette.linkOkBg : tone === 2 ? AppPalette.linkIdleBg : tone === 3 ? AppPalette.linkDownBg : AppPalette.chipRaised
        border.width: Tokens.cardBorderWidth
        border.color: tone === 1 ? AppPalette.linkOkBorder : tone === 2 ? AppPalette.linkIdleBorder
                    : tone === 3 ? AppPalette.linkDownBorder : AppPalette.border
        Text {
            id: wbText
            anchors.centerIn: parent
            text: wb.text
            color: wb.fg
            font.pixelSize: Tokens.fontSm
            font.bold: true
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
        property bool numeric: false
        property int minValue: 0
        property int maxValue: 65535
        readonly property bool hasFocus: wfInput.activeFocus
        signal accepted()
        signal edited()
        function takeFocus() { wfInput.forceActiveFocus() }
        IntValidator { id: wfIntValidator; bottom: wf.minValue; top: wf.maxValue }
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
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                              | (wf.password ? Qt.ImhSensitiveData : Qt.ImhNone)
                              | (wf.numeric ? Qt.ImhDigitsOnly : Qt.ImhNone)
            validator: wf.numeric ? wfIntValidator : null
            onAccepted: wf.accepted()
            onTextEdited: wf.edited()
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

    // Two taps within three seconds: for settings that can cut the module off or reboot it.
    component ConfirmDevButton: DevButton {
        id: cdb
        property string label: ""
        property bool pending: false
        signal confirmed()
        height: Tokens.controlHMd
        fontPixelSize: Tokens.fontMd
        text: pending ? qsTr("Tap again") : label
        danger: pending
        onEnabledChanged: if (!enabled) pending = false
        onClicked: {
            if (pending) {
                pending = false
                cdbTimer.stop()
                cdb.confirmed()
            } else {
                pending = true
                cdbTimer.restart()
            }
        }
        Timer { id: cdbTimer; interval: 3000; onTriggered: cdb.pending = false }
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
        foldKey: "wifi.net"
        width: root.groupWidth
        visible: root._isWifi
        rowMinHeight: Tokens.controlHMd + Tokens.spaceXs

        readonly property bool live: root._isWifi && !!dev.wifiStatusValid
        readonly property int wifiState: live ? dev.wifiState : 0
        readonly property bool connected: live && wifiState === 3
        readonly property bool busy: live && (wifiState === 1 || wifiState === 2)
        // The module runs as an access point (the running role from ID_WIFI_NET, else the AP state of v0): the
        // station-only parts (auto-connect, scan, networks, join, saved networks) are hidden, only the AP state shows.
        readonly property bool apRole: root._wifiAp
        readonly property int clients: live ? dev.wifiClients : 0
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
        readonly property string resetText: live && dev.wifiResetText ? dev.wifiResetText : ""   // firmware 0.11+
        readonly property real lostRequests: live && dev.wifiLostRequests !== undefined ? dev.wifiLostRequests : -1   // 0.14+
        readonly property bool autoConnect: live && !!dev.wifiAutoConnect
        readonly property bool currentSaved: live && !!dev.wifiCurrentSaved
        readonly property bool scanning: root._isWifi && !!dev.wifiScanning
        readonly property var networks: root._isWifi ? dev.wifiNetworks : []
        // Channels rated by the last scan (WifiChannels in C++); empty before the first scan result.
        readonly property var channels: root._isWifi ? dev.wifiChannels : []
        readonly property int bestChannel: root._isWifi ? dev.wifiBestChannel : 0
        readonly property int channelNow: connected ? dev.wifiChannel : 0
        readonly property bool channelsShown: live && !apRole && channels.length > 0
        // Live (owner 2026-09-29: the air as it is now, not an old picture): while on and this page is shown the
        // module scans again 3 s after each scan ends; each scan takes 2-4 s. Off whenever the page is opened.
        property bool watchAir: false
        property real scanDoneAt: 0
        property real ageTick: 0
        onScanningChanged: if (!scanning) { scanDoneAt = Date.now(); ageTick = scanDoneAt }
        // The module's own survey (ID_WIFI_SURVEY, firmware 0.16): the Wi-Fi airtime of each channel, measured in about
        // 2 s, in both roles (the boat's access point too). Live repeats it instead of the scan when the module has it.
        readonly property int surveySupport: root._isWifi ? dev.wifiSurveySupport : 0
        readonly property bool surveying: root._isWifi && !!dev.wifiSurveying
        readonly property var survey: root._isWifi ? dev.wifiSurveyChannels : []
        readonly property int surveyBest: root._isWifi ? dev.wifiSurveyBest : 0
        readonly property int surveySamples: root._isWifi ? dev.wifiSurveySamples : 0
        readonly property string surveyError: root._isWifi && dev.wifiSurveyError ? dev.wifiSurveyError : ""
        readonly property bool surveyShown: live && surveySupport === 1
        readonly property int surveyOwn: {
            for (var i = 0; i < survey.length; ++i)
                if (survey[i].own)
                    return survey[i].channel
            return 0
        }
        // the top of the bars: 5 % at least, else a fifth above the busiest channel
        readonly property int surveyTop: {
            var m = 50
            for (var i = 0; i < survey.length; ++i)
                m = Math.max(m, Math.round(Math.max(survey[i].busy, survey[i].mean) * 1.2))
            return m
        }
        property real surveyDoneAt: 0
        onSurveyingChanged: if (!surveying) { surveyDoneAt = Date.now(); ageTick = surveyDoneAt }
        function surveyOf(c) {
            for (var i = 0; i < survey.length; ++i)
                if (survey[i].channel === c)
                    return survey[i]
            return null
        }
        function percentText(permille) { return (permille / 10).toFixed(1) + " %" }
        function airColor(permille) {
            var dark = AppPalette.isDark
            if (permille < 20) return dark ? "#22C55E" : "#15803D"
            if (permille < 50) return dark ? "#A3E635" : "#4D7C0F"
            if (permille < 100) return dark ? "#FB923C" : "#C2410C"
            return dark ? "#F87171" : "#B91C1C"
        }
        function watchStep() {
            if (!dev)
                return
            if (surveySupport === 1) {
                if (!surveying && Date.now() - surveyDoneAt >= 3000)
                    dev.wifiSurvey()
            } else if (!apRole && !scanning && Date.now() - scanDoneAt >= 3000) {
                dev.wifiScan()
            }
        }
        onWatchAirChanged: if (watchAir) watchStep()
        onFoldedChanged: if (folded) watchAir = false   // Live is for looking at it
        function ageOf(doneAt, running, has) {
            if (running)
                return qsTr("updating…")
            if (doneAt <= 0)
                return has ? qsTr("From an earlier measurement.") : ""
            var s = Math.max(0, Math.round((ageTick - doneAt) / 1000))
            if (s < 2)
                return qsTr("updated just now")
            if (s < 120)
                return qsTr("updated %1 s ago").arg(s)
            return qsTr("updated %1 min ago").arg(Math.round(s / 60))
        }
        function ageText() {
            return surveyShown ? ageOf(surveyDoneAt, surveying, survey.length > 0)
                               : ageOf(scanDoneAt, scanning, channels.length > 0)
        }
        Timer {
            interval: 1000; repeat: true
            running: wifiIsland.watchAir && wifiIsland.visible && !wifiIsland.folded && wifiIsland.live
                     && (wifiIsland.surveySupport === 1 || !wifiIsland.apRole)
            onTriggered: wifiIsland.watchStep()
        }
        Timer {
            interval: 1000; repeat: true
            running: !wifiIsland.folded && (wifiIsland.channelsShown || wifiIsland.surveyShown)
            triggeredOnStart: true
            onTriggered: wifiIsland.ageTick = Date.now()
        }
        function channelLevel(c) {
            for (var i = 0; i < channels.length; ++i)
                if (channels[i].channel === c)
                    return channels[i].level
            return -100
        }
        function levelText(l) { return l <= -99.5 ? qsTr("nothing heard") : qsTr("%1 dBm").arg(Math.round(l)) }
        // the colours of the signal icon's levels: green quiet, red busy
        function levelColor(l) {
            var dark = AppPalette.isDark
            if (l <= -85) return dark ? "#22C55E" : "#15803D"
            if (l <= -72) return dark ? "#A3E635" : "#4D7C0F"
            if (l <= -62) return dark ? "#FB923C" : "#C2410C"
            return dark ? "#F87171" : "#B91C1C"
        }
        readonly property var savedList: root._isWifi ? dev.wifiSavedNetworks : []

        // The network the operator tapped; the join rows stay open while it is set. The module
        // never reveals a stored password and joins with exactly what it is sent, so a saved
        // network is joined by its auto-connect or by typing the password once more.
        property string pickSsid: ""
        property int pickSecurity: 0
        property bool pickSaved: false
        property bool pickHidden: false
        readonly property bool pickActive: (pickSsid.length > 0 || pickHidden) && !apRole
        // A join was sent for this SSID; the module's next status frames carry the verdict.
        property string awaitingSsid: ""

        function stateText(st) {
            switch (st) {
            case 4: return qsTr("Access point")
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
                // radio settings (v7): the Radio island explains its refusals itself
                if (version === 7)
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
            foldIsland: wifiIsland
            minHeight: Tokens.rowH
            verticalPadding: 0

            Rectangle {
                id: wifiChip
                readonly property color fg: (wifiIsland.connected || wifiIsland.apRole) ? AppPalette.linkOkText
                                          : (wifiIsland.busy ? AppPalette.linkIdleText : AppPalette.linkDownText)
                height: Math.round(24 * AppPalette.scale)
                width: wifiChipText.implicitWidth + 2 * Tokens.spaceMd
                radius: height / 2
                color: (wifiIsland.connected || wifiIsland.apRole) ? AppPalette.linkOkBg
                     : (wifiIsland.busy ? AppPalette.linkIdleBg : AppPalette.linkDownBg)
                border.width: Tokens.cardBorderWidth
                border.color: (wifiIsland.connected || wifiIsland.apRole) ? AppPalette.linkOkBorder
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
            label: (wifiIsland.connected || wifiIsland.apRole || wifiIsland.wifiState === 2) ? wifiIsland.ssid : qsTr("Not connected")
            labelBold: wifiIsland.connected || wifiIsland.apRole
            caption: {
                if (!wifiIsland.live)
                    return qsTr("Waiting for the module…")
                if (wifiIsland.apRole)
                    return qsTr("Own access point · ch %1 · %2 · clients: %3").arg(wifiIsland.channel)
                           .arg(wifiIsland.securityText(wifiIsland.security)).arg(wifiIsland.clients)
                         + (wifiIsland.ip.length ? " · " + wifiIsland.ip : "")
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
            caption: wifiIsland.connected ? qsTr("Module up %1").arg(wifiIsland.uptimeText(wifiIsland.uptime))
                                            + (wifiIsland.resetText.length ? qsTr(" · last restart: %1").arg(wifiIsland.resetText) : "")
                                            + (wifiIsland.lostRequests >= 0 ? qsTr(" · requests lost: %1").arg(wifiIsland.lostRequests) : "") : ""
        }

        KIslandRow {
            open: wifiIsland.live && !wifiIsland.apRole
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
            open: wifiIsland.live && !wifiIsland.apRole
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
            open: wifiIsland.live && !wifiIsland.apRole
            label: wifiIsland.scanning ? qsTr("Networks nearby (searching…)") : qsTr("Networks nearby")
        }
        Repeater {
            model: wifiIsland.apRole ? [] : wifiIsland.networks
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
            open: wifiIsland.live && !wifiIsland.apRole && !wifiIsland.scanning && wifiIsland.networks.length === 0
                  && wifiIsland.channels.length === 0
            minHeight: 0
            verticalPadding: Tokens.spaceMd
            label: qsTr("Nothing found yet. Tap Scan.")
            labelPixelSize: Tokens.fontSm
            labelColor: AppPalette.textMuted
        }

        // Channels by the last scan (field test 2026-09-29, owner: to pick the freest channel for the boat). A bar is
        // the power of the other networks on and next to that channel (WifiChannels, wifi_ops.h), the number above it
        // the networks on it; channels 12 and 13 are shown dim (the access point takes 1-11).
        KIslandSection {
            open: wifiIsland.channelsShown || wifiIsland.surveyShown
            label: qsTr("Channels")
        }
        KIslandRow {
            open: wifiIsland.channelsShown || wifiIsland.surveyShown
            label: qsTr("Live")
            caption: (wifiIsland.watchAir
                      ? (wifiIsland.surveyShown ? qsTr("Measuring again and again while this page is open; each measurement takes about 4 s and pauses the boat link.")
                                                : qsTr("Scanning again and again while this page is open; each scan takes 2–4 s and may stall the boat link."))
                      : (wifiIsland.surveyShown ? qsTr("Off: the picture is of one measurement.") : qsTr("Off: the picture is of one scan.")))
                     + " " + wifiIsland.ageText()
            interactive: true
            onClicked: liveSwitch.click()
            KSwitch {
                id: liveSwitch
                flat: true
                checked: wifiIsland.watchAir
                onToggled: wifiIsland.watchAir = checked
            }
        }
        // Wi-Fi airtime measured by the module (ID_WIFI_SURVEY): a bar per channel 1-11, the share above it.
        DevStackedRow {
            open: wifiIsland.surveyShown
            label: qsTr("Wi‑Fi airtime")
            caption: qsTr("Measured by the module: the share of the time the air carried Wi‑Fi frames on each channel, 0.3 s a channel (a lower bound: interference that is not Wi‑Fi is not seen). A bar is the other networks' share in the last measurement, with our own link's on top in a lighter tone (it moves with us; neighbours of our channel • hear it too); the figure and the tick, the mean of the last measurements, are the others' share, which the pick goes by. A measurement takes about 4 s and pauses the boat link.")
            Column {
                width: parent.width
                spacing: Tokens.spaceSm
                DevButton {
                    width: Math.round(160 * AppPalette.scale); height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                    text: wifiIsland.surveying ? qsTr("Measuring…") : qsTr("Measure")
                    enabled: !wifiIsland.surveying
                    toolTipText: qsTr("Measure the Wi‑Fi airtime of every channel now (about 4 s)")
                    onClicked: if (dev) dev.wifiSurvey()
                }
                Item {
                    id: airChart
                    visible: wifiIsland.survey.length > 0
                    width: parent.width
                    height: Math.round(96 * AppPalette.scale)
                    readonly property int n: Math.max(1, wifiIsland.survey.length)
                    readonly property real gap: Math.max(2, Math.round(3 * AppPalette.scale))
                    readonly property real colW: (width - (n - 1) * gap) / n
                    readonly property real labelH: Math.round(16 * AppPalette.scale)
                    readonly property real barH: height - 2 * labelH
                    Row {
                        spacing: airChart.gap
                        Repeater {
                            model: wifiIsland.survey
                            delegate: Item {
                                width: airChart.colW
                                height: airChart.height
                                // the other networks' part (the pick's), ours on top of it
                                readonly property real frac: Math.max(0, Math.min(1, modelData.others / wifiIsland.surveyTop))
                                readonly property real ownFrac: Math.max(0, Math.min(1 - frac, modelData.ownAir / wifiIsland.surveyTop))
                                readonly property real meanFrac: Math.max(0, Math.min(1, modelData.mean / wifiIsland.surveyTop))
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    height: airChart.labelH
                                    verticalAlignment: Text.AlignVCenter
                                    text: (modelData.others / 10).toFixed(1)
                                    color: AppPalette.textMuted
                                    font.pixelSize: Tokens.fontSm
                                }
                                Rectangle {
                                    y: airChart.labelH
                                    width: parent.width
                                    height: airChart.barH
                                    radius: Math.round(3 * AppPalette.scale)
                                    color: AppPalette.rowRaised
                                    border.width: modelData.best ? 2 : 0
                                    border.color: AppPalette.accentBorder
                                    Rectangle {
                                        id: othersBar
                                        anchors.bottom: parent.bottom
                                        width: parent.width
                                        height: Math.max(2, parent.height * frac)
                                        radius: parent.radius
                                        color: wifiIsland.airColor(modelData.others)
                                    }
                                    // our own network's airtime (firmware 0.17): it moves with us
                                    Rectangle {
                                        visible: ownFrac > 0
                                        anchors.bottom: othersBar.top
                                        width: parent.width
                                        height: parent.height * ownFrac
                                        color: AppPalette.accentBorder
                                        opacity: 0.55
                                    }
                                    // the mean of the last measurements, which the pick goes by
                                    Rectangle {
                                        visible: wifiIsland.surveySamples > 1
                                        x: -1
                                        width: parent.width + 2
                                        height: Math.max(2, Math.round(2 * AppPalette.scale))
                                        y: Math.round((parent.height - height) * (1 - meanFrac))
                                        color: AppPalette.textStrong
                                    }
                                }
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    y: airChart.labelH + airChart.barH
                                    height: airChart.labelH
                                    verticalAlignment: Text.AlignVCenter
                                    text: (modelData.own ? "•" : "") + modelData.channel
                                    font.bold: modelData.best || modelData.own
                                    color: modelData.best ? AppPalette.accentText : (modelData.own ? AppPalette.textStrong : AppPalette.textMuted)
                                    font.pixelSize: Tokens.fontSm
                                }
                            }
                        }
                    }
                }
            }
        }
        DevInfoRow {
            open: wifiIsland.surveyShown && wifiIsland.surveyBest > 0
            label: wifiIsland.surveySamples > 1 ? qsTr("Least busy (mean of %1)").arg(wifiIsland.surveySamples)
                                                 : qsTr("Least busy (Wi‑Fi airtime)")
            value: {
                var c = wifiIsland.surveyOf(wifiIsland.surveyBest)
                return qsTr("channel %1").arg(wifiIsland.surveyBest) + (c ? " · " + wifiIsland.percentText(c.mean)
                       + " · " + qsTr("%1 transmitters").arg(c.transmitters) : "")
            }
        }
        DevInfoRow {
            open: wifiIsland.surveyShown && wifiIsland.surveyOwn > 0
            label: qsTr("Our channel now (•)")
            value: {
                var c = wifiIsland.surveyOf(wifiIsland.surveyOwn)
                return qsTr("channel %1").arg(wifiIsland.surveyOwn) + (c ? " · " + qsTr("others %1").arg(wifiIsland.percentText(c.mean))
                       + (c.ownAir > 0 ? " · " + qsTr("ours %1").arg(wifiIsland.percentText(c.ownAir)) : "") : "")
            }
        }
        KIslandRow {
            open: wifiIsland.surveyShown && wifiIsland.surveyError.length > 0
            minHeight: 0
            verticalPadding: Tokens.spaceSm
            label: qsTr("Not measured: %1").arg(wifiIsland.surveyError)
            labelPixelSize: Tokens.fontSm
            labelColor: AppPalette.dangerText
        }

        DevStackedRow {
            open: wifiIsland.channelsShown
            label: wifiIsland.surveyShown ? qsTr("Networks heard (scan)") : ""
            caption: wifiIsland.surveyShown
                     ? qsTr("By the last scan: the power of the other networks on and next to each channel, with how 2.4 GHz channels overlap; our own network is left out.")
                     : qsTr("By the networks heard: their power, counted with how 2.4 GHz channels overlap; our own network is left out. The traffic on a channel is not measured: that needs the module's firmware 0.16.")
            Item {
                id: chanChart
                width: parent.width
                height: Math.round(96 * AppPalette.scale)
                readonly property int n: Math.max(1, wifiIsland.channels.length)
                readonly property real gap: Math.max(2, Math.round(3 * AppPalette.scale))
                readonly property real colW: (width - (n - 1) * gap) / n
                readonly property real labelH: Math.round(16 * AppPalette.scale)
                readonly property real barH: height - 2 * labelH
                Row {
                    spacing: chanChart.gap
                    Repeater {
                        model: wifiIsland.channels
                        delegate: Item {
                            width: chanChart.colW
                            height: chanChart.height
                            // -100 dBm (nothing heard) at the bottom, -30 dBm at the top
                            readonly property real frac: Math.max(0, Math.min(1, (modelData.level + 100) / 70))
                            readonly property bool now: modelData.channel === wifiIsland.channelNow
                            opacity: modelData.ap ? 1.0 : 0.45
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                height: chanChart.labelH
                                verticalAlignment: Text.AlignVCenter
                                text: modelData.networks > 0 ? String(modelData.networks) : ""
                                color: AppPalette.textMuted
                                font.pixelSize: Tokens.fontSm
                            }
                            Rectangle {
                                y: chanChart.labelH
                                width: parent.width
                                height: chanChart.barH
                                radius: Math.round(3 * AppPalette.scale)
                                color: AppPalette.rowRaised
                                border.width: modelData.best ? 2 : 0
                                border.color: AppPalette.accentBorder
                                Rectangle {
                                    anchors.bottom: parent.bottom
                                    width: parent.width
                                    height: Math.max(2, parent.height * frac)
                                    radius: parent.radius
                                    color: wifiIsland.levelColor(modelData.level)
                                }
                            }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                y: chanChart.labelH + chanChart.barH
                                height: chanChart.labelH
                                verticalAlignment: Text.AlignVCenter
                                text: (now ? "•" : "") + modelData.channel
                                font.bold: modelData.best || now
                                color: modelData.best ? AppPalette.accentText : (now ? AppPalette.textStrong : AppPalette.textMuted)
                                font.pixelSize: Tokens.fontSm
                            }
                        }
                    }
                }
            }
        }
        DevInfoRow {
            open: wifiIsland.channelsShown && wifiIsland.bestChannel > 0 && !wifiIsland.surveyShown
            label: qsTr("Least busy")
            value: qsTr("channel %1").arg(wifiIsland.bestChannel) + " · "
                   + wifiIsland.levelText(wifiIsland.channelLevel(wifiIsland.bestChannel))
        }
        DevInfoRow {
            open: wifiIsland.channelsShown && wifiIsland.channelNow > 0 && !wifiIsland.surveyShown
            label: qsTr("The boat link now (•)")
            value: qsTr("channel %1").arg(wifiIsland.channelNow) + " · "
                   + wifiIsland.levelText(wifiIsland.channelLevel(wifiIsland.channelNow))
        }
        KIslandRow {
            readonly property int pick: wifiIsland.surveyShown && wifiIsland.surveyBest > 0 ? wifiIsland.surveyBest : wifiIsland.bestChannel
            readonly property int now: wifiIsland.surveyShown && wifiIsland.surveyOwn > 0 ? wifiIsland.surveyOwn : wifiIsland.channelNow
            open: (wifiIsland.channelsShown || wifiIsland.surveyShown) && pick > 0 && now > 0 && pick !== now
            minHeight: 0
            verticalPadding: Tokens.spaceSm
            label: wifiIsland.apRole
                   ? qsTr("To move the link there, set channel %1 in Role and access point → Channel below, then Apply; the head unit's module follows by itself.").arg(pick)
                   : qsTr("To move the link there, set channel %1 on the boat's module: Role and access point → Channel, then Apply; this module follows by itself.").arg(pick)
            labelPixelSize: Tokens.fontSm
            labelColor: AppPalette.textMuted
        }

        // Saved networks
        KIslandSection {
            open: wifiIsland.savedList.length > 0 && !wifiIsland.apRole
            label: qsTr("Saved networks")
        }
        Repeater {
            model: wifiIsland.apRole ? [] : wifiIsland.savedList
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

    // ── Boat link: the module relays the boat's SBP stream ────────────────
    // Module firmware 0.6+ forwards frames not addressed to itself to the boat over UDP or TCP and
    // writes the boat's frames into this port unchanged, so the boat's devices show up as devices
    // on the same link while the module answers at the address set here (ID_WIFI v5, counters v6;
    // docs/WIFI_MODULE.md). The fields show the module's values until the operator edits them.
    DevIsland {
        id: relayIsland
        foldKey: "wifi.relay"
        width: root.groupWidth
        // firmware 0.10 shows this as line 0 of the Lines island (ID_WIFI_NET v3); kept for 0.7/0.8
        visible: root._isWifi && dev.wifiNetSupport !== 1
        rowMinHeight: Tokens.controlHMd + Tokens.spaceXs

        readonly property int support: root._isWifi ? dev.wifiRelaySupport : -1   // -1 unknown, 0 none, 1 yes
        readonly property bool cfgValid: root._isWifi && !!dev.wifiRelayValid
        readonly property int mode: cfgValid ? dev.wifiRelayMode : 0
        readonly property bool statsValid: root._isWifi && !!dev.wifiRelayStatsValid
        readonly property int relayState: statsValid ? dev.wifiRelayState : 0
        readonly property bool editable: support !== 0
        readonly property bool showCounters: support === 1 && mode !== 0 && statsValid
        readonly property bool editing: relayAddrField.hasFocus || relayIpField.hasFocus
                                        || relayPortField.hasFocus || relayLportField.hasFocus
        property bool dirty: false
        property int modeSel: 0
        readonly property bool pendingApply: visible && support === 1 && dirty

        function stateText(st) {
            switch (st) {
            case 3: return qsTr("Ready")
            case 2: return qsTr("Waiting for the boat")
            case 1: return qsTr("No Wi‑Fi")
            }
            return qsTr("Off")
        }
        function chipText() {
            if (support === 0)
                return qsTr("No relay in firmware")
            if (!cfgValid)
                return qsTr("No data")
            if (mode === 0)
                return qsTr("Off")
            return statsValid ? stateText(relayState) : qsTr("On")
        }
        function sync(force) {
            if (!cfgValid || ((editing || dirty) && !force))
                return
            modeSel = dev.wifiRelayMode
            relayAddrField.text = String(dev.wifiRelayAddr)
            relayIpField.text = dev.wifiRelayIp
            relayPortField.text = String(dev.wifiRelayPort)
            relayLportField.text = String(dev.wifiRelayLocalPort)
            dirty = false
        }
        function apply() {
            if (!dev)
                return false
            var addr = parseInt(relayAddrField.text, 10)
            var rport = parseInt(relayPortField.text, 10)
            var lport = parseInt(relayLportField.text, 10)
            if (isNaN(addr) || addr < 0 || addr > 255) {
                notifications.warning(qsTr("Module address must be 0…255"))
                return false
            }
            if (modeSel !== 0 && (isNaN(rport) || rport < 1 || rport > 65535)) {
                notifications.warning(qsTr("Boat port must be 1…65535"))
                return false
            }
            if (isNaN(rport))
                rport = 0
            if (isNaN(lport))
                lport = 0
            var ip = relayIpField.text.trim()
            if (!ip.length)
                ip = "0.0.0.0"
            if (!dev.wifiSetRelay(modeSel, addr, ip, rport, lport)) {
                notifications.warning(qsTr("Boat IP address must be four numbers 0…255"))
                return false
            }
            dirty = false
            root.forceActiveFocus()
            if (modeSel === 0)
                notifications.info(qsTr("Relay switched off; the module answers at address %1").arg(addr))
            else
                notifications.info(qsTr("Relay settings sent; the module now answers at address %1 and shows up as a new device").arg(addr))
            return true
        }

        Connections {
            target: root._isWifi ? root.dev : null
            function onWifiRelayChanged() { relayIsland.sync(false) }
        }
        onCfgValidChanged: sync(false)
        onVisibleChanged: if (visible && dev) dev.wifiRelayRefresh()
        Component.onCompleted: { sync(true); if (visible && dev) dev.wifiRelayRefresh() }

        Timer {
            interval: 2000
            repeat: true
            // folded as well: the title's chip shows the state these stats carry
            running: relayIsland.visible && relayIsland.support === 1 && relayIsland.mode !== 0
            onTriggered: if (dev) dev.wifiRelayStatsRefresh()
        }

        DevIslandTitle {
            label: qsTr("Boat link")
            foldIsland: relayIsland
            minHeight: Tokens.rowH
            verticalPadding: 0

            Rectangle {
                id: relayChip
                readonly property bool good: relayIsland.support === 1 && relayIsland.mode !== 0 && relayIsland.relayState === 3
                readonly property bool bad: relayIsland.support === 0
                                            || (relayIsland.mode !== 0 && relayIsland.statsValid && relayIsland.relayState !== 3)
                height: Math.round(24 * AppPalette.scale)
                width: relayChipText.implicitWidth + 2 * Tokens.spaceMd
                radius: height / 2
                color: good ? AppPalette.linkOkBg : (bad ? AppPalette.linkDownBg : AppPalette.linkIdleBg)
                border.width: Tokens.cardBorderWidth
                border.color: good ? AppPalette.linkOkBorder : (bad ? AppPalette.linkDownBorder : AppPalette.linkIdleBorder)
                Text {
                    id: relayChipText
                    anchors.centerIn: parent
                    text: relayIsland.chipText()
                    color: relayChip.good ? AppPalette.linkOkText : (relayChip.bad ? AppPalette.linkDownText : AppPalette.linkIdleText)
                    font.pixelSize: Tokens.fontSm
                    font.bold: true
                }
            }
        }

        KIslandRow {
            open: relayIsland.support === 0
            minHeight: 0
            verticalPadding: Tokens.spaceMd
            label: qsTr("Module firmware %1 has no relay. Update it to 0.6 or newer with Upgrade below.").arg(dev ? dev.fwVersion : "")
            labelPixelSize: Tokens.fontSm
            labelColor: AppPalette.dangerText
        }

        KIslandRow {
            open: relayIsland.editable
            label: qsTr("Mode")
            KTabBar {
                width: Math.round(220 * AppPalette.scale)
                fontPixelSize: Tokens.fontMd
                options: [
                    { label: qsTr("Off"), value: 0 },
                    { label: "UDP", value: 1 },
                    { label: "TCP", value: 2 }
                ]
                currentValue: relayIsland.modeSel
                onValueSelected: function(v) { relayIsland.modeSel = v; relayIsland.dirty = true }
            }
        }
        DevStackedRow {
            open: relayIsland.editable
            label: qsTr("Module address on this port")
            caption: qsTr("Must differ from the boat device's address, which is usually 0")
            WifiField {
                id: relayAddrField
                width: parent.width
                numeric: true
                placeholder: "87"
                onEdited: relayIsland.dirty = true
            }
        }
        DevStackedRow {
            open: relayIsland.editable
            label: qsTr("Boat IP address")
            caption: qsTr("0.0.0.0 with UDP: the module answers whoever sends to it first")
            WifiField {
                id: relayIpField
                width: parent.width
                placeholder: "0.0.0.0"
                onEdited: relayIsland.dirty = true
            }
        }
        DevStackedRow {
            open: relayIsland.editable
            label: qsTr("Boat port")
            caption: qsTr("UDP or TCP port of the boat device")
            WifiField {
                id: relayPortField
                width: parent.width
                numeric: true
                placeholder: "0"
                onEdited: relayIsland.dirty = true
            }
        }
        DevStackedRow {
            open: relayIsland.editable
            label: qsTr("Own UDP port")
            caption: qsTr("The port the boat sends to; 0 = same as the boat port")
            WifiField {
                id: relayLportField
                width: parent.width
                numeric: true
                placeholder: "0"
                onEdited: relayIsland.dirty = true
            }
        }
        DevStackedRow {
            open: relayIsland.editable
            Row {
                width: parent.width
                spacing: Tokens.spaceMd
                readonly property real bw: (width - Tokens.spaceMd) / 2
                DevButton {
                    width: parent.bw; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                    text: qsTr("Apply")
                    enabled: relayIsland.support === 1
                    normalBg: AppPalette.accentBg
                    hoverBg: AppPalette.accentBgHover
                    normalBorder: AppPalette.accentBorder
                    hoverBorder: AppPalette.accentBorder
                    textColor: AppPalette.accentText
                    toolTipText: qsTr("Send the settings to the module; it stores and applies them at once")
                    onClicked: relayIsland.apply()
                }
                DevButton {
                    width: parent.bw; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                    text: qsTr("Read back")
                    toolTipText: qsTr("Discard edits and show what the module has")
                    onClicked: {
                        relayIsland.dirty = false
                        relayIsland.sync(true)
                        if (dev)
                            dev.wifiRelayRefresh()
                    }
                }
            }
        }

        KIslandSection {
            open: relayIsland.support === 1 && relayIsland.mode !== 0
            label: qsTr("Relay counters")
        }
        DevInfoRow {
            label: qsTr("Peer")
            value: (relayIsland.showCounters && dev.wifiRelayPeerPort > 0)
                   ? (dev.wifiRelayPeerIp + ":" + dev.wifiRelayPeerPort) : ""
        }
        DevInfoRow {
            label: qsTr("To the boat")
            value: relayIsland.showCounters
                   ? qsTr("%1 frames · %2 packets · %3 dropped").arg(dev.wifiRelayUpFrames).arg(dev.wifiRelayUpPackets).arg(dev.wifiRelayUpDrops)
                   : ""
        }
        DevInfoRow {
            label: qsTr("From the boat")
            value: relayIsland.showCounters
                   ? qsTr("%1 packets · %2 frames · %3 dropped").arg(dev.wifiRelayDownPackets).arg(dev.wifiRelayDownFrames).arg(dev.wifiRelayDownDrops)
                   : ""
        }
        DevInfoRow {
            label: qsTr("For the module")
            value: relayIsland.showCounters ? qsTr("%1 frames").arg(dev.wifiRelayLocalFrames) : ""
        }
    }

    // ── Wi‑Fi module: radio (firmware 0.8+, ID_WIFI v7) ───────────────────
    DevIsland {
        id: radioIsland
        foldKey: "wifi.radio"
        width: root.groupWidth
        visible: root._isWifi && dev.wifiRadioSupport !== 0
        rowMinHeight: Tokens.controlHMd + Tokens.spaceXs

        readonly property var r: root._isWifi ? dev.wifiRadio : ({})
        readonly property bool valid: !!r && r.valid === true
        readonly property bool linkUp: valid && r.phyNow !== 255
        // An access point has no negotiated PHY (0xFF): its width is what the driver was given.
        readonly property bool apRole: root._isWifi && !!dev.wifiNetRole && dev.wifiNetRole.valid === true
                                       && dev.wifiNetRole.running === 1
        // What the link really uses. The width comes from the negotiated PHY: the module's own "width in use" is
        // the driver's setting and said 40 on an HT20 link. The power in use is the driver's step.
        readonly property int widthNow: apRole ? (valid ? r.bandwidthNowMhz : 0) : (linkUp ? (r.phyNow === 5 ? 40 : 20) : 0)
        readonly property real powerNow: valid && r.powerNowDbm > 0 ? r.powerNowDbm : 0
        property real selPower: 20
        property int selProto: 7
        // The width follows the protocols: without 802.11n (b, b/g, LR only) only 20 MHz exists, the 40 MHz button is
        // off, and the operator's choice comes back with b/g/n or b/g/n+LR. The rule is WifiRadioRules in C++.
        property int selBwChoice: 20
        readonly property bool can40: root._isWifi && dev.wifiRadio40Allowed(selProto)
        readonly property int selBw: can40 ? selBwChoice : 20
        property int selPs: 0
        property bool dirty: false
        readonly property bool pendingApply: visible && valid && dirty
        // DevDriver checks the result once the link is back and saves what is in effect if the module could not do
        // it (WifiRadioCheck), whether or not this page is still open.
        function apply() {
            if (!dev.wifiSetRadio(selPower, selProto, selBw, selPs)) {
                notifications.warning(qsTr("Not sent: no connection to the module"))
                return false
            }
            sentLrAp = apRole && selProto === 8
            radioError = ""
            dirty = false
            notifications.info(qsTr("Radio settings sent to the module"))
            return true
        }
        // The last Apply asked an access point for LR only; with firmware before 0.12 an ERR_PAYLOAD to it is that.
        property bool sentLrAp: false
        property string radioError: ""
        function refusalText(resp) {
            if (resp === 3)
                return (sentLrAp && root._isWifi && dev.wifiLrApSupport === 0)
                       ? qsTr("Firmware 0.11 refuses LR only for an access point; update the module to 0.12.")
                       : qsTr("The module refused these radio settings (ERR_PAYLOAD).")
            if (resp === 8)
                return qsTr("The module refused these radio settings: not possible now (ERR_RUNTIME).")
            return qsTr("The module refused these radio settings (code %1).").arg(resp)
        }
        // The menu shows what is in effect; the saved values only while the module has no link. A width in effect
        // without 802.11n is always 20 MHz and says nothing of the operator's choice, which is kept then.
        function sync(force) {
            if (!valid || (dirty && !force))
                return
            selPower = powerNow > 0 ? powerNow : r.powerDbm
            selProto = r.protocolsNow > 0 ? r.protocolsNow : r.protocols
            if (dev.wifiRadio40Allowed(selProto))
                selBwChoice = widthNow > 0 ? widthNow : r.bandwidthMhz
            selPs = r.powerSave
            dirty = false
        }
        // A selection stays until Apply, "Apply and save" under the page, or Read back (owner 2026-09-29: it no
        // longer goes back by itself after 30 s).
        function edited() {
            dirty = true
        }
        function phyText(p) {
            switch (p) {
            case 0: return "LR"
            case 1: return "802.11b"
            case 2: return "802.11g"
            case 4: return "802.11n HT20"
            case 5: return "802.11n HT40"
            }
            return qsTr("no link")
        }
        onValidChanged: sync(false)
        onLinkUpChanged: sync(false)
        onApRoleChanged: sync(false)
        Component.onCompleted: sync(true)
        Connections {
            target: root._isWifi ? root.dev : null
            ignoreUnknownSignals: true
            function onWifiRadioChanged() { radioIsland.sync(false) }
            // Every refusal of a radio setting is explained here, inline and as the notice (the Wi-Fi island leaves v7 alone).
            function onWifiCommandResult(version, resp) {
                if (version !== 7 || resp === 1)
                    return
                radioIsland.radioError = radioIsland.refusalText(resp)
                radioIsland.sentLrAp = false
                notifications.warning(qsTr("Radio: %1").arg(radioIsland.radioError))
            }
        }

        DevIslandTitle { label: qsTr("Radio"); foldIsland: radioIsland }

        DevStackedRow {
            label: qsTr("Transmit power limit, dBm")
            caption: !radioIsland.valid ? ""
                     : qsTr("In use: %1 dBm").arg(radioIsland.powerNow)
                       + (Math.abs(radioIsland.r.powerDbm - radioIsland.powerNow) >= 0.25
                          ? qsTr(" (saved %1 dBm; the driver works in steps)").arg(radioIsland.r.powerDbm) : "")
            KTabBar {
                width: parent.width
                fontPixelSize: Tokens.fontSm
                // the driver's steps: asking 21 gives 20
                options: [ { label: "2", value: 2 }, { label: "8.5", value: 8.5 }, { label: "11", value: 11 },
                           { label: "15", value: 15 }, { label: "18", value: 18 }, { label: "20", value: 20 } ]
                currentValue: radioIsland.selPower
                onValueSelected: function(v) { radioIsland.selPower = v; radioIsland.edited() }
            }
        }
        DevStackedRow {
            label: qsTr("Protocols")
            caption: !radioIsland.valid ? ""
                     : radioIsland.apRole ? qsTr("Access point: each client negotiates its own mode.")
                     : qsTr("Link now: %1").arg(radioIsland.phyText(radioIsland.r.phyNow))
                       + qsTr(". LR needs an LR-capable access point: another Kogger module (Espressif) with LR or b/g/n+LR.")
            KTabBar {
                width: parent.width
                fontPixelSize: Tokens.fontSm
                // LR only in both roles: firmware 0.12 takes it for an access point (module-to-module), 0.11 refuses it there
                options: [ { label: "b", value: 1 }, { label: "b/g", value: 3 }, { label: "b/g/n", value: 7 },
                           { label: "LR", value: 8 }, { label: "b/g/n+LR", value: 15 } ]
                currentValue: radioIsland.selProto
                onValueSelected: function(v) { radioIsland.selProto = v; radioIsland.radioError = ""; radioIsland.edited() }
            }
        }
        KIslandRow {
            open: radioIsland.apRole && radioIsland.selProto === 8
            minHeight: 0
            verticalPadding: Tokens.spaceSm
            caption: qsTr("LR only: phones and laptops cannot join; only Espressif stations with LR (another Kogger module set to LR or b/g/n+LR) can.")
            captionColor: AppPalette.linkIdleText
        }
        KIslandRow {
            open: radioIsland.radioError.length > 0
            minHeight: 0
            verticalPadding: Tokens.spaceSm
            caption: radioIsland.radioError
            captionColor: AppPalette.dangerText
        }
        DevStackedRow {
            label: qsTr("Channel width")
            caption: !radioIsland.can40 ? qsTr("20 MHz only: 40 MHz needs 802.11n (b/g/n or b/g/n+LR); LR, b and b/g run at 20 MHz.")
                     : !radioIsland.valid ? ""
                     : !radioIsland.linkUp && !radioIsland.apRole ? qsTr("No link: the saved width is shown")
                     : qsTr("In use: %1 MHz").arg(radioIsland.widthNow)
                       + (radioIsland.r.bandwidthMhz === radioIsland.widthNow ? ""
                          : radioIsland.apRole ? qsTr(" (saved %1 MHz)").arg(radioIsland.r.bandwidthMhz)
                          : qsTr(" (saved %1 MHz; the access point gives no 40 MHz)").arg(radioIsland.r.bandwidthMhz))
            KTabBar {
                width: Math.round(170 * AppPalette.scale)
                fontPixelSize: Tokens.fontSm
                options: [ { label: qsTr("20 MHz"), value: 20 },
                           { label: qsTr("40 MHz"), value: 40, enabled: radioIsland.can40,
                             tip: radioIsland.can40 ? "" : qsTr("40 MHz needs 802.11n") } ]
                currentValue: radioIsland.selBw
                onValueSelected: function(v) { radioIsland.selBwChoice = v; radioIsland.edited() }
            }
        }
        KIslandRow {
            label: qsTr("Power save")
            KTabBar {
                width: Math.round(170 * AppPalette.scale)
                fontPixelSize: Tokens.fontSm
                options: [ { label: qsTr("Off"), value: 0 }, { label: qsTr("Min"), value: 1 }, { label: qsTr("Max"), value: 2 } ]
                currentValue: radioIsland.selPs
                onValueSelected: function(v) { radioIsland.selPs = v; radioIsland.edited() }
            }
        }
        DevStackedRow {
            caption: (root._isWifi && dev.wifiRadioChecking ? qsTr("Checking what the module uses now… ") : "")
                     + (radioIsland.dirty ? qsTr("Not applied yet: the menu differs from what the module uses. ") : "")
                     + qsTr("Protocol and width changes make the module reconnect: the boat link drops for a few seconds. 40 MHz needs 802.11n and an access point that gives it.")
            Row {
                width: parent.width
                spacing: Tokens.spaceMd
                readonly property real bw: (width - Tokens.spaceMd) / 2
                ConfirmDevButton {
                    width: parent.bw
                    label: qsTr("Apply")
                    enabled: radioIsland.valid
                    onConfirmed: radioIsland.apply()
                }
                DevButton {
                    width: parent.bw; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                    text: qsTr("Read back")
                    onClicked: { radioIsland.dirty = false; radioIsland.sync(true); dev.wifiRefresh() }
                }
            }
        }
    }

    // ── Wi‑Fi module: speed of its port to this host (line 0, ID_UART + ID_FLASH), firmware before 0.12 ──
    // The sequence (module switches, link follows, the module must answer at the new speed, then the speed is
    // saved in it; no answer puts the link back) runs in DevDriver::wifiSetLineBaud, not here: this page is
    // unloaded as soon as it is left. Steps go to the log as "wifi-port:" lines. With 0.12 the Ports island
    // below sets both ports instead.
    DevIsland {
        id: portIsland
        foldKey: "wifi.port"
        width: root.groupWidth
        visible: root._isWifi && !root._wifiPorts
        rowMinHeight: Tokens.controlHMd + Tokens.spaceXs

        readonly property int now: root._isWifi ? dev.baudrate : 0
        readonly property bool busy: root._isWifi && dev.wifiPortBusy
        property int sel: 0
        readonly property bool pendingApply: visible && !busy && sel > 0 && now > 0 && sel !== now
        function apply() {
            if (dev.wifiSetLineBaud(0, sel))
                return true
            notifications.warning(qsTr("The port speed could not be changed now: nothing changed"))
            return false
        }

        Component.onCompleted: if (now > 0) sel = now
        onNowChanged: if (!busy && now > 0) sel = now
        onBusyChanged: if (!busy && now > 0) sel = now

        DevIslandTitle { label: qsTr("Port speed"); foldIsland: portIsland }

        DevStackedRow {
            label: qsTr("Module port to this device, bit/s")
            caption: !portIsland.busy
                     ? qsTr("Now %1. The module keeps the speed it is given here; this device's port follows.").arg(portIsland.now > 0 ? portIsland.now : "—")
                     : qsTr("Changing to %1… (it finishes even if this page is closed)").arg(dev.wifiPortTarget)
            KTabBar {
                width: parent.width
                fontPixelSize: Tokens.fontSm
                options: [ { label: "460k", value: 460800 }, { label: "921k", value: 921600 }, { label: "1M", value: 1000000 },
                           { label: "1.5M", value: 1500000 }, { label: "2M", value: 2000000 }, { label: "3M", value: 3000000 },
                           { label: "4M", value: 4000000 } ]
                currentValue: portIsland.sel
                onValueSelected: function(v) { portIsland.sel = v }
            }
        }
        DevStackedRow {
            caption: qsTr("The boat stream is about 92 KB/s: 921k is full, 2M leaves half free. Both ends must do the speed (the head unit's UART6: up to 4M).")
            ConfirmDevButton {
                width: parent.width
                label: qsTr("Apply %1").arg(portIsland.sel)
                enabled: !portIsland.busy && portIsland.sel > 0 && portIsland.sel !== portIsland.now
                onConfirmed: portIsland.apply()
            }
        }
    }

    // ── Wi‑Fi module: its two physical ports X1 and X2 (firmware 0.12, ID_WIFI_NET v7) ──
    // The ports are equal: the module answers on either, each has its own rate and its own line to the network
    // (X1 = line 0, X2 = line 1). While this island is on screen the module is asked every 2 s for both port
    // summaries (page 0), each port's devices (page 1) and network side (page 2), the line records (v3) and counters
    // (v4); the byte rates come from successive summaries (DevDriver). Changes run in the driver, so they finish
    // when the page is left: a rate (DevDriver::wifiSetLineBaud: this device's port through the checked ID_UART
    // sequence, the other one through its line record) and the line settings (DevDriver::wifiSetLineConfig: the
    // record is read, only the edited fields change, then it is read back). With 0.12 this island replaces Lines.
    DevIsland {
        id: portsIsland
        foldKey: "wifi.ports"
        width: root.groupWidth
        visible: root._wifiPorts
        rowMinHeight: Tokens.controlHMd + Tokens.spaceXs

        readonly property var ports: root._isWifi ? dev.wifiNetPorts : []
        readonly property var devices: root._isWifi ? dev.wifiNetPortDevices : []
        readonly property var peers: root._isWifi ? dev.wifiNetPortPeers : []
        readonly property var lines: root._isWifi ? dev.wifiNetLines : []          // line 0 = X1, line 1 = X2 (v3)
        readonly property var stats: root._isWifi ? dev.wifiNetLineStats : []      // v4
        readonly property int ownPort: root._isWifi ? dev.wifiNetOwnPort : -1
        readonly property bool busy: root._isWifi && dev.wifiPortBusy
        readonly property int busyLine: root._isWifi ? dev.wifiPortLine : -1
        // up to what the module's firmware takes: 5 Mbaud from 0.13
        readonly property var bauds: [9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600,
                                      1000000, 1500000, 2000000, 3000000, 4000000, 5000000]
                                     .filter(function(b) { return !root._isWifi || b <= dev.wifiMaxBaud })
        readonly property bool wide: width >= Math.round(660 * AppPalette.scale)

        function portName(i) { return i === 0 ? "X1" : "X2" }
        readonly property bool ownAddrPending: visible && root._isWifi && dev.wifiNetOwnAddr >= 0
                                               && portsOwnField.text.length > 0
                                               && parseInt(portsOwnField.text, 10) !== dev.wifiNetOwnAddr
        function applyOwnAddr() {
            if (dev.wifiNetSetOwnAddr(parseInt(portsOwnField.text, 10)))
                return true
            notifications.warning(qsTr("Module address must be 1…254: 0 is where devices sit, 255 is refused"))
            return false
        }
        // "UDP 14445 · senders" / "TCP → 10.0.0.10:14444" / "line off": the network end of a connector
        function lineSummary(l) {
            if (!l || l.valid !== true)
                return ""
            if (l.mode === 0)
                return qsTr("line off")
            if (l.mode === 2)
                return "TCP → " + l.ip + ":" + l.rport
            var local = l.lport > 0 ? l.lport : l.rport
            var dest = l.dest === 0 ? "→ " + l.ip + ":" + l.rport
                     : l.dest === 1 ? qsTr("senders") : qsTr("broadcast :%1").arg(l.rport)
            return "UDP " + local + " · " + dest
        }
        function lineStateText(s) {
            switch (s) {
            case 3: return qsTr("ready")
            case 2: return qsTr("no peer")
            case 1: return qsTr("no Wi‑Fi")
            }
            return qsTr("off")
        }
        function swapHint() {
            return qsTr("Two UDP lines cannot share a local port: to swap the ports of X1 and X2, first move one line to a free port, then give the other line the freed one.")
        }
        // Inline text for the end of a line-record change (WifiLineChange::Outcome); resp = the module's code
        function outcomeText(outcome, resp, rate) {
            switch (outcome) {
            case 1: return qsTr("The module did not send the line's settings: nothing changed.")
            case 2: return qsTr("The line's settings are not known yet: nothing changed. Read back and try again.")
            case 3:
                if (resp === 3)
                    return rate ? qsTr("The module refused the rate: invalid value.")
                                : qsTr("The module refused these settings: invalid values.") + " " + swapHint()
                if (resp === 7) return qsTr("The module refused: wrong key.")
                if (resp === 8) return qsTr("The module refused: not possible now or in this role.")
                return qsTr("The module refused (code %1).").arg(resp)
            case 4: return qsTr("No answer from the module: the fields show what it reports now.")
            }
            return ""
        }
        function syncOwnAddr() {
            if (root._isWifi && !portsOwnField.hasFocus && dev.wifiNetOwnAddr >= 0)
                portsOwnField.text = String(dev.wifiNetOwnAddr)
        }
        Component.onCompleted: syncOwnAddr()
        Connections {
            target: root._isWifi ? root.dev : null
            ignoreUnknownSignals: true
            function onWifiNetChanged() { portsIsland.syncOwnAddr() }
        }

        function connectedText(c) {
            switch (c) {
            case 0: return qsTr("nothing")
            case 1: return qsTr("unreadable bytes (wrong rate?)")
            case 2: return qsTr("SBP host")
            case 3: return qsTr("SBP device(s)")
            case 4: return qsTr("SBP host and devices")
            case 5: return "MAVLink"
            case 6: return "u-blox"
            case 7: return qsTr("IP bridge (SLIP)")
            case 8: return qsTr("other framed traffic")
            }
            return qsTr("code %1").arg(c)
        }
        function ageText(sec) {
            if (sec === undefined || sec < 0) return qsTr("never")
            if (sec < 60) return qsTr("%1 s ago").arg(sec.toFixed(1))
            if (sec < 3600) return qsTr("%1 min ago").arg(Math.floor(sec / 60))
            return qsTr("%1 h ago").arg(Math.floor(sec / 3600))
        }
        function bpsText(bps) { return bps >= 0 ? wifiIsland.rateText(Math.round(bps)) : "—" }
        function mavTypeText(t) {
            switch (t) {
            case 0: return qsTr("generic")
            case 1: return qsTr("fixed wing")
            case 2: return qsTr("quadrotor")
            case 4: return qsTr("helicopter")
            case 6: return qsTr("ground station")
            case 10: return qsTr("rover")
            case 11: return qsTr("boat")
            case 12: return qsTr("submarine")
            case 13: return qsTr("hexarotor")
            case 14: return qsTr("octorotor")
            case 18: return qsTr("onboard computer")
            case 26: return qsTr("gimbal")
            case 27: return "ADS-B"
            case 30: return qsTr("camera")
            }
            return qsTr("type %1").arg(t)
        }
        function autopilotText(a) {
            switch (a) {
            case 0: return qsTr("generic autopilot")
            case 3: return "ArduPilot"
            case 8: return qsTr("no autopilot")
            case 12: return "PX4"
            }
            return qsTr("autopilot %1").arg(a)
        }
        // Per-protocol frames from / to the port, only the protocols seen.
        function protocolsText(info) {
            var out = []
            var list = info && info.protocols ? info.protocols : []
            for (var i = 0; i < list.length; ++i)
                if (list[i].fromPort > 0 || list[i].toPort > 0)
                    out.push(list[i].name + " " + list[i].fromPort + "/" + list[i].toPort)
            return out.join(" · ")
        }

        Timer {
            interval: 2000
            repeat: true
            running: portsIsland.visible && !portsIsland.folded
            triggeredOnStart: true
            onTriggered: if (dev) dev.wifiNetRefreshPorts()
        }

        DevIslandTitle { label: qsTr("Ports"); foldIsland: portsIsland }
        KIslandRow {
            minHeight: 0
            verticalPadding: Tokens.spaceSm
            labelPixelSize: Tokens.fontMd
            label: portsIsland.ownPort >= 0 ? qsTr("This device is connected through %1").arg(portsIsland.portName(portsIsland.ownPort))
                                            : qsTr("This device reaches the module over the network")
            caption: qsTr("X1 and X2 are equal: the module answers on either, and each port has its own rate.")
        }

        DevStackedRow {
            Grid {
                id: portsGrid
                width: parent.width
                columns: portsIsland.wide ? 2 : 1
                spacing: Tokens.spaceMd
                readonly property real cellW: Math.max(0, (width - (columns - 1) * spacing) / columns)

                Repeater {
                    id: portsRepeater
                    model: 2
                    delegate: KIsland {
                        id: card
                        required property int index
                        width: portsGrid.cellW
                        rowPadding: Tokens.spaceMd
                        rowMinHeight: Tokens.controlHMd
                        labelPixelSize: Tokens.fontMd

                        readonly property var info: portsIsland.ports.length > index ? portsIsland.ports[index] : null
                        readonly property bool valid: !!info && info.valid === true
                        readonly property var devs: portsIsland.devices.length > index ? portsIsland.devices[index] : []
                        readonly property var net: portsIsland.peers.length > index ? portsIsland.peers[index] : null
                        readonly property bool netValid: !!net && net.valid === true
                        readonly property bool here: valid && info.here === true
                        readonly property bool changing: portsIsland.busy && portsIsland.busyLine === index
                        readonly property bool rateChanging: changing && dev.wifiPortTarget > 0
                        readonly property bool netChanging: changing && dev.wifiPortTarget === 0
                        readonly property int baudNow: valid ? info.baud : 0
                        property int selBaud: 0
                        property bool dirty: false
                        property string rateError: ""

                        // This port's line record (v3) and counters (v4): the network side, edited here.
                        readonly property var rec: portsIsland.lines.length > index ? portsIsland.lines[index] : null
                        readonly property bool recValid: !!rec && rec.valid === true
                        readonly property var otherRec: portsIsland.lines.length > 1 - index ? portsIsland.lines[1 - index] : null
                        readonly property var st: portsIsland.stats.length > index ? portsIsland.stats[index] : null
                        readonly property bool stValid: !!st && st.valid === true
                        readonly property bool netEditing: cardIpField.hasFocus || cardRportField.hasFocus || cardLportField.hasFocus
                                                           || cardTxField.hasFocus || cardRxField.hasFocus
                        property int selMode: 0
                        property int selDest: 1
                        property var synced: null      // the record the fields were filled from
                        property bool netDirty: false
                        property string netError: ""
                        // the island hidden (another device on this page): nothing of an earlier module waits here
                        readonly property bool netPending: portsIsland.visible && recValid && netDirty
                        readonly property bool ratePending: portsIsland.visible && valid && dirty && selBaud > 0 && selBaud !== baudNow

                        // The fields follow the module's record until the operator edits them.
                        function syncNet(force) {
                            // rec itself, not recValid: the binding may still say valid while rec has just gone null
                            // (TypeError at the head unit, 2026-09-28)
                            if (!rec || rec.valid !== true || netChanging || ((netEditing || netDirty) && !force))
                                return
                            selMode = rec.mode
                            selDest = rec.dest
                            cardIpField.text = rec.ip
                            cardRportField.text = String(rec.rport)
                            cardLportField.text = String(rec.lport)
                            cardTxField.text = String(rec.txPin)
                            cardRxField.text = String(rec.rxPin)
                            synced = { mode: rec.mode, dest: rec.dest, ip: rec.ip, rport: rec.rport, lport: rec.lport,
                                       txPin: rec.txPin, rxPin: rec.rxPin }
                            netDirty = false
                        }
                        function markNetDirty() { netDirty = true; netError = "" }
                        // Only what the operator changed: the driver takes the rest from the record it reads just before sending.
                        function netEdits() {
                            var e = {}
                            if (!synced)
                                return e
                            if (selMode !== synced.mode) e.mode = selMode
                            if (selDest !== synced.dest) e.dest = selDest
                            var ip = cardIpField.text.trim()
                            if (ip.length && ip !== synced.ip) e.ip = ip
                            var n = parseInt(cardRportField.text, 10)
                            if (!isNaN(n) && n !== synced.rport) e.rport = n
                            n = parseInt(cardLportField.text, 10)
                            if (!isNaN(n) && n !== synced.lport) e.lport = n
                            if (index === 1) {
                                n = parseInt(cardTxField.text, 10)
                                if (!isNaN(n) && n !== synced.txPin) e.txPin = n
                                n = parseInt(cardRxField.text, 10)
                                if (!isNaN(n) && n !== synced.rxPin) e.rxPin = n
                            }
                            return e
                        }
                        // The other port's line listening on the same local UDP port: the local port, 0 when none.
                        function localPortClash(e) {
                            var mode = e.mode !== undefined ? e.mode : synced.mode
                            var o = otherRec
                            if (mode !== 1 || !o || o.valid !== true || o.mode !== 1)
                                return 0
                            var rport = e.rport !== undefined ? e.rport : synced.rport
                            var lport = e.lport !== undefined ? e.lport : synced.lport
                            var mine = lport > 0 ? lport : rport
                            var theirs = o.lport > 0 ? o.lport : o.rport
                            return mine === theirs ? mine : 0
                        }
                        function applyNet() {
                            var e = netEdits()
                            if (Object.keys(e).length === 0) {
                                netDirty = false
                                return true
                            }
                            var clash = localPortClash(e)
                            if (clash > 0) {
                                netError = qsTr("%1 already listens on UDP %2.").arg(portsIsland.portName(1 - index)).arg(clash)
                                           + " " + portsIsland.swapHint()
                                return false
                            }
                            if (!dev.wifiSetLineConfig(index, e)) {
                                netError = qsTr("Not sent: check the fields (IP a.b.c.d, ports 0–65535, pins −1…127), or wait for the running change.")
                                return false
                            }
                            netError = ""
                            return true
                        }
                        function applyRate() {
                            if (!dev.wifiSetLineBaud(index, selBaud)) {
                                rateError = qsTr("The rate of %1 could not be changed now: nothing changed").arg(portsIsland.portName(index))
                                return false
                            }
                            rateError = ""
                            dirty = false
                            return true
                        }
                        onRecChanged: syncNet(false)
                        onNetChangingChanged: if (!netChanging && !netDirty) syncNet(false)

                        Connections {
                            target: root._isWifi ? root.dev : null
                            ignoreUnknownSignals: true
                            function onWifiLineChangeFinished(line, outcome, resp, rate) {
                                if (line !== card.index)
                                    return
                                var text = outcome === 0 ? "" : portsIsland.outcomeText(outcome, resp, rate)
                                if (rate) {
                                    card.rateError = text
                                } else {
                                    card.netError = text
                                    // done, or no answer (the read-back shows what the module has): refill the fields;
                                    // a refusal keeps the edits so they can be corrected
                                    if (outcome === 0 || outcome === 4) {
                                        card.netDirty = false
                                        card.syncNet(true)
                                    }
                                }
                            }
                        }

                        fillColor: here ? Qt.tint(AppPalette.card, Qt.rgba(AppPalette.accent.r, AppPalette.accent.g, AppPalette.accent.b, 0.10))
                                        : AppPalette.rowRaised
                        borderColor: here ? AppPalette.accentBorder : AppPalette.border

                        // The selector follows the port's rate until the operator picks one; the pick stays until
                        // Apply, "Apply and save" under the page, or Read back (owner 2026-09-29: no going back after
                        // 30 s).
                        function syncBaud(force) {
                            if (!valid || changing || (dirty && !force))
                                return
                            selBaud = info.baud
                            baudCombo.currentIndex = portsIsland.bauds.indexOf(selBaud)
                            dirty = false
                        }
                        onBaudNowChanged: syncBaud(false)
                        onValidChanged: syncBaud(false)
                        // the end of another change of this port (its line) must not drop a rate picked, not applied
                        onChangingChanged: if (!changing && !dirty) syncBaud(true)
                        Component.onCompleted: { syncBaud(true); syncNet(true) }

                        // "X2 · UDP 14445 · senders": which network port this connector maps to, at a glance
                        KIslandRow {
                            label: portsIsland.portName(card.index) + (card.recValid ? " · " + portsIsland.lineSummary(card.rec) : "")
                            labelBold: true
                            labelPixelSize: Tokens.fontLg
                        }
                        KIslandRow {
                            readonly property bool anyBadge: card.here || !card.valid
                                                             || (card.valid && (card.info.provisional === true || card.info.usb === true
                                                                                || card.info.slip === true || card.info.uartRunning !== true))
                            open: anyBadge
                            stacked: true
                            minHeight: 0
                            verticalPadding: Tokens.spaceXs
                            showSeparator: false
                            Flow {
                                width: parent.width
                                spacing: Tokens.spaceXs
                                WifiBadge { visible: card.here; tone: 1; text: qsTr("this device") }
                                WifiBadge { visible: card.valid && card.info.provisional === true; tone: 2; text: qsTr("provisional") }
                                WifiBadge { visible: card.valid && card.info.usb === true; text: "USB" }
                                WifiBadge { visible: card.valid && card.info.slip === true; text: "SLIP" }
                                WifiBadge { visible: card.valid && card.info.uartRunning !== true; tone: 3; text: qsTr("UART off") }
                                WifiBadge { visible: !card.valid; text: qsTr("no data") }
                            }
                        }
                        DevInfoRow {
                            label: qsTr("Rate")
                            value: !card.valid ? "" : (card.info.baud > 0 ? qsTr("%1 bit/s").arg(card.info.baud) : qsTr("UART off"))
                            caption: !card.valid ? ""
                                   : card.info.provisional === true
                                     ? qsTr("Provisional: kept once a request for the module comes through this port, otherwise back to %1 in 10 s").arg(card.info.baudSaved)
                                   : card.info.baudSaved !== card.info.baud ? qsTr("Saved: %1 (in effect after a reboot)").arg(card.info.baudSaved)
                                   : qsTr("Saved")
                            captionColor: (card.valid && card.info.provisional === true) ? AppPalette.linkIdleText : AppPalette.textSecond
                        }
                        DevInfoRow {
                            label: qsTr("Connected")
                            value: card.valid ? portsIsland.connectedText(card.info.connected) : ""
                            caption: !card.valid ? ""
                                   : card.info.reports === true ? qsTr("Gets the module's reports") : qsTr("Gets no reports from the module")
                            captionColor: (card.valid && card.info.connected === 1) ? AppPalette.dangerText : AppPalette.textSecond
                        }
                        DevInfoRow {
                            label: qsTr("Traffic")
                            value: card.valid ? qsTr("RX %1 · TX %2").arg(portsIsland.bpsText(card.info.rxBps)).arg(portsIsland.bpsText(card.info.txBps)) : ""
                            caption: card.valid
                                     ? qsTr("%1 received, %2 sent · last RX %3").arg(wifiIsland.sizeText(card.info.rxBytes))
                                       .arg(wifiIsland.sizeText(card.info.txBytes)).arg(portsIsland.ageText(card.info.rxAge))
                                     : ""
                        }
                        KIslandRow {
                            open: card.valid && portsIsland.protocolsText(card.info).length > 0
                            label: qsTr("Frames RX/TX")
                            caption: portsIsland.protocolsText(card.info)
                        }
                        KIslandRow {
                            open: card.valid
                            label: "SBP"
                            caption: !card.valid ? ""
                                   : qsTr("From the port: %1 requests, %2 content · to it: %3 requests, %4 content")
                                     .arg(card.info.sbpRequestsFrom).arg(card.info.sbpContentFrom)
                                     .arg(card.info.sbpRequestsTo).arg(card.info.sbpContentTo)
                                     + " · " + qsTr("for the module %1, its own frames %2").arg(card.info.moduleRequests).arg(card.info.moduleFrames)
                        }
                        DevInfoRow {
                            label: qsTr("Losses")
                            value: card.valid && (card.info.rxOverflows > 0 || card.info.txDrops > 0)
                                   ? qsTr("%1 RX overflows · %2 TX drops").arg(card.info.rxOverflows).arg(card.info.txDrops) : ""
                            captionColor: AppPalette.dangerText
                        }

                        // Devices the module heard from this port (page 1), most recent first
                        KIslandSection { label: qsTr("Devices on this port") }
                        Repeater {
                            model: card.devs
                            delegate: KIslandRow {
                                required property var modelData
                                label: modelData.kind === 2
                                       ? qsTr("MAVLink %1/%2 · %3").arg(modelData.sysid).arg(modelData.compid)
                                         .arg(modelData.heartbeat ? portsIsland.mavTypeText(modelData.mavType) : qsTr("no heartbeat yet"))
                                       : qsTr("SBP %1").arg(modelData.addr)
                                         + (modelData.name.length ? " · " + modelData.name
                                            : (modelData.board > 0 ? " · " + qsTr("board %1").arg(modelData.board) : ""))
                                caption: (modelData.kind === 2
                                          ? (modelData.heartbeat ? portsIsland.autopilotText(modelData.autopilot) + " · " : "")
                                            + qsTr("%1 frames").arg(modelData.frames) + " · "
                                          : (modelData.fw.length ? qsTr("fw %1").arg(modelData.fw) + " · " : "")
                                            + (modelData.serial > 0 ? qsTr("SN %1").arg(modelData.serial) + " · " : ""))
                                         + qsTr("heard %1").arg(portsIsland.ageText(modelData.age))
                            }
                        }
                        KIslandRow {
                            open: card.devs.length === 0
                            minHeight: 0
                            verticalPadding: Tokens.spaceSm
                            label: qsTr("None heard")
                            labelPixelSize: Tokens.fontSm
                            labelColor: AppPalette.textMuted
                        }

                        // The network side of this port's line: its state and peers (page 2), counters (v4) and settings (v3)
                        KIslandSection { label: qsTr("Network side (line %1)").arg(card.index) }
                        DevInfoRow {
                            label: qsTr("State")
                            value: !card.netValid ? ""
                                   : (card.net.mode === 1 ? "UDP" : card.net.mode === 2 ? "TCP" : qsTr("off"))
                                     + (card.net.mode !== 0 ? " · " + portsIsland.lineStateText(card.net.state) : "")
                        }
                        Repeater {
                            model: card.netValid ? card.net.peers : []
                            delegate: KIslandRow {
                                required property var modelData
                                label: modelData.ip + ":" + modelData.port
                                labelPixelSize: Tokens.fontSm
                                caption: qsTr("last data %1").arg(portsIsland.ageText(modelData.age))
                            }
                        }
                        DevInfoRow {
                            label: qsTr("To the network")
                            value: card.stValid && card.recValid && card.rec.mode !== 0
                                   ? qsTr("%1 frames · %2 packets · %3 dropped").arg(card.st.upFrames).arg(card.st.upPackets).arg(card.st.upDrops) : ""
                        }
                        DevInfoRow {
                            label: qsTr("From the network")
                            value: card.stValid && card.recValid && card.rec.mode !== 0
                                   ? qsTr("%1 packets · %2 frames · %3 dropped").arg(card.st.downPackets).arg(card.st.downFrames).arg(card.st.downDrops) : ""
                        }
                        KIslandRow {
                            open: card.recValid
                            label: qsTr("Mode")
                            KTabBar {
                                width: Math.round(170 * AppPalette.scale)
                                fontPixelSize: Tokens.fontSm
                                options: [ { label: qsTr("Off"), value: 0 }, { label: "UDP", value: 1 }, { label: "TCP", value: 2 } ]
                                currentValue: card.selMode
                                onValueSelected: function(v) { card.selMode = v; card.markNetDirty() }
                            }
                        }
                        DevStackedRow {
                            open: card.recValid && card.selMode === 1
                            label: qsTr("Send to")
                            caption: card.selDest === 0 ? qsTr("The peer below")
                                   : card.selDest === 1 ? qsTr("Whoever sent to this UDP port (up to 4, forgotten after 60 s of silence)")
                                                        : qsTr("Broadcast in the subnet to the peer port")
                            KTabBar {
                                width: parent.width
                                fontPixelSize: Tokens.fontSm
                                options: [ { label: qsTr("Fixed"), value: 0 }, { label: qsTr("Senders"), value: 1 }, { label: qsTr("Broadcast"), value: 2 } ]
                                currentValue: card.selDest
                                onValueSelected: function(v) { card.selDest = v; card.markNetDirty() }
                            }
                        }
                        DevStackedRow {
                            open: card.recValid && (card.selMode === 2 || (card.selMode === 1 && card.selDest === 0))
                            label: qsTr("Peer IP address")
                            WifiField { id: cardIpField; width: parent.width; placeholder: "10.0.0.10"; onEdited: card.markNetDirty() }
                        }
                        KIslandRow {
                            open: card.recValid && card.selMode !== 0
                            label: qsTr("Peer port")
                            WifiField { id: cardRportField; width: Math.round(100 * AppPalette.scale); numeric: true; onEdited: card.markNetDirty() }
                        }
                        KIslandRow {
                            open: card.recValid && card.selMode === 1
                            label: qsTr("Own UDP port")
                            caption: qsTr("0 = the peer port. Each UDP line needs a port of its own: to swap the ports of X1 and X2, move one line to a free port first.")
                            WifiField { id: cardLportField; width: Math.round(100 * AppPalette.scale); numeric: true; onEdited: card.markNetDirty() }
                        }
                        KIslandRow {
                            open: card.recValid && card.index === 1     // line 0's pins are fixed by the module's build
                            label: qsTr("TX / RX pins")
                            caption: qsTr("Allowed 0, 1, 3, 4, 5, 6, 7, 10; −1 switches the UART off; new pins start after a reboot")
                            Row {
                                spacing: Tokens.spaceSm
                                WifiField { id: cardTxField; width: Math.round(56 * AppPalette.scale); numeric: true; minValue: -1; maxValue: 21; onEdited: card.markNetDirty() }
                                WifiField { id: cardRxField; width: Math.round(56 * AppPalette.scale); numeric: true; minValue: -1; maxValue: 21; onEdited: card.markNetDirty() }
                            }
                        }
                        DevStackedRow {
                            open: card.recValid
                            caption: card.netChanging ? qsTr("Applying… (it finishes even if this page is closed)")
                                   : card.here ? qsTr("This device's port: switching its line off ends the network (boat) link through it; the module keeps answering here.")
                                               : qsTr("Only the fields changed here are sent; the module keeps the rest as it has them.")
                            Row {
                                width: parent.width
                                spacing: Tokens.spaceSm
                                readonly property real bw: (width - Tokens.spaceSm) / 2
                                ConfirmDevButton {
                                    width: parent.bw
                                    label: qsTr("Apply")
                                    enabled: card.recValid && card.netDirty && !portsIsland.busy
                                    onConfirmed: card.applyNet()
                                }
                                DevButton {
                                    width: parent.bw; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                                    text: qsTr("Read back")
                                    onClicked: {
                                        card.netDirty = false
                                        card.netError = ""
                                        card.dirty = false      // the rate picked here goes too
                                        dev.wifiNetRefresh()
                                        card.syncNet(true)
                                        card.syncBaud(true)
                                    }
                                }
                            }
                        }
                        KIslandRow {
                            open: card.netError.length > 0
                            minHeight: 0
                            verticalPadding: Tokens.spaceSm
                            showSeparator: false
                            caption: card.netError
                            captionColor: AppPalette.dangerText
                        }

                        // Rate of this port
                        DevStackedRow {
                            label: qsTr("Set the rate")
                            caption: (card.rateChanging ? qsTr("Changing to %1… (it finishes even if this page is closed)").arg(dev.wifiPortTarget)
                                      : card.here && dev.wifiRateSavedAtOnce
                                        ? qsTr("This device's port: the module keeps the new rate at once (firmware 0.15) and the link here follows. If this device cannot do the rate, the link is lost until the module's BOOT button is held 5 s (both ports back to 921600).")
                                      : card.here ? qsTr("This device's port: the link here follows, the module has to answer at the new rate, then it is saved.")
                                                  : qsTr("Not this device's port: saved in the module at once."))
                                     + (card.selBaud !== 5000000 || card.rateChanging ? ""
                                        : card.here && dev.wifiRateSavedAtOnce
                                          ? qsTr(" 5M: both ends must do it (the head unit's UART6 was tried up to 4M only; a USB adapter needs 5M in its data sheet).")
                                        : card.here ? qsTr(" 5M: both ends must do it (the head unit's UART6 was tried up to 4M; a USB adapter needs 5M in its data sheet). Without an answer at 5M the link goes back by itself.")
                                                    : qsTr(" 5M: the device on this port must do it too. The module keeps the rate: set it back here if that device stops answering."))
                            Row {
                                width: parent.width
                                spacing: Tokens.spaceSm
                                readonly property real applyW: Math.round(96 * AppPalette.scale)
                                KCombo {
                                    id: baudCombo
                                    width: parent.width - parent.applyW - Tokens.spaceSm
                                    height: Tokens.controlHMd
                                    fontPixelSize: Tokens.fontMd
                                    model: portsIsland.bauds
                                    currentIndex: -1
                                    displayTextOverride: currentIndex < 0 && card.selBaud > 0 ? String(card.selBaud) : ""
                                    onActivated: function(idx) {
                                        card.selBaud = portsIsland.bauds[idx]
                                        card.dirty = true
                                    }
                                }
                                ConfirmDevButton {
                                    width: parent.applyW
                                    label: qsTr("Apply")
                                    enabled: card.valid && !portsIsland.busy && card.selBaud > 0 && card.selBaud !== card.baudNow
                                    onConfirmed: card.applyRate()
                                }
                            }
                        }
                        KIslandRow {
                            open: card.rateError.length > 0
                            minHeight: 0
                            verticalPadding: Tokens.spaceSm
                            showSeparator: false
                            caption: card.rateError
                            captionColor: AppPalette.dangerText
                        }
                    }
                }
            }
        }

        // The module's own SBP address: one for both ports and the network (Lines island before 0.12)
        KIslandRow {
            label: qsTr("Module address")
            caption: qsTr("Must differ from the devices behind the ports (a sonar is usually 0); 1…254. The module then reappears as a new device.")
            Row {
                spacing: Tokens.spaceSm
                WifiField { id: portsOwnField; width: Math.round(64 * AppPalette.scale); numeric: true; minValue: 1; maxValue: 254 }
                ConfirmDevButton {
                    width: Math.round(96 * AppPalette.scale)
                    label: qsTr("Set")
                    enabled: portsOwnField.text.length > 0
                    onConfirmed: portsIsland.applyOwnAddr()
                }
            }
        }
    }

    // ── Wi‑Fi module: role and access point (firmware 0.10, ID_WIFI_NET v0, v1, v5) ──
    DevIsland {
        id: apIsland
        foldKey: "wifi.role"
        width: root.groupWidth
        visible: root._isWifi && dev.wifiNetSupport === 1
        rowMinHeight: Tokens.controlHMd + Tokens.spaceXs

        readonly property var role: root._isWifi ? dev.wifiNetRole : ({})
        readonly property var ap: root._isWifi ? dev.wifiNetAp : ({})
        readonly property var clients: root._isWifi ? dev.wifiNetClients : []
        readonly property bool roleValid: !!role && role.valid === true
        readonly property bool apValid: !!ap && ap.valid === true
        readonly property bool apRunning: roleValid && role.running === 1
        // The access point's settings only while the module runs as one (or its role is not known yet): in the
        // station role they are not shown; the role selector stays, so the switch to AP remains possible.
        readonly property bool showAp: root._wifiRole !== 0
        readonly property bool editing: apSsidField.hasFocus || apPassField.hasFocus || apChannelField.hasFocus
                                        || apClientsField.hasFocus
        property int selRole: 0
        property int selAuth: 1
        property bool dirty: false
        property bool apDirty: false      // the access point's fields were edited (dirty covers the role too)
        readonly property bool rolePending: visible && roleValid && selRole !== role.saved
        readonly property bool apPending: visible && showAp && apValid && apDirty
        function markAp() {
            dirty = true
            apDirty = true
        }
        // "Save for next boot": the role takes effect after a reboot; edits of the access point stay.
        function saveRole() {
            dev.wifiNetSetRole(selRole, false, false)
            dirty = apDirty
            notifications.info(qsTr("Role saved: %1 after the next reboot").arg(roleText(selRole)))
            return true
        }
        function applyAp() {
            var ok = dev.wifiNetSetAp(parseInt(apChannelField.text, 10), apHiddenSwitch.checked,
                                      parseInt(apClientsField.text, 10), selAuth, apSsidField.text, apPassField.text)
            if (!ok) {
                notifications.warning(qsTr("Check the fields: name 1–32 bytes, password 8–63 characters (none for an open network), channel 1–11, clients 1–10"))
                return false
            }
            dirty = false
            apDirty = false
            apPassField.text = ""
            return true
        }

        function roleText(r) { return r === 1 ? qsTr("access point") : qsTr("station") }
        function sync(force) {
            if (roleValid && (!dirty || force))
                selRole = role.saved
            if (!apValid || ((editing || dirty) && !force))
                return
            apSsidField.text = ap.ssid
            apChannelField.text = String(ap.channel)
            apClientsField.text = String(ap.maxClients)
            selAuth = ap.auth
            apHiddenSwitch.checked = ap.hidden
            apPassField.text = ""
            dirty = false
            apDirty = false
        }
        onVisibleChanged: if (visible && dev) dev.wifiNetRefreshClients()
        Component.onCompleted: sync(true)
        Connections {
            target: root._isWifi ? root.dev : null
            ignoreUnknownSignals: true
            function onWifiNetChanged() { apIsland.sync(false) }
        }

        DevIslandTitle { label: apIsland.showAp ? qsTr("Role and access point") : qsTr("Role"); foldIsland: apIsland }

        KIslandRow {
            label: qsTr("Role")
            caption: apIsland.roleValid ? qsTr("Running: %1").arg(apIsland.roleText(apIsland.role.running)) : ""
            KTabBar {
                width: Math.round(220 * AppPalette.scale)
                fontPixelSize: Tokens.fontSm
                options: [ { label: qsTr("Station"), value: 0 }, { label: qsTr("Access point"), value: 1 } ]
                currentValue: apIsland.selRole
                onValueSelected: function(v) { apIsland.selRole = v; apIsland.dirty = true }
            }
        }
        KIslandRow {
            open: apIsland.roleValid && apIsland.role.saved !== apIsland.role.running
            minHeight: 0
            verticalPadding: Tokens.spaceSm
            label: qsTr("Saved role: %1 — takes effect after a reboot").arg(apIsland.roleText(apIsland.role.saved))
            labelPixelSize: Tokens.fontSm
            labelColor: AppPalette.linkIdleText
        }
        DevStackedRow {
            caption: apIsland.selRole === 1
                     ? (root._wifiPorts
                        ? qsTr("As an access point the module stops joining the boat's network, so this head unit loses the boat link until the role is switched back. It then answers at address 88 unless an own address is set.")
                        : qsTr("As an access point the module stops joining the boat's network, so this head unit loses the boat link until the role is switched back. With a relaying line it answers at its relay address (88 by default)."))
                     : qsTr("As a station the module joins saved networks, as before.")
            Row {
                width: parent.width
                spacing: Tokens.spaceMd
                readonly property real bw: (width - Tokens.spaceMd) / 2
                ConfirmDevButton {
                    width: parent.bw
                    label: qsTr("Apply and reboot")
                    enabled: apIsland.roleValid
                    onConfirmed: {
                        dev.wifiNetSetRole(apIsland.selRole, true, false)
                        apIsland.dirty = false
                        notifications.info(qsTr("Role sent; the module restarts"))
                    }
                }
                DevButton {
                    width: parent.bw; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                    text: qsTr("Save for next boot")
                    enabled: apIsland.roleValid
                    onClicked: apIsland.saveRole()
                }
            }
        }

        KIslandSection { open: apIsland.showAp; label: qsTr("Access point network") }
        DevStackedRow {
            open: apIsland.showAp
            label: qsTr("Network name (SSID)")
            WifiField { id: apSsidField; width: parent.width; placeholder: "Kogger-XXXX"; onEdited: apIsland.markAp() }
        }
        DevStackedRow {
            open: apIsland.showAp
            label: qsTr("Password")
            caption: qsTr("8–63 characters. Leave it empty to keep the current password.")
            WifiField { id: apPassField; width: parent.width; password: true; placeholder: qsTr("unchanged"); onEdited: apIsland.markAp() }
        }
        DevStackedRow {
            open: apIsland.showAp
            label: qsTr("Security")
            KTabBar {
                width: parent.width
                fontPixelSize: Tokens.fontSm
                options: [ { label: qsTr("Open"), value: 0 }, { label: "WPA2", value: 1 }, { label: "WPA2/WPA3", value: 2 } ]
                currentValue: apIsland.selAuth
                onValueSelected: function(v) { apIsland.selAuth = v; apIsland.markAp() }
            }
        }
        KIslandRow {
            open: apIsland.showAp
            label: qsTr("Channel (1–11)")
            WifiField { id: apChannelField; width: Math.round(90 * AppPalette.scale); numeric: true; minValue: 1; maxValue: 11; onEdited: apIsland.markAp() }
        }
        KIslandRow {
            open: apIsland.showAp
            label: qsTr("Clients at most (1–10)")
            WifiField { id: apClientsField; width: Math.round(90 * AppPalette.scale); numeric: true; minValue: 1; maxValue: 10; onEdited: apIsland.markAp() }
        }
        KIslandRow {
            open: apIsland.showAp
            label: qsTr("Hidden network")
            KSwitch { id: apHiddenSwitch; flat: true; onToggled: apIsland.markAp() }
        }
        DevStackedRow {
            open: apIsland.showAp
            caption: apIsland.apRunning
                     ? qsTr("The access point restarts with the new settings; its clients reconnect.")
                     : qsTr("Stored now, used once the module runs as an access point.")
            Row {
                width: parent.width
                spacing: Tokens.spaceMd
                readonly property real bw: (width - Tokens.spaceMd) / 2
                ConfirmDevButton {
                    width: parent.bw
                    label: qsTr("Apply")
                    enabled: apIsland.apValid
                    onConfirmed: apIsland.applyAp()
                }
                DevButton {
                    width: parent.bw; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                    text: qsTr("Read back")
                    onClicked: { apIsland.dirty = false; dev.wifiNetRefresh(); apIsland.sync(true) }
                }
            }
        }

        KIslandSection {
            open: apIsland.apRunning
            label: qsTr("Clients: %1").arg(apIsland.clients.length)
        }
        Repeater {
            model: apIsland.apRunning ? apIsland.clients : []
            delegate: DevInfoRow {
                required property var modelData
                label: modelData.mac
                value: modelData.ip + " · " + modelData.rssi + " " + qsTr("dBm")
            }
        }
        DevStackedRow {
            open: apIsland.apRunning
            DevButton {
                width: parent.width; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                text: qsTr("Refresh clients")
                onClicked: dev.wifiNetRefreshClients()
            }
        }
    }

    // ── Wi‑Fi module: address and DHCP of the access point (ID_WIFI_NET v2) ──
    DevIsland {
        id: ipIsland
        foldKey: "wifi.ip"
        width: root.groupWidth
        // the access point's address and DHCP: not shown while the module runs as a station
        visible: root._isWifi && dev.wifiNetSupport === 1 && root._wifiRole !== 0
        rowMinHeight: Tokens.controlHMd + Tokens.spaceXs

        readonly property var c: root._isWifi ? dev.wifiNetIp : ({})
        readonly property bool valid: !!c && c.valid === true
        readonly property bool editing: ipAddrField.hasFocus || ipMaskField.hasFocus || ipPoolStartField.hasFocus
                                        || ipPoolEndField.hasFocus || ipLeaseField.hasFocus
        property bool dirty: false
        readonly property bool pendingApply: visible && valid && dirty
        function apply() {
            var ok = dev.wifiNetSetIp(ipAddrField.text, ipMaskField.text, ipDhcpSwitch.checked,
                                      ipPoolStartField.text, ipPoolEndField.text,
                                      parseInt(ipLeaseField.text, 10), ipGwSwitch.checked, ipDnsSwitch.checked)
            if (!ok) {
                notifications.warning(qsTr("Addresses must be four numbers a.b.c.d, the lease 1–2880 minutes"))
                return false
            }
            dirty = false
            return true
        }

        function sync(force) {
            if (!valid || ((editing || dirty) && !force))
                return
            ipAddrField.text = c.ip
            ipMaskField.text = c.mask
            ipDhcpSwitch.checked = c.dhcp
            ipPoolStartField.text = c.poolStart
            ipPoolEndField.text = c.poolEnd
            ipLeaseField.text = String(c.leaseMin)
            ipGwSwitch.checked = c.offerGateway
            ipDnsSwitch.checked = c.offerDns
            dirty = false
        }
        Component.onCompleted: sync(true)
        Connections {
            target: root._isWifi ? root.dev : null
            ignoreUnknownSignals: true
            function onWifiNetChanged() { ipIsland.sync(false) }
        }

        DevIslandTitle { label: qsTr("Access point address and DHCP"); foldIsland: ipIsland }
        DevStackedRow {
            label: qsTr("Module address")
            WifiField { id: ipAddrField; width: parent.width; placeholder: "10.0.0.10"; onEdited: ipIsland.dirty = true }
        }
        DevStackedRow {
            label: qsTr("Mask")
            WifiField { id: ipMaskField; width: parent.width; placeholder: "255.255.255.0"; onEdited: ipIsland.dirty = true }
        }
        KIslandRow {
            label: qsTr("DHCP server")
            KSwitch { id: ipDhcpSwitch; flat: true; onToggled: ipIsland.dirty = true }
        }
        DevStackedRow {
            open: ipDhcpSwitch.checked
            label: qsTr("Address pool")
            caption: qsTr("Inside the subnet, without the module's own address, at most 100 addresses")
            Row {
                width: parent.width
                spacing: Tokens.spaceMd
                readonly property real fw: (width - Tokens.spaceMd) / 2
                WifiField { id: ipPoolStartField; width: parent.fw; placeholder: "10.0.0.11"; onEdited: ipIsland.dirty = true }
                WifiField { id: ipPoolEndField; width: parent.fw; placeholder: "10.0.0.30"; onEdited: ipIsland.dirty = true }
            }
        }
        KIslandRow {
            open: ipDhcpSwitch.checked
            label: qsTr("Lease, minutes (1–2880)")
            WifiField { id: ipLeaseField; width: Math.round(90 * AppPalette.scale); numeric: true; minValue: 1; maxValue: 2880; onEdited: ipIsland.dirty = true }
        }
        KIslandRow {
            open: ipDhcpSwitch.checked
            label: qsTr("Offer a gateway")
            caption: qsTr("Off by default: the module has no internet, a phone would route all traffic to it")
            KSwitch { id: ipGwSwitch; flat: true; onToggled: ipIsland.dirty = true }
        }
        KIslandRow {
            open: ipDhcpSwitch.checked
            label: qsTr("Offer DNS")
            KSwitch { id: ipDnsSwitch; flat: true; onToggled: ipIsland.dirty = true }
        }
        DevStackedRow {
            caption: qsTr("Used by the access point. In that role a change applies at once and every client takes a new lease.")
            Row {
                width: parent.width
                spacing: Tokens.spaceMd
                readonly property real bw: (width - Tokens.spaceMd) / 2
                ConfirmDevButton {
                    width: parent.bw
                    label: qsTr("Apply")
                    enabled: ipIsland.valid
                    onConfirmed: ipIsland.apply()
                }
                DevButton {
                    width: parent.bw; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                    text: qsTr("Read back")
                    onClicked: { ipIsland.dirty = false; dev.wifiNetRefresh(); ipIsland.sync(true) }
                }
            }
        }
    }

    // ── Wi‑Fi module: UART lines and their network ports (ID_WIFI_NET v3, v4, v6) ──
    // Firmware 0.10/0.11 only: with 0.12 (ID_WIFI_NET v7) the Ports island carries the lines in its X1/X2 cards.
    // Line 0 is this very port (X1): frames not addressed to the module go to the network and back.
    // Line 1 is the second connector (X2). Each maps to its own UDP port or TCP client. The Connections below
    // also report the answers to every ID_WIFI_NET SETTING of the page, so the island exists while hidden.
    DevIsland {
        id: linesIsland
        foldKey: "wifi.lines"
        width: root.groupWidth
        visible: root._isWifi && dev.wifiNetSupport === 1 && !root._wifiPorts
        rowMinHeight: Tokens.controlHMd + Tokens.spaceXs

        readonly property var lines: root._isWifi ? dev.wifiNetLines : []
        readonly property var stats: root._isWifi ? dev.wifiNetLineStats : []
        property int sel: 0
        readonly property var cur: lines && lines.length > sel ? lines[sel] : null
        readonly property var curStats: stats && stats.length > sel ? stats[sel] : null
        readonly property bool valid: !!cur && cur.valid === true
        readonly property bool editing: lnIpField.hasFocus || lnRportField.hasFocus || lnLportField.hasFocus
                                        || lnBaudField.hasFocus || lnTxField.hasFocus || lnRxField.hasFocus
                                        || lnOwnField.hasFocus
        property int selMode: 0
        property int selDest: 1
        property bool dirty: false
        readonly property bool pendingApply: visible && valid && dirty
        function apply() {
            var ip = lnIpField.text.trim().length ? lnIpField.text.trim() : "0.0.0.0"
            var ok = dev.wifiNetSetLine(sel, selMode, selDest, ip,
                                        parseInt(lnRportField.text, 10) || 0, parseInt(lnLportField.text, 10) || 0,
                                        parseInt(lnBaudField.text, 10) || 0,
                                        isNaN(parseInt(lnTxField.text, 10)) ? -1 : parseInt(lnTxField.text, 10),
                                        isNaN(parseInt(lnRxField.text, 10)) ? -1 : parseInt(lnRxField.text, 10))
            if (!ok) {
                notifications.warning(qsTr("Check the fields: IP a.b.c.d, ports 0–65535, line 1 speed 9600–4000000"))
                return false
            }
            dirty = false
            return true
        }
        readonly property bool ownAddrPending: visible && root._isWifi && dev.wifiNetOwnAddr >= 0
                                               && lnOwnField.text.length > 0
                                               && parseInt(lnOwnField.text, 10) !== dev.wifiNetOwnAddr
        function applyOwnAddr() {
            if (dev.wifiNetSetOwnAddr(parseInt(lnOwnField.text, 10)))
                return true
            notifications.warning(qsTr("Module address must be 1…255: 0 is where devices sit"))
            return false
        }

        function sync(force) {
            if (!valid || ((editing || dirty) && !force))
                return
            selMode = cur.mode
            selDest = cur.dest
            lnIpField.text = cur.ip
            lnRportField.text = String(cur.rport)
            lnLportField.text = String(cur.lport)
            lnBaudField.text = String(cur.baud)
            lnTxField.text = String(cur.txPin)
            lnRxField.text = String(cur.rxPin)
            if (dev.wifiNetOwnAddr >= 0)
                lnOwnField.text = String(dev.wifiNetOwnAddr)
            dirty = false
        }
        function stateText(st) {
            switch (st) {
            case 3: return qsTr("ready")
            case 2: return qsTr("no peer")
            case 1: return qsTr("no Wi‑Fi")
            }
            return qsTr("off")
        }
        onSelChanged: { dirty = false; sync(true) }
        onValidChanged: sync(false)
        Component.onCompleted: sync(true)
        Connections {
            target: root._isWifi ? root.dev : null
            ignoreUnknownSignals: true
            function onWifiNetChanged() { linesIsland.sync(false) }
            function onWifiNetCommandResult(version, resp) {
                if (resp === 1)
                    notifications.info(qsTr("Wi‑Fi module: settings applied"))
                else
                    notifications.warning(qsTr("Wi‑Fi module refused the settings: %1")
                                          .arg(resp === 3 ? qsTr("invalid values")
                                             : resp === 7 ? qsTr("wrong key")
                                             : resp === 8 ? qsTr("not possible now or in this role")
                                             : qsTr("code %1").arg(resp)))
            }
        }
        Timer {
            interval: 2000
            repeat: true
            running: linesIsland.visible && !linesIsland.folded
            triggeredOnStart: true
            onTriggered: if (dev) dev.wifiNetRefreshStats()
        }

        DevIslandTitle { label: qsTr("Lines"); foldIsland: linesIsland }

        KIslandRow {
            label: qsTr("Line")
            KTabBar {
                width: Math.round(250 * AppPalette.scale)
                fontPixelSize: Tokens.fontSm
                options: [ { label: qsTr("0 · X1, this port"), value: 0 }, { label: qsTr("1 · X2"), value: 1 } ]
                currentValue: linesIsland.sel
                onValueSelected: function(v) { linesIsland.sel = v }
            }
        }
        KIslandRow {
            label: qsTr("Mode")
            KTabBar {
                width: Math.round(200 * AppPalette.scale)
                fontPixelSize: Tokens.fontSm
                options: [ { label: qsTr("Off"), value: 0 }, { label: "UDP", value: 1 }, { label: "TCP", value: 2 } ]
                currentValue: linesIsland.selMode
                onValueSelected: function(v) { linesIsland.selMode = v; linesIsland.dirty = true }
            }
        }
        DevStackedRow {
            open: linesIsland.selMode === 1
            label: qsTr("Send to")
            caption: linesIsland.selDest === 0 ? qsTr("The peer below")
                   : linesIsland.selDest === 1 ? qsTr("Whoever sent to this port (up to 4, forgotten after 60 s of silence)")
                                               : qsTr("Broadcast in the subnet to the peer port")
            KTabBar {
                width: parent.width
                fontPixelSize: Tokens.fontSm
                options: [ { label: qsTr("Fixed"), value: 0 }, { label: qsTr("Senders"), value: 1 }, { label: qsTr("Broadcast"), value: 2 } ]
                currentValue: linesIsland.selDest
                onValueSelected: function(v) { linesIsland.selDest = v; linesIsland.dirty = true }
            }
        }
        DevStackedRow {
            open: linesIsland.selMode === 2 || (linesIsland.selMode === 1 && linesIsland.selDest === 0)
            label: qsTr("Peer IP address")
            WifiField { id: lnIpField; width: parent.width; placeholder: "10.0.0.10"; onEdited: linesIsland.dirty = true }
        }
        KIslandRow {
            open: linesIsland.selMode !== 0
            label: qsTr("Peer port")
            WifiField { id: lnRportField; width: Math.round(110 * AppPalette.scale); numeric: true; onEdited: linesIsland.dirty = true }
        }
        KIslandRow {
            open: linesIsland.selMode === 1
            label: qsTr("Own UDP port")
            caption: qsTr("0 = the peer port")
            WifiField { id: lnLportField; width: Math.round(110 * AppPalette.scale); numeric: true; onEdited: linesIsland.dirty = true }
        }
        KIslandRow {
            open: linesIsland.sel === 1
            label: qsTr("Baud rate")
            caption: qsTr("9600–4000000; line 0 uses the device baud rate below")
            WifiField { id: lnBaudField; width: Math.round(130 * AppPalette.scale); numeric: true; minValue: 0; maxValue: 4000000; onEdited: linesIsland.dirty = true }
        }
        KIslandRow {
            open: linesIsland.sel === 1
            label: qsTr("TX / RX pins")
            caption: qsTr("Allowed 0, 1, 3, 4, 5, 6, 7, 10; −1 switches UART1 off; new pins start after a reboot")
            Row {
                spacing: Tokens.spaceSm
                WifiField { id: lnTxField; width: Math.round(64 * AppPalette.scale); numeric: true; minValue: -1; maxValue: 21; onEdited: linesIsland.dirty = true }
                WifiField { id: lnRxField; width: Math.round(64 * AppPalette.scale); numeric: true; minValue: -1; maxValue: 21; onEdited: linesIsland.dirty = true }
            }
        }
        DevStackedRow {
            caption: linesIsland.sel === 0
                     ? qsTr("Line 0 is this port: switching it off ends the boat link, and the module goes back to address 0.")
                     : ""
            Row {
                width: parent.width
                spacing: Tokens.spaceMd
                readonly property real bw: (width - Tokens.spaceMd) / 2
                ConfirmDevButton {
                    width: parent.bw
                    label: qsTr("Apply")
                    enabled: linesIsland.valid
                    onConfirmed: linesIsland.apply()
                }
                DevButton {
                    width: parent.bw; height: Tokens.controlHMd; fontPixelSize: Tokens.fontMd
                    text: qsTr("Read back")
                    onClicked: { linesIsland.dirty = false; dev.wifiNetRefresh(); linesIsland.sync(true) }
                }
            }
        }

        KIslandSection { label: qsTr("Line %1 counters").arg(linesIsland.sel) }
        DevInfoRow {
            label: qsTr("State")
            value: linesIsland.curStats && linesIsland.curStats.valid
                   ? linesIsland.stateText(linesIsland.curStats.state) + (linesIsland.curStats.state >= 2 ? " · " + linesIsland.curStats.peer : "")
                     + (linesIsland.curStats.senders > 0 ? " · " + qsTr("senders: %1").arg(linesIsland.curStats.senders) : "")
                   : ""
        }
        DevInfoRow {
            label: qsTr("To the network")
            value: linesIsland.curStats && linesIsland.curStats.valid
                   ? qsTr("%1 frames · %2 packets · %3 dropped").arg(linesIsland.curStats.upFrames).arg(linesIsland.curStats.upPackets).arg(linesIsland.curStats.upDrops) : ""
        }
        DevInfoRow {
            label: qsTr("From the network")
            value: linesIsland.curStats && linesIsland.curStats.valid
                   ? qsTr("%1 packets · %2 frames · %3 dropped").arg(linesIsland.curStats.downPackets).arg(linesIsland.curStats.downFrames).arg(linesIsland.curStats.downDrops) : ""
        }
        DevInfoRow {
            label: qsTr("UART")
            value: linesIsland.curStats && linesIsland.curStats.valid
                   ? qsTr("%1 receive overflows · %2 transmit drops · %3 frames for the module")
                     .arg(linesIsland.curStats.uartOverflows).arg(linesIsland.curStats.uartDrops).arg(linesIsland.curStats.localFrames) : ""
        }

        KIslandRow {
            label: qsTr("Module address while relaying")
            caption: qsTr("Must differ from the devices behind the lines (a sonar is usually 0). The module then reappears as a new device.")
            Row {
                spacing: Tokens.spaceSm
                WifiField { id: lnOwnField; width: Math.round(64 * AppPalette.scale); numeric: true; minValue: 1; maxValue: 255 }
                ConfirmDevButton {
                    width: Math.round(96 * AppPalette.scale)
                    label: qsTr("Set")
                    enabled: lnOwnField.text.length > 0
                    onConfirmed: linesIsland.applyOwnAddr()
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
        readonly property bool pendingApply: visible && !!dev && dev.netValid
                                             && (ipField.text !== devIp || maskField.text !== devMask
                                                 || hostField.text !== devHost || dhcpSwitch.checked !== dhcpSwitch.wantChecked)
        function apply() {
            if (!dev.applyNet(ipField.text, maskField.text, hostField.text, dhcpSwitch.checked)) {
                notifications.info(qsTr("Address, mask or host address is not four octets"))
                return false
            }
            notifications.info(qsTr("Network settings sent: %1").arg(ipField.text))
            netReadBack.restart()       // покажем то, что прибор принял на деле
            return true
        }

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
                onClicked: if (dev) devNetGroup.apply()
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

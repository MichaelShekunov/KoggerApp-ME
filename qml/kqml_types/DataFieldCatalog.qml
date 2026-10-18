pragma Singleton
import QtQuick 2.15
import "UsblFieldLogic.js" as UsblLogic

QtObject {
    id: catalog

    // THE PALETTE MODEL. WidgetPlaceStep binds a Repeater straight to this, so adding an
    // entry here puts a tile in front of every user on every device — it is shared UI,
    // not a data declaration. Subsystem fields live in their own list below and are only
    // offered once something deliberately reads that list.
    readonly property var fields: [
        { key: "time",      label: qsTr("Time"),           unit: "",             group: "general" },
        { key: "depth",     label: qsTr("Depth"),          unit: qsTr("m"),      group: "general" },
        { key: "rfDepth",   label: qsTr("Rangefinder"),    unit: qsTr("m"),      group: "general" },
        { key: "btDepth",   label: qsTr("Bottom track"),   unit: qsTr("m"),      group: "general" },
        // My maps (src/map_export): the depth under the boat from the opened KOGGER maps.
        { key: "mapDepth",  label: qsTr("Depth by map"),   unit: qsTr("m"),      group: "general" },
        { key: "speed",     label: qsTr("Speed"),          unit: qsTr("km/h"),   group: "general" },
        { key: "apSpeed",   label: qsTr("Ground speed"),   unit: qsTr("m/s"),    group: "autopilot" },
        { key: "coord",     label: qsTr("Coordinate"),     unit: "",             group: "general" },
        { key: "selPoint",  label: qsTr("Selected point"), unit: "",             group: "general" },
        { key: "temp",      label: qsTr("Temperature"),    unit: "°C",           group: "general" },
        { key: "sysBattery",label: qsTr("Device charge"),  unit: "%",            group: "general" },
        { key: "apVoltage", label: qsTr("Battery"),        unit: "V",            group: "autopilot" },
        { key: "apCurrent", label: qsTr("Current"),        unit: "A",            group: "autopilot" },
        { key: "apMode",    label: qsTr("Flight mode"),    unit: "",             group: "autopilot" },
        { key: "apArm",     label: qsTr("Arm state"),      unit: "",             group: "autopilot" },
        // The autopilot's home to the boat now (MavVehicle, over MAVLink): home is where the boat was armed, the
        // launch point RTL returns to.
        { key: "apHomeDist", label: qsTr("From launch point"), unit: qsTr("m"),   group: "autopilot" },
        // A running mission (MavVehicle, mission_progress.h): the way left, its time at the boat's speed, the battery at
        // its end once its trend is known.
        { key: "apMissionLeft",  label: qsTr("Mission left"),      unit: qsTr("m"),   group: "autopilot" },
        { key: "apMissionEta",   label: qsTr("Mission time left"), unit: qsTr("min"), group: "autopilot" },
        { key: "apBatteryAtEnd", label: qsTr("Battery at finish"), unit: "%",         group: "autopilot" },
        // The radio link (DeviceManagerWrapper.air*, once a second): what it carries by kind as a bar, the head unit
        // module's Wi-Fi signal.
        { key: "linkAir",  label: qsTr("On the link"),  unit: qsTr("KB/s"), group: "link" },
        { key: "linkRssi", label: qsTr("Wi-Fi signal"), unit: qsTr("dBm"),  group: "link" },
        // The computer the app runs on (SystemMonitor, context property systemMonitor): "System" draws all of it in one
        // cell (SystemOverview.qml), the others one value each.
        { key: "sysOverview", label: qsTr("System"),          unit: "",           group: "system" },
        { key: "sysCpu",      label: qsTr("CPU"),             unit: qsTr("GHz"),  group: "system" },
        { key: "sysCpuTemp",  label: qsTr("CPU temperature"), unit: "°C",         group: "system" },
        { key: "sysGpu",      label: qsTr("GPU"),             unit: "%",          group: "system" },
        { key: "sysRam",      label: qsTr("Memory"),          unit: qsTr("GB"),   group: "system" },
        { key: "sysStorage",  label: qsTr("Storage"),         unit: qsTr("GB"),   group: "system" },
        { key: "sysUptime",   label: qsTr("Uptime"),          unit: "",           group: "system" }
    ]

    // The palette's stand-in for SystemMonitor: the head unit (RK3588: 4 cores to 1.8 GHz, 4 to 2.4 GHz; one eMMC with
    // "/" and "/data") at work.
    readonly property var systemSample: ({
        available: true,
        coreFreqMHz: [1416, 1008, 1800, 1200, 2400, 1800, 2208, 816],
        coreMaxMHz: [1800, 1800, 1800, 1800, 2400, 2400, 2400, 2400],
        coreLoad: [0.22, 0.10, 0.64, 0.31, 0.86, 0.45, 0.57, 0.08],
        cpuLoad: 0.40, cpuFreqMHz: 2400, cpuTempC: 52, throttled: false,
        gpuLoad: 0.12, gpuFreqMHz: 300, gpuMaxMHz: 1000, gpuTempC: 48,
        memTotalGB: 15.6, memUsedGB: 3.1,
        volumes: [{ name: "/", kind: "eMMC", wearPct: 10, totalGB: 11.7, usedGB: 8.7 },
                  { name: "/data", kind: "", wearPct: -1, data: true, totalGB: 16.5, usedGB: 0.1 }],
        uptimeS: 11520
    })

    // USBL fields — deliberately NOT in `fields`, so the palette is unchanged for the
    // vast majority of users who have no acoustic beacon.
    //
    // They are fully wired otherwise: hasField/label/unit/isValid/formatValue all resolve
    // them, so a cell referencing one renders correctly. What is missing is a way to
    // CREATE such a cell, and that waits on the palette expansion mechanism — one entry
    // per beacon that has actually answered, rather than ten dead tiles for everyone.
    //
    // `addressable: true` means the field answers "for WHICH beacon?", so a cell has to
    // carry an address alongside the key.
    readonly property var usblFields: [
        { key: "usblRange",       label: qsTr("Beacon range"),    unit: qsTr("m"),  group: "usbl", addressable: true },
        { key: "usblAzimuth",     label: qsTr("Beacon azimuth"),  unit: "°",        group: "usbl", addressable: true },
        { key: "usblElevation",   label: qsTr("Beacon elevation"),unit: "°",        group: "usbl", addressable: true },
        { key: "usblSnr",         label: qsTr("Acoustic SNR"),    unit: qsTr("dB"), group: "usbl", addressable: true },
        { key: "usblBeaconCoord", label: qsTr("Beacon position"), unit: "",         group: "usbl", addressable: true },
        { key: "usblBeaconDepth", label: qsTr("Beacon depth"),    unit: qsTr("m"),  group: "usbl", addressable: true },
        { key: "usblAge",         label: qsTr("Fix age"),         unit: qsTr("s"),  group: "usbl", addressable: true },
        { key: "usblState",       label: qsTr("Acoustic link"),   unit: "",         group: "usbl", addressable: true },
        { key: "usblAddress",     label: qsTr("Last beacon"),     unit: "",         group: "usbl" },
        { key: "usblPresent",     label: qsTr("USBL data"),       unit: "",         group: "usbl" }
    ]

    // Codes from UsblFieldLogic, translated here — the logic module never returns prose.
    readonly property var _usblStateText: ({
        "tracking": qsTr("TRACKING"),
        "stale":    qsTr("STALE"),
        "none":     qsTr("NO SOLUTION")
    })

    function isAddressable(key) { return UsblLogic.isAddressable(key) }
    function addressAny() { return UsblLogic.ANY }
    // A beacon's identity colour, constant for the life of that address. The badge in the
    // settings pane, the badge on the scene panel and (when it lands) the marker on the map
    // are the same beacon, so they take the same colour from one table.
    function usblAddressColor(addr) { return UsblLogic.addressColor(addr) }
    // Forwarded so WorkspaceStore has one owner of the rule rather than a second copy —
    // it cannot import this module's .js directly.
    function normAddr(a) { return UsblLogic.normAddr(a) }
    // The beacons that have actually answered — offering one that never has is offering a
    // permanently blank widget.
    function knownUsblAddresses(ds) {
        return ds ? UsblLogic.knownAddresses(ds.usblSolutions) : []
    }
    // Reading ds.usblSolutions HERE would not be tracked by the caller's binding; it is
    // read in the caller's expression via the `ds` parameter, same contract as the rest
    // of this file.
    function _usblEntry(ds, addr) {
        return ds ? UsblLogic.pick(ds.usblSolutions, addr) : null
    }
    // `store.nowMs` is WorkspaceStore's 1 s clock, and it has to exist: Date.now() inside a
    // binding is evaluated once and then frozen, so without a ticking property `usblAge` and
    // the tracking/stale flip in `usblState` would only advance when some OTHER dependency
    // changed. The fallback keeps a caller that passes no store rendering something sane.
    function _nowMs(store) {
        return (store && store.nowMs) ? store.nowMs : Date.now()
    }

    // Searches BOTH lists. `fields` is what the palette offers; this is what the app can
    // resolve. A cell referencing a USBL field renders correctly even though there is no
    // way to create one from the palette yet -- and normalizeWidgetDef keeps it on load
    // instead of dropping it as unknown.
    function _meta(key) {
        for (var i = 0; i < fields.length; ++i)
            if (fields[i].key === key)
                return fields[i]
        for (var j = 0; j < usblFields.length; ++j)
            if (usblFields[j].key === key)
                return usblFields[j]
        return null
    }

    function hasField(key) { return _meta(key) !== null }

    function label(key) {
        var m = _meta(key)
        return m ? m.label : key
    }

    function unit(key) {
        var m = _meta(key)
        return m ? m.unit : ""
    }

    function _dms(deg, isLat) {
        if (deg === undefined || deg === null || isNaN(deg))
            return ""
        var hemi = isLat ? (deg >= 0 ? "N" : "S") : (deg >= 0 ? "E" : "W")
        return hemi + " " + Math.abs(deg).toFixed(4) + "°"
    }

    // `veh` is the boat's autopilot link (MavVehicle, the context property `boat`): home, position, link.
    function _homeDistValid(veh) {
        return !!(veh && veh.connected && veh.homeValid && veh.positionValid)
    }
    function _distanceText(m) {
        return m < 1000 ? Math.round(m) + " " + qsTr("m") : (m / 1000).toFixed(2) + " " + qsTr("km")
    }
    // The fields SystemMonitor feeds (a cell showing one subscribes to it while it lives).
    function usesSystem(key) {
        return key === "sysOverview" || key === "sysCpu" || key === "sysCpuTemp" || key === "sysGpu" || key === "sysRam"
               || key === "sysStorage" || key === "sysUptime"
    }
    function _sysOk(sys) {
        return !!(sys && sys.available)
    }
    // The volume this app keeps its data on (the maps, its log), or the first.
    function _dataVolume(sys) {
        var v = sys && sys.volumes ? sys.volumes : []
        for (var i = 0; i < v.length; ++i)
            if (v[i].data === true) return v[i]
        return v.length ? v[0] : null
    }
    // A running time: "12 min", "3 h 12 min", "3 d 14 h".
    function uptimeText(s) {
        var d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60)
        return d > 0 ? d + " " + qsTr("d") + " " + h + " " + qsTr("h")
             : h > 0 ? h + " " + qsTr("h") + " " + m + " " + qsTr("min") : m + " " + qsTr("min")
    }
    function gbPair(used, total) {
        return used.toFixed(1) + " / " + total.toFixed(total >= 100 ? 0 : 1) + " " + qsTr("GB")
    }
    // The System widget's levels: a load 0..1 (half, four fifths), degrees C (60, 75), a fill 0..1 (70 %, 90 %).
    function sysLoadColor(l) {
        return AppPalette.systemLevelColors[l < 0.5 ? 0 : l < 0.8 ? 1 : 2]
    }
    function sysTempColor(t) {
        return AppPalette.systemLevelColors[t < 60 ? 0 : t < 75 ? 1 : 2]
    }
    function sysFillColor(f) {
        return f < 0.7 ? AppPalette.systemFill : AppPalette.systemLevelColors[f < 0.9 ? 1 : 2]
    }
    function _durationText(s) {
        return s >= 90 ? Math.round(s / 60) + " " + qsTr("min") : Math.round(s) + " " + qsTr("s")
    }
    function _missionValid(veh) {
        return !!(veh && veh.missionProgressValid)
    }
    function _airTotal(dmw) {
        return dmw.airEchogramKBs + dmw.airTelemetryKBs + dmw.airServiceKBs
    }
    // A cell drawn with a stacked bar under its value: [{ v, c }] parts by kind, [] for a plain value.
    function bars(key, dmw, sys) {
        if (_sysOk(sys)) {
            var dim = Qt.rgba(AppPalette.textSecond.r, AppPalette.textSecond.g, AppPalette.textSecond.b, 0.18)
            if (key === "sysCpu" && sys.cpuLoad >= 0)
                return [{ v: sys.cpuLoad, c: sysLoadColor(sys.cpuLoad) }, { v: 1 - sys.cpuLoad, c: dim }]
            if (key === "sysGpu" && sys.gpuLoad >= 0)
                return [{ v: sys.gpuLoad, c: sysLoadColor(sys.gpuLoad) }, { v: 1 - sys.gpuLoad, c: dim }]
            if (key === "sysCpuTemp" && isFinite(sys.cpuTempC)) {
                var t = Math.max(0, Math.min(1, (sys.cpuTempC - 20) / 80))   // 20..100 degrees
                return [{ v: t, c: sysTempColor(sys.cpuTempC) }, { v: 1 - t, c: dim }]
            }
            if (key === "sysRam" && sys.memTotalGB > 0)
                return [{ v: sys.memUsedGB, c: sysFillColor(sys.memUsedGB / sys.memTotalGB) },
                        { v: Math.max(0, sys.memTotalGB - sys.memUsedGB), c: dim }]
            var dv = _dataVolume(sys)
            if (key === "sysStorage" && dv)
                return [{ v: dv.usedGB, c: sysFillColor(dv.totalGB > 0 ? dv.usedGB / dv.totalGB : 0) },
                        { v: Math.max(0, dv.totalGB - dv.usedGB), c: dim }]
        }
        if (key !== "linkAir" || !dmw || !dmw.airValid)
            return []
        return [{ v: dmw.airEchogramKBs, c: AppPalette.airColors[0] },
                { v: dmw.airTelemetryKBs, c: AppPalette.airColors[1] },
                { v: dmw.airServiceKBs, c: AppPalette.airColors[2] }]
    }

    function _autopilotValid(dmw) {
        if (!dmw)
            return false
        return (!isNaN(dmw.vruVoltage) || !isNaN(dmw.vruCurrent) || !isNaN(dmw.vruVelocityH)
                || dmw.pilotArmState >= 0 || dmw.pilotModeState >= 0)
    }

    // `addr` is the beacon a USBL cell is pinned to (-1 = whichever answered last). Every
    // other field ignores it, so existing call sites keep working unchanged.
    function isValid(key, ds, dmw, store, sysbat, addr, veh, sys) {
        if (UsblLogic.isUsblField(key)) {
            if (key === "usblPresent")
                return !!(ds && ds.hasUsblData)
            return UsblLogic.isValid(key, _usblEntry(ds, addr), _nowMs(store))
        }
        switch (key) {
        case "time":      return !!(store && store.systemTimeValid)
        case "sysBattery": return !!(sysbat && sysbat.available)
        case "depth":     return !!(ds && ds.isLastDepthValid)
        case "rfDepth":   return !!(ds && ds.isLastRangefinderDepthValid)
        case "btDepth":   return !!(ds && ds.isLastBottomTrackDepthValid)
        case "mapDepth":  return _mapLib() !== null && _mapLib().boatDepthValid
        case "speed":     return !!(ds && ds.isSpeedValid && ds.isBoatCoordinateValid)
        case "coord":     return !!(ds && ds.isBoatCoordinateValid)
        case "selPoint":  return !!(ds && ds.isActiveContactIndxValid)
        case "temp":      return !!(ds && ds.isLastTempValid)
        case "apSpeed":   return _autopilotValid(dmw) && !isNaN(dmw.vruVelocityH)
        case "apVoltage": return _autopilotValid(dmw) && !isNaN(dmw.vruVoltage)
        case "apCurrent": return _autopilotValid(dmw) && !isNaN(dmw.vruCurrent)
        case "apMode":    return _autopilotValid(dmw) && dmw.pilotModeState >= 0
        case "apArm":     return _autopilotValid(dmw) && dmw.pilotArmState >= 0
        case "apHomeDist": return _homeDistValid(veh)
        case "apMissionLeft":  return _missionValid(veh)
        case "apMissionEta":   return _missionValid(veh) && isFinite(veh.missionEtaS)
        case "apBatteryAtEnd": return _missionValid(veh) && veh.missionBatteryAtEnd >= 0
        case "linkAir":        return !!(dmw && dmw.airValid)
        case "linkRssi":       return !!(dmw && dmw.airValid && dmw.airRssi < 0)
        case "sysOverview":    return _sysOk(sys)
        case "sysCpu":         return _sysOk(sys) && sys.cpuLoad >= 0
        case "sysCpuTemp":     return _sysOk(sys) && isFinite(sys.cpuTempC)
        case "sysGpu":         return _sysOk(sys) && (sys.gpuLoad >= 0 || sys.gpuFreqMHz > 0)
        case "sysRam":         return _sysOk(sys) && sys.memTotalGB > 0
        case "sysStorage":     return _sysOk(sys) && !!sys.volumes && sys.volumes.length > 0
        case "sysUptime":      return _sysOk(sys) && sys.uptimeS >= 0
        }
        return false
    }

    function formatValue(key, ds, dmw, store, sysbat, addr, veh, sys) {
        if (UsblLogic.isUsblField(key))
            return _formatUsbl(key, ds, store, addr)
        switch (key) {
        case "time":
            return (store && store.systemTimeValid) ? store.systemTimeHms : "—"
        case "sysBattery":
            return (sysbat && sysbat.available) ? (sysbat.level + " %") : "—"
        case "depth":
            return (ds && ds.isLastDepthValid) ? (ds.depth.toFixed(2) + " " + qsTr("m")) : "—"
        case "rfDepth":
            return (ds && ds.isLastRangefinderDepthValid) ? (ds.lastRangefinderDepth.toFixed(2) + " " + qsTr("m")) : "—"
        case "btDepth":
            return (ds && ds.isLastBottomTrackDepthValid) ? (ds.lastBottomTrackDepth.toFixed(2) + " " + qsTr("m")) : "—"
        case "mapDepth":
            return (_mapLib() !== null && _mapLib().boatDepthValid) ? (_mapLib().boatDepth.toFixed(1) + " " + qsTr("m")) : "—"
        case "speed":
            return (ds && ds.isSpeedValid && ds.isBoatCoordinateValid) ? (ds.speed.toFixed(1) + " " + qsTr("km/h")) : "—"
        case "coord":
            return (ds && ds.isBoatCoordinateValid) ? (_dms(ds.boatLatitude, true) + "\n" + _dms(ds.boatLongitude, false)) : "—"
        case "selPoint":
            return (ds && ds.isActiveContactIndxValid) ? (ds.distToContact.toFixed(1) + " " + qsTr("m") + "\n" + ds.angleToContact.toFixed(1) + "°") : "—"
        case "temp":
            return (ds && ds.isLastTempValid) ? (ds.lastTemp.toFixed(1) + " °C") : "—"
        case "apSpeed":
            return (dmw && !isNaN(dmw.vruVelocityH)) ? (dmw.vruVelocityH.toFixed(1) + " " + qsTr("m/s")) : "—"
        case "apVoltage":
            return (dmw && !isNaN(dmw.vruVoltage)) ? (dmw.vruVoltage.toFixed(1) + " V") : "—"
        case "apCurrent":
            return (dmw && !isNaN(dmw.vruCurrent)) ? (dmw.vruCurrent.toFixed(1) + " A") : "—"
        case "apMode":
            return (dmw && dmw.pilotModeState >= 0) ? String(dmw.pilotModeState) : "—"
        case "apArm":
            return (dmw && dmw.pilotArmState >= 0) ? (dmw.pilotArmState > 0 ? "ARMED" : "DISARMED") : "—"
        case "apHomeDist":
            return _homeDistValid(veh)
                   ? _distanceText(veh.distanceM(veh.homeLatitude, veh.homeLongitude, veh.latitude, veh.longitude)) : "—"
        case "apMissionLeft":
            return _missionValid(veh) ? _distanceText(veh.missionRemainingM) : "—"
        case "apMissionEta":
            return (_missionValid(veh) && isFinite(veh.missionEtaS)) ? _durationText(veh.missionEtaS) : "—"
        case "apBatteryAtEnd":
            return (_missionValid(veh) && veh.missionBatteryAtEnd >= 0) ? (veh.missionBatteryAtEnd + " %") : "—"
        case "linkAir":
            return (dmw && dmw.airValid) ? (_airTotal(dmw).toFixed(1) + " " + qsTr("KB/s")) : "—"
        case "linkRssi":
            return (dmw && dmw.airValid && dmw.airRssi < 0) ? (dmw.airRssi + " " + qsTr("dBm")) : "—"
        case "sysOverview":
            return _sysOk(sys) ? qsTr("System") : "—"
        case "sysCpu":
            if (!_sysOk(sys) || sys.cpuLoad < 0) return "—"
            return (sys.cpuFreqMHz > 0 ? (sys.cpuFreqMHz / 1000).toFixed(2) + " " + qsTr("GHz") + " · " : "")
                   + Math.round(sys.cpuLoad * 100) + " %"
        case "sysCpuTemp":
            return (_sysOk(sys) && isFinite(sys.cpuTempC)) ? (Math.round(sys.cpuTempC) + " °C" + (sys.throttled ? " ▼" : "")) : "—"
        case "sysGpu": {
            if (!_sysOk(sys) || !(sys.gpuLoad >= 0 || sys.gpuFreqMHz > 0)) return "—"
            var g = []
            if (sys.gpuLoad >= 0) g.push(Math.round(sys.gpuLoad * 100) + " %")
            if (sys.gpuFreqMHz > 0) g.push(sys.gpuFreqMHz + " " + qsTr("MHz"))
            if (isFinite(sys.gpuTempC)) g.push(Math.round(sys.gpuTempC) + " °C")
            return g.join(" · ")
        }
        case "sysRam":
            return (_sysOk(sys) && sys.memTotalGB > 0) ? gbPair(sys.memUsedGB, sys.memTotalGB) : "—"
        case "sysStorage":
            return (_sysOk(sys) && _dataVolume(sys)) ? gbPair(_dataVolume(sys).usedGB, _dataVolume(sys).totalGB) : "—"
        case "sysUptime":
            return (_sysOk(sys) && sys.uptimeS >= 0) ? uptimeText(sys.uptimeS) : "—"
        }
        return "—"
    }

    // Units and translated words are added HERE, never in the logic module: a unit baked
    // into the raw value could not be translated and would be wrong the day this grows a
    // feet/metres switch.
    function _formatUsbl(key, ds, store, addr) {
        if (key === "usblPresent")
            return (ds && ds.hasUsblData) ? qsTr("yes") : qsTr("no")

        var e = _usblEntry(ds, addr)
        var now = _nowMs(store)

        if (key === "usblState")
            return _usblStateText[UsblLogic.stateCode(e, now)] || "—"

        var v = UsblLogic.rawValue(key, e, now)
        if (v === null || v === undefined)
            return "—"

        switch (key) {
        case "usblRange":       return v + " " + qsTr("m")
        case "usblAzimuth":     return v + "°"
        case "usblElevation":   return v + "°"
        case "usblSnr":         return v + " " + qsTr("dB")
        case "usblBeaconDepth": return v + " " + qsTr("m")
        case "usblBeaconCoord": return v
        case "usblAge":         return (v < 10 ? v.toFixed(1) : Math.round(v)) + " " + qsTr("s")
        case "usblAddress":     return v
        }
        return String(v)
    }

    // The app's `mapLibrary` (Core's context property); null where there is none (the QML checks).
    function _mapLib() {
        return typeof mapLibrary !== "undefined" && mapLibrary ? mapLibrary : null
    }

    function sampleValue(key, store) {
        switch (key) {
        case "time":      return (store && store.systemTimeValid) ? store.systemTimeHms : "12:34:56"
        case "depth":     return "3.20 " + qsTr("m")
        case "rfDepth":   return "3.15 " + qsTr("m")
        case "btDepth":   return "3.24 " + qsTr("m")
        case "mapDepth":  return "3.4 " + qsTr("m")
        case "speed":     return "5.4 " + qsTr("km/h")
        case "apSpeed":   return "1.5 " + qsTr("m/s")
        case "coord":     return "N 40.6035°\nE 45.0010°"
        case "selPoint":  return "12.5 " + qsTr("m") + "\n135.0°"
        case "temp":      return "18.4 °C"
        case "sysBattery": return "87 %"
        case "apVoltage": return "12.4 V"
        case "apCurrent": return "3.2 A"
        case "apMode":    return "3"
        case "apArm":     return "ARMED"
        case "apMissionLeft":  return "1.24 " + qsTr("km")
        case "apMissionEta":   return "14 " + qsTr("min")
        case "apBatteryAtEnd": return "46 %"
        case "linkAir":        return "7.3 " + qsTr("KB/s")
        case "linkRssi":       return "-52 " + qsTr("dBm")
        case "sysOverview":    return qsTr("System")
        case "sysCpu":         return "2.40 " + qsTr("GHz") + " · 40 %"
        case "sysCpuTemp":     return "52 °C"
        case "sysGpu":         return "12 % · 300 " + qsTr("MHz") + " · 48 °C"
        case "sysRam":         return gbPair(3.1, 15.6)
        case "sysStorage":     return gbPair(0.1, 16.5)
        case "sysUptime":      return uptimeText(11520)
        case "usblRange":       return "18.3 " + qsTr("m")
        case "usblAzimuth":     return "127.4°"
        case "usblElevation":   return "-22.8°"
        case "usblSnr":         return "24 " + qsTr("dB")
        case "usblBeaconCoord": return "N 59.9386°\nE 30.3141°"
        case "usblBeaconDepth": return "11.20 " + qsTr("m")
        case "usblAge":         return "1.4 " + qsTr("s")
        case "usblState":       return _usblStateText["tracking"]
        case "usblAddress":     return "2"
        case "usblPresent":     return qsTr("yes")
        }
        return "—"
    }

    function previewAvailable(key, sysbat) {
        if (key === "sysBattery")
            return !!(sysbat && sysbat.available)
        return true
    }

    function previewValue(key, store, sysbat) {
        if (key === "sysBattery")
            return formatValue(key, null, null, store, sysbat)
        return sampleValue(key, store)
    }
}

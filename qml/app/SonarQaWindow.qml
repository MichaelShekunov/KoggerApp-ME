import QtQuick 2.15
import QtQuick.Window 2.15
import kqml_types 1.0
import KoggerSonarQa 1.0

// Sonar acceptance on the water stand (developer mode; docs/SONAR_QA.md). A view of the application's one runner
// SonarQa: closing the window does not stop a run, opening it again shows where the run is.
Window {
    id: win
    objectName: "sonarQaWindow"
    title: qsTr("Sonar acceptance: nanoSSS")
    width: Math.round(1080 * AppPalette.scale)
    height: Math.round(860 * AppPalette.scale)
    minimumWidth: Math.round(720 * AppPalette.scale)
    minimumHeight: Math.round(520 * AppPalette.scale)
    color: AppPalette.bg

    readonly property var devs: (typeof deviceManagerWrapper !== "undefined" && deviceManagerWrapper)
                                ? deviceManagerWrapper.devs : []
    readonly property var nanoDevs: {
        var out = []
        for (var i = 0; i < devs.length; ++i)
            if (devs[i] && (devs[i].devType === 17 || devs[i].devType === 129))
                out.push(devs[i])
        return out
    }
    readonly property var info: SonarQa.deviceInfo
    readonly property var ref: SonarQa.reference
    readonly property var res: SonarQa.result
    property int chartSide: 0
    property bool confirmBaseline: false
    // why each kind cannot start now, refreshed once a second while idle
    property var reasons: ({ baseline: "", check: "", unit: "" })

    function refreshReasons() {
        reasons = { baseline: SonarQa.canStart("baseline"), check: SonarQa.canStart("check"),
                    unit: SonarQa.canStart("unit") }
    }
    function fmtTime(iso) { return iso ? iso.replace("T", " ").substring(0, 19) : "—" }
    function fmtEta(s) { return s >= 60 ? qsTr("%1 min %2 s").arg(Math.floor(s / 60)).arg(s % 60) : qsTr("%1 s").arg(s) }
    function checkTitle(id) {
        switch (id) {
        case "sweep": return qsTr("Same sweep as the reference")
        case "echo_band_mean": return qsTr("Reflection in the band, mean")
        case "echo_band_max": return qsTr("Reflection in the band, worst")
        case "resonance": return qsTr("Resonance")
        case "bandwidth": return qsTr("Bandwidth")
        case "noise": return qsTr("Noise, transmitter off")
        case "ringdown": return qsTr("Ringing after the burst")
        case "position": return qsTr("Target position (placement)")
        case "saturation": return qsTr("Saturation")
        case "balance": return qsTr("Balance of the sides")
        case "supply": return qsTr("Supply")
        case "temperature": return qsTr("Temperature")
        case "temperature_delta": return qsTr("Temperature against the reference")
        case "tx_off": return qsTr("Transmitter off at 0 pulses")
        case "reflection": return qsTr("Reflection seen")
        }
        return id
    }
    function verdictText(v) {
        switch (v) {
        case "pass": return qsTr("PASSED")
        case "fail": return qsTr("FAILED")
        case "baseline": return qsTr("REFERENCE SET")
        case "check_ok": return qsTr("REFERENCE CHECKED")
        case "check_drift": return qsTr("REFERENCE MOVED AWAY FROM ITS BASELINE")
        }
        return ""
    }
    // "Supply: <what it says>; ..." for each failed check of the reference's last check
    function failuresText(list) {
        var out = []
        for (var i = 0; i < (list || []).length; ++i)
            out.push(checkTitle(list[i].id) + (list[i].side >= 0 ? qsTr(", side %1").arg(list[i].side + 1) : "")
                     + (list[i].detail ? " — " + list[i].detail : ""))
        return out.join("\n")
    }
    function fmtValue(c) {
        var v = Number(c.value)
        if (c.id === "sweep" || (c.id === "reflection" && v === 0))
            return ""
        if (c.id === "reflection")
            return Math.round(v) + " / " + Math.round(c.limit)
        if ((c.id === "supply" || c.id === "temperature") && c.level !== "ok" && v === 0)
            return "—"
        if (c.unit === "x")
            return v.toFixed(2) + "×"
        if (c.id === "saturation")
            return Math.round(v * 100) + " %"
        return (Math.abs(v) >= 100 ? v.toFixed(0) : v.toFixed(1)) + (c.unit ? " " + c.unit : "")
    }
    // A real nanoSSS is taken as soon as one is connected; with the simulated sonar on, the choice is left to the
    // buttons.
    function pickDevice() {
        if (SonarQa.busy || SonarQa.demoAvailable)
            return
        if (nanoDevs.length > 0 && nanoDevs.indexOf(SonarQa.device) < 0)
            SonarQa.device = nanoDevs[0]
    }

    Component.onCompleted: { SonarQa.window = win; pickDevice(); refreshReasons() }
    onNanoDevsChanged: pickDevice()
    Connections {
        target: SonarQa
        function onRunEnded() { win.refreshReasons() }
        function onDeviceChanged() { win.refreshReasons() }
        function onStateChanged() { win.refreshReasons() }
    }
    Timer {
        interval: 1000; repeat: true
        running: win.visible && !SonarQa.busy
        onTriggered: { SonarQa.refreshDevice(); win.refreshReasons() }
    }

    component QaButton: KButton {
        height: Tokens.controlHMd
        width: Math.max(Math.round(150 * AppPalette.scale), implicitWidth + Tokens.spaceLg * 2)
        fontPixelSize: Tokens.fontMd
        normalBg: AppPalette.controlRaised
        hoverBg: Qt.lighter(AppPalette.controlRaised, 1.2)
    }
    component Note: Text {
        width: parent ? parent.width : 0
        wrapMode: Text.WordWrap
        color: AppPalette.textSecond
        font.pixelSize: Tokens.fontSm
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentHeight: col.height + Tokens.spaceLg * 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: col
            x: Tokens.spaceLg; y: Tokens.spaceLg
            width: flick.width - Tokens.spaceLg * 2
            spacing: Tokens.spaceLg

            // ── the sonar ──
            KIsland {
                title: qsTr("Sonar on the stand")
                KIslandRow {
                    label: qsTr("Sonar")
                    caption: win.info.connected
                             ? qsTr("serial %1 · firmware %2 · board %3").arg(win.info.serial).arg(win.info.firmware || "—")
                                   .arg(win.info.board) + (win.info.isReference ? qsTr(" · the reference") : "")
                             : qsTr("No nanoSSS connected")
                    stacked: true
                    KCombo {
                        objectName: "qaDevice"
                        width: Math.round(320 * AppPalette.scale)
                        enabled: !SonarQa.busy && win.nanoDevs.length > 0
                        model: win.nanoDevs.map(function(d) { return d.devName + " · " + d.devSN })
                        currentIndex: Math.max(0, win.nanoDevs.indexOf(SonarQa.device))
                        onActivated: function(i) { SonarQa.device = win.nanoDevs[i] }
                    }
                }
                KIslandRow {
                    label: qsTr("Number from the sticker")
                    caption: qsTr("Empty: the number the sonar reports. Some firmware reports one number for every sonar: then type each sonar's own.")
                    stacked: true
                    NodesTextField {
                        objectName: "qaSerial"
                        width: Math.round(320 * AppPalette.scale)
                        fontPixelSize: Tokens.fontMd
                        value: SonarQa.serialEntry
                        placeholder: win.info.serial || ""
                        onCommitted: function(v) { SonarQa.serialEntry = v.trim(); win.refreshReasons() }
                    }
                }
                KIslandRow {
                    visible: SonarQa.demoAvailable
                    label: qsTr("Simulated sonar")
                    caption: qsTr("KOGGER_SONARQA_DEMO: a sonar in a tank, to try the window without a stand")
                    stacked: true
                    Row {
                        spacing: Tokens.spaceSm
                        QaButton { objectName: "qaDemoRef"; text: qsTr("Reference 1001"); enabled: !SonarQa.busy; onClicked: SonarQa.useDemo(1001, "") }
                        QaButton { objectName: "qaDemoGood"; text: qsTr("Good 2001"); enabled: !SonarQa.busy; onClicked: SonarQa.useDemo(2001, "") }
                        QaButton { objectName: "qaDemoWeak"; text: qsTr("Weak 2002"); enabled: !SonarQa.busy; onClicked: SonarQa.useDemo(2002, "weak") }
                    }
                }
            }

            // ── a sonar under test ──
            KIsland {
                title: qsTr("Test")
                KIslandRow {
                    label: qsTr("Sonar under test")
                    caption: win.reasons.unit !== "" ? win.reasons.unit
                             : qsTr("The same sweep and settings as the reference's check; the result is saved by serial number")
                    captionColor: win.reasons.unit !== "" ? AppPalette.dangerText : AppPalette.textSecond
                    stacked: true
                    QaButton {
                        objectName: "qaStartUnit"
                        text: qsTr("Test this sonar")
                        enabled: !SonarQa.busy && win.reasons.unit === ""
                        onClicked: SonarQa.start("unit")
                    }
                }
            }

            // ── the run going on ──
            KIsland {
                visible: SonarQa.busy || SonarQa.status !== ""
                title: SonarQa.busy ? qsTr("Run in progress") : qsTr("Last run")
                Item {
                    width: parent ? parent.width : 0
                    height: runCol.height + Tokens.spaceLg * 2
                    Column {
                        id: runCol
                        x: Tokens.spaceLg; y: Tokens.spaceLg
                        width: parent.width - Tokens.spaceLg * 2
                        spacing: Tokens.spaceSm
                        Row {
                            width: parent.width
                            spacing: Tokens.spaceMd
                            Text {
                                objectName: "qaStatus"
                                width: parent.width - stopBtn.width - Tokens.spaceMd
                                anchors.verticalCenter: parent.verticalCenter
                                text: SonarQa.status + (SonarQa.busy ? "  ·  " + qsTr("about %1 left").arg(win.fmtEta(SonarQa.etaSec)) : "")
                                color: AppPalette.textStrong; font.pixelSize: Tokens.fontMd
                                wrapMode: Text.WordWrap
                            }
                            QaButton { id: stopBtn; objectName: "qaStop"; visible: SonarQa.busy; text: qsTr("Stop"); danger: true; onClicked: SonarQa.stop() }
                        }
                        Rectangle {
                            visible: SonarQa.busy
                            width: parent.width; height: Math.round(8 * AppPalette.scale); radius: height / 2
                            color: AppPalette.trackOff
                            Rectangle {
                                objectName: "qaProgress"
                                width: parent.width * SonarQa.progress; height: parent.height; radius: parent.radius
                                color: AppPalette.toggleOn
                            }
                        }
                        Text {
                            visible: SonarQa.busy
                            text: qsTr("Supply %1 V · temperature %2 °C")
                                  .arg(SonarQa.live.supply !== undefined ? Number(SonarQa.live.supply).toFixed(2) : "—")
                                  .arg(SonarQa.live.temperature !== undefined ? Number(SonarQa.live.temperature).toFixed(1) : "—")
                            color: AppPalette.textSecond; font.pixelSize: Tokens.fontSm
                        }
                        Canvas {
                            id: liveCanvas
                            visible: SonarQa.busy
                            width: parent.width; height: Math.round(150 * AppPalette.scale)
                            readonly property var data: SonarQa.live
                            onDataChanged: requestPaint()
                            onPaint: {
                                var ctx = getContext("2d")
                                ctx.reset()
                                ctx.fillStyle = AppPalette.bgDeep
                                ctx.fillRect(0, 0, width, height)
                                var sides = data && data.sides ? data.sides : []
                                var colors = [AppPalette.toggleOn, "#E8A33D"]
                                for (var s = 0; s < sides.length; ++s) {
                                    var pts = sides[s]
                                    ctx.strokeStyle = colors[s % 2]
                                    ctx.lineWidth = 1
                                    ctx.beginPath()
                                    for (var i = 0; i < pts.length; ++i) {
                                        var x = i * width / Math.max(1, pts.length - 1)
                                        var y = height - pts[i] * height / 255
                                        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                                    }
                                    ctx.stroke()
                                }
                                ctx.fillStyle = AppPalette.textSecond
                                ctx.font = Tokens.fontSm + "px sans-serif"
                                if (data && data.freqKhz !== undefined)
                                    ctx.fillText(data.freqKhz + " kHz · " + data.pulses + " pulses", 6, 14)
                            }
                        }
                    }
                }
            }

            // ── the result ──
            KIsland {
                id: resultIsland
                visible: !!win.res.kind
                title: qsTr("Result")
                Item {
                    width: parent ? parent.width : 0
                    height: resCol.height + Tokens.spaceLg * 2
                    Column {
                        id: resCol
                        x: Tokens.spaceLg; y: Tokens.spaceLg
                        width: parent.width - Tokens.spaceLg * 2
                        spacing: Tokens.spaceMd
                        Rectangle {
                            width: parent.width; height: verdict.implicitHeight + Tokens.spaceMd * 2
                            radius: Tokens.radiusMd
                            readonly property bool bad: win.res.verdict === "fail" || win.res.verdict === "check_drift"
                            color: bad ? AppPalette.dangerBg : AppPalette.accentBg
                            Text {
                                id: verdict
                                objectName: "qaVerdict"
                                anchors.centerIn: parent
                                text: win.verdictText(win.res.verdict) + (win.res.warnings > 0 ? "  ·  " + qsTr("warnings: %1").arg(win.res.warnings) : "")
                                color: parent.bad ? AppPalette.dangerText : AppPalette.accentText
                                font.pixelSize: Tokens.fontLg * 1.4; font.bold: true
                            }
                        }
                        Note {
                            text: qsTr("Serial %1 · firmware %2 · %3").arg(win.res.serial).arg(win.res.firmware).arg(win.fmtTime(win.res.time))
                                  + "\n" + (win.res.saved ? qsTr("Saved: %1").arg(win.res.file) : qsTr("Not saved: %1").arg(win.res.error))
                        }
                        Note {
                            visible: (win.res.checks || []).length > 0
                            text: win.res.kind === "check"
                                  ? qsTr("Each line: what is checked · the figure (how far from the baseline) · the numbers behind it: \"now\" is this run of the reference, \"baseline\" its first run.")
                                  : win.res.kind === "baseline"
                                  ? qsTr("Each line: what the reference's own run shows. Warnings here do not stop anything, but say what will not be compared.")
                                  : qsTr("Each line: what is checked · the figure (how far from the reference) · the numbers behind it: \"this sonar\" against \"reference\" (its latest check).")
                        }
                        Item {
                            visible: (win.res.checks || []).length > 0 && win.res.kind !== "baseline"
                            width: parent.width; height: Math.round(24 * AppPalette.scale)
                            Text { x: Math.round(10 * AppPalette.scale) + Tokens.spaceSm; anchors.verticalCenter: parent.verticalCenter
                                   text: qsTr("Check"); color: AppPalette.textSecond; font.pixelSize: Tokens.fontSm; font.bold: true }
                            Text { x: parent.width * 0.5; anchors.verticalCenter: parent.verticalCenter
                                   text: qsTr("Difference"); color: AppPalette.textSecond; font.pixelSize: Tokens.fontSm; font.bold: true }
                            Text { x: parent.width * 0.62; anchors.verticalCenter: parent.verticalCenter
                                   text: qsTr("What the numbers are"); color: AppPalette.textSecond; font.pixelSize: Tokens.fontSm; font.bold: true }
                        }
                        Column {
                            objectName: "qaChecks"
                            width: parent.width
                            visible: (win.res.checks || []).length > 0
                            Repeater {
                                model: win.res.checks || []
                                delegate: Item {
                                    required property var modelData
                                    width: resCol.width
                                    height: Math.max(Math.round(30 * AppPalette.scale), detailText.implicitHeight + Tokens.spaceSm * 2)
                                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: AppPalette.separator }
                                    Rectangle {
                                        id: dot
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: Math.round(10 * AppPalette.scale); height: width; radius: width / 2
                                        color: modelData.level === "fail" ? AppPalette.dangerText
                                             : modelData.level === "warn" ? "#E8A33D" : AppPalette.toggleOn
                                    }
                                    Text {
                                        x: dot.width + Tokens.spaceSm; anchors.verticalCenter: parent.verticalCenter
                                        width: parent.width * 0.48 - x
                                        text: win.checkTitle(modelData.id) + (modelData.side >= 0 ? qsTr(", side %1").arg(modelData.side + 1) : "")
                                        color: AppPalette.textStrong; font.pixelSize: Tokens.fontMd; elide: Text.ElideRight
                                    }
                                    Text {
                                        x: parent.width * 0.5; anchors.verticalCenter: parent.verticalCenter
                                        width: parent.width * 0.11
                                        text: win.fmtValue(modelData)
                                        color: AppPalette.textStrong; font.pixelSize: Tokens.fontMd; font.bold: modelData.level !== "ok"
                                    }
                                    Text {
                                        id: detailText
                                        x: parent.width * 0.62; anchors.verticalCenter: parent.verticalCenter
                                        width: parent.width * 0.38
                                        text: modelData.detail
                                        color: AppPalette.textSecond; font.pixelSize: Tokens.fontSm; wrapMode: Text.WordWrap
                                    }
                                }
                            }
                        }
                        Row {
                            spacing: Tokens.spaceSm
                            visible: (win.res.curves && win.res.curves.sides || []).length > 1
                            QaButton { text: qsTr("Side 1"); checkable: true; checked: win.chartSide === 0; onClicked: win.chartSide = 0 }
                            QaButton { text: qsTr("Side 2"); checkable: true; checked: win.chartSide === 1; onClicked: win.chartSide = 1 }
                        }
                        Repeater {
                            model: [ { key: "echo", ref: "refEcho", title: qsTr("Reflection where the reference has it, codes (2.5 a dB)"), max: 255 },
                                     { key: "noise", ref: "refNoise", title: qsTr("Noise with the transmitter off, codes"), max: 255 },
                                     { key: "ringdown", ref: "refRingdown", title: qsTr("Ringing after the burst, samples"), max: -1 } ]
                            delegate: Column {
                                required property var modelData
                                width: resCol.width
                                spacing: 2
                                Text { text: modelData.title + "  —  " + qsTr("this sonar") + " / " + qsTr("reference (dashed)")
                                       color: AppPalette.textSecond; font.pixelSize: Tokens.fontSm }
                                Canvas {
                                    id: chart
                                    objectName: "qaChart_" + modelData.key
                                    width: parent.width; height: Math.round(150 * AppPalette.scale)
                                    readonly property var curves: win.res.curves
                                    readonly property int side: win.chartSide
                                    onCurvesChanged: requestPaint()
                                    onSideChanged: requestPaint()
                                    onWidthChanged: requestPaint()
                                    onPaint: {
                                        var ctx = getContext("2d")
                                        ctx.reset()
                                        ctx.fillStyle = AppPalette.bgDeep
                                        ctx.fillRect(0, 0, width, height)
                                        if (!curves || !curves.sides || curves.sides.length <= side)
                                            return
                                        var f = curves.freqs, sd = curves.sides[side]
                                        var a = sd[modelData.key] || [], r = sd[modelData.ref] || []
                                        var top = modelData.max
                                        if (top < 0) {
                                            top = 1
                                            for (var i = 0; i < a.length; ++i) top = Math.max(top, a[i])
                                            for (var j = 0; j < r.length; ++j) top = Math.max(top, r[j])
                                            top *= 1.1
                                        }
                                        var padL = 34, padB = 16
                                        var w = width - padL - 4, h = height - padB - 4
                                        var f0 = f[0], f1 = f[f.length - 1]
                                        function X(k) { return padL + (f[k] - f0) * w / Math.max(1, f1 - f0) }
                                        function Y(v) { return 4 + h - v * h / top }
                                        ctx.strokeStyle = AppPalette.separator; ctx.lineWidth = 1
                                        ctx.fillStyle = AppPalette.textSecond; ctx.font = "10px sans-serif"
                                        for (var g = 0; g <= 4; ++g) {
                                            var gv = top * g / 4
                                            ctx.beginPath(); ctx.moveTo(padL, Y(gv)); ctx.lineTo(padL + w, Y(gv)); ctx.stroke()
                                            ctx.fillText(Math.round(gv), 2, Y(gv) + 3)
                                        }
                                        for (var fk = Math.ceil(f0 / 150) * 150; fk <= f1; fk += 150) {
                                            var gx = padL + (fk - f0) * w / Math.max(1, f1 - f0)
                                            ctx.fillText(fk + " kHz", gx - 18, height - 3)
                                        }
                                        function line(v, color, dash) {
                                            ctx.strokeStyle = color; ctx.lineWidth = dash ? 1 : 2
                                            ctx.setLineDash(dash ? [4, 3] : [])
                                            ctx.beginPath()
                                            var pen = false   // a step not read (no reflection there) breaks the line
                                            for (var k = 0; k < v.length; ++k) {
                                                if (isNaN(v[k])) { pen = false; continue }
                                                if (!pen) ctx.moveTo(X(k), Y(v[k])); else ctx.lineTo(X(k), Y(v[k]))
                                                pen = true
                                            }
                                            ctx.stroke()
                                            ctx.setLineDash([])
                                        }
                                        line(r, AppPalette.textSecond, true)
                                        line(a, AppPalette.toggleOn, false)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ── the reference ──
            KIsland {
                title: qsTr("Reference sonar")
                KIslandRow {
                    label: qsTr("Reference")
                    caption: win.ref.set
                             ? qsTr("serial %1 · baseline %2").arg(win.ref.serial).arg(win.fmtTime(win.ref.baselineTime))
                             : qsTr("Not set: connect the reference sonar and make its baseline")
                    stacked: true
                }
                KIslandRow {
                    visible: win.ref.set
                    label: qsTr("Last check")
                    caption: win.fmtTime(win.ref.checkTime) + " · "
                             + (win.ref.checkPassed ? qsTr("agrees with the baseline") : qsTr("moved away from the baseline")
                                + ((win.ref.checkFailures || []).length > 0 ? ":\n" + win.failuresText(win.ref.checkFailures) : ""))
                    captionColor: win.ref.checkPassed ? AppPalette.textSecond : AppPalette.dangerText
                    stacked: true
                }
                KIslandRow {
                    visible: win.ref.set
                    label: qsTr("Sonars left until the next check")
                    caption: qsTr("%1 of %2").arg(win.ref.unitsLeft).arg(win.ref.unitsPerCheck)
                    stacked: true
                }
                KIslandRow {
                    label: qsTr("Runs of the reference")
                    caption: win.ref.set ? win.reasons.check : win.reasons.baseline
                    stacked: true
                    Column {
                        spacing: Tokens.spaceSm
                        Row {
                            spacing: Tokens.spaceSm
                            QaButton {
                                objectName: "qaStartCheck"
                                text: qsTr("Check the reference")
                                enabled: !SonarQa.busy && win.reasons.check === ""
                                onClicked: SonarQa.start("check")
                            }
                            QaButton {
                                objectName: "qaStartBaseline"
                                text: qsTr("Make the baseline")
                                enabled: !SonarQa.busy && win.reasons.baseline === ""
                                onClicked: {
                                    if (win.ref.set)
                                        win.confirmBaseline = true
                                    else
                                        SonarQa.start("baseline")
                                }
                            }
                            QaButton {
                                objectName: "qaAcceptCheck"
                                visible: win.ref.set && !win.ref.checkPassed
                                text: qsTr("Accept the check")
                                onClicked: SonarQa.acceptCheck()
                            }
                        }
                        Row {
                            visible: win.confirmBaseline
                            spacing: Tokens.spaceSm
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("Replace the reference (serial %1) and its baseline with this sonar?").arg(win.ref.serial)
                                color: AppPalette.dangerText; font.pixelSize: Tokens.fontMd
                            }
                            QaButton { objectName: "qaConfirmYes"; text: qsTr("Replace"); danger: true
                                       onClicked: { win.confirmBaseline = false; SonarQa.start("baseline") } }
                            QaButton { text: qsTr("Cancel"); onClicked: win.confirmBaseline = false }
                        }
                    }
                }
            }

            // ── the sweep and the limits ──
            KIsland {
                id: paramsIsland
                title: qsTr("Sweep and limits")
                KIslandRow {
                    label: qsTr("Sweep")
                    caption: qsTr("Used by the next baseline; checks and tests take the baseline's. Samples count both sides of a nanoSSS (2000 = 1000 a side). Above 30 pulses the firmware switches to chirp.")
                    stacked: true
                    Flow {
                        width: parent ? parent.width : 0
                        spacing: Tokens.spaceSm
                        Repeater {
                            model: [ { key: "freqFromKhz", title: qsTr("From, kHz"), from: 40, to: 1000 },
                                     { key: "freqToKhz", title: qsTr("To, kHz"), from: 40, to: 1000 },
                                     { key: "freqStepKhz", title: qsTr("Step, kHz"), from: 1, to: 100 },
                                     { key: "pingsPerStep", title: qsTr("Pings a step"), from: 1, to: 100 },
                                     { key: "quietPings", title: qsTr("Quiet pings a step"), from: 1, to: 100 },
                                     { key: "discardPings", title: qsTr("Dropped after a change"), from: 0, to: 10 },
                                     { key: "periodMs", title: qsTr("Period, ms"), from: 10, to: 1000 },
                                     { key: "samples", title: qsTr("Samples"), from: 100, to: 5000 },
                                     { key: "txPulses", title: qsTr("Pulses"), from: 1, to: 120 },
                                     { key: "reflectionMinDb", title: qsTr("Reflection over the floor, dB"), from: 6, to: 60 },
                                     { key: "unitsPerCheck", title: qsTr("Sonars a check"), from: 1, to: 100 } ]
                            delegate: Column {
                                required property var modelData
                                spacing: 2
                                Text { text: modelData.title; color: AppPalette.textSecond; font.pixelSize: Tokens.fontSm }
                                KSpinBox {
                                    width: Math.round(140 * AppPalette.scale)
                                    from: modelData.from; to: modelData.to
                                    value: SonarQa.params[modelData.key] || 0
                                    enabled: !SonarQa.busy
                                    onValueModified: function(v) {
                                        var p = SonarQa.params
                                        p[modelData.key] = v
                                        SonarQa.params = p
                                    }
                                }
                            }
                        }
                    }
                }
                KIslandRow {
                    label: qsTr("Limits")
                    caption: qsTr("Starting values: set them from the spread of the reference and of known-good sonars on this stand.")
                    stacked: true
                    Flow {
                        width: parent ? parent.width : 0
                        spacing: Tokens.spaceSm
                        Repeater {
                            model: [ { key: "echoBandMeanDb", title: qsTr("Reflection mean, dB"), div: 10 },
                                     { key: "echoBandMaxDb", title: qsTr("Reflection worst, dB"), div: 10 },
                                     { key: "resonanceKhz", title: qsTr("Resonance shift, kHz"), div: 1 },
                                     { key: "noiseDb", title: qsTr("Noise above, dB"), div: 10 },
                                     { key: "ringdownRatio", title: qsTr("Ringing, times"), div: 100 },
                                     { key: "balanceDb", title: qsTr("Balance, dB"), div: 10 },
                                     { key: "supplyV", title: qsTr("Supply, V"), div: 100 } ]
                            delegate: Column {
                                required property var modelData
                                spacing: 2
                                Text { text: modelData.title; color: AppPalette.textSecond; font.pixelSize: Tokens.fontSm }
                                KSpinBox {
                                    width: Math.round(140 * AppPalette.scale)
                                    from: 0; to: 10000
                                    divisor: modelData.div; decimals: modelData.div >= 100 ? 2 : modelData.div >= 10 ? 1 : 0
                                    value: Math.round((SonarQa.tolerances[modelData.key] || 0) * modelData.div)
                                    enabled: !SonarQa.busy
                                    onValueModified: function(v) {
                                        var t = SonarQa.tolerances
                                        t[modelData.key] = v / modelData.div
                                        SonarQa.tolerances = t
                                    }
                                }
                            }
                        }
                    }
                }
                KIslandRow {
                    label: qsTr("The owner's sweep and the starting limits")
                    QaButton { text: qsTr("Reset"); enabled: !SonarQa.busy; onClicked: SonarQa.resetParams() }
                }
            }

            // ── the journal ──
            KIsland {
                title: qsTr("Journal")
                KIslandRow {
                    label: qsTr("Results folder")
                    caption: SonarQa.folder
                    stacked: true
                    QaButton { text: qsTr("Open folder"); visible: Qt.platform.os !== "android"; onClicked: SonarQa.openFolder() }
                }
                Column {
                    width: parent ? parent.width : 0
                    Repeater {
                        model: SonarQa.journal
                        delegate: Text {
                            required property var modelData
                            x: Tokens.spaceLg
                            width: parent ? parent.width - Tokens.spaceLg * 2 : 0
                            topPadding: 3; bottomPadding: 3
                            text: win.fmtTime(modelData.time) + "   " + modelData.kind + "   " + modelData.serial + "   "
                                  + win.verdictText(modelData.verdict)
                            color: modelData.verdict === "fail" || modelData.verdict === "check_drift"
                                   ? AppPalette.dangerText : AppPalette.textStrong
                            font.pixelSize: Tokens.fontSm
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }
    }
}

<#
.SYNOPSIS
  Offline check of the "System" widget (owner 2026-10-01; docs/HANDOFF.md): qml/app/SystemOverview.qml, the system
  fields of DataFieldCatalog and the widget cell's subscription to SystemMonitor.

.DESCRIPTION
  Runs the real qml/app/SystemOverview.qml, qml/app/WidgetCellContent.qml and the kqml_types sources over a mock
  SystemMonitor shaped like the head unit (RK3588: 4 cores to 1800 MHz, 4 to 2400 MHz; one eMMC with "/" and "/data"):
    1. the cores: one bar each, a wider gap between the clusters, the fill as high as frequency / maximum, coloured by load;
    2. the header: the frequency and load, falling back to the load alone when narrow; the temperature chip by level;
       the "held below its maximum" mark;
    3. the rows: memory then the volumes, the amount (or a percentage when narrow), the disk's kind and wear, the fill;
    4. every cell size (1x1, 2x1, 2x2, 4x2 at two scales): nothing drawn outside the cell, no text cut, rows left out
       from the bottom up, two columns in a wide cell;
    5. no data: the dash;
    6. the catalog: the six fields valid, formatted and with bars from the mock, a dash without it;
    7. the widget cell: subscribed while it shows a system field, unsubscribed when it changes or goes.
  -Picture saves the cells at every size, a hot and throttled computer, a Windows one and one without data.
  Text is measured in the fonts of -FontDir (the offscreen platform has none of its own): by default the repo's
  resources/fonts/arial.ttf; the head unit draws in DejaVu Sans (fc-match sans-serif, 2026-10-01), so pass a folder
  with DejaVuSans.ttf and DejaVuSans-Bold.ttf to measure as it does. -Raw prints the whole result. Exit code 0 when
  all pass, 1 when a check fails, 2 when qmltestrunner is not found.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\qml_check\sysgui_check.ps1 [-QtBin D:\Qt\6.8.3\llvm-mingw_64\bin] [-Picture sys.png] [-FontDir D:\Temp\sysfonts]
#>
param(
    [string]$QtBin = $(if ($env:QT_BIN) { $env:QT_BIN } else { 'D:\Qt\6.8.3\llvm-mingw_64\bin' }),
    [string]$Picture = '',
    [string]$FontDir = '',
    [switch]$Raw
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runner = Join-Path $QtBin 'qmltestrunner.exe'
if (-not (Test-Path $runner)) {
    Write-Host "sysgui_check: no qmltestrunner at $runner (pass -QtBin or set QT_BIN)"
    exit 2
}
$utf8 = New-Object System.Text.UTF8Encoding $false
function Write-Utf8([string]$path, [string]$text) { [System.IO.File]::WriteAllText($path, $text, $utf8) }

# kqml_types from the sources, with a qmldir of its own (the built one prefers the app's resources).
function New-ModuleDir([string]$work) {
    $mod = Join-Path $work 'imports\kqml_types'
    New-Item -ItemType Directory -Force $mod | Out-Null
    $lines = @('module kqml_types')
    $src = Join-Path $repo 'qml\kqml_types'
    foreach ($f in Get-ChildItem $src -File | Where-Object { $_.Extension -in '.qml', '.js' } | Sort-Object Name) {
        $s = [System.IO.File]::ReadAllText($f.FullName, $utf8)
        $single = $f.Extension -eq '.qml' -and $s -match '(?m)^\s*pragma\s+Singleton'
        if ($f.Name -in 'AppPalette.qml', 'Tokens.qml') {
            # the app's C++ context property "theme"; null here, so the files take their own fallbacks
            $s = ([regex]'QtObject \{').Replace($s, "QtObject {`n    readonly property var theme: null", 1)
        }
        Write-Utf8 (Join-Path $mod $f.Name) $s
        $lines += ('{0}{1} 1.0 {2}' -f $(if ($single) { 'singleton ' } else { '' }), $f.BaseName, $f.Name)
    }
    Write-Utf8 (Join-Path $mod 'qmldir') (($lines -join "`n") + "`n")
}

$test = @'
import QtQuick
import QtTest
import kqml_types 1.0

Item {
    id: root
    width: 1100; height: 760

    Rectangle { anchors.fill: parent; color: "#0B1A3A" }

    // SystemMonitor as the head unit reads it at work
    QtObject {
        id: mon
        property int subs: 0
        function subscribe() { ++subs }
        function unsubscribe() { --subs }
        property bool available: true
        property var coreFreqMHz: [1416, 1008, 1800, 1200, 2400, 1800, 2208, 816]
        property var coreMaxMHz: [1800, 1800, 1800, 1800, 2400, 2400, 2400, 2400]
        property var coreLoad: [0.22, 0.10, 0.64, 0.31, 0.86, 0.45, 0.57, 0.08]
        property real cpuLoad: 0.40
        property int cpuFreqMHz: 2400
        property real cpuTempC: 52
        property real gpuTempC: 48
        property int gpuFreqMHz: 300
        property int gpuMaxMHz: 1000
        property real gpuLoad: 0.12
        property bool throttled: false
        property real memTotalGB: 15.6
        property real memUsedGB: 3.1
        property var volumes: [{ name: "/", kind: "eMMC", wearPct: 10, totalGB: 11.7, usedGB: 8.7 },
                               { name: "/data", kind: "", wearPct: -1, data: true, totalGB: 16.5, usedGB: 0.1 }]
        property int uptimeS: 11520
    }
    // hot, held back, memory and the disk nearly full, the eMMC beyond its rated life
    QtObject {
        id: hot
        property bool available: true
        property var coreFreqMHz: [1008, 1008, 1008, 1008, 1416, 1416, 1416, 1416]
        property var coreMaxMHz: [1800, 1800, 1800, 1800, 2400, 2400, 2400, 2400]
        property var coreLoad: [0.97, 0.92, 0.99, 0.88, 1.0, 0.95, 0.98, 0.91]
        property real cpuLoad: 0.95
        property int cpuFreqMHz: 1416
        property real cpuTempC: 86
        property real gpuTempC: 81
        property int gpuFreqMHz: 1000
        property int gpuMaxMHz: 1000
        property real gpuLoad: 0.97
        property bool throttled: true
        property real memTotalGB: 15.6
        property real memUsedGB: 14.8
        property var volumes: [{ name: "/", kind: "eMMC", wearPct: 110, totalGB: 11.7, usedGB: 11.4 },
                               { name: "KINGSTON", kind: "USB", wearPct: -1, totalGB: 58.6, usedGB: 41.0 }]
        property int uptimeS: 309600
    }
    // everything at its top: the widest numbers each line can have
    QtObject {
        id: peak
        property bool available: true
        property var coreFreqMHz: [1800, 1800, 1800, 1800, 2400, 2400, 2400, 2400]
        property var coreMaxMHz: [1800, 1800, 1800, 1800, 2400, 2400, 2400, 2400]
        property var coreLoad: [1, 1, 1, 1, 1, 1, 1, 1]
        property real cpuLoad: 1.0
        property int cpuFreqMHz: 2400
        property real cpuTempC: 101
        property real gpuTempC: 101
        property int gpuFreqMHz: 1000
        property int gpuMaxMHz: 1000
        property real gpuLoad: 1.0
        property bool throttled: true
        property real memTotalGB: 15.6
        property real memUsedGB: 15.5
        property var volumes: [{ name: "/", kind: "eMMC", wearPct: 110, totalGB: 11.7, usedGB: 11.6 },
                               { name: "/data", kind: "", wearPct: -1, data: true, totalGB: 16.5, usedGB: 16.4 }]
        property int uptimeS: 8639999
    }
    // a Windows computer: no temperatures, no GPU, two disks
    QtObject {
        id: win
        property bool available: true
        property var coreFreqMHz: { var a = []; for (var i = 0; i < 32; ++i) a.push(4300); return a }
        property var coreMaxMHz: { var a = []; for (var i = 0; i < 32; ++i) a.push(4300); return a }
        property var coreLoad: { var a = []; for (var i = 0; i < 32; ++i) a.push((i * 37 % 100) / 100); return a }
        property real cpuLoad: 0.31
        property int cpuFreqMHz: 4300
        property real cpuTempC: NaN
        property real gpuTempC: NaN
        property int gpuFreqMHz: -1
        property int gpuMaxMHz: -1
        property real gpuLoad: -1
        property bool throttled: false
        property real memTotalGB: 125.6
        property real memUsedGB: 20.1
        property var volumes: [{ name: "C:", kind: "", wearPct: -1, data: true, totalGB: 930.4, usedGB: 512.0 },
                               { name: "D:", kind: "", wearPct: -1, totalGB: 1863.0, usedGB: 1700.2 }]
        property int uptimeS: 1098331
    }

    function findAll(item, name, out) {
        if (!item) return out
        if (item.objectName === name) out.push(item)
        var kids = item.children || []
        for (var i = 0; i < kids.length; ++i) findAll(kids[i], name, out)
        return out
    }
    function findOne(item, name) { var a = findAll(item, name, []); return a.length ? a[0] : null }
    function byIndex(a) { return a.slice().sort(function(x, y) { return x.index - y.index }) }
    function shown(item) {
        for (var p = item; p; p = p.parent) if (!p.visible) return false
        return true
    }
    function texts(item, out) {
        if (!item) return out
        if (item.hasOwnProperty("elide") && item.hasOwnProperty("truncated")) out.push(item)
        var kids = item.children || []
        for (var i = 0; i < kids.length; ++i) texts(kids[i], out)
        return out
    }
    function overview(w, h, k, s) {
        var o = Qt.createQmlObject('import QtQuick; SystemOverview { }', root)
        o.k = k; o.width = w; o.height = h; o.sys = s
        return o
    }
    // a cell n x m units of the popup's grid (84 px and 4 px gaps at scale k)
    function cellPx(n, k) { return n * Math.round(84 * k) + (n - 1) * Math.round(4 * k) }

    TestCase {
        name: "SysGui"
        when: windowShown

        function test_1_cores() {
            var ov = overview(cellPx(2, 1.11), cellPx(2, 1.11), 1.11, mon)
            wait(20)
            var box = findOne(ov, "sysCores")
            var cores = byIndex(findAll(ov, "sysCore", []))
            compare(cores.length, 8)
            for (var i = 0; i < 8; ++i)
                compare(cores[i].newCluster, i === 4, "a new cluster at the fifth core only")
            fuzzyCompare(cores[4].width - cores[3].width, box.clusterGap - box.gap, 0.01)
            var sum = 0
            for (i = 0; i < 8; ++i) sum += cores[i].width
            fuzzyCompare(sum, box.width, 1, "the bars fill the row")
            for (i = 0; i < 8; ++i) {
                var fill = findOne(cores[i], "sysCoreFill")
                var want = Math.max(2, Math.round(box.height * Math.min(1, mon.coreFreqMHz[i] / mon.coreMaxMHz[i])))
                compare(fill.height, want, "core " + i + " as high as its frequency of its maximum")
                verify(Qt.colorEqual(fill.color, DataFieldCatalog.sysLoadColor(mon.coreLoad[i])), "core " + i)
            }
            verify(Qt.colorEqual(findOne(cores[0], "sysCoreFill").color, "#22C55E"), "22 % green")
            verify(Qt.colorEqual(findOne(cores[2], "sysCoreFill").color, "#F59E0B"), "64 % amber")
            verify(Qt.colorEqual(findOne(cores[4], "sysCoreFill").color, "#EF4444"), "86 % red")
            ov.destroy()
        }

        function test_2_header() {
            // roomy: the caption, the load and the frequency
            var ov = overview(700, cellPx(2, 1.11), 1.11, mon)
            wait(20)
            compare(findOne(ov, "sysCpu").text, "40 %")
            verify(shown(findOne(ov, "sysCpuCaption")), "the caption")
            var ghz = findOne(ov, "sysCpuGhz")
            verify(shown(ghz), "the frequency")
            compare(ghz.text, "2.40 GHz")
            var chip = findOne(ov, "sysTemp")
            verify(shown(chip), "the temperature")
            compare(chip.children[0].text, "52\u00b0")
            verify(Qt.colorEqual(chip.border.color, "#22C55E"), "52 green")
            mon.cpuTempC = 65
            verify(Qt.colorEqual(chip.border.color, "#F59E0B"), "65 amber")
            mon.cpuTempC = 80
            verify(Qt.colorEqual(chip.border.color, "#EF4444"), "80 red")
            mon.cpuTempC = NaN
            verify(!chip.visible, "no temperature, no chip")
            mon.cpuTempC = 52
            var mark = findOne(ov, "sysThrottled")
            verify(!mark.visible)
            mon.throttled = true
            verify(shown(mark), "held below its maximum")
            verify(mark.x + mark.width <= chip.x, "left of the temperature")
            mon.throttled = false
            mon.cpuLoad = -1
            compare(findOne(ov, "sysCpu").text, "\u2014", "no load yet")
            mon.cpuLoad = 0.40
            ov.destroy()
            // the smallest cell is compact: no header, its CPU line instead (test_3b)
            var small = overview(cellPx(1, 1.11), cellPx(1, 1.11), 1.11, mon)
            wait(20)
            verify(small._compact, "1 x 1: compact")
            verify(!shown(findOne(small, "sysCpu")) && !shown(findOne(small, "sysCores")), "no header, no cores")
            small.destroy()
        }

        function rowParts(r) {
            return { label: findOne(r, "sysRowLabel"), pill: findOne(r, "sysRowPill"), value: findOne(r, "sysRowValue"),
                     fill: findOne(r, "sysRowFill") }
        }

        function test_3_rows() {
            // roomy: every line in full
            var ov = overview(700, cellPx(2, 1.11), 1.11, mon)
            wait(20)
            verify(ov._wide, "two columns")
            var rows = byIndex(findAll(ov, "sysRow", []))
            compare(rows.length, 3, "memory and two volumes on the right")
            for (var i = 0; i < 3; ++i) { verify(rows[i].visible, "row " + i); compare(rows[i].form, 0, "row " + i + " in full") }
            var r0 = rowParts(rows[0]), r1 = rowParts(rows[1]), r2 = rowParts(rows[2])
            compare(r0.label.text, "RAM")
            compare(r0.value.text, "3.1 / 15.6 GB")
            verify(!r0.pill.visible)
            compare(r1.label.text, "/")
            verify(shown(r1.pill), "the disk's kind and wear")
            compare(r1.pill.children[0].text, "eMMC \u226410 %")
            verify(Qt.colorEqual(r1.pill.border.color, "#22C55E"), "10 % of its life: green")
            compare(r1.value.text, "8.7 / 11.7 GB")
            compare(r2.label.text, "/data")
            verify(!r2.pill.visible, "the disk's kind on its first volume only")
            compare(r2.value.text, "0.1 / 16.5 GB")
            for (i = 0; i < 3; ++i) {
                var p = rowParts(rows[i])
                var f = rows[i].frac
                compare(p.fill.width, Math.max(p.fill.height, Math.round(p.fill.parent.width * f)), "row " + i + " fill")
                verify(Qt.colorEqual(p.fill.color, DataFieldCatalog.sysFillColor(f)), "row " + i + " colour")
                verify(p.label.x + p.label.width <= p.value.x, "row " + i + ": the label left of the amount")
            }
            fuzzyCompare(rows[0].frac, 3.1 / 15.6, 1e-9)
            verify(Qt.colorEqual(r0.fill.color, "#38BDF8"), "20 % blue")
            verify(Qt.colorEqual(r1.fill.color, "#F59E0B"), "74 % amber")
            var foot = findOne(ov, "sysFoot")
            verify(shown(foot), "the footer")
            compare(findOne(ov, "sysUptime").text, "up 3 h 12 min")
            var bottom = foot.mapToItem(ov, 0, foot.height).y
            fuzzyCompare(bottom, ov.height - ov._pad, 1, "wide: spread down to the bottom")
            ov.destroy()
            // hot: the worn eMMC in red, nearly full memory in red
            var hotOv = overview(700, cellPx(2, 1.11), 1.11, hot)
            wait(20)
            var hrows = byIndex(findAll(hotOv, "sysRow", []))
            var h1 = rowParts(hrows[1])
            compare(h1.pill.children[0].text, "eMMC worn out")
            verify(Qt.colorEqual(h1.pill.border.color, "#EF4444"), "worn out in red")
            compare(rowParts(hrows[2]).pill.children[0].text, "USB", "a stick: its kind, no wear")
            verify(Qt.colorEqual(rowParts(hrows[0]).fill.color, "#EF4444"), "95 % red")
            compare(findOne(hotOv, "sysUptime").text, "up 3 d 14 h")
            hotOv.destroy()
            // a small wide cell (2 x 1): the amounts kept, shortened (the unit, the spaces); the disk as the name
            var two = overview(cellPx(2, 1.11), cellPx(1, 1.11), 1.11, mon)
            wait(20)
            var trows = byIndex(findAll(two, "sysRow", []))
            verify(/3\.1 ?\/ ?15\.6/.test(rowParts(trows[0]).value.text), "memory in numbers: " + rowParts(trows[0]).value.text)
            verify(/8\.7 ?\/ ?11\.7/.test(rowParts(trows[1]).value.text), "the eMMC in numbers: " + rowParts(trows[1]).value.text)
            verify(rowParts(trows[1]).label.text === "eMMC" || rowParts(trows[1]).pill.children[0].text.indexOf("eMMC") === 0,
                   "the disk named: " + rowParts(trows[1]).label.text)
            two.destroy()
        }

        // The owner's 1 x 1 (2026-10-01): the CPU and the GPU, each with its temperature and load, memory and the
        // system volume in numbers - at both scales, in every mock, in the font given, whichever layout it takes.
        function test_3c_rows_kept() {
            // A new reading keeps the lines, only their texts change: made anew each second (the array as the
            // Repeater's model) they jerked the head unit's UI ~150 ms a second (2026-10-22).
            var ov = overview(700, cellPx(2, 1.11), 1.11, mon)
            var was = mon.memUsedGB
            try {
                wait(20)
                var before = byIndex(findAll(ov, "sysRow", []))
                compare(before.length, 3)
                mon.memUsedGB = 4.2
                wait(20)
                var after = byIndex(findAll(ov, "sysRow", []))
                compare(after.length, 3)
                for (var i = 0; i < 3; ++i)
                    verify(after[i] === before[i], "row " + i + " is the same item after a new reading")
                compare(rowParts(after[0]).value.text, "4.2 / 15.6 GB", "and shows the new amount")
            } finally {
                mon.memUsedGB = was   // the next tests read mon as it was
                ov.destroy()
            }
        }

        function test_3b_compact() {
            var mocks = [{ m: mon, name: "head unit", gpu: true }, { m: hot, name: "hot", gpu: true },
                         { m: peak, name: "peak", gpu: true }, { m: win, name: "Windows", gpu: false }]
            var scales = [1.11, 1.5]
            for (var c = 0; c < scales.length; ++c) for (var q = 0; q < mocks.length; ++q) {
                var k = scales[c], s = mocks[q].m
                var tag = "1x1 k " + k + " " + mocks[q].name
                var ov = overview(cellPx(1, k), cellPx(1, k), k, s)
                wait(10)
                var rows = byIndex(findAll(ov, "sysRow", [])).filter(function(r) { return r.visible })
                var byKey = {}
                for (var i = 0; i < rows.length; ++i) byKey[rows[i].line.key] = rowParts(rows[i])
                var load = Math.round(s.cpuLoad * 100), temp = Math.round(s.cpuTempC) + "\u00b0"
                if (ov._compact) {
                    var cpu = byKey["cpu"]
                    verify(cpu !== undefined, tag + ": the CPU line")
                    verify(new RegExp("^" + load + " ?%").test(cpu.value.text), tag + ": the CPU's load " + cpu.value.text)
                    if (isFinite(s.cpuTempC))
                        verify(shown(cpu.pill) && cpu.pill.children[0].text.indexOf(temp) >= 0, tag + ": the CPU's temperature")
                } else {
                    verify(new RegExp("^" + load + " ?%").test(findOne(ov, "sysCpu").text), tag + ": the CPU's load in the header")
                    if (isFinite(s.cpuTempC))
                        verify(shown(findOne(ov, "sysTemp")), tag + ": the CPU's temperature in the header")
                }
                if (mocks[q].gpu) {
                    var gpu = byKey["gpu"]
                    verify(gpu !== undefined, tag + ": the GPU line")
                    verify(new RegExp("^" + Math.round(s.gpuLoad * 100) + " ?%").test(gpu.value.text), tag + ": the GPU's load " + gpu.value.text)
                    verify(shown(gpu.pill) && gpu.pill.children[0].text === Math.round(s.gpuTempC) + "\u00b0", tag + ": the GPU's temperature")
                } else {
                    verify(byKey["gpu"] === undefined, tag + ": no GPU line without a GPU")
                }
                var ram = byKey["ram"]
                verify(ram !== undefined && /\d+\.\d ?\/ ?\d+(\.\d)?/.test(ram.value.text), tag + ": memory in numbers " + (ram ? ram.value.text : ""))
                var dataAt = 0
                for (var vi = 0; vi < s.volumes.length; ++vi) if (s.volumes[vi].data === true) { dataAt = vi; break }
                var vol = byKey["vol" + dataAt]
                verify(vol !== undefined && /\d+\.\d ?\/ ?\d+(\.\d)?/.test(vol.value.text), tag + ": the data volume in numbers " + (vol ? vol.value.text : ""))
                if (s === mon)
                    compare(vol.label.text, "/data", tag + ": the head unit's data partition")
                var tx = texts(ov, [])
                for (i = 0; i < tx.length; ++i)
                    if (shown(tx[i])) verify(!tx[i].truncated, tag + ": \"" + tx[i].text + "\" cut")
                ov.destroy()
            }
        }

        function test_3a_gpu() {
            // wide: under the cores, on the left
            var ov = overview(700, cellPx(2, 1.11), 1.11, mon)
            wait(20)
            var g = findOne(ov, "sysGpuRow")
            verify(shown(g), "the GPU line")
            verify(g.mapToItem(ov, 0, 0).x < ov.width / 2, "on the left")
            var box = findOne(ov, "sysCores")
            verify(g.mapToItem(ov, 0, 0).y >= box.mapToItem(ov, 0, box.height).y, "under the cores")
            fuzzyCompare(g.mapToItem(ov, 0, g.height).y, ov.height - ov._pad, 1, "down to the bottom")
            var gp = rowParts(g)
            compare(gp.label.text, "GPU")
            verify(shown(gp.pill), "its temperature")
            compare(gp.pill.children[0].text, "48\u00b0")
            verify(Qt.colorEqual(gp.pill.border.color, "#22C55E"), "48 green")
            compare(gp.value.text, "12 % \u00b7 300 MHz")
            compare(gp.fill.width, Math.max(gp.fill.height, Math.round(gp.fill.parent.width * 0.12)), "the bar: its load")
            verify(Qt.colorEqual(gp.fill.color, "#22C55E"), "12 % green")
            mon.gpuLoad = -1
            compare(gp.value.text, "300 MHz", "no load reported: the frequency")
            compare(gp.fill.width, Math.max(gp.fill.height, Math.round(gp.fill.parent.width * 0.3)), "no load: 300 of 1000 MHz")
            verify(Qt.colorEqual(gp.fill.color, AppPalette.systemFill), "no load: the plain colour")
            mon.gpuLoad = 0.12
            ov.destroy()
            // one column: the first line under the cores
            var sq = overview(cellPx(2, 1.11), cellPx(2, 1.11), 1.11, mon)
            wait(20)
            verify(!findOne(sq, "sysGpuRow").visible, "not on the left in one column")
            var srows = byIndex(findAll(sq, "sysRow", []))
            compare(srows[0].line.key, "gpu")
            verify(srows[0].visible)
            sq.destroy()
            // hot: a red chip and bar, the top frequency
            var h = overview(700, cellPx(2, 1.11), 1.11, hot)
            wait(20)
            var hp = rowParts(findOne(h, "sysGpuRow"))
            compare(hp.value.text, "97 % \u00b7 1000 MHz")
            verify(Qt.colorEqual(hp.pill.border.color, "#EF4444"), "81 red")
            verify(Qt.colorEqual(hp.fill.color, "#EF4444"), "97 % red")
            compare(hp.fill.width, Math.round(hp.fill.parent.width * 0.97), "97 % of the bar")
            h.destroy()
            // nothing known of the GPU (the Windows mock): no line
            var w = overview(700, cellPx(2, 1.11), 1.11, win)
            wait(20)
            verify(!findOne(w, "sysGpuRow").visible)
            verify(findAll(w, "sysRow", []).every(function(r) { return r.line.key !== "gpu" }), "no GPU line")
            w.destroy()
        }

        function test_4_sizes() {
            var units = [[1, 1], [2, 1], [2, 2], [4, 2]]
            var scales = [1.11, 1.5]
            var sources = [mon, hot, peak, win]
            for (var u = 0; u < units.length; ++u) for (var c = 0; c < scales.length; ++c) for (var m = 0; m < sources.length; ++m) {
                var k = scales[c]
                var w = cellPx(units[u][0], k), h = cellPx(units[u][1], k)
                var ov = overview(w, h, k, sources[m])
                wait(10)
                var tag = units[u][0] + "x" + units[u][1] + " k " + k + " #" + m
                compare(ov._wide, units[u][0] > units[u][1], tag + " two columns when wide")
                var all = []
                var walk = function(it) {
                    var kids = it.children || []
                    for (var i = 0; i < kids.length; ++i) { if (shown(kids[i])) { all.push(kids[i]); walk(kids[i]) } }
                }
                walk(ov)
                for (var i = 0; i < all.length; ++i) {
                    if (all[i].width <= 0 || all[i].height <= 0) continue
                    var p = all[i].mapToItem(ov, 0, 0)
                    verify(p.x >= -0.5 && p.y >= -0.5 && p.x + all[i].width <= w + 0.5 && p.y + all[i].height <= h + 0.5,
                           tag + ": " + (all[i].objectName || all[i]) + " inside the cell at " + p.x + "," + p.y + " "
                           + all[i].width + "x" + all[i].height)
                }
                var tx = texts(ov, [])
                for (i = 0; i < tx.length; ++i)
                    if (shown(tx[i]) && tx[i].objectName !== "sysRowLabel") verify(!tx[i].truncated, tag + ": \"" + tx[i].text + "\" cut")
                // a volume's name is cut only when nothing else is left to drop
                var labels = findAll(ov, "sysRowLabel", [])
                for (i = 0; i < labels.length; ++i)
                    if (shown(labels[i]) && labels[i].truncated) {
                        compare(labels[i].parent.form, labels[i].parent._cands.length - 1, tag + ": \"" + labels[i].text + "\" cut before the rest gave way")
                        verify(!rowParts(labels[i].parent).pill.visible, tag + ": \"" + labels[i].text + "\" cut for the disk's kind")
                    }
                var rows = byIndex(findAll(ov, "sysRow", []))
                for (i = 1; i < rows.length; ++i)
                    verify(!rows[i].visible || rows[i - 1].visible, tag + ": rows left out from the bottom up")
                var foot = findOne(ov, "sysFoot")
                if (!ov._wide && foot.visible)
                    verify(rows.every(function(r) { return r.visible }), tag + ": the footer after all the rows")
                verify(rows.length === 0 || rows[0].visible, tag + ": the first line always")
                var gpuRow = findOne(ov, "sysGpuRow")
                if (shown(gpuRow)) rows.push(gpuRow)
                // on a line: the name, then the disk, then the amount, none over another
                for (i = 0; i < rows.length; ++i) {
                    if (!rows[i].visible) continue
                    var rp = rowParts(rows[i])
                    var labelEnd = rp.label.x + rp.label.width
                    if (rp.pill.visible) {
                        verify(labelEnd <= rp.pill.x, tag + ": row " + i + " the name over the disk")
                        verify(rp.pill.x + rp.pill.width <= rp.value.x, tag + ": row " + i + " the disk over the amount")
                    } else {
                        verify(labelEnd <= rp.value.x, tag + ": row " + i + " the name over the amount")
                    }
                }
                // each core's bar no thinner than the gaps between them
                var box = findOne(ov, "sysCores")
                if (box.n > 1)
                    verify(box.barW >= Math.max(1, box.gap), tag + ": bars " + box.barW + " px against gaps " + box.gap)
                ov.destroy()
            }
            // every width of a cell one and two units high: no text cut but a name with nothing else to give
            var hs = [cellPx(1, 1.11), cellPx(2, 1.11)]
            for (var hi = 0; hi < hs.length; ++hi) {
                var sweep = overview(80, hs[hi], 1.11, mon)
                for (var ww = 80; ww <= 420; ww += 2) {
                    sweep.width = ww
                    var swTag = "w " + ww + " h " + hs[hi]
                    var swTexts = texts(sweep, [])
                    for (var ti = 0; ti < swTexts.length; ++ti) {
                        var tt = swTexts[ti]
                        if (!shown(tt) || !tt.truncated) continue
                        verify(tt.objectName === "sysRowLabel" && tt.parent.form === tt.parent._cands.length - 1,
                               swTag + ": \"" + tt.text + "\" cut")
                    }
                }
                sweep.destroy()
            }
            // every height of a column and of a wide cell: the footer only after all the rows, nothing past the bottom
            var probes = [{ w: cellPx(2, 1.11), wide: false }, { w: cellPx(4, 1.11), wide: true }]
            for (var q = 0; q < probes.length; ++q) {
                var sw = overview(probes[q].w, 60, 1.11, mon)
                for (var hh = 60; hh <= 260; hh += 1) {
                    sw.height = hh
                    var t2 = (probes[q].wide ? "wide" : "column") + " h " + hh
                    if (sw._showFoot)
                        compare(sw._rowsShown, sw._infoRows.length, t2 + ": the footer after all the rows")
                    var used = sw._rowsH + (sw._showFoot ? (sw._rowsShown > 0 ? sw._gapY : 0) + sw._lineH : 0)
                    verify(used <= sw._infoRoom + 0.01, t2 + ": " + used + " px of " + sw._infoRoom)
                    if (!sw._wide)
                        verify(sw._headH + sw._gapY + sw._cores1H <= sw._innerH, t2 + ": the cores past the bottom")
                }
                sw.destroy()
            }
        }

        function test_5_empty() {
            var ov = overview(cellPx(2, 1.11), cellPx(2, 1.11), 1.11, null)
            wait(20)
            verify(findOne(ov, "sysNoData").visible, "the dash")
            compare(findAll(ov, "sysRow", []).length, 0)
            ov.sys = { available: false }
            verify(findOne(ov, "sysNoData").visible)
            ov.sys = mon
            verify(!findOne(ov, "sysNoData").visible)
            ov.destroy()
        }

        function test_6_catalog() {
            var keys = ["sysOverview", "sysCpu", "sysCpuTemp", "sysGpu", "sysRam", "sysStorage", "sysUptime"]
            for (var i = 0; i < keys.length; ++i) {
                verify(DataFieldCatalog.hasField(keys[i]), keys[i])
                verify(DataFieldCatalog.usesSystem(keys[i]), keys[i])
                verify(DataFieldCatalog.isValid(keys[i], null, null, null, null, undefined, null, mon), keys[i])
                verify(!DataFieldCatalog.isValid(keys[i], null, null, null, null, undefined, null, null), keys[i])
                compare(DataFieldCatalog.formatValue(keys[i], null, null, null, null, undefined, null, null), "\u2014", keys[i])
                verify(String(DataFieldCatalog.sampleValue(keys[i], null)).length > 0, keys[i])
            }
            verify(!DataFieldCatalog.usesSystem("depth"))
            function fmt(key, s) { return DataFieldCatalog.formatValue(key, null, null, null, null, undefined, null, s) }
            compare(fmt("sysOverview", mon), "System")
            compare(fmt("sysCpu", mon), "2.40 GHz \u00b7 40 %")
            compare(fmt("sysCpuTemp", mon), "52 \u00b0C")
            compare(fmt("sysCpuTemp", hot), "86 \u00b0C \u25bc")
            compare(fmt("sysGpu", mon), "12 % \u00b7 300 MHz \u00b7 48 \u00b0C")
            compare(fmt("sysGpu", win), "\u2014", "nothing known of the GPU")
            verify(!DataFieldCatalog.isValid("sysGpu", null, null, null, null, undefined, null, win))
            fuzzyCompare(DataFieldCatalog.bars("sysGpu", null, mon)[0].v, 0.12, 1e-9)
            compare(DataFieldCatalog.bars("sysGpu", null, win).length, 0)
            compare(fmt("sysRam", mon), "3.1 / 15.6 GB")
            compare(fmt("sysStorage", mon), "0.1 / 16.5 GB", "the volume with the app's data")
            compare(fmt("sysStorage", hot), "11.4 / 11.7 GB", "none marked: the first")
            compare(fmt("sysStorage", win), "512.0 / 930 GB")
            compare(fmt("sysUptime", mon), "3 h 12 min")
            compare(fmt("sysUptime", hot), "3 d 14 h")
            var b = DataFieldCatalog.bars("sysCpu", null, mon)
            compare(b.length, 2)
            fuzzyCompare(b[0].v, 0.40, 1e-9)
            verify(Qt.colorEqual(b[0].c, "#22C55E"))
            b = DataFieldCatalog.bars("sysCpuTemp", null, mon)
            fuzzyCompare(b[0].v, (52 - 20) / 80, 1e-9)
            b = DataFieldCatalog.bars("sysRam", null, mon)
            fuzzyCompare(b[0].v, 3.1, 1e-9)
            fuzzyCompare(b[1].v, 12.5, 1e-9)
            b = DataFieldCatalog.bars("sysStorage", null, hot)
            verify(Qt.colorEqual(b[0].c, "#EF4444"), "a nearly full disk in red")
            compare(DataFieldCatalog.bars("sysCpuTemp", null, win).length, 0, "no temperature, no bar")
            compare(DataFieldCatalog.bars("depth", null, mon).length, 0)
            var s = DataFieldCatalog.systemSample
            verify(s.available && s.coreLoad.length === 8 && s.coreMaxMHz[7] === 2400 && s.volumes.length === 2)
        }

        function test_7_subscription() {
            mon.subs = 0
            var cell = Qt.createQmlObject('import QtQuick; WidgetCellContent { width: 191; height: 191; rep: "value"; field: "sysRam" }', root)
            cell.sys = mon
            compare(mon.subs, 1, "a system field subscribes")
            cell.field = "depth"
            compare(mon.subs, 0, "another field lets go")
            cell.field = "sysOverview"
            compare(mon.subs, 1)
            var ov = findOne(cell, "sysOverview")
            verify(ov.visible, "System drawn whole")
            compare(findAll(ov, "sysCore", []).length, 8)
            var other = Qt.createQmlObject('import QtQuick; WidgetCellContent { width: 93; height: 93; field: "sysCpu" }', root)
            other.sys = mon
            compare(mon.subs, 2, "each cell counts")
            other.destroy()
            tryCompare(mon, "subs", 1, 2000, "a cell gone lets go")
            cell.sys = DataFieldCatalog.systemSample
            compare(mon.subs, 0, "the palette's sample has nothing to subscribe")
            compare(findAll(ov, "sysCore", []).length, 8, "the sample drawn")
            cell.sys = mon
            compare(mon.subs, 1)
            cell.destroy()
            tryCompare(mon, "subs", 0, 2000)
        }

        function test_8_picture() {
            if (!"%PICTURE%".length)
                return
            var k = 1.11
            var specs = [
                { x: 20,  y: 40,  u: [1, 1], s: mon }, { x: 140, y: 40, u: [2, 1], s: mon },
                { x: 360, y: 40,  u: [2, 2], s: mon }, { x: 580, y: 40, u: [4, 2], s: mon },
                { x: 20,  y: 270, u: [1, 1], s: null }, { x: 140, y: 270, u: [2, 2], s: hot },
                { x: 360, y: 270, u: [4, 2], s: hot }, { x: 20, y: 500, u: [4, 2], s: win },
                { x: 440, y: 500, u: [2, 1], s: hot }, { x: 660, y: 500, u: [1, 1], s: hot } ]
            for (var i = 0; i < specs.length; ++i) {
                var w = cellPx(specs[i].u[0], k), h = cellPx(specs[i].u[1], k)
                var card = Qt.createQmlObject('import QtQuick; import kqml_types 1.0; Rectangle { radius: 10; color: AppPalette.bg }', root)
                card.x = specs[i].x; card.y = specs[i].y; card.width = w + 16; card.height = h + 16
                var ov = Qt.createQmlObject('import QtQuick; SystemOverview { x: 8; y: 8 }', card)
                ov.k = k; ov.width = w; ov.height = h; ov.sys = specs[i].s
            }
            wait(150)
            grabImage(root).save("%PICTURE%")
        }
    }
}
'@

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('sysgui_check_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    New-ModuleDir $work
    Copy-Item (Join-Path $repo 'qml\app\SystemOverview.qml') (Join-Path $work 'SystemOverview.qml')
    Copy-Item (Join-Path $repo 'qml\app\WidgetCellContent.qml') (Join-Path $work 'WidgetCellContent.qml')
    $pic = if ($Picture) { [System.IO.Path]::GetFullPath($Picture).Replace('\', '/') } else { '' }
    $path = Join-Path $work 'tst_sysgui.qml'
    Write-Utf8 $path $test.Replace('%PICTURE%', $pic)
    $env:PATH = "$QtBin;$env:PATH"
    if (-not $FontDir) {
        $FontDir = Join-Path $work 'fonts'
        New-Item -ItemType Directory -Force $FontDir | Out-Null
        Copy-Item (Join-Path $repo 'resources\fonts\arial.ttf') $FontDir
    }
    $env:QT_QPA_FONTDIR = (Resolve-Path $FontDir).Path
    $results = Join-Path $work 'results.txt'
    $p = Start-Process -FilePath $runner -NoNewWindow -Wait -PassThru `
        -ArgumentList @('-platform', 'offscreen', '-import', "`"$(Join-Path $work 'imports')`"", '-input', "`"$path`"",
                        '-o', "`"$results,txt`"") `
        -RedirectStandardOutput (Join-Path $work 'run.out') -RedirectStandardError (Join-Path $work 'run.err')
    $out = @()
    if (Test-Path $results) { $out += @(Get-Content $results -Encoding UTF8) }
    $out | Where-Object { $Raw -or $_ -match '^(PASS|FAIL|Totals|   Actual|   Expected|   Loc)' } | ForEach-Object { Write-Host $_ }
    if ($p.ExitCode -ne 0 -or -not (Test-Path $results)) {
        @(Get-Content (Join-Path $work 'run.err') -Encoding UTF8) | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
        Write-Host 'sysgui_check: failed'
        $code = 1
    } else {
        Write-Host 'sysgui_check: all passed'
        $code = 0
    }
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
exit $code

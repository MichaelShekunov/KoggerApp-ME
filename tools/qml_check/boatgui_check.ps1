<#
.SYNOPSIS
  Offline check of the boat's mission progress, track by link quality, ghost and the "On the link" widget
  (owner 2026-09-30; docs/HANDOFF.md).

.DESCRIPTION
  Runs the real qml/scene3d/BoatMissionLayer.qml, qml/scene3d/BoatMissionPanel.qml, qml/app/WidgetCellContent.qml
  and the kqml_types sources (DataFieldCatalog, AppPalette, ...) over a mock view (a linear projection, with
  geoToScreenList) and a mock vehicle and wrapper:
    1. the track: a stretch per quality run in its colour, the stretch over a loss dashed; hidden when switched off;
    2. the ghost: at its projected place, "no link N s" under it, a dashed line from where the boat was seen;
    3. the panel: the progress line (left, time, battery at the end), the ghost line, the track switch and legend;
    4. the catalog: the new fields valid and formatted from the mocks, "\u2014" without them; the bar parts;
    5. the widget cell: the bar's parts as wide as their shares.
  -Picture saves the map layer with the track and the ghost. Exit code 0 when all pass, 1 when a check fails, 2 when
  qmltestrunner is not found.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\qml_check\boatgui_check.ps1 [-QtBin D:\Qt\6.8.3\llvm-mingw_64\bin] [-Picture gui.png]
#>
param(
    [string]$QtBin = $(if ($env:QT_BIN) { $env:QT_BIN } else { 'D:\Qt\6.8.3\llvm-mingw_64\bin' }),
    [string]$Picture = ''
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runner = Join-Path $QtBin 'qmltestrunner.exe'
if (-not (Test-Path $runner)) {
    Write-Host "boatgui_check: no qmltestrunner at $runner (pass -QtBin or set QT_BIN)"
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
import QtQuick.Shapes
import QtTest
import kqml_types 1.0

Item {
    id: root
    width: 800; height: 600

    Rectangle { anchors.fill: parent; color: "#0B1A3A" }

    // A linear map: 1e-5 degree = 1 px, (60, 30) at (400, 300), north up.
    QtObject {
        id: view
        signal cameraChanged()
        function geoToScreen(lat, lon) { return Qt.point(400 + (lon - 30) * 1e5, 300 - (lat - 60) * 1e5) }
        function geoToScreenList(lats, lons) {
            var out = []
            for (var i = 0; i < lats.length; ++i)
                out.push(geoToScreen(lats[i], lons[i]))
            return out
        }
        function screenToGeo(x, y) { return { valid: true, lat: 60 + (300 - y) / 1e5, lon: 30 + (x - 400) / 1e5 } }
    }

    QtObject {
        id: veh
        // planChanged and linkTrackShownChanged come with their properties
        signal positionChanged(); signal homeChanged(); signal missionStateChanged()
        signal linkTrackChanged(); signal ghostChanged()
        property var plan: []
        property bool homeValid: true
        property real homeLatitude: 60.0
        property real homeLongitude: 30.0
        property bool positionValid: true
        property real latitude: 60.001
        property real longitude: 30.001
        property int currentPlanIndex: -1
        property bool armed: true
        property int mode: 11
        property bool planReturnHome: false
        // the track: good, good, weak, then a loss
        property bool linkTrackShown: true
        property var linkTrackLats: [60.0, 60.0005, 60.001, 60.0015, 60.002]
        property var linkTrackLons: [30.0, 30.0, 30.0, 30.0, 30.0]
        property var linkTrackQualities: [0, 0, 0, 2, 5]
        // the ghost
        property bool ghostValid: true
        property bool ghostMoving: true
        property real ghostLatitude: 60.0008
        property real ghostLongitude: 30.001
        property int ghostAgeS: 12
        // progress
        property bool missionProgressValid: true
        property real missionRemainingM: 1240
        property real missionEtaS: 840
        property int missionBatteryAtEnd: 46
        property bool detected: true
        property bool connected: false
        property int heartbeatAgeS: 12
        property string modeName: "RTL"
        function distanceM(a, b, c, d) { return Math.hypot((c - a) * 111195, (d - b) * 55597) }
        function clearLinkTrack() { linkTrackLats = []; linkTrackLons = []; linkTrackQualities = []; linkTrackChanged() }
    }

    QtObject {
        id: dmw
        property bool airValid: true
        property real airEchogramKBs: 3.6
        property real airTelemetryKBs: 2.7
        property real airServiceKBs: 0.9
        property int airRssi: -61
    }

    BoatMissionLayer { id: layer; anchors.fill: parent; view: view; vehicle: veh }

    function shapes() {
        var out = []
        for (var i = 0; i < layer.children.length; ++i) {
            var c = layer.children[i]
            if (c.hasOwnProperty("preferredRendererType") && c.hasOwnProperty("modelData"))
                out.push(c)
        }
        return out
    }
    function find(item, pred) {
        if (!item) return null
        if (pred(item)) return item
        var kids = item.children || []
        for (var i = 0; i < kids.length; ++i) {
            var r = find(kids[i], pred)
            if (r) return r
        }
        return null
    }

    TestCase {
        name: "BoatGui"
        when: windowShown

        function test_1_track_runs() {
            tryVerify(function() { return shapes().length === 3 }, 2000, "three runs: good, weak, lost")
            var runs = layer._trackRuns
            compare(runs[0].q, 0)
            compare(runs[0].pts.length, 3)            // the first two stretches, one polyline
            compare(runs[1].q, 2)
            compare(runs[2].q, 5)
            var s = shapes()
            verify(Qt.colorEqual(s[0].data[0].strokeColor, AppPalette.linkQualityColors[0]), String(s[0].data[0].strokeColor))
            compare(s[2].data[0].strokeStyle, ShapePath.DashLine, "the stretch over a loss dashed")
            compare(s[0].data[0].strokeStyle, ShapePath.SolidLine)
            veh.linkTrackShown = false
            tryVerify(function() { return shapes().length === 0 }, 2000, "switched off: no track")
            veh.linkTrackShown = true
            tryVerify(function() { return shapes().length === 3 }, 2000)
        }

        function test_2_ghost() {
            var mark = find(layer, function(c) { return c.hasOwnProperty("z") && c.z === 3 && c.width === layer.markerSize })
            verify(mark !== null && mark.visible, "the ghost marker")
            var p = view.geoToScreen(veh.ghostLatitude, veh.ghostLongitude)
            fuzzyCompare(mark.x + mark.width / 2, p.x, 1)
            fuzzyCompare(mark.y + mark.height / 2, p.y, 1)
            var label = find(mark, function(c) { return c.hasOwnProperty("text") && String(c.text).indexOf("12") >= 0 })
            verify(label !== null, "no link 12 s under it")
            veh.ghostValid = false
            veh.ghostChanged()
            tryVerify(function() { return !mark.visible }, 2000, "gone with the link back")
            veh.ghostValid = true
            veh.ghostChanged()
        }

        function test_3_panel() {
            var panel = Qt.createQmlObject('import QtQuick; BoatMissionPanel { maxHeight: 2000 }', root)
            panel.vehicle = veh
            var progress = find(panel, function(c) { return c.hasOwnProperty("text") && String(c.text).indexOf("1.24") >= 0 })
            verify(progress !== null && progress.visible, "the progress line")
            verify(String(progress.text).indexOf("14") >= 0 && String(progress.text).indexOf("46") >= 0, progress.text)
            var ghost = find(panel, function(c) { return c.hasOwnProperty("text") && String(c.text).indexOf("No link 12") === 0 })
            verify(ghost !== null && ghost.visible, "the ghost line")
            var legend = find(panel, function(c) { return c.hasOwnProperty("text") && c.text === qsTr("lost") })
            verify(legend !== null && legend.visible, "the track legend")
            veh.missionProgressValid = false
            verify(!progress.visible)
            veh.missionProgressValid = true
            panel.destroy()
        }

        function test_4_catalog() {
            compare(DataFieldCatalog.formatValue("apMissionLeft", null, dmw, null, null, undefined, veh), "1.24 " + qsTr("km"))
            compare(DataFieldCatalog.formatValue("apMissionEta", null, dmw, null, null, undefined, veh), "14 " + qsTr("min"))
            compare(DataFieldCatalog.formatValue("apBatteryAtEnd", null, dmw, null, null, undefined, veh), "46 %")
            compare(DataFieldCatalog.formatValue("linkAir", null, dmw, null, null, undefined, veh), "7.2 " + qsTr("KB/s"))
            compare(DataFieldCatalog.formatValue("linkRssi", null, dmw, null, null, undefined, veh), "-61 " + qsTr("dBm"))
            verify(DataFieldCatalog.isValid("linkAir", null, dmw, null, null, undefined, veh))
            compare(DataFieldCatalog.formatValue("apMissionLeft", null, null, null, null, undefined, null), "\u2014")
            compare(DataFieldCatalog.formatValue("linkAir", null, null, null, null, undefined, null), "\u2014")
            var b = DataFieldCatalog.bars("linkAir", dmw)
            compare(b.length, 3)
            compare(b[0].v, 3.6)
            compare(DataFieldCatalog.bars("depth", dmw).length, 0)
            verify(DataFieldCatalog.hasField("linkAir") && DataFieldCatalog.hasField("apMissionEta"))
        }

        function test_5_cell_bar() {
            var cell = Qt.createQmlObject('import QtQuick; WidgetCellContent { width: 200; height: 100; rep: "value" }', root)
            cell.value = "7.2 KB/s"
            cell.bars = DataFieldCatalog.bars("linkAir", dmw)
            wait(50)
            var row = find(cell, function(c) { return c.hasOwnProperty("total") })
            verify(row !== null && row.visible, "the bar")
            fuzzyCompare(row.total, 7.2, 1e-6)
            var parts = []
            for (var i = 0; i < row.children.length; ++i)
                if (row.children[i].hasOwnProperty("modelData")) parts.push(row.children[i])
            compare(parts.length, 3)
            fuzzyCompare(parts[0].width, row.width * 3.6 / 7.2, 0.5)
            fuzzyCompare(parts[1].width, row.width * 2.7 / 7.2, 0.5)
            cell.value = "\u2014"
            verify(!row.visible, "no bar without a value")
            cell.destroy()
        }

        function test_6_picture() {
            if (!"%PICTURE%".length)
                return
            wait(100)
            grabImage(root).save("%PICTURE%")
        }
    }
}
'@

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('boatgui_check_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    New-ModuleDir $work
    Copy-Item (Join-Path $repo 'qml\scene3d\BoatMissionLayer.qml') (Join-Path $work 'BoatMissionLayer.qml')
    Copy-Item (Join-Path $repo 'qml\scene3d\BoatMissionPanel.qml') (Join-Path $work 'BoatMissionPanel.qml')
    Copy-Item (Join-Path $repo 'qml\app\WidgetCellContent.qml') (Join-Path $work 'WidgetCellContent.qml')
    $pic = if ($Picture) { [System.IO.Path]::GetFullPath($Picture).Replace('\', '/') } else { '' }
    $path = Join-Path $work 'tst_boatgui.qml'
    Write-Utf8 $path $test.Replace('%PICTURE%', $pic)
    $env:PATH = "$QtBin;$env:PATH"
    $results = Join-Path $work 'results.txt'
    $p = Start-Process -FilePath $runner -NoNewWindow -Wait -PassThru `
        -ArgumentList @('-platform', 'offscreen', '-import', "`"$(Join-Path $work 'imports')`"", '-input', "`"$path`"",
                        '-o', "`"$results,txt`"") `
        -RedirectStandardOutput (Join-Path $work 'run.out') -RedirectStandardError (Join-Path $work 'run.err')
    $out = @()
    if (Test-Path $results) { $out += @(Get-Content $results -Encoding UTF8) }
    $out | Where-Object { $_ -match '^(PASS|FAIL|Totals|   Actual|   Expected|   Loc)' } | ForEach-Object { Write-Host $_ }
    if ($p.ExitCode -ne 0 -or -not (Test-Path $results)) {
        @(Get-Content (Join-Path $work 'run.err') -Encoding UTF8) | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
        Write-Host 'boatgui_check: failed'
        $code = 1
    } else {
        Write-Host 'boatgui_check: all passed'
        $code = 0
    }
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
exit $code

<#
.SYNOPSIS
  Offline check of the split screen's border and handle (owner 2026-09-30; docs/HANDOFF.md).

.DESCRIPTION
  Runs the real code: the kqml_types sources (AppPalette, Tokens, KDragBar, ...) as a module and the split-zone
  delegate cut out of qml/app/WorkspaceView.qml, over three dark panes laid out with PaneFrame's inset rule (a
  vertical split at a fractional x, its right half split again: a T junction) and a mock store. Checks:
    1. the border fills the gap between the panes exactly (2 px, Tokens.paneGapPx), in AppPalette.borderFocus;
    2. the handle is shown without hover, at the small size (AppPalette.splitBar*), centred on the border;
    3. the grab area stays finger-sized (AppPalette.dragBar*) and a first press starts a resize;
    4. in the edit mode (the panes touch) the border is 1 px;
    5. a moved split: the zone moves with the panes' animation and settles on the new gap;
    6. the rendered picture, by pixels: the border's two columns / rows, the panes' colour on both sides, the T
       junction closed.
  Exit code 0 when all pass, 1 when a check fails, 2 when qmltestrunner is not found.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\qml_check\split_check.ps1 [-QtBin D:\Qt\6.8.3\llvm-mingw_64\bin] [-Picture split.png]
#>
param(
    [string]$QtBin = $(if ($env:QT_BIN) { $env:QT_BIN } else { 'D:\Qt\6.8.3\llvm-mingw_64\bin' }),
    [string]$Picture = ''
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runner = Join-Path $QtBin 'qmltestrunner.exe'
if (-not (Test-Path $runner)) {
    Write-Host "split_check: no qmltestrunner at $runner (pass -QtBin or set QT_BIN)"
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

# The split zone: the Repeater's "delegate: Item {" whose next line is "id: splitDragZone", through its brace.
function Get-Delegate {
    $page = [System.IO.File]::ReadAllLines((Join-Path $repo 'qml\app\WorkspaceView.qml'), $utf8)
    for ($i = 0; $i -lt $page.Length - 1; $i++) {
        if ($page[$i].Contains('delegate: Item {') -and $page[$i + 1].Contains('id: splitDragZone')) {
            $depth = 0
            for ($j = $i; $j -lt $page.Length; $j++) {
                $s = ($page[$j] -replace '"[^"]*"', '""')
                $c = $s.IndexOf('//')
                if ($c -ge 0) { $s = $s.Substring(0, $c) }
                $depth += ($s.ToCharArray() | Where-Object { $_ -eq '{' }).Count
                $depth -= ($s.ToCharArray() | Where-Object { $_ -eq '}' }).Count
                if ($depth -le 0) {
                    $block = $page[$i..$j]
                    $block[0] = $block[0].Replace('delegate: Item {', 'Item {')
                    return ($block | ForEach-Object { '        ' + $_ }) -join "`n"
                }
            }
        }
    }
    throw 'split_check: the split-zone delegate not found in WorkspaceView.qml'
}

$template = @'
import QtQuick
import QtTest
import kqml_types 1.0

Item {
    id: root
    width: 800; height: 480

    Rectangle { anchors.fill: parent; color: AppPalette.bg }

    Item {
        id: workspace
        anchors.fill: parent
        property QtObject store: QtObject {
            property int modePickerLeafId: -1
            property int maximizedLeafId: -1
            property int edgeResizeMovingSplitId: -1
            property bool editableMode: false
            property bool pointerOverSidebar: false
            property bool edgeResizeGhostActive: false
            property real edgeResizeGhostCoord: 0
            property bool layoutTransitionSuspended: false
            property int resizeStarts: 0
            function nearestSplitHandleAtPoint(x, y, o, r) { return null }
            function beginResizeForSplitHandle(id, o, x, y) { resizeStarts++; return true }
            function updateEdgeResizePreview(x, y) {}
            function commitEdgeResize() {}
            function cancelEdgeResizePreview() {}
            function setSplitRatioById(id, r) {}
        }

        // Panes as PaneFrame lays them out: the content ends Tokens.paneGapPx before floor(right edge) unless the
        // pane is at the outer edge, and starts at floor(x).
        component Pane: Rectangle {
            property real px; property real py; property real pw; property real ph
            readonly property bool outerRight: px + pw >= workspace.width - 0.5
            readonly property bool outerBottom: py + ph >= workspace.height - 0.5
            x: Math.floor(px)
            y: Math.floor(py)
            width: (outerRight ? px + pw : Math.floor(px + pw) - Tokens.paneGapPx) - x
            height: (outerBottom ? py + ph : Math.floor(py + ph) - Tokens.paneGapPx) - y
            radius: Tokens.paneRadiusPx
            color: "#0B1A3A"
            Rectangle { anchors.centerIn: parent; width: parent.width * 0.6; height: 3; color: "#E6B800" }
        }
        Pane { id: left;  px: 0;     py: 0;     pw: 400.4; ph: 480 }
        Pane { id: rtop;  px: 400.4; py: 0;     pw: 399.6; ph: 240.6 }
        Pane { id: rbot;  px: 400.4; py: 240.6; pw: 399.6; ph: 239.4 }
    }

    Component {
        id: zoneComp
%DELEGATE%
    }

    property var vz: null
    property var hz: null
    Component.onCompleted: {
        vz = zoneComp.createObject(workspace, { handleData: { splitId: 1, orientation: "vertical", x: 400.4, y: 0, width: 0, height: 480 } })
        hz = zoneComp.createObject(workspace, { handleData: { splitId: 2, orientation: "horizontal", x: 400.4, y: 240.6, width: 399.6, height: 0 } })
    }

    function child(zone, pred) {
        for (var i = 0; i < zone.children.length; ++i)
            if (pred(zone.children[i]))
                return zone.children[i]
        return null
    }
    function line(zone) { return child(zone, function(c) { return c.hasOwnProperty("lineStart") }) }
    function bar(zone) { return child(zone, function(c) { return c.hasOwnProperty("dotCount") }) }
    function grab(zone) { return child(zone, function(c) { return c.hasOwnProperty("resizing") }) }
    function absX(item) { return item.mapToItem(workspace, 0, 0).x }
    function absY(item) { return item.mapToItem(workspace, 0, 0).y }

    TestCase {
        name: "Split"
        when: windowShown

        function test_1_border_in_the_gap() {
            var l = line(root.vz)
            verify(l !== null, "the border")
            verify(l.visible)
            compare(absX(l), 398)                       // floor(400.4) - 2
            compare(l.width, 2)
            compare(l.height, 480)
            compare(absX(l), left.x + left.width)       // right where the left pane's content ends ...
            compare(absX(l) + l.width, rtop.x)          // ... up to where the right one starts
            compare(String(l.color), String(AppPalette.borderFocus))
            var h = line(root.hz)
            compare(absY(h), 238)                       // floor(240.6) - 2
            compare(absY(h), rtop.y + rtop.height)
            compare(absY(h) + h.height, rbot.y)
            compare(absX(h), 400)                       // the T junction: from where the right panes start
        }

        function test_2_handle_small_shown_on_the_border() {
            var b = bar(root.vz)
            verify(b !== null, "the handle")
            verify(b.visible && b.opacity >= 0.8, "shown without hover: " + b.opacity)
            compare(b.width, AppPalette.splitBarThicknessPx)
            compare(b.height, AppPalette.splitBarLengthPx)
            verify(b.width * 2 <= AppPalette.dragBarThicknessPx && b.height * 2 <= AppPalette.dragBarLengthPx, "half the old size")
            var l = line(root.vz)
            fuzzyCompare(absX(b) + b.width / 2, absX(l) + l.width / 2, 0.51)    // centred on the border
            fuzzyCompare(absY(b) + b.height / 2, 240, 0.51)                     // halfway along it
            compare(String(b.barColor), String(l.color))
        }

        function test_3_grab_finger_sized_at_the_first_touch() {
            var g = grab(root.vz)
            verify(g.enabled, "grabs without a reveal first")
            compare(g.width, AppPalette.splitHitSizePx)
            compare(g.height, AppPalette.dragBarLengthPx + 2 * 14)
            var before = workspace.store.resizeStarts
            mousePress(g, g.width / 2, g.height / 2)
            mouseRelease(g, g.width / 2, g.height / 2)
            compare(workspace.store.resizeStarts, before + 1, "one press started a resize")
        }

        function test_4_edit_mode_one_pixel() {
            workspace.store.editableMode = true
            var l = line(root.vz)
            compare(l.width, 1)
            compare(absX(l), 399)
            workspace.store.editableMode = false
            compare(line(root.vz).width, 2)
        }

        function test_5_follows_a_moved_split() {
            var l = line(root.vz)
            root.vz.handleData = { splitId: 1, orientation: "vertical", x: 300.7, y: 0, width: 0, height: 480 }
            verify(absX(l) > 298 + 0.5, "moving with the panes' animation, not jumping: " + absX(l))
            tryCompare(root.vz, "x", 300 - Math.floor(AppPalette.splitHitSizePx / 2), 2000)
            compare(absX(l), 298)                      // floor(300.7) - 2 once it has settled
            root.vz.handleData = { splitId: 1, orientation: "vertical", x: 400.4, y: 0, width: 0, height: 480 }
            tryCompare(root.vz, "x", 400 - Math.floor(AppPalette.splitHitSizePx / 2), 2000)
        }

        function test_6_picture() {
            wait(100)
            var img = grabImage(root)
            if ("%PICTURE%".length)
                img.save("%PICTURE%")
            var pane = Qt.color("#0B1A3A"), edge = AppPalette.borderFocus
            function same(c, d) { return Math.abs(c.r - d.r) < 0.02 && Math.abs(c.g - d.g) < 0.02 && Math.abs(c.b - d.b) < 0.02 }
            var row = 100
            verify(same(img.pixel(397, row), pane), "col 397 is the left pane: " + img.pixel(397, row))
            verify(same(img.pixel(398, row), edge), "col 398 is the border: " + img.pixel(398, row))
            verify(same(img.pixel(399, row), edge), "col 399 is the border: " + img.pixel(399, row))
            verify(same(img.pixel(400, row), pane), "col 400 is the right pane: " + img.pixel(400, row))
            var col = 700
            verify(same(img.pixel(col, 237), pane), "row 237 is the top pane: " + img.pixel(col, 237))
            verify(same(img.pixel(col, 238), edge) && same(img.pixel(col, 239), edge), "rows 238-239 the border")
            verify(same(img.pixel(col, 240), pane), "row 240 is the bottom pane: " + img.pixel(col, 240))
            verify(same(img.pixel(400, 238), edge) && same(img.pixel(401, 239), edge), "the T junction closed")
        }
    }
}
'@

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('split_check_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    New-ModuleDir $work
    $pic = if ($Picture) { [System.IO.Path]::GetFullPath($Picture).Replace('\', '/') } else { '' }
    $path = Join-Path $work 'tst_split.qml'
    Write-Utf8 $path ($template.Replace('%DELEGATE%', (Get-Delegate)).Replace('%PICTURE%', $pic))
    $env:PATH = "$QtBin;$env:PATH"
    $results = Join-Path $work 'results.txt'
    $outFile = Join-Path $work 'run.out'
    $errFile = Join-Path $work 'run.err'
    $p = Start-Process -FilePath $runner -NoNewWindow -Wait -PassThru `
        -ArgumentList @('-platform', 'offscreen', '-import', "`"$(Join-Path $work 'imports')`"", '-input', "`"$path`"",
                        '-o', "`"$results,txt`"") `
        -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    $out = @()
    if (Test-Path $results) { $out += @(Get-Content $results -Encoding UTF8) }
    $out | Where-Object { $_ -match '^(PASS|FAIL|Totals|   Actual|   Expected|   Loc)' } | ForEach-Object { Write-Host $_ }
    if ($p.ExitCode -ne 0 -or -not (Test-Path $results)) {
        @(Get-Content $errFile -Encoding UTF8) | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
        Write-Host 'split_check: failed'
        $code = 1
    } else {
        Write-Host 'split_check: all passed'
        $code = 0
    }
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
exit $code

<#
.SYNOPSIS
  Offline check: the boat panel's Close at the bottom, always at hand (owner 2026-09-30; docs/HANDOFF.md).

.DESCRIPTION
  Runs the real qml/scene3d/BoatMissionPanel.qml over the kqml_types sources (no vehicle):
    1. short content: the Close button at the bottom, below the scrolling part, the panel no taller than it needs;
    2. content taller than the panel (a small maxHeight): the panel stops at maxHeight, the content scrolls, and at
       its end the header's X is out of sight while the bottom Close has not moved and is inside the panel;
    3. a tap on it asks to close the panel (closeRequested).
  Exit code 0 when all pass, 1 when a check fails, 2 when qmltestrunner is not found.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\qml_check\boatpanel_check.ps1 [-QtBin D:\Qt\6.8.3\llvm-mingw_64\bin] [-Picture boat.png]
#>
param(
    [string]$QtBin = $(if ($env:QT_BIN) { $env:QT_BIN } else { 'D:\Qt\6.8.3\llvm-mingw_64\bin' }),
    [string]$Picture = ''
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runner = Join-Path $QtBin 'qmltestrunner.exe'
if (-not (Test-Path $runner)) {
    Write-Host "boatpanel_check: no qmltestrunner at $runner (pass -QtBin or set QT_BIN)"
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
    width: 420; height: 900

    BoatMissionPanel { id: panel; x: 10; y: 10; vehicle: null; maxHeight: 1000 }
    SignalSpy { id: closed; target: panel; signalName: "closeRequested" }

    function find(item, pred) {
        if (!item) return null
        if (pred(item)) return item
        var kids = item.children || []
        for (var i = 0; i < kids.length; ++i) {
            var r = find(kids[i], pred)
            if (r) return r
        }
        if (item.contentItem && item.contentItem !== item) {
            var c = find(item.contentItem, pred)
            if (c) return c
        }
        return null
    }
    function footer() {
        for (var i = 0; i < panel.children.length; ++i) {
            var c = panel.children[i]
            if (c.hasOwnProperty("normalBg") && c.text === qsTr("Close")) return c
        }
        return null
    }
    function flick() {
        for (var i = 0; i < panel.children.length; ++i)
            if (panel.children[i].hasOwnProperty("contentHeight")) return panel.children[i]
        return null
    }
    function headerX() { return find(flick(), function(c) { return c.hasOwnProperty("iconSource") && String(c.iconSource).indexOf("x.svg") >= 0 }) }
    function yIn(item) { return item.mapToItem(panel, 0, 0).y }

    TestCase {
        name: "BoatPanel"
        when: windowShown

        function test_1_short_content() {
            var f = footer(), fl = flick()
            verify(f !== null, "a Close button among the panel's own children, not in the scrolling part")
            verify(f.visible && f.enabled)
            verify(fl !== null)
            fuzzyCompare(yIn(f) + f.height, panel.height - panel.pad, 0.5)        // at the bottom
            verify(fl.y + fl.height <= yIn(f) + 0.5, "the scrolling part ends above it")
            verify(panel.height < panel.maxHeight, "no taller than it needs: " + panel.height)
            verify(!fl.interactive, "nothing to scroll")
        }

        function test_2_long_content_close_stays() {
            panel.maxHeight = 160
            var f = footer(), fl = flick()
            tryCompare(panel, "height", 160)
            verify(fl.interactive, "the content scrolls")
            var before = yIn(f)
            fl.contentY = fl.contentHeight - fl.height            // to the end
            wait(50)
            var x = headerX()
            verify(x !== null, "the header's X")
            verify(yIn(x) + x.height <= fl.y + 0.5, "the header's X is out of sight: " + yIn(x))
            compare(yIn(f), before, "the bottom Close did not move")
            verify(yIn(f) >= 0 && yIn(f) + f.height <= panel.height, "inside the panel")
            verify(f.visible && f.enabled)
            fl.contentY = 0
            panel.maxHeight = 1000
        }

        function test_4_picture() {
            if (!"%PICTURE%".length)
                return
            panel.maxHeight = 160
            wait(50)
            grabImage(root).save("%PICTURE%")
            panel.maxHeight = 1000
        }

        function test_3_tap_closes() {
            var f = footer()
            closed.clear()
            mouseClick(f)
            compare(closed.count, 1)
        }
    }
}
'@

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('boatpanel_check_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    New-ModuleDir $work
    Copy-Item (Join-Path $repo 'qml\scene3d\BoatMissionPanel.qml') (Join-Path $work 'BoatMissionPanel.qml')
    $path = Join-Path $work 'tst_boatpanel.qml'
    $pic = if ($Picture) { [System.IO.Path]::GetFullPath($Picture).Replace('\', '/') } else { '' }
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
        Write-Host 'boatpanel_check: failed'
        $code = 1
    } else {
        Write-Host 'boatpanel_check: all passed'
        $code = 0
    }
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
exit $code

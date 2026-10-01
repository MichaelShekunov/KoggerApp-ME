<#
.SYNOPSIS
  Offline check of KScrollStrip and the main menu's device chain in it (owner 2026-10-01: the chain no longer fit
  the menu's width and was cut; docs/HANDOFF.md).

.DESCRIPTION
  Runs the real qml/kqml_types/KScrollStrip.qml (with the kqml_types sources) under qmltestrunner over a row of
  cells standing for the device badges:
    1. narrower than its room: as wide as the row, no scrolling, no clip, no fade;
    2. wider: as wide as its room, clipped; the wheel scrolls it to its end and no further; only a cut edge fades;
    3. a finger drags it;
    4. a tap reaches the cell under it, scrolling or not;
    5. a chain that got shorter, or more room, leaves it in view (not scrolled past its end);
    6. no fade colour, no fade.
  Then reads qml/app/HotActionsPanel.qml: the chain component is a KScrollStrip, and both places that load it (the
  collapsed row and the expanded menu) give it its room (connRoom) and fade (connFade). Exit code 0 when all pass,
  1 when a check fails, 2 when qmltestrunner is not found.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\qml_check\scrollstrip_check.ps1 [-QtBin D:\Qt\6.8.3\llvm-mingw_64\bin]
#>
param(
    [string]$QtBin = $(if ($env:QT_BIN) { $env:QT_BIN } else { 'D:\Qt\6.8.3\llvm-mingw_64\bin' })
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runner = Join-Path $QtBin 'qmltestrunner.exe'
if (-not (Test-Path $runner)) {
    Write-Host "scrollstrip_check: no qmltestrunner at $runner (pass -QtBin or set QT_BIN)"
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
    width: 600; height: 300

    property int clicks: 0

    // a strip of n cells 40 wide, 10 apart (10 cells: 490)
    Component {
        id: stripComp
        KScrollStrip {
            id: s
            property int n: 5
            fadeColor: "#0B1220"
            fadeWidth: 16
            Row {
                spacing: 10
                Repeater {
                    model: s.n
                    delegate: Rectangle {
                        required property int index
                        objectName: "cell" + index
                        width: 40; height: 40
                        color: "#38BDF8"
                        MouseArea { anchors.fill: parent; onClicked: root.clicks++ }
                    }
                }
            }
        }
    }
    function make(props) { return stripComp.createObject(root, props) }
    function find(item, pred) {
        if (!item) return null
        if (pred(item)) return item
        var kids = item.children || []
        for (var i = 0; i < kids.length; ++i) { var r = find(kids[i], pred); if (r) return r }
        return null
    }
    function flickOf(s) { return find(s, function(c) { return c.hasOwnProperty("flickableDirection") }) }
    function named(s, n) { return find(s, function(c) { return c.objectName === n }) }

    TestCase {
        name: "ScrollStrip"
        when: windowShown

        // mouseWheel is the TestCase's own
        function toEnd(s) { for (var i = 0; i < 20; ++i) mouseWheel(s, s.width / 2, 20, 0, -120) }

        function test_1_fits() {
            var s = make({ n: 3, maxWidth: 400 })
            wait(20)
            compare(s.width, 140, "as wide as its row")
            verify(!s.overflows)
            var f = flickOf(s)
            verify(!f.interactive && !f.clip, "no scrolling, no clip")
            verify(!named(s, "stripFadeStart").visible && !named(s, "stripFadeEnd").visible, "no fade")
            s.destroy()
        }

        function test_2_overflows() {
            var s = make({ n: 10, maxWidth: 200 })
            wait(20)
            compare(s.width, 200, "as wide as its room")
            verify(s.overflows)
            var f = flickOf(s)
            verify(f.interactive && f.clip, "scrolls, clipped")
            verify(s.atStart && !s.atEnd)
            verify(!named(s, "stripFadeStart").visible && named(s, "stripFadeEnd").visible, "only the cut end fades")
            mouseWheel(s, 100, 20, 0, -120)
            compare(s.contentX, 120, "the wheel scrolls it")
            verify(named(s, "stripFadeStart").visible && named(s, "stripFadeEnd").visible, "both ends cut")
            toEnd(s)
            compare(s.contentX, 490 - 200, "to its end and no further")
            verify(s.atEnd && !named(s, "stripFadeEnd").visible && named(s, "stripFadeStart").visible)
            mouseWheel(s, 100, 20, -120, 0)
            compare(s.contentX, 490 - 200, "a sideways wheel at the end stays")
            mouseWheel(s, 100, 20, 120, 0)
            compare(s.contentX, 490 - 200 - 120, "and goes back")
            s.destroy()
        }

        function test_3_finger() {
            var s = make({ n: 10, maxWidth: 200 })
            wait(20)
            mouseDrag(s, 180, 20, -140, 0, Qt.LeftButton, Qt.NoModifier, 20)
            tryVerify(function() { return s.contentX > 40 }, 2000, "a drag scrolls it: " + s.contentX)
            s.destroy()
        }

        function test_4_tap() {
            root.clicks = 0
            var s = make({ n: 10, maxWidth: 200 })
            wait(20)
            mouseClick(s, 20, 20)
            compare(root.clicks, 1, "a tap reaches the cell under it while it scrolls")
            var t = make({ n: 3, maxWidth: 400 })
            t.y = 100
            wait(20)
            mouseClick(t, 20, 20)
            compare(root.clicks, 2, "and when it fits")
            s.destroy(); t.destroy()
        }

        function test_5_back_in_view() {
            var s = make({ n: 10, maxWidth: 200 })
            wait(20)
            toEnd(s)
            compare(s.contentX, 290)
            s.n = 6
            wait(20)
            compare(s.contentX, 290 - 200, "a shorter chain: at its new end, not past it")
            s.maxWidth = 1000
            wait(20)
            compare(s.width, 290)
            verify(!s.overflows)
            compare(s.contentX, 0, "room for all: from its start")
            s.destroy()
        }

        function test_6_no_fade() {
            var s = make({ n: 10, maxWidth: 200, fadeColor: "transparent" })
            wait(20)
            verify(s.overflows)
            mouseWheel(s, 100, 20, 0, -120)
            verify(!named(s, "stripFadeStart").visible && !named(s, "stripFadeEnd").visible, "no fade colour, no fade")
            s.destroy()
        }
    }
}
'@

$code = 0
# the menu: the chain component is a strip whose room is the expanded menu's width there and the collapsed room
# elsewhere, fading only in the menu; it is loaded in both places
$panel = [System.IO.File]::ReadAllText((Join-Path $repo 'qml\app\HotActionsPanel.qml'), $utf8)
$m = [regex]::Match($panel, 'id:\s*qaConnectionsComp\s*\r?\n\s*KScrollStrip\s*\{(?<body>[\s\S]*?)Row\s*\{')
$body = $m.Groups['body'].Value
$okRoom = $body -match '_inMenu:\s*!!parent && parent\.parent === topRow' -and $body -match 'maxWidth:\s*_inMenu \? root\._flowContentW : root\._connRoomCollapsed' -and $body -match 'fadeColor:\s*_inMenu \? root\.hotkeysLayerColor : "transparent"'
$loads = ([regex]::Matches($panel, 'sourceComponent: key === "connections" \? qaConnectionsComp')).Count
$collapsed = $panel -match 'readonly property real _connRoomCollapsed:\s*_windowW - x - collapsedDeviceRow\.x'
if ($m.Success -and $okRoom -and $collapsed -and $loads -ge 2) {
    Write-Host "PASS   : the menu's chain is a KScrollStrip, its room by its place, loaded in $loads places"
} else {
    Write-Host "FAIL!  : the menu's chain: strip $($m.Success), room $okRoom, collapsed room $collapsed, places $loads"
    $code = 1
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('scrollstrip_check_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    New-ModuleDir $work
    $path = Join-Path $work 'tst_scrollstrip.qml'
    Write-Utf8 $path $test
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
        $code = 1
    }
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
Write-Host $(if ($code -eq 0) { 'scrollstrip_check: all passed' } else { 'scrollstrip_check: failed' })
exit $code

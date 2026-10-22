#requires -Version 5.1
<#
.SYNOPSIS
    A grid panel's size: up to 4 cells across and 5 down (the owner, 2026-10-22).

.DESCRIPTION
    1. Offline (qmltestrunner, offscreen): the editor's size step (qml/app/WidgetSizeStep.qml, the kqml_types sources
       as a module, a stand-in store with the store's limits): a card for every size from 1 x 1 to 4 x 5, four to a
       row, in order; a click on one sets that size and moves to the next step.
    2. With -Exe, in the app (the test desktop, a test profile of its own written fresh): a stored 4 x 5 panel and a
       1 x 5 one are loaded, shown, and as tall as five rows; a 4 x 6 one is not loaded (the store's check).
    Exit code 0 when every check holds, 1 when one fails, 2 when qmltestrunner is not found.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\qml_check\widgetsize_check.ps1 [-Exe <build>\KoggerApp.exe]
#>
[CmdletBinding()]
param(
    [string] $QtBin = $(if ($env:QT_BIN) { $env:QT_BIN } else { 'D:\Qt\6.8.3\llvm-mingw_64\bin' }),
    [string] $Exe = '',
    [string] $Profile = 'widgetsize',
    [int]    $TimeoutS = 120
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runner = Join-Path $QtBin 'qmltestrunner.exe'
if (-not (Test-Path $runner)) { Write-Host "widgetsize_check: no qmltestrunner at $runner"; exit 2 }
$utf8 = New-Object System.Text.UTF8Encoding $false
function Write-Utf8([string]$path, [string]$text) { [System.IO.File]::WriteAllText($path, $text, $utf8) }
$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}

# The store's limits, read from the store itself: the stand-in uses what the app uses.
$storeText = [System.IO.File]::ReadAllText((Join-Path $repo 'qml\app\WorkspaceStore.qml'), $utf8)
$maxCols = [int]([regex]::Match($storeText, 'readonly property int widgetMaxCols: (\d+)').Groups[1].Value)
$maxRows = [int]([regex]::Match($storeText, 'readonly property int widgetMaxRows: (\d+)').Groups[1].Value)
Expect ($maxCols -eq 4 -and $maxRows -eq 5) 'the store''s limits' "$maxCols across, $maxRows down"

# --- 1. the size step, offline ---------------------------------------------------------------------------------------
$test = @'
import QtQuick
import QtTest
import kqml_types 1.0

Item {
    id: root
    width: 460
    height: 900

    QtObject {
        id: mockStore
        readonly property int widgetMaxCols: %COLS%
        readonly property int widgetMaxRows: %ROWS%
        property int widgetHoverCols: 0
        property int widgetHoverRows: 0
        property int widgetEditStep: 1
        property var sizes: []
        function widgetDraftSetSize(c, r) { sizes = sizes.concat([c + "x" + r]) }
    }

    function findAll(item, pred, out) {
        if (!item) return out
        if (pred(item)) out.push(item)
        var kids = item.children || []
        for (var i = 0; i < kids.length; ++i) findAll(kids[i], pred, out)
        return out
    }

    TestCase {
        name: "WidgetSize"
        when: windowShown

        function test_1_cards() {
            var step = createTemporaryObject(Qt.createComponent("WidgetSizeStep.qml"), root, { store: mockStore })
            verify(step, "the step made")
            wait(20)
            var cards = findAll(step, function(i) { return i.hasOwnProperty("modelData") && i.modelData && i.modelData.c !== undefined }, [])
            compare(cards.length, %COLS% * %ROWS%, "a card for each size")
            var labels = cards.map(function(c) { return c.modelData.c + "x" + c.modelData.r })
            compare(labels[0], "1x1")
            compare(labels[%COLS% - 1], "%COLS%x1", "the first row: one row, every width")
            compare(labels[labels.length - 1], "%COLS%x%ROWS%", "the last: the largest")
            verify(labels.indexOf("1x5") >= 0 && labels.indexOf("4x5") >= 0, "five rows offered")
            verify(labels.indexOf("5x1") < 0, "no fifth column")
            for (var i = 1; i < cards.length; ++i) {
                var a = cards[i - 1].mapToItem(step, 0, 0), b = cards[i].mapToItem(step, 0, 0)
                if (i % %COLS% !== 0)
                    verify(b.x > a.x && b.y === a.y, labels[i] + " right of " + labels[i - 1])
                else
                    verify(b.y > a.y, labels[i] + " on a new row")
            }
            var last = cards[cards.length - 1]
            verify(last.mapToItem(step, last.width, 0).x <= step.width + 0.5, "four cards fit across")
            var c15 = cards[labels.indexOf("1x5")]
            mouseClick(c15)
            compare(mockStore.sizes.join(","), "1x5", "a click sets that size")
            tryCompare(mockStore, "widgetEditStep", 2)
        }
    }
}
'@
$test = $test.Replace('%COLS%', [string]$maxCols).Replace('%ROWS%', [string]$maxRows)

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('widgetsize_check_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    # kqml_types from the sources, a qmldir of its own (as split_check does)
    $mod = Join-Path $work 'imports\kqml_types'
    New-Item -ItemType Directory -Force $mod | Out-Null
    $lines = @('module kqml_types')
    foreach ($f in Get-ChildItem (Join-Path $repo 'qml\kqml_types') -File | Where-Object { $_.Extension -in '.qml', '.js' } | Sort-Object Name) {
        $s = [System.IO.File]::ReadAllText($f.FullName, $utf8)
        $single = $f.Extension -eq '.qml' -and $s -match '(?m)^\s*pragma\s+Singleton'
        if ($f.Name -in 'AppPalette.qml', 'Tokens.qml') {
            $s = ([regex]'QtObject \{').Replace($s, "QtObject {`n    readonly property var theme: null", 1)
        }
        Write-Utf8 (Join-Path $mod $f.Name) $s
        $lines += ('{0}{1} 1.0 {2}' -f $(if ($single) { 'singleton ' } else { '' }), $f.BaseName, $f.Name)
    }
    Write-Utf8 (Join-Path $mod 'qmldir') (($lines -join "`n") + "`n")
    Copy-Item -LiteralPath (Join-Path $repo 'qml\app\WidgetSizeStep.qml') -Destination $work
    $path = Join-Path $work 'tst_widgetsize.qml'
    Write-Utf8 $path $test
    $env:PATH = "$QtBin;$env:PATH"
    $results = Join-Path $work 'results.txt'
    $p = Start-Process -FilePath $runner -NoNewWindow -Wait -PassThru `
        -ArgumentList @('-platform', 'offscreen', '-import', "`"$(Join-Path $work 'imports')`"", '-input', "`"$path`"", '-o', "`"$results,txt`"") `
        -RedirectStandardOutput (Join-Path $work 'run.out') -RedirectStandardError (Join-Path $work 'run.err')
    $out = @()
    if (Test-Path $results) { $out += @(Get-Content $results -Encoding UTF8) }
    $out | Where-Object { $_ -match '^(FAIL|   Actual|   Expected|   Loc)' } | ForEach-Object { Write-Host $_ }
    $pass = $p.ExitCode -eq 0 -and (Test-Path $results)
    if (-not $pass) { @(Get-Content (Join-Path $work 'run.err') -Encoding UTF8) | Select-Object -Last 20 | ForEach-Object { Write-Host $_ } }
    Expect $pass 'the size step' (($out | Where-Object { $_ -match '^Totals' }) -join ' ')
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

# --- 2. stored panels in the app --------------------------------------------------------------------------------------
if ($Exe) {
    . (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
    $ini = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile.ini"
    $log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
    function Ini-String([string] $s) { '"' + $s.Replace('\', '\\').Replace('"', '\"') + '"' }
    $cell = { param($r, $c) '{"row":' + $r + ',"col":' + $c + ',"field":"temp","rep":"value","big":false}' }
    $grid = { param($id, $cols, $rows) '{"id":"' + $id + '","kind":"grid","name":"","cols":' + $cols + ',"rows":' + $rows + ',"transparency":0,"cells":[' + ((& $cell 0 0)) + ']}' }
    $defs = @()
    $defs += (& $grid 'w_size_45' 4 5)
    $defs += (& $grid 'w_size_15' 1 5)
    $defs += (& $grid 'w_size_14' 1 4)
    $defs += (& $grid 'w_size_46' 4 6)
    $widgets = '[' + ($defs -join ',') + ']'
    $null = $widgets | ConvertFrom-Json
    $shown = '{"w_size_45":true,"w_size_15":true,"w_size_14":true,"w_size_46":true}'
    $layouts = '[{"layout":{"type":"leaf","mode":"2D","paneId":1},"popupLinks":[],"echogramStates":{}}]'
    $seed = @('[General]', 'schemaVersion=8', '', '[main]', 'ui\welcomeShown=true', 'workspace\activeLayoutIndexStored=0',
              ('workspace\layoutsJson=' + (Ini-String $layouts)), ('workspace\widgetShownJson=' + (Ini-String $shown)),
              ('workspace\widgetsJson=' + (Ini-String $widgets)), '')
    if (Test-Path -LiteralPath $ini) { Remove-Item -LiteralPath $ini -Force }
    [System.IO.File]::WriteAllText($ini, ($seed -join "`r`n"), $utf8)
    $sp = Join-Path ([System.IO.Path]::GetTempPath()) "kogger_widgetsize_steps.json"
    '{"live": true, "steps": [{"op": "wait", "ms": 4000}, {"op": "docks"}, {"op": "quit"}]}' | Set-Content -LiteralPath $sp -Encoding ASCII
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
    $psi.EnvironmentVariables['PATH'] = 'D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin;' + $env:PATH
    $psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
    $psi.EnvironmentVariables['KOGGER_SETTINGS_INI'] = '1'
    $psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $sp
    $app = Start-TestDesktopProcess $psi
    if (-not $app.WaitForExit($TimeoutS * 1000)) { $app.Kill(); throw "the app did not end in $TimeoutS s" }
    $lines = Get-Content -LiteralPath $log -Encoding UTF8
    $start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
    $snap = $null
    foreach ($l in $lines[($start - 1)..($lines.Count - 1)]) { if ($l -match 'echoedit script: docks (\{.*\})$') { $snap = $Matches[1] | ConvertFrom-Json } }
    Expect ($null -ne $snap) 'the app''s snapshot' $(if ($snap) { 'read' } else { "none in $log" })
    if ($snap) {
        $panel = { param($id) @($snap.panels) | Where-Object { $_.id -eq $id } | Select-Object -First 1 }
        $p45 = & $panel 'w_size_45'; $p15 = & $panel 'w_size_15'; $p14 = & $panel 'w_size_14'; $p46 = & $panel 'w_size_46'
        Expect ($p45 -and $p45.visible -and $p15 -and $p15.visible) 'a 4 x 5 and a 1 x 5 panel loaded and shown' "4x5 $([bool]$p45), 1x5 $([bool]$p15)"
        $n = $snap.natural
        # five rows against four: one more cell and gap, the same width per column
        $step = [double]$n.w_size_15.h - [double]$n.w_size_14.h
        Expect ($n.w_size_15 -and $n.w_size_14 -and $step -gt 0 -and [double]$n.w_size_45.h -eq [double]$n.w_size_15.h -and [double]$n.w_size_15.w -eq [double]$n.w_size_14.w) `
            'five rows tall' "1x5 $($n.w_size_15.w) x $($n.w_size_15.h), 1x4 $($n.w_size_14.w) x $($n.w_size_14.h), 4x5 $($n.w_size_45.w) x $($n.w_size_45.h)"
        Expect (-not $p46) 'a 4 x 6 panel not loaded' $(if ($p46) { 'it was' } else { 'no panel' })
    }
    Remove-Item -LiteralPath $ini -Force -ErrorAction SilentlyContinue
}

"widgetsize check: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
exit ([int]($failed -gt 0))

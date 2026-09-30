<#
.SYNOPSIS
  Offline check: a layout change clears the echograms' synced cursors (owner 2026-09-30; docs/HANDOFF.md).

.DESCRIPTION
  Runs the real code: the _paneSetKey / _paneSetKeyOf / onLeafRectsChanged block cut out of
  qml/app/WorkspaceView.qml, with a mock core counting clearEchogramSyncCursors() calls, fed with leafRects the way
  WorkspaceStore.rebuildLayoutCaches() sets them:
    1. the first layout clears nothing;
    2. the same panes at other sizes (a resize, a split dragged) clear nothing;
    3. two panes to one clears once;
    4. a pane's mode changed (2D to 3D) clears; the same again clears nothing more;
    5. another content in a pane clears.
  Exit code 0 when all pass, 1 when a check fails, 2 when qmltestrunner is not found.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\qml_check\synccursor_check.ps1 [-QtBin D:\Qt\6.8.3\llvm-mingw_64\bin]
#>
param(
    [string]$QtBin = $(if ($env:QT_BIN) { $env:QT_BIN } else { 'D:\Qt\6.8.3\llvm-mingw_64\bin' })
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runner = Join-Path $QtBin 'qmltestrunner.exe'
if (-not (Test-Path $runner)) {
    Write-Host "synccursor_check: no qmltestrunner at $runner (pass -QtBin or set QT_BIN)"
    exit 2
}
$utf8 = New-Object System.Text.UTF8Encoding $false
function Write-Utf8([string]$path, [string]$text) { [System.IO.File]::WriteAllText($path, $text, $utf8) }

# From "property string _paneSetKey" through the closing brace of "onLeafRectsChanged: {".
function Get-Hook {
    $page = [System.IO.File]::ReadAllLines((Join-Path $repo 'qml\app\WorkspaceView.qml'), $utf8)
    $start = -1
    for ($i = 0; $i -lt $page.Length; $i++) {
        if ($page[$i].Trim() -eq 'property string _paneSetKey: ""') { $start = $i; break }
    }
    if ($start -lt 0) { throw 'synccursor_check: _paneSetKey not found in WorkspaceView.qml' }
    for ($i = $start; $i -lt $page.Length; $i++) {
        if ($page[$i].Trim().StartsWith('onLeafRectsChanged: {')) {
            $depth = 0
            for ($j = $i; $j -lt $page.Length; $j++) {
                $s = ($page[$j] -replace '"[^"]*"', '""')
                $c = $s.IndexOf('//')
                if ($c -ge 0) { $s = $s.Substring(0, $c) }
                $depth += ($s.ToCharArray() | Where-Object { $_ -eq '{' }).Count
                $depth -= ($s.ToCharArray() | Where-Object { $_ -eq '}' }).Count
                if ($depth -le 0) { return ($page[$start..$j] -join "`n") }
            }
        }
    }
    throw 'synccursor_check: onLeafRectsChanged not found after _paneSetKey'
}

$template = @'
import QtQuick
import QtTest

Item {
    id: workspace
    property var leafRects: []
    property QtObject core: QtObject {
        property int clears: 0
        function clearEchogramSyncCursors() { clears++ }
    }
%HOOK%

    function pane(id, mode, cid, x, w) {
        return { leafId: id, x: x, y: 0, width: w, height: 480, pane: { paneId: id, mode: mode, contentId: cid } }
    }

    TestCase {
        name: "SyncCursor"
        function test_1_first_layout_clears_nothing() {
            workspace.leafRects = [pane(1, "2D", "a", 0, 400), pane(2, "3D", "b", 400, 400)]
            compare(workspace.core.clears, 0)
        }
        function test_2_resize_clears_nothing() {
            workspace.leafRects = [pane(1, "2D", "a", 0, 300), pane(2, "3D", "b", 300, 500)]
            workspace.leafRects = [pane(1, "2D", "a", 0, 640), pane(2, "3D", "b", 640, 640)]
            compare(workspace.core.clears, 0)
        }
        function test_3_two_panes_to_one_clears() {
            workspace.leafRects = [pane(1, "2D", "a", 0, 800)]
            compare(workspace.core.clears, 1)
        }
        function test_4_mode_change_clears_once() {
            workspace.leafRects = [pane(1, "3D", "a", 0, 800)]
            compare(workspace.core.clears, 2)
            workspace.leafRects = [pane(1, "3D", "a", 0, 800)]
            compare(workspace.core.clears, 2)
        }
        function test_5_other_content_clears() {
            workspace.leafRects = [pane(1, "3D", "c", 0, 800)]
            compare(workspace.core.clears, 3)
        }
    }
}
'@

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('synccursor_check_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    $path = Join-Path $work 'tst_synccursor.qml'
    Write-Utf8 $path ($template.Replace('%HOOK%', ((Get-Hook) -split "`n" | ForEach-Object { $_ }) -join "`n"))
    $env:PATH = "$QtBin;$env:PATH"
    $results = Join-Path $work 'results.txt'
    $p = Start-Process -FilePath $runner -NoNewWindow -Wait -PassThru `
        -ArgumentList @('-platform', 'offscreen', '-input', "`"$path`"", '-o', "`"$results,txt`"") `
        -RedirectStandardOutput (Join-Path $work 'run.out') -RedirectStandardError (Join-Path $work 'run.err')
    $out = @()
    if (Test-Path $results) { $out += @(Get-Content $results -Encoding UTF8) }
    $out | Where-Object { $_ -match '^(PASS|FAIL|Totals|   Actual|   Expected|   Loc)' } | ForEach-Object { Write-Host $_ }
    if ($p.ExitCode -ne 0 -or -not (Test-Path $results)) {
        @(Get-Content (Join-Path $work 'run.err') -Encoding UTF8) | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
        Write-Host 'synccursor_check: failed'
        $code = 1
    } else {
        Write-Host 'synccursor_check: all passed'
        $code = 0
    }
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
exit $code

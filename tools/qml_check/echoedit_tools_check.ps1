<#
.SYNOPSIS
  Offline check: the echogram-edit tools mean the same in the palette, in Plot2D.qml and in C++
  (docs/echogram-editing.md).

.DESCRIPTION
  A tool is a number in three places that nothing ties together at build time: the palette's buttons
  (qml/app/BottomTrackEditPopup.qml, core.bottomTrackEditTool), the table in Plot2D.qml that turns it
  into a mouse tool, and the MouseTool enum (src/scene2d/plot2D_defs.h) that Plot2D::setMousePosition
  acts on. A step of the history is a number in two: echoedit::Command::Kind
  (src/echogram_edit/echo_edit_model.h) and the palette's kindName() for the undo and redo tooltips.
    1. every palette tool 0..11 is offered once, and the table sends it to the mouse tool it is meant for
       (pencil -> MouseToolDistance, raise -> MouseToolDistanceMax, mark -> MouseToolMarkBad,
       smooth -> MouseToolSmooth, ...);
    2. the real _applyBtTool, cut out of Plot2D.qml, run with a mock core and plot: the same numbers;
    3. the real kindName, cut out of the palette, names every Command::Kind, in that order.
  Exit code 0 when all pass, 1 when a check fails, 2 when qmltestrunner is not found.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\qml_check\echoedit_tools_check.ps1 [-QtBin D:\Qt\6.8.3\llvm-mingw_64\bin]
#>
param(
    [string]$QtBin = $(if ($env:QT_BIN) { $env:QT_BIN } else { 'D:\Qt\6.8.3\llvm-mingw_64\bin' })
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runner = Join-Path $QtBin 'qmltestrunner.exe'
if (-not (Test-Path $runner)) {
    Write-Host "echoedit_tools_check: no qmltestrunner at $runner (pass -QtBin or set QT_BIN)"
    exit 2
}
$utf8 = New-Object System.Text.UTF8Encoding $false
function Read-Text([string]$rel) { return [System.IO.File]::ReadAllText((Join-Path $repo $rel), $utf8) }
function Write-Utf8([string]$path, [string]$text) { [System.IO.File]::WriteAllText($path, $text, $utf8) }

$fails = 0
function Check([bool]$ok, [string]$what, [string]$detail = '') {
    if ($ok) { Write-Host "PASS $what" } else { Write-Host "FAIL $what $(if ($detail) { ": $detail" })"; $script:fails++ }
}

# The MouseTool enum, its values counted as C++ does (MouseToolNone = 0, MouseToolNothing = 1, then +1).
$defs = Read-Text 'src\scene2d\plot2D_defs.h'
$enumBody = [regex]::Match($defs, 'typedef enum : uint8_t \{(?<b>[^}]*)\} MouseTool;').Groups['b'].Value
$mouse = @{}
$v = -1
foreach ($line in ($enumBody -split "`n")) {
    $l = ($line -replace '//.*$', '').Trim().TrimEnd(',')
    if (-not $l) { continue }
    if ($l -match '^(\w+)\s*=\s*(\d+)$') { $v = [int]$Matches[2]; $mouse[$Matches[1]] = $v }
    elseif ($l -match '^(\w+)$') { $v++; $mouse[$Matches[1]] = $v }
}
Check ($mouse.Count -ge 10) 'the MouseTool enum read' (($mouse.GetEnumerator() | Sort-Object Value | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ', ')

# What each palette tool is meant to be, by its number.
$meant = @('MouseToolNothing', 'MouseToolDistance', 'MouseToolDistanceMax', 'MouseToolDistanceMin', 'MouseToolDistanceErase',
           'MouseToolMarkBad', 'MouseToolUnmarkBad', 'MouseToolTrimStart', 'MouseToolTrimEnd',
           'MouseToolSmooth', 'MouseToolFillGaps', 'MouseToolMosaicOut')

# The palette's tools: { tool: N, icon: ..., tip: ... }.
$palette = Read-Text 'qml\app\BottomTrackEditPopup.qml'
$tools = @([regex]::Matches($palette, '\{\s*tool:\s*(\d+),') | ForEach-Object { [int]$_.Groups[1].Value })
$all = (0..($meant.Count - 1)) -join ','
Check ((($tools | Sort-Object) -join ',') -eq $all) "the palette offers the tools 0..$($meant.Count - 1), each once" ($tools -join ',')

# The table in Plot2D.qml.
$plotQml = Read-Text 'qml\scene2d\Plot2D.qml'
$mapText = [regex]::Match($plotQml, 'var map = \[(?<m>[^\]]*)\]').Groups['m'].Value
$map = @($mapText -split ',' | ForEach-Object { [int]$_.Trim() })
$wrong = @()
for ($t = 0; $t -lt $meant.Count; $t++) {
    if ($t -ge $map.Count -or -not $mouse.ContainsKey($meant[$t]) -or $map[$t] -ne $mouse[$meant[$t]]) {
        $wrong += "tool $t -> $(if ($t -lt $map.Count) { $map[$t] } else { 'none' }), meant $($meant[$t])=$($mouse[$meant[$t]])"
    }
}
if ($map.Count -ne $meant.Count) { $wrong += "the table has $($map.Count) tools, the palette $($meant.Count)" }
Check ($wrong.Count -eq 0) 'Plot2D.qml sends each palette tool to its mouse tool' ($wrong -join '; ')

# Command::Kind, in order.
$model = Read-Text 'src\echogram_edit\echo_edit_model.h'
$kindBody = [regex]::Match($model, 'enum Kind : uint8_t \{(?<b>[^}]*)\};').Groups['b'].Value
$kinds = @(($kindBody -split "`n") | ForEach-Object { ($_ -replace '//.*$', '').Trim().TrimEnd(',') } | Where-Object { $_ } |
           ForEach-Object { ($_ -split '\s*=\s*')[0] })
Check ($kinds.Count -ge 16) 'Command::Kind read' ($kinds -join ', ')

# The real functions, cut out of the two files: from "function name(" to its closing brace.
function Get-Function([string]$text, [string]$name) {
    $i = $text.IndexOf("function $name(")
    if ($i -lt 0) { throw "echoedit_tools_check: function $name not found" }
    $depth = 0
    for ($j = $text.IndexOf('{', $i); $j -lt $text.Length; $j++) {
        if ($text[$j] -eq '{') { $depth++ }
        elseif ($text[$j] -eq '}') { $depth--; if ($depth -eq 0) { return $text.Substring($i, $j - $i + 1) } }
    }
    throw "echoedit_tools_check: function $name not closed"
}
$applyBtTool = (Get-Function $plotQml '_applyBtTool') -replace '\bcore\b', 'mockCore' -replace '\bplot\.', 'mockPlot.'
$kindName = Get-Function $palette 'kindName'

$test = @"
import QtQuick
import QtTest

Item {
    id: root
    property QtObject mockCore: QtObject { property int bottomTrackEditTool: 0 }
    property QtObject mockPlot: QtObject {
        property int last: -1
        function plotMouseTool(m) { last = m }
    }
    $applyBtTool
    $kindName

    TestCase {
        name: "EchoEditTools"
        function test_1_applyBtTool() {
            var expected = [$($map -join ', ')]
            for (var t = 0; t < expected.length; ++t) {
                root.mockCore.bottomTrackEditTool = t
                root._applyBtTool()
                compare(root.mockPlot.last, expected[t], "tool " + t)
            }
            root.mockCore.bottomTrackEditTool = 99
            root._applyBtTool()
            compare(root.mockPlot.last, 1, "an unknown tool navigates")
        }
        function test_2_kindName() {
            for (var k = 0; k < $($kinds.Count); ++k)
                verify(root.kindName(k) !== "", "kind " + k + " has a name")
            compare(root.kindName($($kinds.Count)), "", "past the last kind: no name")
        }
    }
}
"@

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('echoedit_tools_check_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    $path = Join-Path $work 'tst_echoedit_tools.qml'
    Write-Utf8 $path $test
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
        $fails++
    }
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
if ($fails -eq 0) { Write-Host 'echoedit_tools_check: all passed'; exit 0 }
Write-Host "echoedit_tools_check: $fails failed"
exit 1

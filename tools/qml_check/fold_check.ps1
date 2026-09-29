<#
.SYNOPSIS
  Offline check of the folding sections of the device page (docs/WIFI_MODULE.md 2i).

.DESCRIPTION
  Runs the real code: the kqml_types sources (KIsland, KIslandRow, ...) as a module, DisclosureIndicator, and the
  DevIsland / DevIslandTitle components and the foldPrefs block cut out of qml/app/DeviceSettingsPage.qml. Three
  qmltestrunner processes in a row over one settings file stand for three app starts:
    1. every section with a key starts folded (its title row only, no footer), a section without a key never folds,
       a folded section's hidden rows take no click, its title opens it;
    2. the open one is remembered; a second one opens beside it (not an accordion); the first folds again;
    3. that is remembered too.
  Exit code 0 when all pass, 1 when a start fails, 2 when qmltestrunner is not found.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\qml_check\fold_check.ps1 [-QtBin D:\Qt\6.8.3\llvm-mingw_64\bin]
#>
param(
    [string]$QtBin = $(if ($env:QT_BIN) { $env:QT_BIN } else { 'D:\Qt\6.8.3\llvm-mingw_64\bin' })
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runner = Join-Path $QtBin 'qmltestrunner.exe'
if (-not (Test-Path $runner)) {
    Write-Host "fold_check: no qmltestrunner at $runner (pass -QtBin or set QT_BIN)"
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
    Copy-Item (Join-Path $repo 'qml\app\DisclosureIndicator.qml') (Join-Path $work 'DisclosureIndicator.qml')
}

# The lines from the first one matching $pred through its closing brace.
function Get-Block([string[]]$page, [scriptblock]$pred, [string]$what) {
    for ($i = 0; $i -lt $page.Length; $i++) {
        if (& $pred $page[$i]) {
            $depth = 0
            for ($j = $i; $j -lt $page.Length; $j++) {
                $s = ($page[$j] -replace '"[^"]*"', '""')
                $c = $s.IndexOf('//')
                if ($c -ge 0) { $s = $s.Substring(0, $c) }
                $depth += ($s.ToCharArray() | Where-Object { $_ -eq '{' }).Count
                $depth -= ($s.ToCharArray() | Where-Object { $_ -eq '}' }).Count
                if ($depth -le 0) { return $page[$i..$j] }
            }
        }
    }
    throw "fold_check: $what not found in DeviceSettingsPage.qml"
}

function Get-PageCode([string]$ini) {
    $page = [System.IO.File]::ReadAllLines((Join-Path $repo 'qml\app\DeviceSettingsPage.qml'), $utf8)
    $prefs = @(Get-Block $page { param($l) $l.Contains('Settings { id: foldPrefs') } 'foldPrefs')
    $prefs += @(Get-Block $page { param($l) $l.Contains('function sectionOpen(key)') } 'sectionOpen()')
    $prefs += @(Get-Block $page { param($l) $l.Contains('function setSectionOpen(key, open)') } 'setSectionOpen()')
    $iniUrl = 'file:///' + $ini.Replace('\', '/')
    $prefs[0] = ([regex]'Settings \{ id: foldPrefs;').Replace($prefs[0], "Settings { id: foldPrefs; location: `"$iniUrl`";", 1)
    $comps = @(Get-Block $page { param($l) $l.Trim().StartsWith('component DevIsland: KIsland {') } 'DevIsland')
    $comps += @(Get-Block $page { param($l) $l.Trim().StartsWith('component DevIslandTitle: KIslandRow {') } 'DevIslandTitle')
    return (($prefs + $comps) -join "`n")
}

# B never folds (no key), C and A fold; A is the last section, so nothing lies under its hidden rows.
$islands = @'
    Column {
        id: page
        width: parent.width
        spacing: 12
        DevIsland {
            id: islB
            DevIslandTitle { id: titleB; label: "B" }
            KIslandRow { id: rowB1; label: "row b1" }
        }
        DevIsland {
            id: islC
            foldKey: "t.c"
            DevIslandTitle { id: titleC; label: "C"; foldIsland: islC }
            KIslandRow { id: rowC1; label: "row c1" }
        }
        DevIsland {
            id: islA
            foldKey: "t.a"
            footer: "footer A"
            DevIslandTitle { id: titleA; label: "A"; foldIsland: islA }
            KIslandRow { id: rowA1; label: "row a1"; interactive: true; property bool hit: false; onClicked: hit = true }
            KIslandRow { id: rowA2; label: "row a2" }
        }
    }
'@

# A Column lays out on its polish, before the next frame: heights after a toggle are waited for.
$runs = [ordered]@{
    'start1' = @'
        function test_1_folded_by_default() {
            compare(foldPrefs.openSections, "")
            verify(islA.folded && islC.folded && !islB.folded)
            verify(titleA.height > 10)
            fuzzyCompare(islA.implicitHeight, titleA.height, 0.5)          // the title only: no rows, no footer
            fuzzyCompare(islC.implicitHeight, titleC.height, 0.5)
            fuzzyCompare(islB.implicitHeight, titleB.height + rowB1.height, 0.5)
            verify(titleA.leading[0].visible && titleA.leading[0].width > 0 && !titleA.leading[0].expanded)
            verify(!titleB.leading[0].visible && titleB.leading[0].width === 0)
            verify(titleA.interactive && !titleB.interactive)
        }
        function test_2_folded_row_takes_no_click() {
            var p = rowA1.mapToItem(root, rowA1.width / 2, rowA1.height / 2)
            verify(p.y > islA.mapToItem(root, 0, islA.height).y)
            mouseClick(root, p.x, p.y)
            verify(!rowA1.hit)
        }
        function test_3_title_opens() {
            mouseClick(titleA)
            verify(!islA.folded && islC.folded)
            compare(foldPrefs.openSections, "t.a")
            verify(titleA.leading[0].expanded)
            tryVerify(function() { return islA.implicitHeight > titleA.height + rowA1.height + rowA2.height })  // + footer
            mouseClick(rowA1)
            verify(rowA1.hit)
        }
'@
    'start2' = @'
        function test_1_remembered() {
            compare(foldPrefs.openSections, "t.a")
            verify(!islA.folded && islC.folded)
        }
        function test_2_each_on_its_own() {
            mouseClick(titleC)
            verify(!islA.folded && !islC.folded)
            compare(foldPrefs.openSections, "t.a,t.c")
            mouseClick(titleA)
            verify(islA.folded && !islC.folded)
            compare(foldPrefs.openSections, "t.c")
            tryVerify(function() { return Math.abs(islA.implicitHeight - titleA.height) < 0.5 })
        }
'@
    'start3' = @'
        function test_1_remembered_again() {
            compare(foldPrefs.openSections, "t.c")
            verify(islA.folded && !islC.folded && !islB.folded)
            fuzzyCompare(islC.implicitHeight, titleC.height + rowC1.height, 0.5)
        }
'@
}

$template = @'
import QtQuick
import QtTest
import QtCore
import kqml_types 1.0

Item {
    id: root
    width: 420
    height: 900
%CODE%
%ISLANDS%
    TestCase {
        name: "Fold_%NAME%"
        when: windowShown
%TESTS%
    }
}
'@

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('fold_check_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    New-ModuleDir $work
    $code = Get-PageCode (Join-Path $work 'fold_prefs.ini')
    $env:PATH = "$QtBin;$env:PATH"
    $env:QT_FORCE_STDERR_LOGGING = '1'
    $failed = 0
    foreach ($name in $runs.Keys) {
        $path = Join-Path $work "tst_$name.qml"
        Write-Utf8 $path ($template.Replace('%CODE%', $code).Replace('%ISLANDS%', $islands).Replace('%NAME%', $name).Replace('%TESTS%', $runs[$name]))
        $outFile = Join-Path $work "$name.out"
        $errFile = Join-Path $work "$name.err"
        $p = Start-Process -FilePath $runner -NoNewWindow -Wait -PassThru `
            -ArgumentList @('-platform', 'offscreen', '-import', "`"$(Join-Path $work 'imports')`"", '-input', "`"$path`"") `
            -RedirectStandardOutput $outFile -RedirectStandardError $errFile
        $out = @(Get-Content $outFile -Encoding UTF8) + @(Get-Content $errFile -Encoding UTF8)
        $out | Where-Object { $_ -match '^(PASS|FAIL|Totals|   Actual|   Expected|   Loc)' } | ForEach-Object { Write-Host $_ }
        if ($p.ExitCode -ne 0) {
            $failed++
            $out | Select-Object -Last 40 | ForEach-Object { Write-Host $_ }
        }
    }
    if ($failed -eq 0) { Write-Host 'fold_check: all passed'; $code = 0 } else { Write-Host "fold_check: $failed start(s) failed"; $code = 1 }
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
exit $code

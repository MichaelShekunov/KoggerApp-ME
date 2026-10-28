<#
.SYNOPSIS
  Offline check of the echogram settings panel's "Horizontal scale" row (qml/app/EchogramSettingsPanel.qml).

.DESCRIPTION
  The real EchogramSettingsPanel.qml (and ParamCard.qml) runs offscreen in qmltestrunner over a mock echogram
  that steps its scale as qPlot2D does (the steps of src/scene2d/plot2D_columns.h). Checked: the row and its
  caption are there; the box shows x1 and steps up and down through the plot (x2, x1, x1/2, x1/5), held at the
  ends; a change of the plot's scale from elsewhere (Alt + wheel) shows in the box, after the box was clicked
  too; "1:1" sets 1 and is off at 1. -Picture saves the panel. Exit code 0 when all pass, 1 when a check
  fails, 2 when qmltestrunner is not found.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\qml_check\hscale_panel_check.ps1 [-QtBin D:\Qt\6.8.3\llvm-mingw_64\bin] [-Picture panel.png]
#>
param(
    [string]$QtBin = $(if ($env:QT_BIN) { $env:QT_BIN } else { 'D:\Qt\6.8.3\llvm-mingw_64\bin' }),
    [string]$Picture = ''
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runner = Join-Path $QtBin 'qmltestrunner.exe'
if (-not (Test-Path $runner)) {
    Write-Host "hscale_panel_check: no qmltestrunner at $runner (pass -QtBin or set QT_BIN)"
    exit 2
}
$utf8 = New-Object System.Text.UTF8Encoding $false
function Read-Text([string]$path) { return [System.IO.File]::ReadAllText($path, $utf8) }
function Write-Utf8([string]$path, [string]$text) { [System.IO.File]::WriteAllText($path, $text, $utf8) }

# The steps the mock takes: the ones plot2D_columns.h snaps to, read from it.
$header = Read-Text (Join-Path $repo 'src\scene2d\plot2D_columns.h')
$m = [regex]::Match($header, 'kScales\[\]\s*=\s*\{([^}]*)\}')
if (-not $m.Success) { Write-Host 'hscale_panel_check: no kScales in plot2D_columns.h'; exit 1 }
$steps = @($m.Groups[1].Value -split ',' | ForEach-Object {
    $t = $_.Trim() -replace 'f', ''
    if ($t -match '^([0-9.]+)\s*/\s*([0-9.]+)$') { [double]$Matches[1] / [double]$Matches[2] } else { [double]$t }
})
$stepsJs = '[' + (($steps | ForEach-Object { $_.ToString([Globalization.CultureInfo]::InvariantCulture) }) -join ', ') + ']'

function New-ModuleDir([string]$work) {
    $mod = Join-Path $work 'imports\kqml_types'
    New-Item -ItemType Directory -Force $mod | Out-Null
    $lines = @('module kqml_types')
    $src = Join-Path $repo 'qml\kqml_types'
    foreach ($f in Get-ChildItem $src -File | Where-Object { $_.Extension -in '.qml', '.js' } | Sort-Object Name) {
        $s = Read-Text $f.FullName
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
    width: 520; height: 900

    Rectangle { anchors.fill: parent; color: AppPalette.bg }

    // qPlot2D as the panel sees it: the scale snaps to the steps, a step moves along them, held at the ends.
    QtObject {
        id: plot
        readonly property var steps: %STEPS%
        property real horizontalScale: 1
        // as plot2d_columns::scaleIndex: the nearest step in proportion (develop's wheel leaves any tenth)
        readonly property int horizontalScaleIndex: { var b = 0; for (var i = 1; i < steps.length; ++i) if (Math.abs(Math.log(horizontalScale / steps[i])) < Math.abs(Math.log(horizontalScale / steps[b]))) b = i; return b }
        property int stepCalls: 0
        function horizontalScaleSteps() { return steps }
        function stepHorizontalScaleBy(n) {
            ++stepCalls
            horizontalScale = steps[Math.max(0, Math.min(steps.length - 1, horizontalScaleIndex + n))]
        }
        function resetHorizontalScale() { horizontalScale = 1 }
        function echogramThemeStops(i) { return [] }
        property QtObject viewState: QtObject {
            property bool echogramVisible: true
            property int echoThemeIndex: 0
            property int compensationIndex: 0
            property bool deconvEnabled: false
            property int deconvAngle: 10
            property int deconvIterations: 8
            property bool deconvGeometry: true
        }
    }
    QtObject { id: store; function openTgcSettings() {} }

    EchogramSettingsPanel { id: panel; x: 10; y: 10; width: 500; plot: plot; store: store }

    TestCase {
        name: "HorizontalScale"
        when: windowShown

        function find(item, pred) {
            if (pred(item)) return item
            for (var i = 0; i < item.children.length; ++i) {
                var r = find(item.children[i], pred)
                if (r) return r
            }
            return null
        }
        function row() { return find(panel, function (x) { return x.label === "Horizontal scale" && x.caption !== undefined }) }
        function box() { return find(row(), function (x) { return x.textFromValue !== undefined && typeof x.textFromValue === "function" }) }
        // The box's text, "x" for the multiplication sign (the runner's report writes it in the local code page).
        function shown() {
            return find(box(), function (x) { return x.cursorPosition !== undefined && x.readOnly !== undefined }).text
                .replace(/\u00d7/g, "x")
        }
        function oneToOne() { return find(row(), function (x) { return x.text === "1:1" && x.clicked !== undefined }) }

        function test_1_row() {
            verify(row() !== null, "the row is there")
            compare(row().caption, "Alt + wheel over the echogram")
            verify(box() !== null, "the box is there")
            compare(shown(), "x1")
            verify(find(box(), function (x) { return x.readOnly !== undefined && x.cursorPosition !== undefined }).readOnly, "no typing in the box")
        }

        function test_2_steps() {
            plot.horizontalScale = 1
            box().increment(); compare(plot.horizontalScale, 2); compare(shown(), "x2")
            box().decrement(); compare(plot.horizontalScale, 1); compare(shown(), "x1")
            box().decrement(); compare(shown(), "x1/2")
            box().decrement(); compare(plot.horizontalScale, 0.2); compare(shown(), "x1/5")
            for (var i = 0; i < 6; ++i) box().decrement()
            compare(plot.horizontalScale, plot.steps[0], "held at the most compressed")
            compare(shown(), "x1/" + Math.round(1 / plot.steps[0]))
            for (var k = 0; k < 20; ++k) box().increment()
            compare(plot.horizontalScale, plot.steps[plot.steps.length - 1], "held at the most stretched")
            compare(shown(), "x" + plot.steps[plot.steps.length - 1])
        }

        function test_3_from_elsewhere() {
            plot.horizontalScale = 1
            box().increment()                 // the box set by a click first: the plot's change must still show
            compare(shown(), "x2")
            plot.horizontalScale = 0.2        // as Alt + wheel sets it
            compare(shown(), "x1/5")
            plot.horizontalScale = 0.7        // a tenth between the steps: the nearest one shown
            compare(shown(), "x1/2")
            plot.horizontalScale = 6
            compare(shown(), "x6")
        }

        function test_4_one_to_one() {
            plot.horizontalScale = 4
            verify(oneToOne().enabled, "on away from 1")
            oneToOne().clicked()
            compare(plot.horizontalScale, 1)
            compare(shown(), "x1")
            verify(!oneToOne().enabled, "off at 1")
        }

        function test_5_picture() {
            if (!"%PICTURE%".length)
                return
            plot.horizontalScale = 0.2
            wait(100)
            grabImage(root).save("%PICTURE%")
        }
    }
}
'@

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('hscale_panel_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
$fails = 0
try {
    New-ModuleDir $work
    # The panel reads the app's context property "theme" for the instruments' grade: none here.
    $panel = (Read-Text (Join-Path $repo 'qml\app\EchogramSettingsPanel.qml')).Replace(
        'readonly property int instruments: theme ? theme.instrumentsGrade : 0', 'readonly property int instruments: 0')
    Write-Utf8 (Join-Path $work 'EchogramSettingsPanel.qml') $panel
    Copy-Item (Join-Path $repo 'qml\app\ParamCard.qml') $work
    $pic = if ($Picture) { [System.IO.Path]::GetFullPath($Picture).Replace('\', '/') } else { '' }
    $path = Join-Path $work 'tst_hscale_panel.qml'
    Write-Utf8 $path $test.Replace('%STEPS%', $stepsJs).Replace('%PICTURE%', $pic)
    $env:PATH = "$QtBin;$env:PATH"
    $fonts = Join-Path $work 'fonts'
    New-Item -ItemType Directory -Force $fonts | Out-Null
    Copy-Item (Join-Path $repo 'resources\fonts\arial.ttf') $fonts
    $env:QT_QPA_FONTDIR = $fonts
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
        $fails++
    }
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
if ($fails -eq 0) { Write-Host 'hscale_panel_check: all passed'; exit 0 }
Write-Host "hscale_panel_check: $fails failed"
exit 1

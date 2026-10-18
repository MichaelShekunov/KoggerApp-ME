<#
.SYNOPSIS
  Offline check of the mosaic quality settings (owner 2026-10-18; docs/mosaic-quality.md).

.DESCRIPTION
  Runs the real qml/app/MosaicQualitySettings.qml over the kqml_types sources and a mock controller (setQuality
  recorded, beamState set by the test):
    1. at start the four values go to the controller once, as the controls hold them;
    2. each control sends its new value: the switch, both crops, the overlap rule;
    3. the line under the chart: nothing before a state, "measuring", "not measured yet" with the numbers the state
       gives, and per side "down 6 dB at" or, where the data ends first, "no 6 dB fall up to";
    4. the chart: shown only with a model and the correction on; each side's curve drawn in its colour at its dB;
    5. the correction off hides the chart and the line.
  -Picture saves the block with a model. Exit code 0 when all pass, 1 when a check fails, 2 when qmltestrunner is
  not found.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\qml_check\mosaicquality_check.ps1 [-QtBin D:\Qt\6.8.3\llvm-mingw_64\bin] [-Picture mq.png]
#>
param(
    [string]$QtBin = $(if ($env:QT_BIN) { $env:QT_BIN } else { 'D:\Qt\6.8.3\llvm-mingw_64\bin' }),
    [string]$Picture = ''
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runner = Join-Path $QtBin 'qmltestrunner.exe'
if (-not (Test-Path $runner)) {
    Write-Host "mosaicquality_check: no qmltestrunner at $runner (pass -QtBin or set QT_BIN)"
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
    width: 440; height: 560

    QtObject {
        id: mock
        property var beamState: ({})
        property var calls: []
        function setQuality(correction, nadirCrop, farCrop, overlap) {
            calls.push([correction, nadirCrop, farCrop, overlap])
        }
    }
    MosaicQualitySettings { id: block; x: 10; y: 10; width: 420; controller: mock }

    function find(item, name) {
        if (!item) return null
        if (item.objectName === name) return item
        var kids = item.children || []
        for (var i = 0; i < kids.length; ++i) {
            var r = find(kids[i], name)
            if (r) return r
        }
        return null
    }
    function last() { return mock.calls[mock.calls.length - 1] }
    function curve(db) { var c = []; for (var k = 0; k < 90; ++k) c.push(k < 85 ? db : null); return c }
    function model() {
        return { correction: true, holding: false, valid: true, epochs: 3782, minPings: 300, minCover: 30,
                 left:  { curve: curve(-3), peak: 12.4, edge6: 75.4, dataTo: 88, rangeSpan: 11.2 },
                 right: { curve: curve(-9), peak: 19.6, edge6: -1,   dataTo: 44, rangeSpan: 6.0 } }
    }
    // the colour drawn nearest (x, y) of the chart, within 2 px up or down: its distance from `want`
    function distNear(img, chart, x, y, want) {
        var best = 9
        for (var dy = -2; dy <= 2; ++dy) {
            var p = chart.mapToItem(root, x, y + dy)
            var c = img.pixel(Math.round(p.x), Math.round(p.y))
            var d = Math.abs(c.r - want.r) + Math.abs(c.g - want.g) + Math.abs(c.b - want.b)
            best = Math.min(best, d)
        }
        return best
    }

    TestCase {
        name: "MosaicQuality"
        when: windowShown

        function test_1_sent_once_at_start() {
            tryVerify(function() { return mock.calls.length >= 1 }, 2000, "the values go to the controller at start")
            wait(100)
            compare(mock.calls.length, 1, "once")
            var c = mock.calls[0]
            compare(c[0], find(block, "mosaicBeamCorrection").checked)
            compare(c[1], find(block, "mosaicNadirCrop").value)
            compare(c[2], find(block, "mosaicFarCrop").value)
            compare(c[3], find(block, "mosaicOverlap").currentIndex)
        }

        function test_2_each_control_sends() {
            var sw = find(block, "mosaicBeamCorrection"), nadir = find(block, "mosaicNadirCrop")
            var far = find(block, "mosaicFarCrop"), ov = find(block, "mosaicOverlap")
            var n = mock.calls.length
            sw.checked = !sw.checked; sw.toggled()
            compare(mock.calls.length, n + 1); compare(last()[0], sw.checked)
            nadir.value = 7; nadir.valueModified(7)
            compare(last()[1], 7)
            far.value = 80; far.valueModified(80)
            compare(last()[2], 80)
            ov.currentIndex = 2; ov.activated(2)
            compare(last()[3], 2)
            // back to the defaults (the runner keeps the values in its own settings)
            sw.checked = true; sw.toggled()
            nadir.value = 0; nadir.valueModified(0)
            far.value = 90; far.valueModified(90)
            ov.currentIndex = 1; ov.activated(1)
            compare(last(), [true, 0, 90, 1])
        }

        function test_3_status_line() {
            var st = find(block, "mosaicBeamStatus")
            mock.beamState = {}
            compare(st.text, "")
            mock.beamState = { correction: true, holding: true }
            compare(st.text, "Measuring the beam from the echograms\u2026")
            mock.beamState = { correction: true, holding: false, valid: false, minPings: 300, minCover: 30 }
            compare(st.text, "Not measured yet: needs 300 pings with a bottom track, seen out to 30\u00B0 from straight down")
            mock.beamState = model()
            compare(st.text, "Measured on 3782 pings. Left: brightest at 12\u00B0, down 6 dB at 75\u00B0; "
                             + "Right: brightest at 20\u00B0, no 6 dB fall up to 44\u00B0. Near to far range: 11 dB evened out")
        }

        function test_4_chart() {
            var chart = find(block, "mosaicBeamChart")
            mock.beamState = { correction: true, holding: false, valid: false, minPings: 300, minCover: 30 }
            wait(50)
            verify(!chart.visible, "no chart without a model")
            mock.beamState = model()
            tryVerify(function() { return chart.visible })
            wait(300)
            var img = grabImage(root)
            var x = chart.xOf(45.5)
            var dl = distNear(img, chart, x, chart.yOf(-3), AppPalette.accent)
            var dr = distNear(img, chart, x, chart.yOf(-9), AppPalette.linkIdleBorder)
            verify(dl < 0.3, "the left curve at -3 dB in its colour: " + dl)
            verify(dr < 0.3, "the right curve at -9 dB in its colour: " + dr)
            var off = distNear(img, chart, x, chart.yOf(-15), AppPalette.accent)
            verify(off > 0.3, "and not where it is not: " + off)
            if ("%PICTURE%".length) {
                img.save("%PICTURE%")
            }
        }

        function test_5_correction_off_hides() {
            var sw = find(block, "mosaicBeamCorrection")
            mock.beamState = model()
            sw.checked = false; sw.toggled()
            verify(!find(block, "mosaicBeamChart").visible)
            verify(!find(block, "mosaicBeamStatus").visible)
            sw.checked = true; sw.toggled()
            verify(find(block, "mosaicBeamChart").visible)
        }
    }
}
'@

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('mosaicquality_check_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    New-ModuleDir $work
    Copy-Item (Join-Path $repo 'qml\app\MosaicQualitySettings.qml') (Join-Path $work 'MosaicQualitySettings.qml')
    $path = Join-Path $work 'tst_mosaicquality.qml'
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
        Write-Host 'mosaicquality_check: failed'
        $code = 1
    } else {
        Write-Host 'mosaicquality_check: all passed'
        $code = 0
    }
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
exit $code

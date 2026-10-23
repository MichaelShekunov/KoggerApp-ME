<#
.SYNOPSIS
  Offline check of the device page's Antenna swap section: the Wi-Fi module's radio switched off (firmware 0.20,
  ID_WIFI_RF; docs/WIFI_MODULE.md 2o).

.DESCRIPTION
  Runs the real code: the kqml_types sources as a module, DisclosureIndicator, the page's own components (DevIsland,
  DevIslandTitle, DevStackedRow, DevButton, ConfirmDevButton, WifiBadge) and the section itself, all cut out of
  qml/app/DeviceSettingsPage.qml, against a made-up module (the DevDriver properties and calls the section uses):
    - hidden before firmware 0.20 or while the version is not known;
    - over a wire: "until switched on" is offered and the default; over the network: 30 s .. 10 min only, 1 min the
      default, "until on" not taken;
    - one tap on Switch the radio off sends nothing, the second sends the seconds chosen; nothing sent: a warning;
    - off with a timer: the seconds left tick, the title's badge shows them; Switch on only over a wire, else why not;
    - off until switched on, and off known only from the status;
    - a refusal shown inline and as one notice.
  Exit code 0 when all pass, 1 when one fails, 2 when qmltestrunner is not found.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\qml_check\radiooff_check.ps1 [-QtBin D:\Qt\6.8.3\llvm-mingw_64\bin]
#>
param(
    [string]$QtBin = $(if ($env:QT_BIN) { $env:QT_BIN } else { 'D:\Qt\6.8.3\llvm-mingw_64\bin' }),
    # keep the generated test (its folder is printed) to read a compile error's line
    [switch]$KeepWork
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runner = Join-Path $QtBin 'qmltestrunner.exe'
if (-not (Test-Path $runner)) {
    Write-Host "radiooff_check: no qmltestrunner at $runner (pass -QtBin or set QT_BIN)"
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

# The lines from $start through the brace that closes the one opened there.
function Get-BlockFrom([string[]]$page, [int]$start, [string]$what) {
    $depth = 0
    for ($j = $start; $j -lt $page.Length; $j++) {
        $s = ($page[$j] -replace '"[^"]*"', '""')
        $c = $s.IndexOf('//')
        if ($c -ge 0) { $s = $s.Substring(0, $c) }
        $depth += ($s.ToCharArray() | Where-Object { $_ -eq '{' }).Count
        $depth -= ($s.ToCharArray() | Where-Object { $_ -eq '}' }).Count
        if ($depth -le 0 -and $j -gt $start) { return $page[$start..$j] }
    }
    throw "radiooff_check: the end of $what not found in DeviceSettingsPage.qml"
}
function Get-Component([string[]]$page, [string]$head) {
    for ($i = 0; $i -lt $page.Length; $i++) {
        if ($page[$i].Trim().StartsWith($head)) { return Get-BlockFrom $page $i $head }
    }
    throw "radiooff_check: '$head' not found in DeviceSettingsPage.qml"
}
# The section: the "DevIsland {" line above "id: radioOffIsland".
function Get-Section([string[]]$page) {
    for ($i = 0; $i -lt $page.Length; $i++) {
        if ($page[$i].Trim() -eq 'id: radioOffIsland') {
            for ($k = $i - 1; $k -ge 0; $k--) {
                if ($page[$k].Trim() -eq 'DevIsland {') { return Get-BlockFrom $page $k 'the Antenna swap section' }
            }
        }
    }
    throw 'radiooff_check: the Antenna swap section (id: radioOffIsland) not found in DeviceSettingsPage.qml'
}

$page = [System.IO.File]::ReadAllLines((Join-Path $repo 'qml\app\DeviceSettingsPage.qml'), $utf8)
$code = @()
foreach ($h in 'component DevIsland: KIsland {', 'component DevStackedRow: KIslandRow {', 'component DevIslandTitle: KIslandRow {',
               'component DevButton: KButton {', 'component ConfirmDevButton: DevButton {', 'component WifiBadge: Rectangle {') {
    $code += @(Get-Component $page $h)
}
$section = Get-Section $page

$tests = @'
        function init() {
            fakeDev.wifiRfSupport = 1
            fakeDev.wifiRadioOff = false
            fakeDev.wifiRadioOffTimed = false
            fakeDev.wifiRadioOffUntilOn = false
            fakeDev.wifiRadioWired = true
            fakeDev.wifiRfError = ""
            fakeDev.leftS = 0
            fakeDev.connected = true
            fakeDev.sent = []
            notifications.infos = []
            notifications.warnings = []
            radioOffIsland.chosen = -1
            radioOffButton.pending = false
            fakeDev.wifiRfChanged()
        }
        function waitRow(row) { tryVerify(function() { return row.open && row.height > 1 && row.height === row.contentHeight }, 3000) }
        function confirmOff() {
            waitRow(radioOffButtonRow())
            mouseClick(radioOffButton)
            verify(radioOffButton.pending)
            compare(fakeDev.sent.length, 0)                 // one tap sends nothing
            mouseClick(radioOffButton)
            verify(!radioOffButton.pending)
        }
        function radioOffButtonRow() {
            var p = radioOffButton.parent
            while (p && p.isKIslandRow !== true)
                p = p.parent
            return p
        }

        function test_01_hidden_before_020() {
            fakeDev.wifiRfSupport = 0
            verify(!radioOffIsland.visible)
            fakeDev.wifiRfSupport = -1
            verify(!radioOffIsland.visible)
            fakeDev.wifiRfSupport = 1
            verify(radioOffIsland.visible)
        }
        function test_02_wired_until_on_by_default() {
            compare(JSON.stringify(radioOffIsland.choices), "[0,60,120,300,600]")
            compare(radioOffIsland.seconds, 0)
            compare(radioOffTabs.currentValue, 0)
            compare(radioOffTabs.options[0].label, "Until on")
            compare(radioOffTabs.options[4].label, "10 min")
            confirmOff()
            compare(JSON.stringify(fakeDev.sent), '["off 0"]')
            compare(notifications.infos.length, 1)
            compare(notifications.warnings.length, 0)
        }
        function test_03_network_a_time_only() {
            fakeDev.wifiRadioWired = false
            compare(JSON.stringify(radioOffIsland.choices), "[30,60,120,300,600]")
            compare(radioOffIsland.seconds, 60)
            compare(radioOffTabs.options[0].label, "30 s")
            radioOffTabs.valueSelected(0)                   // not offered here: the default stays
            compare(radioOffIsland.seconds, 60)
            radioOffTabs.valueSelected(300)
            compare(radioOffIsland.seconds, 300)
            compare(radioOffTabs.currentValue, 300)
            confirmOff()
            compare(JSON.stringify(fakeDev.sent), '["off 300"]')
        }
        function test_04_a_choice_kept_when_the_path_is_learnt() {
            fakeDev.wifiRadioWired = false
            radioOffTabs.valueSelected(120)
            fakeDev.wifiRadioWired = true                   // the port summary came: over a wire
            compare(radioOffIsland.seconds, 120)
            radioOffIsland.chosen = 30
            compare(radioOffIsland.seconds, 0)              // 30 s is not offered over a wire: its default
        }
        function test_05_not_sent() {
            fakeDev.connected = false
            confirmOff()
            compare(fakeDev.sent.length, 0)
            compare(notifications.warnings.length, 1)
            compare(notifications.infos.length, 0)
        }
        function test_06_off_with_a_timer_over_a_wire() {
            fakeDev.leftS = 42
            fakeDev.wifiRadioOffTimed = true
            fakeDev.wifiRadioOff = true
            fakeDev.wifiRfChanged()
            waitRow(radioOffState)
            tryVerify(function() { return radioOffState.caption.indexOf("42 s") >= 0 }, 3000)
            verify(!radioOffButtonRow().open)               // no second "off" while off
            verify(radioOffBadge.visible)
            // the middle dot by its code: PowerShell 5.1 reads this file as ANSI
            compare(radioOffBadge.text, "Radio off " + String.fromCharCode(0xB7) + " 42 s")
            verify(!radioOffRemote.open)
            waitRow(radioOnRow)
            mouseClick(radioOnButton)
            compare(JSON.stringify(fakeDev.sent), '["on"]')
            fakeDev.leftS = 41
            tryVerify(function() { return radioOffState.caption.indexOf("41 s") >= 0 }, 2500)   // it ticks
            fakeDev.leftS = 0
            tryVerify(function() { return radioOffState.caption.indexOf("The time is up") === 0 }, 2500)
            compare(radioOffBadge.text, "Radio off")
        }
        function test_07_off_over_the_network_no_switch_on() {
            fakeDev.wifiRadioWired = false
            fakeDev.leftS = 90
            fakeDev.wifiRadioOffTimed = true
            fakeDev.wifiRadioOff = true
            fakeDev.wifiRfChanged()
            tryVerify(function() { return radioOffState.caption.indexOf("1 min 30 s") >= 0 }, 3000)
            verify(radioOffRemote.open)
            verify(!radioOnRow.open)
        }
        function test_08_until_on_and_not_known() {
            fakeDev.wifiRadioOff = true
            fakeDev.wifiRadioOffUntilOn = true
            fakeDev.wifiRfChanged()
            compare(radioOffState.caption.indexOf("Until you switch it on"), 0)
            compare(radioOffBadge.text, "Radio off")
            fakeDev.wifiRadioOffUntilOn = false             // off by the status only
            compare(radioOffState.caption.indexOf("Asking the module"), 0)
            fakeDev.wifiRadioOff = false
            tryVerify(function() { return !radioOffState.open && radioOffButtonRow().open }, 3000)
            verify(!radioOffBadge.visible)
        }
        function test_09_a_refusal_once() {
            fakeDev.wifiRfError = "the radio is busy"
            fakeDev.wifiRfChanged()
            verify(radioOffError.open)
            compare(radioOffError.caption, "the radio is busy")
            compare(notifications.warnings.length, 1)
            compare(notifications.warnings[0], "Radio: the radio is busy")
            fakeDev.wifiRfChanged()                         // the same refusal again: no second notice
            compare(notifications.warnings.length, 1)
            fakeDev.wifiRfError = ""
            fakeDev.wifiRfChanged()
            verify(!radioOffError.open)
        }
        function test_10_spans() {
            compare(radioOffIsland.spanText(30), "30 s")
            compare(radioOffIsland.spanText(60), "1 min")
            compare(radioOffIsland.spanText(90), "1 min 30 s")
            compare(radioOffIsland.spanText(600), "10 min")
        }
'@

$template = @'
import QtQuick
import QtTest
import kqml_types 1.0

Item {
    id: root
    width: 420
    height: 1000
    readonly property bool _isWifi: true
    readonly property real groupWidth: 400
    property var dev: fakeDev
    function sectionOpen(key) { return true }
    function setSectionOpen(key, open) {}

    QtObject {
        id: notifications
        property var infos: []
        property var warnings: []
        function info(t) { infos = infos.concat([t]) }
        function warning(t) { warnings = warnings.concat([t]) }
    }
    // The DevDriver properties and calls the section uses (src/device/dev_driver.h, ID_WIFI_RF).
    QtObject {
        id: fakeDev
        property int wifiRfSupport: 1
        property bool wifiRadioOff: false
        property bool wifiRadioOffTimed: false
        property bool wifiRadioOffUntilOn: false
        property bool wifiRadioWired: true
        property string wifiRfError: ""
        property int leftS: 0
        property bool connected: true
        property var sent: []
        signal wifiRfChanged()
        function wifiRadioOffLeft() { return leftS }
        function wifiRadioSwitchOff(s) { if (!connected) return false; sent = sent.concat(["off " + s]); return true }
        function wifiRadioSwitchOn() { if (!connected) return false; sent = sent.concat(["on"]); return true }
    }
%CODE%
    Column {
        width: parent.width
%SECTION%
    }
    TestCase {
        name: "RadioOff"
        when: windowShown
%TESTS%
    }
}
'@

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('radiooff_check_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    New-ModuleDir $work
    $env:PATH = "$QtBin;$env:PATH"
    $env:QT_FORCE_STDERR_LOGGING = '1'
    $path = Join-Path $work 'tst_radiooff.qml'
    Write-Utf8 $path ($template.Replace('%CODE%', ($code -join "`n")).Replace('%SECTION%', ($section -join "`n")).Replace('%TESTS%', $tests))
    $outFile = Join-Path $work 'out.txt'
    $errFile = Join-Path $work 'err.txt'
    $p = Start-Process -FilePath $runner -NoNewWindow -Wait -PassThru `
        -ArgumentList @('-platform', 'offscreen', '-import', "`"$(Join-Path $work 'imports')`"", '-input', "`"$path`"") `
        -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    $out = @(Get-Content $outFile -Encoding UTF8) + @(Get-Content $errFile -Encoding UTF8)
    $out | Where-Object { $_ -match '^(PASS|FAIL|Totals|   Actual|   Expected|   Loc)' } | ForEach-Object { Write-Host $_ }
    if ($p.ExitCode -ne 0) {
        $out | Select-Object -Last 40 | ForEach-Object { Write-Host $_ }
        Write-Host 'radiooff_check: FAILED'
        $rc = 1
    } else {
        Write-Host 'radiooff_check: all passed'
        $rc = 0
    }
}
finally {
    if ($KeepWork) { Write-Host "radiooff_check: the test is kept in $work" }
    else { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}
exit $rc

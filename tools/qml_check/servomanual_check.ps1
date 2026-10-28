<#
.SYNOPSIS
  Offline check of the servo panel's manual mode (docs/SERVO_MANUAL.md): the Manual switch, the knob
  (ServoAngleDial) and the angle field.

.DESCRIPTION
  Runs the real qml/app/ServoPanelBody.qml and ServoAngleDial.qml with the kqml_types sources against a made-up
  Recorder (the DevDriver properties and calls the panel uses). The one change to the panel: its device list
  (_allDevs, from the app's deviceManagerWrapper) becomes a plain property the test fills.
    - manual off: the knob's row closed, the scan and its switch usable;
    - the switch on: the device told, the knob's row open, the scan and Enabled wait, Current angle the angle held;
    - a drag: pressing sends nothing; a quarter turn clockwise moves the angle by the turn, cut at the servo's range,
      each step as a move and one set at the end; turning back moves it at once; Shift turns a tenth as much;
    - the wheel: 1 degree a notch, 0.1 with Ctrl; the field: 0.01 a step;
    - a firmware without manual mode: the switch back off, the reason under it.
  Exit code 0 when all pass, 1 when one fails, 2 when qmltestrunner is not found.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\qml_check\servomanual_check.ps1 [-QtBin D:\Qt\6.8.3\llvm-mingw_64\bin]
#>
param(
    [string]$QtBin = $(if ($env:QT_BIN) { $env:QT_BIN } else { 'D:\Qt\6.8.3\llvm-mingw_64\bin' }),
    # keep the generated test (its folder is printed) to read a compile error's line
    [switch]$KeepWork,
    # a picture of the panel in manual mode at 25 degrees, to look at (Windows' fonts for the offscreen platform)
    [string]$Shot
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runner = Join-Path $QtBin 'qmltestrunner.exe'
if (-not (Test-Path $runner)) {
    Write-Host "servomanual_check: no qmltestrunner at $runner (pass -QtBin or set QT_BIN)"
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
    foreach ($f in 'DisclosureIndicator.qml', 'ServoAngleDial.qml') {
        Copy-Item (Join-Path $repo "qml\app\$f") (Join-Path $work $f)
    }
    $body = [System.IO.File]::ReadAllText((Join-Path $repo 'qml\app\ServoPanelBody.qml'), $utf8)
    $from = 'readonly property var _allDevs: (typeof deviceManagerWrapper !== "undefined" && deviceManagerWrapper)'
    if (-not $body.Contains($from)) { throw 'servomanual_check: the device list (_allDevs) not found in ServoPanelBody.qml' }
    $body = $body.Replace($from, "property var _allDevs: []`n    readonly property var _appDevs: (typeof deviceManagerWrapper !== `"undefined`" && deviceManagerWrapper)")
    Write-Utf8 (Join-Path $work 'ServoPanelBody.qml') $body
}

$test = @'
import QtQuick
import QtTest
import kqml_types 1.0

Item {
    id: root
    width: 420
    height: 1200

    // The DevDriver properties and calls the panel uses (src/device/dev_q_property.h, dev_driver.h).
    QtObject {
        id: fakeDev
        property bool isBoardInited: true
        property bool isServoSupport: true
        property string devName: "Recorder"
        property int devSN: 4242
        property int devAddress: 0
        property bool servoEnabled: true
        property bool servoReverse: false
        property int servoPwmMinUs: 500
        property int servoPwmMaxUs: 2500
        property int servoPwmHz: 50
        property int servoTriggerKind: 1
        property int servoTriggerFunction: 1
        property int servoTriggerAddress: 0
        property var triggerWrites: []
        function setServoTrigger(f, a) {
            triggerWrites = triggerWrites.concat([f + "/" + a]); servoTriggerFunction = f; servoTriggerAddress = a
        }
        property real servoAngleRangeDeg: 90
        property real servoStepDeg: 4.5
        property real servoRangeDeg: 60
        property real servoCenterDeg: 10
        property real servoCurrentAngleDeg: 0
        property bool servoManual: false
        property real servoManualDeg: 0
        property int servoManualSupport: 0
        property int pwmRouteOut1: 1
        property int pwmRouteOut2: 0
        property int pwmRouteOut3: 0
        property var moves: []
        property var sets: []
        property var manualWrites: []
        signal servoControlChanged()
        signal pwmRouteChanged()
        // As DevDriver: into manual mode at the scan's center, cut to the range.
        onServoManualChanged: {
            manualWrites = manualWrites.concat([servoManual])
            if (servoManual)
                servoManualDeg = Math.max(-servoAngleRangeDeg / 2, Math.min(servoAngleRangeDeg / 2, servoCenterDeg))
        }
        function servoManualMove(d) { moves = moves.concat([d]); servoManualDeg = d }
        function servoManualSet(d) { sets = sets.concat([d]); servoManualDeg = d }
    }

    // DeviceTopologyModel's group of the recorder's link: two sonars on its ports.
    QtObject { id: sonarA; property string devName: "Basic2D"; property int devSN: 11; property int busAddress: 1; property bool isRecorder: false }
    QtObject { id: sonarB; property string devName: "NanoSSS"; property int devSN: 22; property int busAddress: 9; property bool isRecorder: false }
    QtObject {
        id: fakeTopology
        property var groups: [1]
        function groupForDevice(d) { return d === fakeDev ? { children: [{ device: sonarA }, { device: sonarB }] } : {} }
    }

    Rectangle { anchors.fill: parent; color: AppPalette.bg }   // the app's background: the islands' titles are light
    Flickable {
        anchors.fill: parent
        contentHeight: body.height
        ServoPanelBody {
            id: body
            active: false
            width: 400
        }
    }

    readonly property string shotPath: "%SHOT%"
    TestCase {
        name: "ServoManual"
        when: windowShown

        function item(name) {
            var it = findChild(body, name)
            verify(it !== null, name + " found")
            return it
        }
        function waitOpen(row) { tryVerify(function() { return row.open && row.height > 1 && row.height === row.contentHeight }, 3000) }
        function near(a, b) { return Math.abs(a - b) < 0.011 }
        // A drag's angle comes from whole-pixel pointer positions: ~1.3 degrees a pixel at this radius.
        function nearDrag(a, b) { return Math.abs(a - b) < 1.5 }
        function last(list) { return list.length > 0 ? list[list.length - 1] : NaN }
        function at(dial, deg, r) {
            var a = deg * Math.PI / 180
            return { x: dial.width / 2 + Math.sin(a) * r, y: dial.height / 2 - Math.cos(a) * r }
        }
        // A drag around the knob's centre from one pointer angle to another, 5 degrees a step.
        function turn(dial, fromDeg, toDeg, mods) {
            var r = dial.width * 0.38
            var p = at(dial, fromDeg, r)
            var dir = toDeg >= fromDeg ? 1 : -1
            for (var d = fromDeg + 5 * dir; dir > 0 ? d <= toDeg : d >= toDeg; d += 5 * dir) {
                p = at(dial, d, r)
                mouseMove(dial, p.x, p.y, -1, Qt.LeftButton, mods === undefined ? Qt.NoModifier : mods)
            }
            return p
        }
        function enterManual() {
            mouseClick(item("servoManualSwitch"))
            tryVerify(function() { return fakeDev.servoManual })
            waitOpen(item("servoManualDialRow"))
        }

        function initTestCase() {
            body._allDevs = [fakeDev]
            body.topology = fakeTopology
            tryVerify(function() { return body.dev === fakeDev })
        }
        function init() {
            fakeDev.servoManual = false
            fakeDev.servoManualSupport = 0
            fakeDev.servoAngleRangeDeg = 90
            fakeDev.servoCenterDeg = 10
            fakeDev.moves = []
            fakeDev.sets = []
            fakeDev.manualWrites = []
        }

        function test_01_off() {
            verify(!item("servoManualDialRow").open)
            verify(item("servoScanIsland").enabled)
            verify(item("servoEnabledSwitch").enabled)
            compare(item("servoManualRow").caption, "")
        }
        function test_02_switch_on() {
            enterManual()
            compare(JSON.stringify(fakeDev.manualWrites), "[true]")
            verify(!item("servoScanIsland").enabled)
            compare(item("servoScanIsland").footer, "Waits while the servo is in manual mode.")
            verify(!item("servoEnabledSwitch").enabled)
            compare(item("servoCurrentAngle").text, "10.00°")
            compare(item("servoManualDial").angle, 10)
            compare(item("servoManualDial").limit, 45)
        }
        function test_03_drag_cut_and_back() {
            enterManual()
            var dial = item("servoManualDial")
            var p = at(dial, 0, dial.width * 0.38)
            mousePress(dial, p.x, p.y)
            compare(fakeDev.moves.length, 0)                       // pressing does not jump
            turn(dial, 0, 20)
            verify(nearDrag(last(fakeDev.moves), 30), "20 degrees turned: " + last(fakeDev.moves))
            compare(item("servoCurrentAngle").text, last(fakeDev.moves).toFixed(2) + "°")
            turn(dial, 20, 90)
            verify(near(last(fakeDev.moves), 45), "cut at the range: " + last(fakeDev.moves))
            var n = fakeDev.moves.length
            p = turn(dial, 90, 70)                                  // back from past the end: at once
            verify(nearDrag(last(fakeDev.moves), 25), "back 20: " + last(fakeDev.moves))
            verify(fakeDev.moves.length > n)
            compare(fakeDev.sets.length, 0)
            mouseRelease(dial, p.x, p.y)
            compare(fakeDev.sets.length, 1)
            verify(near(fakeDev.sets[0], last(fakeDev.moves)), "set at the end, the last move: " + fakeDev.sets[0])
            compare(dial.angle, fakeDev.sets[0])
        }
        function test_04_shift_finer() {
            enterManual()
            var dial = item("servoManualDial")
            var p = at(dial, 0, dial.width * 0.38)
            mousePress(dial, p.x, p.y)
            p = turn(dial, 0, -90, Qt.ShiftModifier)
            verify(Math.abs(last(fakeDev.moves) - 1) < 0.15, "a quarter turn back with Shift, 10 - 9: " + last(fakeDev.moves))
            mouseRelease(dial, p.x, p.y)
            verify(near(fakeDev.sets[0], last(fakeDev.moves)))
        }
        function test_05_wheel() {
            enterManual()
            var dial = item("servoManualDial")
            mouseWheel(dial, dial.width / 2, dial.height / 2, 0, 120)
            verify(near(last(fakeDev.sets), 11), "a notch: " + last(fakeDev.sets))
            mouseWheel(dial, dial.width / 2, dial.height / 2, 0, -120, Qt.NoButton, Qt.ControlModifier)
            verify(near(last(fakeDev.sets), 10.9), "a notch back with Ctrl: " + last(fakeDev.sets))
            compare(fakeDev.moves.length, 0)
        }
        function test_06_field() {
            enterManual()
            var spin = item("servoManualSpin")
            compare(spin.devValue, 1000)
            compare(spin.from, -4500)
            compare(spin.to, 4500)
            fakeDev.servoManualSet(12.34)
            compare(spin.devValue, 1234)
        }
        function test_08_shot() {
            if (!shotPath)
                skip("no -Shot")
            enterManual()
            fakeDev.servoManualSet(25)
            wait(400)
            grabImage(root).save(shotPath)   // void in Qt 6; the script looks for the file
        }
        // The PWM frequency row (ID_SERVO_CONTROL v1): a pulse must be shorter than the period, 1e6 / Hz us.
        function test_09_pwm_frequency() {
            body._advanced = true
            var row = item("servoPwmHzRow"), hz = item("servoPwmHzSpin"), max = item("servoPwmMaxSpin")
            fakeDev.servoPwmMaxUs = 2500
            fakeDev.servoPwmHz = 50
            verify(row.visible)
            compare(hz.devValue, 50)
            compare(hz.to, 399)                                     // 1e6 / (2500 + 1)
            compare(max.to, 2500)
            compare(row.caption, "Period 20000 µs, the pulse must be shorter. With PWM max 2500 µs: up to 399 Hz.")
            fakeDev.servoPwmMaxUs = 900
            compare(hz.to, 1000)
            hz.writeBack(1000)
            compare(fakeDev.servoPwmHz, 1000)
            compare(max.to, 999)                                    // period 1000 us
            compare(row.caption, "Period 1000 µs, the pulse must be shorter. With PWM max 900 µs: up to 1000 Hz.")
            fakeDev.servoPwmHz = 333
            compare(max.to, 2500)                                   // period 3003 us
            fakeDev.servoPwmHz = 0                                  // a firmware without the setting: 50 Hz, no row
            verify(!row.visible)
            compare(max.to, 2500)
            fakeDev.servoPwmHz = 50
            fakeDev.servoPwmMaxUs = 2500
            body._advanced = false
        }
        // The step source (ID_SERVO_CONTROL v2): any sonar on Sonar A or B, or one of the recorder's sonars.
        function test_10_steps_on() {
            var row = item("servoTriggerRow"), combo = item("servoTriggerCombo")
            fakeDev.servoTriggerKind = 1
            fakeDev.servoTriggerFunction = 1
            fakeDev.servoTriggerAddress = 0
            fakeDev.triggerWrites = []
            verify(row.visible)
            compare(JSON.stringify(combo.model), JSON.stringify(["Any sonar on Sonar A", "Any sonar on Sonar B",
                    "Basic2D · 11 · address 1", "NanoSSS · 22 · address 9"]))
            compare(combo.wantIndex, 0)
            combo.writeBack(3)
            compare(JSON.stringify(fakeDev.triggerWrites), JSON.stringify(["2/9"]))
            compare(combo.wantIndex, 3)
            combo.writeBack(3)                                      // the same again: nothing sent
            compare(fakeDev.triggerWrites.length, 1)
            combo.writeBack(2)
            compare(fakeDev.triggerWrites[1], "1/1")
            fakeDev.servoTriggerFunction = 1                        // a sonar not seen now stays listed
            fakeDev.servoTriggerAddress = 5
            compare(combo.model.length, 5)
            compare(combo.model[4], "Address 5 on Sonar A, not seen now")
            compare(combo.wantIndex, 4)
            fakeDev.servoTriggerKind = 0                            // a firmware without v2: no row
            verify(!row.visible)
            fakeDev.servoTriggerKind = 1
            fakeDev.servoTriggerFunction = 1
            fakeDev.servoTriggerAddress = 0
        }
        function test_07_refused() {
            enterManual()
            // DevDriver on RespErrorPayload: manual dropped here, support known as none.
            fakeDev.servoManualSupport = -1
            fakeDev.servoManual = false
            tryVerify(function() { return !item("servoManualSwitch").checked })
            compare(item("servoManualRow").caption, "The firmware does not support manual mode")
            verify(item("servoScanIsland").enabled)
            tryVerify(function() { return !item("servoManualDialRow").open })
        }
    }
}
'@

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('servomanual_check_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    New-ModuleDir $work
    $env:PATH = "$QtBin;$env:PATH"
    $env:QT_FORCE_STDERR_LOGGING = '1'
    $path = Join-Path $work 'tst_servomanual.qml'
    Write-Utf8 $path ($test.Replace('%SHOT%', $Shot.Replace('\', '/')))
    if ($Shot) { $env:QT_QPA_FONTDIR = Join-Path $env:WINDIR 'Fonts' }
    $outFile = Join-Path $work 'out.txt'
    $errFile = Join-Path $work 'err.txt'
    $p = Start-Process -FilePath $runner -NoNewWindow -Wait -PassThru `
        -ArgumentList @('-platform', 'offscreen', '-import', "`"$(Join-Path $work 'imports')`"", '-input', "`"$path`"") `
        -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    $out = @(Get-Content $outFile -Encoding UTF8) + @(Get-Content $errFile -Encoding UTF8)
    $out | Where-Object { $_ -match '^(PASS|FAIL|Totals|   Actual|   Expected|   Loc)' } | ForEach-Object { Write-Host $_ }
    $warn = @($out | Where-Object { $_ -match 'qml:\d+.*(Error|TypeError|ReferenceError|Unable to assign|Cannot assign)' })
    $warn | ForEach-Object { Write-Host "QML: $_" }
    if ($p.ExitCode -ne 0 -or $warn.Count -gt 0) {
        $out | Select-Object -Last 40 | ForEach-Object { Write-Host $_ }
        Write-Host 'servomanual_check: FAILED'
        $rc = 1
    } else {
        Write-Host 'servomanual_check: all passed'
        $rc = 0
    }
}
finally {
    if ($KeepWork) { Write-Host "servomanual_check: the test is kept in $work" }
    else { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}
exit $rc

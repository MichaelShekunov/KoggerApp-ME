<#
.SYNOPSIS
  Offline check of the Recorder's board view (qml/app/RecorderBoardView.qml; docs/RECORDER_PORTS.md).

.DESCRIPTION
  Runs the real RecorderBoardView with the kqml_types sources against a made-up Recorder carrying the port table the
  real one reported on 2026-10-24 (tools/ports_check/ports_device.ps1), VBAT 12.4 V, sonar A live:
    - 8 blocks, one a connector, inside the drawing and not over each other or the board;
    - the contacts in board order with their live values (transmit/receive, VBAT, PWM pulse, ADC volts);
    - SONAR1's menu by default: sonar port, its link from the recorder's status, reserved rows;
    - a tap on IN3,4 opens its first active contact's menu (the servo on IN3, what drives OUT1); a tap on IN4 opens
      its own and closes IN3's;
    - a value change (ID_PIN v1) and a voltage change show at once.
  Exit code 0 when all pass, 1 when one fails, 2 when qmltestrunner is not found.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\qml_check\recorderboard_check.ps1 [-Shot board.png]
#>
param(
    [string]$QtBin = $(if ($env:QT_BIN) { $env:QT_BIN } else { 'D:\Qt\6.8.3\llvm-mingw_64\bin' }),
    [switch]$KeepWork,
    # a picture of the view with IN3,4 selected, to look at
    [string]$Shot
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runner = Join-Path $QtBin 'qmltestrunner.exe'
if (-not (Test-Path $runner)) {
    Write-Host "recorderboard_check: no qmltestrunner at $runner (pass -QtBin or set QT_BIN)"
    exit 2
}
$utf8 = New-Object System.Text.UTF8Encoding $false
function Write-Utf8([string]$path, [string]$text) { [System.IO.File]::WriteAllText($path, $text, $utf8) }

function New-ModuleDir([string]$work) {
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
    Copy-Item (Join-Path $repo 'qml\app\RecorderBoardView.qml') (Join-Path $work 'RecorderBoardView.qml')
}

$test = @'
import QtQuick
import QtTest
import kqml_types 1.0

Item {
    id: root
    width: 700
    height: 1400
    readonly property string shotPath: "%SHOT%"

    function bus(id, label, supply, fn, uart, tx, rx, baud, pins) {
        return { id: id, label: label, supply: supply, function: fn, functionSaved: fn, functionCaps: 1, uart: uart,
                 uartSaved: uart, uartCaps: 0, txPin: tx, rxPin: rx, baud: baud, pins: pins }
    }
    function pin(id, label, mcu, role, timer, param, value) {
        return { id: id, label: label, mcuPin: mcu, roleCaps: 0x79, role: role, roleSaved: role, timer: timer,
                 param: param, value: value }
    }
    function table(in3Value) {
        return [
            bus(0, "SONAR1", 1, 1, 2, 0, 1, 57600, [pin(0, "TX", "PA2", 1, 0, 0, 0), pin(1, "RX", "PA3", 1, 0, 0, 0)]),
            bus(1, "SONAR2", 1, 2, 3, 2, 3, 57600, [pin(2, "TX", "PC4", 1, 0, 0, 0), pin(3, "RX", "PC5", 1, 0, 0, 0)]),
            bus(2, "TELEM", 2, 3, 1, 4, 5, 5000000, [pin(4, "TX", "PA9", 1, 0, 0, 0), pin(5, "RX", "PA10", 1, 0, 0, 0)]),
            bus(3, "GNSS", 2, 4, 4, 6, 7, 9600, [pin(6, "TX", "PC1", 1, 0, 0, 0), pin(7, "RX", "PC0", 1, 0, 0, 0)]),
            bus(4, "I2C", 2, 0, 0, 255, 255, 0, [pin(8, "SCL", "PB10", 0, 2, 0, 0), pin(9, "SDA", "PB11", 0, 2, 0, 0)]),
            bus(5, "IN1,2", 2, 0, 0, 255, 255, 0, [pin(10, "IN1", "PB0", 3, 1, 0, 1500), pin(11, "IN2", "PB1", 0, 1, 0, 0)]),
            bus(6, "IN3,4", 2, 0, 0, 255, 255, 0, [pin(12, "IN3", "PA0", 3, 2, 0, in3Value), pin(13, "IN4", "PA1", 0, 2, 0, 0)]),
            bus(7, "OUTS", 0, 0, 0, 255, 255, 0, [pin(14, "O1", "PA6", 3, 16, 0, 1500), pin(15, "O2", "PB15", 0, 15, 0, 0),
                                                 pin(16, "O3", "PB14", 0, 15, 0, 0)])]
    }

    QtObject {
        id: fakeDev
        property var portBuses: root.table(1500)
        property int voltage0Mv: 12400
        property int voltage1Mv: -1
        property bool recorderStatusValid: true
        property int recorderStatusFlags: 1
        property int recorderDegradedFlags: 2
        property int pwmRouteOut1: 1
        property int pwmRouteOut2: 0
        property int pwmRouteOut3: 0
        property bool servoManual: false
        property bool servoEnabled: true
    }

    Rectangle { anchors.fill: parent; color: AppPalette.bg }   // the app's background: the view's texts are light
    Flickable {
        anchors.fill: parent
        contentHeight: view.height
        RecorderBoardView { id: view; width: 680; dev: fakeDev }
    }

    TestCase {
        name: "RecorderBoard"
        when: windowShown

        function item(name) { var it = findChild(view, name); verify(it !== null, name + " found"); return it }
        function texts(it) {
            var out = []
            function walk(n) {
                if (n.text !== undefined && n.visible && String(n.text).length)
                    out.push(String(n.text))
                for (var i = 0; i < n.children.length; ++i)
                    walk(n.children[i])
            }
            walk(it)
            return out
        }
        function has(list, s) { return list.indexOf(s) >= 0 }
        function overlap(a, b) {
            var p = a.mapToItem(view, 0, 0), q = b.mapToItem(view, 0, 0)
            return p.x < q.x + b.width && q.x < p.x + a.width && p.y < q.y + b.height && q.y < p.y + a.height
        }

        function test_01_blocks() {
            var labels = ["SONAR1", "SONAR2", "TELEM", "GNSS", "I2C", "IN1,2", "IN3,4", "OUTS"]
            var blocks = labels.map(function(l) { return item("boardBlock_" + l) })
            for (var i = 0; i < blocks.length; ++i) {
                var p = blocks[i].mapToItem(view, 0, 0)
                verify(p.x >= 0 && p.y >= 0 && p.x + blocks[i].width <= view.width + 0.5, labels[i] + " inside")
                for (var j = i + 1; j < blocks.length; ++j)
                    verify(!overlap(blocks[i], blocks[j]), labels[i] + " clear of " + labels[j])
            }
        }
        function test_02_values() {
            var s1 = texts(item("boardBlock_SONAR1"))
            verify(has(s1, "SONAR1 · sonar A · 57600 baud"), s1.join(" | "))
            verify(has(s1, "transmit") && has(s1, "receive") && has(s1, "12.40 V"), s1.join(" | "))
            compare(s1.indexOf("TX") < s1.indexOf("RX") && s1.indexOf("RX") < s1.indexOf("GND") && s1.indexOf("GND") < s1.indexOf("VBAT"), true)
            var in34 = texts(item("boardBlock_IN3,4"))
            verify(has(in34, "PWM OUT1 · 1500 µs") && has(in34, "off"), in34.join(" | "))
            compare(in34.indexOf("+5V") < in34.indexOf("GND") && in34.indexOf("GND") < in34.indexOf("IN4") && in34.indexOf("IN4") < in34.indexOf("IN3"), true)
            var outs = texts(item("boardBlock_OUTS"))
            verify(!has(outs, "+5V") && !has(outs, "VBAT"), "OUTS has no supply pin: " + outs.join(" | "))
        }
        function test_03_default_menu() {
            verify(item("menuSonar").visible)
            var m = texts(item("menuSonar"))
            verify(has(m, "data coming in") && has(m, "57600 baud") && has(m, "USART2"), m.join(" | "))
            verify(has(m, "reserved"), "reserved rows shown")
            verify(!item("menuNav").visible && !item("menuTelemetry").visible)
        }
        function test_04_select_and_pins() {
            item("boardBlock_IN3,4").clicked()
            verify(!item("menuSonar").visible)
            tryVerify(function() { return findChild(view, "menuPwm_IN3") !== null && findChild(view, "menuPwm_IN3").visible })
            var m = texts(findChild(view, "menuPwm_IN3"))
            verify(has(m, "OUT1") && has(m, "the servo, scanning") && has(m, "1500 µs") && has(m, "TIM2"), m.join(" | "))
            findChild(view, "boardPin_IN4").clicked()
            tryVerify(function() { return !findChild(view, "menuPwm_IN3").visible })
            verify(has(texts(findChild(view, "boardMenu")), "The contact is off."))
            fakeDev.servoManual = true
            findChild(view, "boardPin_IN3").clicked()
            tryVerify(function() { return has(texts(findChild(view, "menuPwm_IN3")), "the servo, held by hand") })
            fakeDev.servoManual = false
        }
        function test_05_live() {
            fakeDev.portBuses = root.table(1720)
            fakeDev.voltage0Mv = 11980
            tryVerify(function() { return has(texts(item("boardBlock_IN3,4")), "PWM OUT1 · 1720 µs") })
            tryVerify(function() { return has(texts(item("boardBlock_SONAR1")), "11.98 V") })
        }
        function test_06_shot() {
            if (!shotPath)
                skip("no -Shot")
            item("boardBlock_IN3,4").clicked()
            wait(300)
            grabImage(root).save(shotPath)
        }
    }
}
'@

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('recorderboard_check_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    New-ModuleDir $work
    $env:PATH = "$QtBin;$env:PATH"
    $env:QT_FORCE_STDERR_LOGGING = '1'
    if ($Shot) { $env:QT_QPA_FONTDIR = Join-Path $env:WINDIR 'Fonts' }
    $path = Join-Path $work 'tst_recorderboard.qml'
    Write-Utf8 $path ($test.Replace('%SHOT%', $Shot.Replace('\', '/')))
    $outFile = Join-Path $work 'out.txt'
    $errFile = Join-Path $work 'err.txt'
    $p = Start-Process -FilePath $runner -NoNewWindow -Wait -PassThru `
        -ArgumentList @('-platform', 'offscreen', '-import', "`"$(Join-Path $work 'imports')`"", '-input', "`"$path`"") `
        -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    $out = @(Get-Content $outFile -Encoding UTF8) + @(Get-Content $errFile -Encoding UTF8)
    $out | Where-Object { $_ -match '^(PASS|FAIL|SKIP|Totals|   Actual|   Expected|   Loc)' } | ForEach-Object { Write-Host $_ }
    $warn = @($out | Where-Object { $_ -match 'RecorderBoardView\.qml:\d+' })
    $warn | ForEach-Object { Write-Host "QML: $_" }
    if ($p.ExitCode -ne 0 -or $warn.Count -gt 0) {
        $out | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
        Write-Host 'recorderboard_check: FAILED'
        $rc = 1
    } else {
        Write-Host 'recorderboard_check: all passed'
        $rc = 0
    }
}
finally {
    if ($KeepWork) { Write-Host "recorderboard_check: the test is kept in $work" }
    else { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}
exit $rc

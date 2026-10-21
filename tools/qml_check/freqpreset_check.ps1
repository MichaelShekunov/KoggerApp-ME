<#
.SYNOPSIS
  Offline check: a device's frequency button and the sonar's presets (qml/app/FreqPresetButton.qml, ID_PRESET).

.DESCRIPTION
  Runs the real FreqPresetButton.qml over the kqml_types sources with two stand-in sonars, one without presets
  and one that keeps them (slot 0 used, slot 1 empty):
    1. without presets: 700 in the circle, a tap sets 700 kHz and closes the pill, a long press is a tap;
    2. slot 0 used: its frequency in the circle and its name beside it, lit when the sonar runs it, a tap
       recalls it, a long press asks to save into it and the release recalls nothing;
    3. slot 1 empty on a sonar that keeps presets: 450 and no name, a tap sets 450 kHz, a long press asks to
       save into slot 1;
    4. a short press on a used slot recalls it and asks nothing.
  With -Mutants it also runs the check over copies of the button with one fault each and expects every one of
  them to fail. Exit code 0 when all pass, 1 when a check fails or a mutant passes, 2 when qmltestrunner is not
  found.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\qml_check\freqpreset_check.ps1 [-QtBin D:\Qt\6.8.3\llvm-mingw_64\bin] [-Mutants]
#>
param(
    [string]$QtBin = $(if ($env:QT_BIN) { $env:QT_BIN } else { 'D:\Qt\6.8.3\llvm-mingw_64\bin' }),
    [switch]$Mutants
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runner = Join-Path $QtBin 'qmltestrunner.exe'
if (-not (Test-Path $runner)) {
    Write-Host "freqpreset_check: no qmltestrunner at $runner (pass -QtBin or set QT_BIN)"
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
    width: 400; height: 200

    QtObject {
        id: plain           // a sonar without presets
        property int transFreq: 0
        property int transPulse: 10
        property bool presetsKnown: false
        // a used slot in the list all the same: the button goes by presetsKnown, not by what the list holds
        property var presetSlots: [ { slot: 0, used: true, name: "Stale", freqKhz: 600, pulses: 5 } ]
        property int recalls: 0
        function presetRecall(s) { recalls++ }
    }
    QtObject {
        id: keeps           // a sonar that keeps presets: slot 0 used, slot 1 empty
        property int transFreq: 0
        property int transPulse: 0
        property bool presetsKnown: true
        property var presetSlots: [ { slot: 0, used: true, name: "Narrow", freqKhz: 815, pulses: 25 },
                                    { slot: 1, used: false, name: "", freqKhz: 0, pulses: 0 } ]
        property int recalls: 0
        property int lastRecall: -1
        function presetRecall(s) { recalls++; lastRecall = s }
    }

    FreqPresetButton { id: b; x: 200; y: 20; width: 40; height: 40; holdMs: 150
                       tipFixed: "fixed"; tipPresetFormat: "%1: %2 kHz" }
    SignalSpy { id: done; target: b; signalName: "done" }
    SignalSpy { id: save; target: b; signalName: "saveAsked" }

    TestCase {
        name: "FreqPresetButton"
        when: windowShown

        function init() { done.clear(); save.clear() }
        // A press held ms long, then the release (the button's own MouseArea gives pressEnded, then clicked).
        function hold(ms) { b.pressStarted(1, 1); wait(ms); b.pressEnded(); b.clicked() }

        function test_1_without_presets() {
            b.dev = plain; b.slot = 0; b.khz = 700
            compare(b.glyph, "700")
            compare(b.toolTipText, "fixed")
            verify(!findChild(b, "presetNameLabel").visible, "no name beside it")
            b.clicked()
            compare(plain.transFreq, 700, "a tap sets 700 kHz")
            compare(done.count, 1, "and closes the pill")
            hold(400)
            compare(save.count, 0, "a long press asks no save")
            compare(done.count, 2, "a long press is a tap")
            compare(plain.recalls, 0)
        }

        function test_2_used_slot() {
            b.dev = keeps; b.slot = 0; b.khz = 700
            compare(b.glyph, "815", "the slot's frequency in the circle")
            compare(b.toolTipText, "Narrow: 815 kHz")
            verify(findChild(b, "presetNameLabel").visible, "its name beside it")
            compare(findChild(b, "presetNameText").text, "Narrow")
            verify(!b.active, "not lit while the sonar runs something else")
            keeps.transFreq = 815; keeps.transPulse = 30
            verify(!b.active, "not lit at its frequency with other pulses")
            keeps.transPulse = 25
            verify(b.active, "lit when the sonar runs it")
            b.clicked()
            compare(keeps.recalls, 1, "a tap recalls")
            compare(keeps.lastRecall, 0)
            compare(done.count, 1)
            hold(400)
            compare(save.count, 1, "a long press asks to save")
            compare(save.signalArguments[0][0], 0, "into slot 0")
            compare(keeps.recalls, 1, "the release recalls nothing")
            compare(done.count, 1)
        }

        function test_3_empty_slot() {
            b.dev = keeps; b.slot = 1; b.khz = 450
            compare(b.glyph, "450")
            verify(!findChild(b, "presetNameLabel").visible)
            b.clicked()
            compare(keeps.transFreq, 450, "a tap sets 450 kHz")
            hold(400)
            compare(save.count, 1, "a long press asks to save into the empty slot")
            compare(save.signalArguments[0][0], 1)
            compare(keeps.transFreq, 450)
        }

        function test_4_short_press() {
            b.dev = keeps; b.slot = 0; b.khz = 700
            var before = keeps.recalls
            hold(40)
            wait(300)                      // past the hold time: the release must have stopped it
            compare(save.count, 0, "a short press asks nothing")
            compare(keeps.recalls, before + 1, "and recalls")
        }
    }
}
'@

function Invoke-Check([string]$buttonText, [string]$tag) {
    $work = Join-Path ([System.IO.Path]::GetTempPath()) ('freqpreset_check_' + $tag + '_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force $work | Out-Null
    try {
        New-ModuleDir $work
        Write-Utf8 (Join-Path $work 'FreqPresetButton.qml') $buttonText
        $path = Join-Path $work 'tst_freqpreset.qml'
        Write-Utf8 $path $test
        $results = Join-Path $work 'results.txt'
        $p = Start-Process -FilePath $runner -NoNewWindow -Wait -PassThru `
            -ArgumentList @('-platform', 'offscreen', '-import', "`"$(Join-Path $work 'imports')`"", '-input', "`"$path`"",
                            '-o', "`"$results,txt`"") `
            -RedirectStandardOutput (Join-Path $work 'run.out') -RedirectStandardError (Join-Path $work 'run.err')
        $out = @()
        if (Test-Path $results) { $out += @(Get-Content $results -Encoding UTF8) }
        $err = @(Get-Content (Join-Path $work 'run.err') -Encoding UTF8 -ErrorAction SilentlyContinue)
        return [pscustomobject]@{ Ok = ($p.ExitCode -eq 0 -and (Test-Path $results)); Out = $out; Err = $err }
    }
    finally {
        Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    }
}

$env:PATH = "$QtBin;$env:PATH"
$button = [System.IO.File]::ReadAllText((Join-Path $repo 'qml\app\FreqPresetButton.qml'), $utf8).Replace("`r`n", "`n")
$r = Invoke-Check $button 'tree'
$r.Out | Where-Object { $_ -match '^(PASS|FAIL|Totals|   Actual|   Expected|   Loc)' } | ForEach-Object { Write-Host $_ }
if (-not $r.Ok) {
    $r.Err | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
    Write-Host 'freqpreset_check: failed'
    exit 1
}
Write-Host 'freqpreset_check: all passed'
if (-not $Mutants) { exit 0 }

$faults = @(
    @{ Name = 'a used slot ignored';                From = 'return (s && s.used) ? s : null'; To = 'return null' },
    @{ Name = 'an empty slot taken for a preset';   From = 'return (s && s.used) ? s : null'; To = 'return s ? s : null' },
    @{ Name = 'presets on a sonar that has none';   From = 'if (!dev || !dev.presetsKnown || !dev.presetSlots'; To = 'if (!dev || !dev.presetSlots' },
    @{ Name = 'the release after a hold recalls';   From = "_held = false`n            return"; To = '_held = false' },
    @{ Name = 'a hold on any sonar asks to save';   From = "if (dev && dev.presetsKnown)`n            holdTimer.restart()"; To = 'holdTimer.restart()' },
    @{ Name = 'the hold not stopped on release';    From = 'onPressEnded: holdTimer.stop()'; To = 'onPressEnded: {}' },
    @{ Name = 'lit by the frequency alone';         From = 'dev.transFreq === preset.freqKhz && dev.transPulse === preset.pulses'; To = 'dev.transFreq === preset.freqKhz' },
    @{ Name = 'the pill stays open after a tap';    From = "dev.transFreq = khz`n        done()"; To = 'dev.transFreq = khz' },
    @{ Name = 'the name not shown';                 From = 'visible: !!btn.preset'; To = 'visible: false' },
    @{ Name = 'the save asked for the other slot';  From = 'btn.saveAsked(btn.slot)'; To = 'btn.saveAsked(1 - btn.slot)' }
)
$missed = 0
$i = 0
foreach ($m in $faults) {
    ++$i
    if (-not $button.Contains($m.From)) { throw "mutant '$($m.Name)': its text is not in FreqPresetButton.qml" }
    $mr = Invoke-Check ($button.Replace($m.From, $m.To)) ("m$i")
    $failed = @($mr.Out | Where-Object { $_ -match '^FAIL' })
    if (-not $mr.Ok -and $failed.Count -gt 0) {
        Write-Host ("mutant {0} ({1}): caught by {2} check(s), first: {3}" -f $i, $m.Name, $failed.Count, $failed[0])
    } else {
        Write-Host ("mutant {0} ({1}): MISSED" -f $i, $m.Name)
        ++$missed
    }
}
Write-Host ("freqpreset_check mutants: {0} of {1} caught" -f ($faults.Count - $missed), $faults.Count)
exit ([int]($missed -gt 0))

<#
.SYNOPSIS
  Offline check of a device pill's state behind a recorder (qml/app/RecorderStatus.js pillState; docs/HANDOFF.md).

.DESCRIPTION
  Runs the real RecorderStatus.js over mock devices:
    1. a recorder that is not recording (storage lost, a failed recording, recording off) shows red itself, and a
       working sonar behind it, whose port the recorder calls lost, is green by its own frames (owner 2026-09-30);
    2. a recording recorder that calls the sonar's port lost still makes it red (as before);
    3. a port the recorder sees live makes it green; a silent sonar (dataStale) is "warn" whatever the recorder says;
    4. behind a recorder that is not recording, a sonar whose link receives nothing is "warn", not green.
  Exit code 0 when all pass, 1 when a check fails, 2 when qmltestrunner is not found.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\qml_check\recorderpill_check.ps1 [-QtBin D:\Qt\6.8.3\llvm-mingw_64\bin]
#>
param(
    [string]$QtBin = $(if ($env:QT_BIN) { $env:QT_BIN } else { 'D:\Qt\6.8.3\llvm-mingw_64\bin' })
)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runner = Join-Path $QtBin 'qmltestrunner.exe'
if (-not (Test-Path $runner)) {
    Write-Host "recorderpill_check: no qmltestrunner at $runner (pass -QtBin or set QT_BIN)"
    exit 2
}
$utf8 = New-Object System.Text.UTF8Encoding $false
function Write-Utf8([string]$path, [string]$text) { [System.IO.File]::WriteAllText($path, $text, $utf8) }

$test = @'
import QtQuick
import QtTest
import "RecorderStatus.js" as RecorderStatus

Item {
    function recorder(state, statusFlags, degradedFlags, criticalFlags) {
        return { isRecorder: true, recorderStatusValid: true, recorderRecordingState: state, recorderStatusFlags: statusFlags,
                 recorderDegradedFlags: degradedFlags, recorderCriticalFlags: criticalFlags, recorderSecondsSinceLastWrite: 0,
                 linkConnected: true, dataStale: false, linkReceivesData: true }
    }
    function sonar(stale, receiving) {
        return { isRecorder: false, linkConnected: true, dataStale: stale, linkReceivesData: receiving }
    }

    TestCase {
        name: "RecorderPill"

        function test_1_recorder_not_recording() {
            var noStorage = recorder(3, 0, 1, 1)             // storage lost; port 1 (the sonar) lost
            compare(RecorderStatus.pillState(noStorage, null, -1), "down", "the recorder itself red")
            compare(RecorderStatus.pillState(sonar(false, true), noStorage, 1), "ok", "the working sonar behind it green")
            compare(RecorderStatus.pillState(sonar(false, true), recorder(4, 0, 1, 0), 1), "ok", "recording off")
            compare(RecorderStatus.pillState(sonar(false, true), recorder(3, 0, 1, 0), 1), "ok", "a failed recording")
        }

        function test_2_recording_recorder_lost_port() {
            compare(RecorderStatus.pillState(sonar(false, true), recorder(2, 0, 1, 0), 1), "down")
        }

        function test_3_live_port_and_silent_sonar() {
            compare(RecorderStatus.pillState(sonar(false, true), recorder(2, 1, 0, 0), 1), "ok")
            compare(RecorderStatus.pillState(sonar(true, true), recorder(2, 1, 0, 0), 1), "warn")
            compare(RecorderStatus.pillState(sonar(true, true), recorder(3, 0, 1, 1), 1), "warn")
        }

        function test_4_not_recording_and_link_quiet() {
            compare(RecorderStatus.pillState(sonar(false, false), recorder(3, 0, 1, 1), 1), "warn")
        }
    }
}
'@

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('recorderpill_check_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    Copy-Item (Join-Path $repo 'qml\app\RecorderStatus.js') (Join-Path $work 'RecorderStatus.js')
    $path = Join-Path $work 'tst_recorderpill.qml'
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
        Write-Host 'recorderpill_check: failed'
        $code = 1
    } else {
        Write-Host 'recorderpill_check: all passed'
        $code = 0
    }
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
exit $code

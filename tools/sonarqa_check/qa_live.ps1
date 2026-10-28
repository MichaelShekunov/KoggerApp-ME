#requires -Version 5.1
<#
.SYNOPSIS
    The sonar acceptance window end to end in the running app, with the simulated sonar (KOGGER_SONARQA_DEMO=fast):
    no stand, no device. docs/SONAR_QA.md.

.DESCRIPTION
    One run of KoggerApp on the test desktop (KOGGER_PROFILE reset from the transfer folder's KoggerApp-perf.reg, a
    live edit script, results in a folder of its own through KOGGER_SONARQA_ROOT). The script opens the developer
    page, opens the window with its button, and with the window's own buttons makes the baseline of the simulated
    reference (1001), tests a weak sonar (2002) and a good one (2001), taking a picture of the window after each.
    Checked:
      open      the developer page opened, the window's button and every window button clicked
      runs      the journal has the baseline, 2002 failed and 2001 passed, in that order, each with its .json and .ksqa
      shots     the five pictures written: after each run, then the good one's checks and charts (look at them)
      qml       no QML error in the log
    Exit code 0 when every check holds.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\sonarqa_check\qa_live.ps1 -Exe <build>\KoggerApp.exe -PathPrefix "D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [string] $PathPrefix,
    [string] $Profile = 'perf',
    [string] $Out = "$env:TEMP\kogger_sonarqa_live",
    [string] $PerfReg = 'D:\YandexDisk\DelphiProjects\KoggerApp_myEdition_transfer\KoggerApp-perf.reg',
    [int]    $RunMs = 30000,
    [int]    $TimeoutS = 240
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$root = Join-Path $Out 'sonar_qa'
if (Test-Path -LiteralPath $PerfReg) {
    cmd.exe /c "reg import `"$PerfReg`" >nul 2>&1"   # reg.exe reports success on stderr
}
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
$utf8 = New-Object System.Text.UTF8Encoding $false

function Shot([string] $name) { '{"op": "grabWindow", "name": "sonarQaWindow", "path": "' + (Join-Path $Out $name).Replace('\', '/') + '"}' }
$steps = @('{"op": "wait", "ms": 3000}',
           '{"op": "storeCall", "method": "openDeveloperSettings"}', '{"op": "wait", "ms": 1000}',
           '{"op": "clickItem", "name": "openSonarQa"}', '{"op": "wait", "ms": 1500}',
           '{"op": "clickItem", "name": "qaDemoRef"}', '{"op": "wait", "ms": 1500}',
           '{"op": "clickItem", "name": "qaStartBaseline"}', ('{"op": "wait", "ms": ' + $RunMs + '}'), (Shot 'baseline.png'),
           '{"op": "clickItem", "name": "qaDemoWeak"}', '{"op": "wait", "ms": 1500}',
           '{"op": "clickItem", "name": "qaStartUnit"}', ('{"op": "wait", "ms": ' + $RunMs + '}'), (Shot 'weak.png'),
           '{"op": "clickItem", "name": "qaDemoGood"}', '{"op": "wait", "ms": 1500}',
           '{"op": "clickItem", "name": "qaStartUnit"}', ('{"op": "wait", "ms": ' + $RunMs + '}'), (Shot 'good.png'),
           '{"op": "scrollTo", "name": "qaChecks"}', '{"op": "wait", "ms": 500}', (Shot 'good_checks.png'),
           '{"op": "scrollTo", "name": "qaChart_echo"}', '{"op": "wait", "ms": 500}', (Shot 'good_charts.png'),
           '{"op": "quit"}')
$scriptPath = Join-Path $Out 'script.json'
[System.IO.File]::WriteAllText($scriptPath, ('{"live": true, "steps": [' + ($steps -join ",`n") + ']}'), $utf8)

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
$psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
$psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $scriptPath
$psi.EnvironmentVariables['KOGGER_SONARQA_DEMO'] = 'fast'
$psi.EnvironmentVariables['KOGGER_SONARQA_ROOT'] = $root
$before = if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
$proc = Start-TestDesktopProcess $psi
if (-not $proc.WaitForExit($TimeoutS * 1000)) { $proc.Kill(); throw "the app did not end in $TimeoutS s" }

$fs = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); [void]$fs.Seek($before, 'Begin')
$text = (New-Object System.IO.StreamReader($fs, [Text.Encoding]::UTF8)).ReadToEnd(); $fs.Close()
$mine = @($text -split "`r?`n")
$mine | Set-Content -LiteralPath (Join-Path $Out 'kogger.log') -Encoding UTF8

$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}
$called = @($mine | Where-Object { $_ -match 'echoedit script: storeCall openDeveloperSettings called' }).Count
$clicks = @($mine | Where-Object { $_ -match 'echoedit script: clickItem (\S+) #0 of \d+: clicked' } | ForEach-Object { $Matches[1] })
$wantClicks = 'openSonarQa qaDemoRef qaStartBaseline qaDemoWeak qaStartUnit qaDemoGood qaStartUnit'
Expect ($called -eq 1 -and ($clicks -join ' ') -eq $wantClicks) 'open' ("developer page {0}; clicked: {1}" -f $called, ($clicks -join ' '))

$journal = Join-Path $root 'journal.csv'
$rows = if (Test-Path $journal) { @(Get-Content -LiteralPath $journal | Select-Object -Skip 1 | ForEach-Object { $c = $_ -split ';'; '{0}/{1}/{2}' -f $c[1], $c[2], $c[4] }) } else { @() }
# -File and the extension test, not -Include: PowerShell 5.1 lists each file twice with -Include and -Recurse
$files = @(Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue |
           Where-Object { ($_.Extension -eq '.json' -or $_.Extension -eq '.ksqa') -and $_.Name -ne 'state.json' })
Expect (($rows -join ' ') -eq 'baseline/1001/baseline unit/2002/fail unit/2001/pass' -and $files.Count -eq 6) 'runs' (
    "journal: {0}; files {1}" -f ($rows -join ' '), $files.Count)
$shots = @('baseline.png', 'weak.png', 'good.png', 'good_checks.png', 'good_charts.png' | Where-Object { Test-Path (Join-Path $Out $_) })
Expect ($shots.Count -eq 5) 'shots' ("{0} of 5 in {1}" -f $shots.Count, $Out)
$qml = @($mine | Where-Object { $_ -match '\.qml:\d+' -and $_ -match 'Error|TypeError|ReferenceError|Unable to assign|Cannot assign|is not defined' })
Expect ($qml.Count -eq 0) 'qml' ($(if ($qml.Count) { $qml[0] } else { 'no QML error' }))
"sonarqa live: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
"logs: {0}" -f $Out
exit ([int]($failed -gt 0))

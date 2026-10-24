#requires -Version 5.1
<#
.SYNOPSIS
    The serial rate search against a live GODIO ring (LinkRateSearch, docs/RING_UPDATE.md): reopened at a rate below
    the line's, the app must find the line by listening and send nothing that takes the leader out of its role.

.DESCRIPTION
    One run of KoggerApp on the test desktop with a test profile of its own; the port pinned at -Baud (the line's rate)
    with the speed search off. A live edit script reads both boards' ring counters (ID_RING_STAT), then:
      control  the port reopened at -SlowBaud with the search off for -ControlMs, then back at -Baud: whatever the app
               sends at the slow rate looks like a BREAK to the leader (the firmware's session: 5 of 5 at any rate
               below the line's), so "leads on a timeout" grows on both boards -- the run can see the harm at all
      search   the port reopened at -SlowBaud with the search on: it listens first and finds the line
    and reads the counters after each. Checked:
      read       both boards answered all three reads
      control    leads grew on both boards during the control
      search     leads did not grow on either board during the search (nor skipped, nor lost)
      qml        no QML error in the log
    It costs the ring a few pings during the control. Exit code 0 when every check holds.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\ringstat_check\ratesearch_device.ps1 -Exe <build>\KoggerApp.exe -Port COM18 -Baud 921600
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [Parameter(Mandatory = $true)] [string] $Port,
    [int]    $Baud = 921600,
    [int]    $SlowBaud = 115200,
    [int]    $ControlMs = 1000,
    [int[]]  $Addrs = @(1, 2),
    [string] $PathPrefix,
    [string] $Profile = 'ratesearch',
    [string] $Out = "$env:TEMP\kogger_ratesearch_device",
    [int]    $TimeoutS = 180
)

$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding $false
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$ini = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile.ini"
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
$seed = @('[General]', 'schemaVersion=8', '', '[main]', 'ui\welcomeShown=true', 'ui\instrumentsGradeList=2', '')
if (Test-Path -LiteralPath $ini) { Remove-Item -LiteralPath $ini -Force }
New-Item -ItemType Directory -Force -Path (Split-Path $ini) | Out-Null
[System.IO.File]::WriteAllText($ini, ($seed -join "`r`n"), $utf8)
$cfg = Join-Path $env:LOCALAPPDATA "KOGGER\KoggerApp-$Profile"
New-Item -ItemType Directory -Force -Path $cfg | Out-Null
$links = @(
    '<?xml version="1.0" encoding="UTF-8"?>', '<pinned_links>', '    <link>',
    ('        <uuid>{0}</uuid>' -f ([guid]::NewGuid().ToString('B'))),
    '        <control_type>1</control_type>', ('        <port_name>{0}</port_name>' -f $Port),
    ('        <baudrate>{0}</baudrate>' -f $Baud), '        <parity>false</parity>', '        <link_type>1</link_type>',
    '        <address></address>', '        <source_port>0</source_port>', '        <destination_port>0</destination_port>',
    '        <is_pinned>true</is_pinned>', '        <is_hided>false</is_hided>', '        <is_not_available>false</is_not_available>',
    '        <connection_status>false</connection_status>', '        <auto_speed_selection>false</auto_speed_selection>',
    '    </link>', '</pinned_links>', '')
[System.IO.File]::WriteAllText((Join-Path $cfg 'pinned_links.xml'), ($links -join "`n"), $utf8)

$steps = New-Object System.Collections.Generic.List[string]
function Reads {
    foreach ($a in $Addrs) {
        $steps.Add(('{{"op": "ringStat", "addr": {0}, "do": "read"}}' -f $a)); $steps.Add('{"op": "wait", "ms": 1500}')
    }
}
$steps.Add('{"op": "wait", "ms": 8000}'); Reads
$steps.Add(('{{"op": "link", "port": "{0}", "close": true, "baud": {1}, "search": false, "open": true}}' -f $Port, $SlowBaud))
$steps.Add(('{{"op": "wait", "ms": {0}}}' -f $ControlMs))
$steps.Add(('{{"op": "link", "port": "{0}", "close": true, "baud": {1}, "open": true}}' -f $Port, $Baud))
$steps.Add('{"op": "wait", "ms": 6000}'); Reads
$steps.Add(('{{"op": "link", "port": "{0}", "close": true, "baud": {1}, "search": true, "open": true}}' -f $Port, $SlowBaud))
$steps.Add('{"op": "wait", "ms": 8000}'); Reads
$steps.Add('{"op": "quit"}')
$sp = Join-Path $Out 'steps.json'
[System.IO.File]::WriteAllText($sp, ('{"live": true, "steps": [' + ($steps -join ",`n") + ']}'), $utf8)

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
$psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
$psi.EnvironmentVariables['KOGGER_SETTINGS_INI'] = '1'
$psi.EnvironmentVariables['KOGGER_AUTOTEST'] = $Port
$psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $sp
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
@($mine | Where-Object { $_ -match 'echoedit script: link ' } | ForEach-Object { $_ -replace '^.*echoedit script: ', '' })
$pat = 'ring-stat: address (\d+), .*; relayed (\d+), BREAKs (\d+), lost passing on (\d+), leads on a timeout (\d+), skipped (\d+)'
$reads = @($mine | ForEach-Object {
    if ($_ -match $pat) {
        [pscustomobject]@{ Addr = [int]$Matches[1]; Lost = [long]$Matches[4]; Leads = [long]$Matches[5]; Skipped = [long]$Matches[6] }
    } })
$all = $true; $controlGrew = $true; $searchClean = $true
foreach ($a in $Addrs) {
    $r = @($reads | Where-Object { $_.Addr -eq $a })
    if ($r.Count -lt 3) { $all = $false; "board {0}: {1} read(s)" -f $a, $r.Count; continue }
    "board {0}: leads {1} -> {2} (control) -> {3} (search); skipped {4} -> {5} -> {6}; lost {7} -> {8} -> {9}" -f $a,
        $r[0].Leads, $r[1].Leads, $r[2].Leads, $r[0].Skipped, $r[1].Skipped, $r[2].Skipped, $r[0].Lost, $r[1].Lost, $r[2].Lost
    if ($r[1].Leads -le $r[0].Leads) { $controlGrew = $false }
    if ($r[2].Leads -ne $r[1].Leads -or $r[2].Skipped -ne $r[1].Skipped -or $r[2].Lost -ne $r[1].Lost) { $searchClean = $false }
}
Expect $all 'read' ("{0} answers from boards {1}" -f $reads.Count, ($Addrs -join ', '))
Expect $controlGrew 'control' ("sending at {0} took the leader out of its role on every board" -f $SlowBaud)
Expect $searchClean 'search' ("reopened at {0} with the search on: no counter moved" -f $SlowBaud)
$qml = @($mine | Where-Object { $_ -match '\.qml:\d+' -and $_ -match 'Error|TypeError|ReferenceError|Unable to assign|Cannot assign' })
Expect ($qml.Count -eq 0) 'qml' ($(if ($qml.Count) { $qml[0] } else { 'no QML error' }))
"rate search device: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
"log: {0}" -f (Join-Path $Out 'kogger.log')
exit ([int]($failed -gt 0))

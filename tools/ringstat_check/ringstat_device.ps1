#requires -Version 5.1
<#
.SYNOPSIS
    A live GODIO ring's sync counters (ID_RING_STAT 0x76, firmware 1.16+) read by the app over a real serial port:
    read only, nothing is set on the boards and the counters are not reset.

.DESCRIPTION
    One run of KoggerApp on the test desktop, with a test profile of its own (KOGGER_SETTINGS_INI written fresh, and a
    pinned_links.xml that pins the port at -Baud with the speed search off: a search would put bytes at other rates on
    the ring's line). KOGGER_AUTOTEST=<port> and a live edit script: wait for the boards, read each board's counters,
    hold the line -HoldS seconds, read them again, dump the last board's page, quit. Reported per board: the two reads
    and what grew between them. Checked:
      read       every board in -Addrs answered both reads
      skipped    no board skipped a ping while the app held the line (the counter did not grow)
      lost       no relayed frame was lost meanwhile
      page       the counters' island shows on the page
      qml        no QML error in the log
    With -ConnectOnly the run only holds the line -HoldS seconds and reports the minute tallies of what the app sent
    to address 0 (an older build, to see what its search costs: read the counters before and after with a full run).
    Exit code 0 when every check holds.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\ringstat_check\ringstat_device.ps1 -Exe <build>\KoggerApp.exe -Port COM18 -Baud 460800
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [Parameter(Mandatory = $true)] [string] $Port,
    [int]    $Baud = 460800,
    [int[]]  $Addrs = @(1, 2),
    [int]    $HoldS = 60,
    [switch] $ConnectOnly,
    # The speed search left on: the port starts at -Baud and the app looks for the line's rate itself.
    [switch] $Search,
    [string] $PathPrefix,
    [string] $Profile = 'ringdevice',
    [string] $Out = "$env:TEMP\kogger_ringstat_device",
    [int]    $TimeoutS = 300
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

# The port pinned at the ring's rate with the speed search off (LinkManager reads this at a cold start).
$cfg = Join-Path $env:LOCALAPPDATA "KOGGER\KoggerApp-$Profile"
New-Item -ItemType Directory -Force -Path $cfg | Out-Null
$links = @(
    '<?xml version="1.0" encoding="UTF-8"?>', '<pinned_links>', '    <link>',
    ('        <uuid>{0}</uuid>' -f ([guid]::NewGuid().ToString('B'))),
    '        <control_type>1</control_type>', ('        <port_name>{0}</port_name>' -f $Port),
    ('        <baudrate>{0}</baudrate>' -f $Baud), '        <parity>false</parity>', '        <link_type>1</link_type>',
    '        <address></address>', '        <source_port>0</source_port>', '        <destination_port>0</destination_port>',
    '        <is_pinned>true</is_pinned>', '        <is_hided>false</is_hided>', '        <is_not_available>false</is_not_available>',
    '        <connection_status>false</connection_status>',
    ('        <auto_speed_selection>{0}</auto_speed_selection>' -f $(if ($Search) { 'true' } else { 'false' })),
    '    </link>', '</pinned_links>', '')
[System.IO.File]::WriteAllText((Join-Path $cfg 'pinned_links.xml'), ($links -join "`n"), $utf8)

if ($ConnectOnly) {
    $steps = @(('{{"op": "wait", "ms": {0}}}' -f ($HoldS * 1000)), '{"op": "quit"}')
} else {
    $steps = New-Object System.Collections.Generic.List[string]
    $steps.Add('{"op": "wait", "ms": 8000}')
    foreach ($a in $Addrs) {
        $steps.Add(('{{"op": "ringStat", "addr": {0}, "do": "read"}}' -f $a)); $steps.Add('{"op": "wait", "ms": 1500}')
        $steps.Add(('{{"op": "ringStat", "addr": {0}, "do": "show"}}' -f $a))
    }
    $steps.Add(('{{"op": "wait", "ms": {0}}}' -f ($HoldS * 1000)))
    foreach ($a in $Addrs) {
        $steps.Add(('{{"op": "ringStat", "addr": {0}, "do": "read"}}' -f $a)); $steps.Add('{"op": "wait", "ms": 1500}')
        $steps.Add(('{{"op": "ringStat", "addr": {0}, "do": "show"}}' -f $a))
    }
    $steps.Add(('{{"op": "devicePage", "addr": {0}}}' -f $Addrs[-1])); $steps.Add('{"op": "wait", "ms": 1500}')
    $steps.Add('{"op": "probe", "tag": "page", "root": "DeviceSettingsPage"}'); $steps.Add('{"op": "quit"}')
}
$sp = Join-Path $Out 'steps.json'
[System.IO.File]::WriteAllText($sp, ('{"live": true, "steps": [' + ($steps -join ",`n") + ']}'), $utf8)

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
$psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
$psi.EnvironmentVariables['KOGGER_SETTINGS_INI'] = '1'
$psi.EnvironmentVariables['KOGGER_AUTOTEST'] = $Port
$psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $sp
$psi.EnvironmentVariables['KOGGER_UI_PROBE'] = $Out
$psi.EnvironmentVariables['KOGGER_UI_PROBE_DELAY_MS'] = '600000'
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

# What the app sent to address 0 (its search) and the chart frames each board sent, minute by minute.
$sent0 = @($mine | Where-Object { $_ -match 'traffic \[[^\]]*\] sent:' } | ForEach-Object {
    $m = [regex]::Matches($_, 'KP1 \[0\] (\S+ v\d) [\d.]+ KB/s ([\d.]+)/s')
    ($m | ForEach-Object { '{0} {1}/s' -f $_.Groups[1].Value, $_.Groups[2].Value }) -join ', ' })
$charts = @($mine | Where-Object { $_ -match 'traffic \[[^\]]*\]: ' } | ForEach-Object {
    $m = [regex]::Matches($_, 'KP1 \[(\d+)\] CHART_Z v0 [\d.]+ KB/s ([\d.]+)/s')
    ($m | ForEach-Object { '[{0}] {1}/s' -f $_.Groups[1].Value, $_.Groups[2].Value }) -join ', ' })
"sent to address 0, a minute each: {0}" -f $(if ($sent0.Count) { ($sent0 | ForEach-Object { "($_)" }) -join ' ' } else { 'no tally' })
"chart frames received, a minute each: {0}" -f $(if ($charts.Count) { ($charts | ForEach-Object { "($_)" }) -join ' ' } else { 'no tally' })
$held = @($mine | Where-Object { $_ -match 'search: another device of the line answers' })
"search held: {0}" -f $held.Count

if (-not $ConnectOnly) {
    $pat = 'ring-stat: address (\d+), (leader|follower)( \(a board before it\))?, guard ([\d.]+) ms, BREAK interval ([^;]+); ' +
           'relayed (\d+), BREAKs (\d+), lost passing on (\d+), leads on a timeout (\d+), skipped (\d+)'
    $reads = @($mine | ForEach-Object {
        if ($_ -match $pat) {
            $at = [datetime]::ParseExact($_.Substring(0, 23), 'yyyy-MM-dd HH:mm:ss.fff', $null)   # the log line's time
            [pscustomobject]@{ At = $at; Addr = [int]$Matches[1]; Role = $Matches[2]; Up = [bool]$Matches[3]; Guard = $Matches[4]
                               Interval = $Matches[5]; Relayed = [long]$Matches[6]; Breaks = [long]$Matches[7]
                               Lost = [long]$Matches[8]; Leads = [long]$Matches[9]; Skipped = [long]$Matches[10] }
        } })
    $allRead = $true; $noSkip = $true; $noLost = $true
    foreach ($a in $Addrs) {
        $r = @($reads | Where-Object { $_.Addr -eq $a })
        if ($r.Count -lt 2) { $allRead = $false; "board {0}: {1} read(s)" -f $a, $r.Count; continue }
        $x = $r[0]; $y = $r[-1]
        "board {0}: {1}{2}, guard {3} ms, interval {4}; first read relayed {5}, BREAKs {6}, lost {7}, leads {8}, skipped {9}; last read leads {10}" -f `
            $a, $x.Role, $(if ($x.Up) { ' (a board before it)' } else { '' }), $x.Guard, $x.Interval, $x.Relayed, $x.Breaks, $x.Lost, $x.Leads, $x.Skipped, $y.Leads
        $span = ($y.At - $x.At).TotalSeconds
        "board {0}: grew in {1:F1} s between the reads: relayed +{2}, BREAKs +{3} ({4:F2}/s), lost +{5}, leads +{6}, skipped +{7}" -f `
            $a, $span, ($y.Relayed - $x.Relayed), ($y.Breaks - $x.Breaks), $(if ($span -gt 0) { ($y.Breaks - $x.Breaks) / $span } else { 0 }),
            ($y.Lost - $x.Lost), ($y.Leads - $x.Leads), ($y.Skipped - $x.Skipped)
        if ($y.Skipped -ne $x.Skipped) { $noSkip = $false }
        if ($y.Lost -ne $x.Lost) { $noLost = $false }
    }
    Expect $allRead 'read' ("{0} answers from boards {1}" -f $reads.Count, ($Addrs -join ', '))
    Expect $noSkip 'skipped' 'no board skipped a ping while the app held the line'
    Expect $noLost 'lost' 'no relayed frame lost meanwhile'
    $json = Join-Path $Out 'ui-page.json'
    $shown = $false
    if (Test-Path $json) {
        $doc = [System.IO.File]::ReadAllText($json, $utf8) | ConvertFrom-Json
        $stack = New-Object System.Collections.Stack; $stack.Push($doc.root)
        while ($stack.Count) {
            $n = $stack.Pop()
            if ($n.name -eq 'ringStatIsland') { $shown = [bool]$n.visible }
            foreach ($c in @($n.children)) { if ($c) { $stack.Push($c) } }
        }
    }
    Expect $shown 'page' ("the counters' island on board {0}'s page: {1}" -f $Addrs[-1], $shown)
}
$qml = @($mine | Where-Object { $_ -match '\.qml:\d+' -and $_ -match 'Error|TypeError|ReferenceError|Unable to assign|Cannot assign' })
Expect ($qml.Count -eq 0) 'qml' ($(if ($qml.Count) { $qml[0] } else { 'no QML error' }))
"ring stat device: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
"log: {0}" -f (Join-Path $Out 'kogger.log')
exit ([int]($failed -gt 0))

#requires -Version 5.1
<#
.SYNOPSIS
    A real Recorder's port table read by the app (ID_BUS / ID_PIN, Recorder firmware feat/port-config phase 1): read
    only, nothing is set on the device.

.DESCRIPTION
    One run of KoggerApp on the test desktop (a test profile of its own, KOGGER_SETTINGS_INI, written fresh; the
    owner's settings are not touched) with KOGGER_AUTOTEST=<port> and a live edit script: wait for the table, log it,
    open the Recorder's page, wait 70 s with its Connectors group open (the traffic tally of that minute shows the
    live poll), dump the page, log the table again, quit. Checked against what the Recorder's session read on the
    device 2026-10-24 (the recorder fork's docs/Port-Config-Spec.md):
      buses      8: SONAR1 SONAR2 TELEM GNSS I2C IN1,2 IN3,4 OUTS; supplies 1 1 2 2 2 2 2 0; functions 1 2 3 4 0 0 0 0;
                 UARTs 2 3 1 4 0 0 0 0
      pins       17; IN1, IN3, O1 PWM to OUT1; the other universal pins off; device-port pins UART with no role caps
      outs       the servo panel's OUT1 row: IN1, IN3, O1
      page       the Connectors group shows a card per bus, no text cut
      live       ID_PIN v1 asked about 5 times a second while the group is open (the minute's traffic tally)
    Exit code 0 when every check holds.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\ports_check\ports_device.ps1 -Exe <build>\KoggerApp.exe -Port COM17
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [Parameter(Mandatory = $true)] [string] $Port,
    [string] $Profile = 'portsdevice',
    [string] $Out = "$env:TEMP\kogger_ports_device",
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

$steps = @(
    '{"op": "waitPorts", "timeoutMs": 120000}', '{"op": "ports"}',
    '{"op": "devicePage", "recorder": true}', '{"op": "wait", "ms": 70000}',
    '{"op": "probe", "tag": "page", "root": "DeviceSettingsPage"}', '{"op": "ports"}', '{"op": "quit"}')
$sp = Join-Path $Out 'steps.json'
[System.IO.File]::WriteAllText($sp, ('{"live": true, "steps": [' + ($steps -join ",`n") + ']}'), $utf8)

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
$psi.EnvironmentVariables['PATH'] = 'D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin;' + $env:PATH
$psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
$psi.EnvironmentVariables['KOGGER_SETTINGS_INI'] = '1'
$psi.EnvironmentVariables['KOGGER_AUTOTEST'] = $Port
$psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $sp
$psi.EnvironmentVariables['KOGGER_UI_PROBE'] = $Out
$psi.EnvironmentVariables['KOGGER_UI_PROBE_DELAY_MS'] = '600000'
$proc = Start-TestDesktopProcess $psi
if (-not $proc.WaitForExit($TimeoutS * 1000)) { $proc.Kill(); throw "the app did not end in $TimeoutS s" }

$lines = Get-Content -LiteralPath $log -Encoding UTF8
$start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
$mine = @($lines[($start - 1)..($lines.Count - 1)])
$mine | Set-Content -LiteralPath (Join-Path $Out 'kogger.log') -Encoding UTF8

$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}
$head = @($mine | Where-Object { $_ -match 'echoedit script: ports of ' } | Select-Object -First 1)
$busLines = @($mine | Where-Object { $_ -match 'echoedit script: port \d+ ' })
$first = @($busLines | Select-Object -First 8)
$buses = @($first | ForEach-Object {
    if ($_ -match 'port (\d+) "([^"]*)": supply (\d+), function (\d+)/(\d+) caps 0x(\w+), uart (\d+)/(\d+) caps 0x(\w+), tx (\d+) rx (\d+), baud (\d+); (.*)$') {
        [pscustomobject]@{ Id = [int]$Matches[1]; Label = $Matches[2]; Supply = [int]$Matches[3]; Function = [int]$Matches[4]
                           Uart = [int]$Matches[7]; Pins = $Matches[13] }
    } })
$labels = ($buses | ForEach-Object { $_.Label }) -join ' '
Expect ($head.Count -eq 1 -and $head[0] -match 'support 1, 8 buses' -and $labels -eq 'SONAR1 SONAR2 TELEM GNSS I2C IN1,2 IN3,4 OUTS' -and
        (($buses | ForEach-Object { $_.Supply }) -join '') -eq '11222220' -and (($buses | ForEach-Object { $_.Function }) -join '') -eq '12340000' -and
        (($buses | ForEach-Object { $_.Uart }) -join '') -eq '23140000') 'buses' ($labels)
$pinText = ($buses | ForEach-Object { $_.Pins }) -join '; '
$pins = @($pinText -split '; ' | Where-Object { $_ })
$pwm = @($pins | Where-Object { $_ -match ' role 3/3 ' -and $_ -match ' param 0 ' } | ForEach-Object { ($_ -split ' ')[0] })
$off = @($pins | Where-Object { $_ -match '^(SCL|SDA|IN2|IN4|O2|O3) ' -and $_ -match ' role 0/0 ' })
$uart = @($pins | Where-Object { $_ -match '^(TX|RX) ' -and $_ -match ' caps 0x00 role 1/1 ' })
Expect ($pins.Count -eq 17 -and ($pwm -join ',') -eq 'IN1,IN3,O1' -and $off.Count -eq 6 -and $uart.Count -eq 8) 'pins' (
    "{0} pins; PWM to OUT1: {1}; off {2}; UART {3}" -f $pins.Count, ($pwm -join ','), $off.Count, $uart.Count)
Expect ($head.Count -eq 1 -and $head[0] -match 'OUT pins \[IN1, IN3, O1 \|  \| \]') 'outs' ($(if ($head.Count) { $head[0] -replace '^.*OUT pins ', 'OUT pins ' } else { '-' }))

$json = Join-Path $Out 'ui-page.json'
$cards = @(); $cut = @(); $groupShown = $false
if (Test-Path $json) {
    $doc = [System.IO.File]::ReadAllText($json, $utf8) | ConvertFrom-Json
    $stack = New-Object System.Collections.Stack
    $stack.Push($doc.root)
    while ($stack.Count) {
        $n = $stack.Pop()
        if ($n.name -eq 'recorderPortsGroup') { $groupShown = [bool]$n.visible }
        if ($n.name -and ([string]$n.name).StartsWith('portBus_') -and $n.visible) { $cards += ([string]$n.name).Substring(8) }
        if ($n.truncated -and $n.visible) { $cut += [string]$n.text }
        foreach ($c in @($n.children)) { if ($c) { $stack.Push($c) } }
    }
}
Expect ($groupShown -and $cards.Count -eq 8 -and $cut.Count -eq 0) 'page' (
    "group shown {0}; cards {1}; cut texts {2}" -f $groupShown, (($cards | Sort-Object) -join ' '), $(if ($cut.Count) { $cut -join ' | ' } else { 'none' }))
$tally = @($mine | Where-Object { $_ -match 'sent:.*KP1 \[0\] id 0x16 v1 [\d.]+ KB/s ([\d.]+)/s' } | ForEach-Object {
    if ($_ -match 'KP1 \[0\] id 0x16 v1 [\d.]+ KB/s ([\d.]+)/s') { [double]$Matches[1] } })
Expect ($tally.Count -ge 1 -and ($tally | Measure-Object -Maximum).Maximum -ge 3.0 -and ($tally | Measure-Object -Maximum).Maximum -le 5.5) 'live' (
    "ID_PIN v1 per second in the minute tallies: {0}" -f $(if ($tally.Count) { $tally -join ', ' } else { 'none logged' }))
"ports device: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
"log: {0}" -f (Join-Path $Out 'kogger.log')
exit ([int]($failed -gt 0))

#requires -Version 5.1
<#
.SYNOPSIS
    A Recorder's Connectors group end to end in the running app, against a stand playing a Recorder with the port table
    the real one reported on 2026-10-24 (ports_device.ps1): the page and its live poll, no device needed.

.DESCRIPTION
    One run of KoggerApp on the test desktop (KOGGER_PROFILE reset from the transfer folder's KoggerApp-perf.reg,
    KOGGER_AUTOTEST=UDP:<port>, a live edit script, KOGGER_UI_PROBE). The stand is a Recorder at route 250: ID_VERSION
    v0 as board 9 every 300 ms, ID_BUS v0 for its 8 buses, ID_PIN v0 for its 17 pins, ID_PIN v1 with every pin (IN1,
    IN3, O1 PWM at 1500 us). The script waits for the table, waits 2 s with no page, opens the Recorder's page for 3 s,
    dumps it, quits. Checked:
      ask        each bus and each pin asked once, one frame at a time (the next asked only after an answer)
      quiet      no ID_PIN v1 before the page is open
      live       ID_PIN v1 about 5 times a second while it is open (10..17 in 3 s)
      page       the Connectors group shows 8 cards, "PWM to OUT1 · 1500 µs" among the texts, nothing cut
      servo      the servo panel's OUT1 row would read "OUT1: IN1, IN3, O1" (the property behind it)
      qml        no QML error in the log
    Exit code 0 when every check holds.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\ports_check\ports_live.ps1 -Exe <build>\KoggerApp.exe -PathPrefix "D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [string] $PathPrefix,
    [string] $Profile = 'perf',
    [int]    $Port = 47651,
    [string] $Out = "$env:TEMP\kogger_ports_live",
    [string] $PerfReg = 'D:\YandexDisk\DelphiProjects\KoggerApp_myEdition_transfer\KoggerApp-perf.reg',
    [int]    $TimeoutS = 90
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
if (Test-Path -LiteralPath $PerfReg) {
    cmd.exe /c "reg import `"$PerfReg`" >nul 2>&1"   # reg.exe reports success on stderr
}
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
$utf8 = New-Object System.Text.UTF8Encoding $false
$route = 250

function Kp1([int] $type, [int] $ver, [int] $id, [byte[]] $payload) {
    $mode = ($type -band 3) -bor (($ver -band 7) -shl 3) -bor 0x40
    $body = New-Object System.Collections.Generic.List[byte]
    $body.Add([byte]$route); $body.Add([byte]$mode); $body.Add([byte]$id); $body.Add([byte]$payload.Length)
    $body.AddRange($payload)
    $c1 = 0; $c2 = 0
    foreach ($b in $body) { $c1 = ($c1 + $b) -band 0xFF; $c2 = ($c2 + $c1) -band 0xFF }
    $f = New-Object System.Collections.Generic.List[byte]
    $f.Add(0xBB); $f.Add(0x55); $f.AddRange($body); $f.Add([byte]$c1); $f.Add([byte]$c2)
    return , $f.ToArray()
}
function Text([System.Collections.Generic.List[byte]] $l, [string] $s) {
    $b = [Text.Encoding]::ASCII.GetBytes($s); $l.Add([byte]$b.Length); $l.AddRange($b)
}
# The real Recorder's table (ports_device.ps1, 2026-10-24): label, supply, function caps, function, UART caps, UART,
# TX, RX, baud, pins.
$buses = @(
    @('SONAR1', 1, 0x1F, 1, 0x0A, 2, 0, 1, 57600, @(0, 1)), @('SONAR2', 1, 0x1F, 2, 0x04, 3, 2, 3, 57600, @(2, 3)),
    @('TELEM', 2, 0x1F, 3, 0x01, 1, 4, 5, 5000000, @(4, 5)), @('GNSS', 2, 0x17, 4, 0x08, 4, 6, 7, 9600, @(6, 7)),
    @('I2C', 2, 0x1F, 0, 0x0C, 0, 255, 255, 0, @(8, 9)), @('IN1,2', 2, 0x01, 0, 0, 0, 255, 255, 0, @(10, 11)),
    @('IN3,4', 2, 0x01, 0, 0, 0, 255, 255, 0, @(12, 13)), @('OUTS', 0, 0x01, 0, 0, 0, 255, 255, 0, @(14, 15, 16)))
# label, MCU pin, bus, role caps, role, timer, param, value
$pins = @(
    @('TX', 'PA2', 0, 0, 1, 0, 0, 0), @('RX', 'PA3', 0, 0, 1, 0, 0, 0), @('TX', 'PC4', 1, 0, 1, 0, 0, 0), @('RX', 'PC5', 1, 0, 1, 0, 0, 0),
    @('TX', 'PA9', 2, 0, 1, 0, 0, 0), @('RX', 'PA10', 2, 0, 1, 0, 0, 0), @('TX', 'PC1', 3, 0, 1, 0, 0, 0), @('RX', 'PC0', 3, 0, 1, 0, 0, 0),
    @('SCL', 'PB10', 4, 0x39, 0, 2, 0, 0), @('SDA', 'PB11', 4, 0x39, 0, 2, 0, 0), @('IN1', 'PB0', 5, 0x79, 3, 1, 0, 1500),
    @('IN2', 'PB1', 5, 0x79, 0, 1, 0, 0), @('IN3', 'PA0', 6, 0x79, 3, 2, 0, 1500), @('IN4', 'PA1', 6, 0x79, 0, 2, 0, 0),
    @('O1', 'PA6', 7, 0x79, 3, 16, 0, 1500), @('O2', 'PB15', 7, 0x39, 0, 15, 0, 0), @('O3', 'PB14', 7, 0x39, 0, 15, 0, 0))
function BusFrame([int] $i) {
    $b = $buses[$i]; $l = New-Object System.Collections.Generic.List[byte]
    $l.Add([byte]$i); $l.Add([byte]$buses.Count); $l.Add([byte]$b[1]); $l.Add([byte]$b[2]); $l.Add([byte]$b[3]); $l.Add([byte]$b[3])
    $l.Add([byte]$b[4]); $l.Add([byte]$b[5]); $l.Add([byte]$b[5]); $l.Add([byte]$b[6]); $l.Add([byte]$b[7])
    $l.AddRange([BitConverter]::GetBytes([uint32]$b[8]))
    $l.Add([byte]$b[9].Count); foreach ($p in $b[9]) { $l.Add([byte]$p) }
    Text $l $b[0]
    return , (Kp1 1 0 0x17 $l.ToArray())
}
function PinFrame([int] $i) {
    $p = $pins[$i]; $l = New-Object System.Collections.Generic.List[byte]
    $l.Add([byte]$i); $l.Add([byte]$p[2]); $l.AddRange([BitConverter]::GetBytes([uint32]$p[3]))
    $l.Add([byte]$p[4]); $l.Add([byte]$p[4]); $l.Add([byte]$p[5]); $l.Add([byte]$p[6])
    $l.AddRange([BitConverter]::GetBytes([int32]$p[7]))
    Text $l $p[0]; Text $l $p[1]
    return , (Kp1 1 0 0x16 $l.ToArray())
}
function ValuesFrame() {
    $l = New-Object System.Collections.Generic.List[byte]
    $l.Add([byte]$pins.Count)
    for ($i = 0; $i -lt $pins.Count; $i++) { $l.Add([byte]$i); $l.Add([byte]$pins[$i][4]); $l.AddRange([BitConverter]::GetBytes([int32]$pins[$i][7])) }
    return , (Kp1 1 1 0x16 $l.ToArray())
}
$version = Kp1 1 0 0x20 ([byte[]](0, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 9, 9, 9))   # board 9: RECORDER Mini

$steps = @('{"op": "waitPorts", "timeoutMs": 30000}', '{"op": "wait", "ms": 2000}', '{"op": "ports"}',
           '{"op": "devicePage", "recorder": true}', '{"op": "wait", "ms": 3000}',
           '{"op": "probe", "tag": "page", "root": "DeviceSettingsPage"}', '{"op": "quit"}')
$scriptPath = Join-Path $Out 'script.json'
[System.IO.File]::WriteAllText($scriptPath, ('{"live": true, "steps": [' + ($steps -join ",`n") + ']}'), $utf8)

$udp = New-Object System.Net.Sockets.UdpClient(New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, ($Port + 1)))
[void]$udp.Client.IOControl(-1744830452, [byte[]](0, 0, 0, 0), $null)   # SIO_UDP_CONNRESET off
$udp.Client.ReceiveTimeout = 20
$app = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, $Port)
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
$psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
$psi.EnvironmentVariables['KOGGER_AUTOTEST'] = "UDP:$Port"
$psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $scriptPath
$psi.EnvironmentVariables['KOGGER_UI_PROBE'] = $Out
$psi.EnvironmentVariables['KOGGER_UI_PROBE_DELAY_MS'] = '600000'
$before = if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
$proc = Start-TestDesktopProcess $psi

$asks = New-Object System.Collections.Generic.List[object]   # the app's 0x16/0x17 GETs: time, id, ver, arg
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$deadline = (Get-Date).AddSeconds($TimeoutS)
$beat = [DateTime]::MinValue
try {
    while (-not $proc.HasExited) {
        if ((Get-Date) -gt $deadline) { $proc.Kill(); throw "the app did not end in $TimeoutS s" }
        if (((Get-Date) - $beat).TotalMilliseconds -ge 300) { $beat = Get-Date; [void]$udp.Send($version, $version.Length, $app) }
        $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
        try { $d = $udp.Receive([ref]$remote) } catch [System.Net.Sockets.SocketException] { continue }
        $t = $clock.Elapsed.TotalMilliseconds
        $i = 0
        while ($i + 8 -le $d.Length) {
            if ($d[$i] -ne 0xBB -or $d[$i + 1] -ne 0x55) { $i++; continue }
            $len = $d[$i + 5]; $total = 6 + $len + 2
            if ($i + $total -gt $d.Length) { break }
            $mode = $d[$i + 3]; $type = $mode -band 3; $ver = ($mode -shr 3) -band 7; $id = $d[$i + 4]
            if ($type -eq 3 -and ($id -eq 0x16 -or $id -eq 0x17)) {
                $arg = if ($len -ge 1) { [int]$d[$i + 6] } else { -1 }
                $asks.Add([pscustomobject]@{ T = $t; Id = $id; Ver = $ver; Arg = $arg })
                $f = $null
                if ($id -eq 0x17 -and $arg -ge 0 -and $arg -lt $buses.Count) { $f = BusFrame $arg }
                elseif ($id -eq 0x16 -and $ver -eq 0 -and $arg -ge 0 -and $arg -lt $pins.Count) { $f = PinFrame $arg }
                elseif ($id -eq 0x16 -and $ver -eq 1) { $f = ValuesFrame }
                if ($f) { [void]$udp.Send($f, $f.Length, $app) }
            }
            $i += $total
        }
    }
} finally {
    $udp.Close()
    if (-not $proc.HasExited) { $proc.Kill() }
}
$fs = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); [void]$fs.Seek($before, 'Begin')
$text = (New-Object System.IO.StreamReader($fs, [Text.Encoding]::UTF8)).ReadToEnd(); $fs.Close()
$mine = @($text -split "`r?`n")
$mine | Set-Content -LiteralPath (Join-Path $Out 'kogger.log') -Encoding UTF8

$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}
$all = $asks.ToArray()
$busAsks = @($all | Where-Object { $_.Id -eq 0x17 })
$pinAsks = @($all | Where-Object { $_.Id -eq 0x16 -and $_.Ver -eq 0 })
$v1 = @($all | Where-Object { $_.Id -eq 0x16 -and $_.Ver -eq 1 })
$busIds = ($busAsks | ForEach-Object { $_.Arg }) -join ','
$pinIds = ($pinAsks | ForEach-Object { $_.Arg }) -join ','
Expect ($busIds -eq '0,1,2,3,4,5,6,7' -and $pinIds -eq (0..16 -join ',')) 'ask' ("buses {0}; pins {1}" -f $busIds, $pinIds)
# The page opens at the devicePage step: the log line's time against the stand's clock is not shared, so the boundary
# is the first v1: none may come before the table is read plus the script's 2 s wait.
$tableDone = if ($pinAsks.Count) { $pinAsks[-1].T } else { 0 }
$early = @($v1 | Where-Object { $_.T -lt $tableDone + 1800 })
Expect ($early.Count -eq 0) 'quiet' ("{0} ID_PIN v1 within 1.8 s after the table was read (the page not open yet)" -f $early.Count)
$span = if ($v1.Count -ge 2) { ($v1[-1].T - $v1[0].T) / 1000.0 } else { 0 }
Expect ($v1.Count -ge 10 -and $v1.Count -le 17) 'live' ("{0} ID_PIN v1 over {1:F1} s" -f $v1.Count, $span)
$json = Join-Path $Out 'ui-page.json'
$cards = @(); $cut = @(); $texts = @(); $groupShown = $false
if (Test-Path $json) {
    $doc = [System.IO.File]::ReadAllText($json, $utf8) | ConvertFrom-Json
    $stack = New-Object System.Collections.Stack
    $stack.Push($doc.root)
    while ($stack.Count) {
        $n = $stack.Pop()
        if ($n.name -eq 'recorderPortsGroup') { $groupShown = [bool]$n.visible }
        if ($n.name -and ([string]$n.name).StartsWith('portBus_') -and $n.visible) { $cards += ([string]$n.name).Substring(8) }
        if ($n.text -and $n.visible) { $texts += [string]$n.text }
        if ($n.truncated -and $n.visible) { $cut += [string]$n.text }
        foreach ($c in @($n.children)) { if ($c) { $stack.Push($c) } }
    }
}
$pwmTexts = @($texts | Where-Object { $_ -match '1500' -and $_ -match 'OUT1' })
Expect ($groupShown -and $cards.Count -eq 8 -and $pwmTexts.Count -eq 3 -and $cut.Count -eq 0) 'page' (
    "group shown {0}; cards {1}; PWM texts {2}; cut {3}" -f $groupShown, $cards.Count, $pwmTexts.Count, $(if ($cut.Count) { $cut -join ' | ' } else { 'none' }))
$portsLine = @($mine | Where-Object { $_ -match 'echoedit script: ports of ' } | Select-Object -Last 1)
Expect ($portsLine.Count -eq 1 -and $portsLine[0] -match 'OUT pins \[IN1, IN3, O1 \|') 'servo' ($(if ($portsLine.Count) { $portsLine[0] -replace '^.*OUT pins ', '' } else { '-' }))
$qml = @($mine | Where-Object { $_ -match '\.qml:\d+' -and $_ -match 'Error|TypeError|ReferenceError|Unable to assign|Cannot assign' })
Expect ($qml.Count -eq 0) 'qml' ($(if ($qml.Count) { $qml[0] } else { 'no QML error' }))
"ports live: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
"logs: {0}" -f $Out
exit ([int]($failed -gt 0))

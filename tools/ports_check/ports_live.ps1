#requires -Version 5.1
<#
.SYNOPSIS
    A Recorder's Connectors group end to end in the running app, against a stand playing a Recorder with the port table
    the real one reported on 2026-10-24 (ports_device.ps1): the page and its live poll, no device needed.

.DESCRIPTION
    One run of KoggerApp on the test desktop (KOGGER_PROFILE reset from the transfer folder's KoggerApp-perf.reg,
    KOGGER_AUTOTEST=UDP:<port>, a live edit script, KOGGER_UI_PROBE). The stand is a Recorder at route 250: ID_VERSION
    v0 as board 9 every 300 ms, ID_BUS v0 for its 8 buses (the four UART ones settable as the firmware of
    2026-10-27: functions swapped by SETTING v0, rates by v2..v4), ID_PIN v0 for its 17 pins, ID_PIN v1 with every pin (IN1,
    IN3, O1 PWM at 1500 us). The script waits for the table, waits 2 s with no page, opens the Recorder's page for 3 s,
    dumps it, quits. Checked:
      ask        each bus and each pin asked once, one frame at a time (the next asked only after an answer)
      quiet      no ID_PIN v1 before the page is open
      live       ID_PIN v1 about 5 times a second while it is open (10..17 in 3 s)
      page       the Connectors group shows the board with 8 blocks, "OUT1 · 1500 µs" three times among the texts, nothing cut
      fit        every block inside the drawing's width (the drawing scaled down on a panel under 560 px)
      servo      the servo panel's OUT1 row would read "OUT1: IN1, IN3, O1" (the property behind it)
      uart out   the UART connectors' settings, in the order they went out: each function's settings read once after
                 the table (v2 1, v2 2, v3 3, v4 4); TELEM given telemetry (SETTING v0, ID_FLASH, the four connectors
                 read again); telemetry 921600 (v3, ID_FLASH, read back, its connector read); navigation 9600 (v4,
                 the same); SONAR1 given navigation refused by the stand (RespErrorRuntime, as through a UART) and
                 nothing after; a rate off the list never sent
      uart state the driver's connectors, settings and state after each step
      uart page  TELEM's menu: function and speed shown but off on this UDP link, "Speed now" 921600, the note whole;
                 its block reads "telemetry"; SONAR1's note is the refusal over the link
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
    [int]    $TimeoutS = 90,
    # a picture of the window with the page open, to look at
    [string] $Shot
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

function Kp1([int] $type, [int] $ver, [int] $id, [byte[]] $payload, [int] $flags = 0x40) {
    $mode = ($type -band 3) -bor (($ver -band 7) -shl 3) -bor $flags
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
# The real Recorder's table (ports_device.ps1, 2026-10-24) as the settings firmware reports it (2026-10-27): label,
# supply, function caps, function, UART caps, UART, TX, RX, baud, pins. On the four UART connectors (caps 0x1E, their
# own UART only) the function and the rate in use are the stand's ($busFunction, Rate).
$buses = @(
    @('SONAR1', 1, 0x1E, 1, 0x02, 2, 0, 1, 0, @(0, 1)), @('SONAR2', 1, 0x1E, 3, 0x04, 3, 2, 3, 0, @(2, 3)),
    @('TELEM', 2, 0x1E, 2, 0x01, 1, 4, 5, 0, @(4, 5)), @('GNSS', 2, 0x1E, 4, 0x08, 4, 6, 7, 0, @(6, 7)),
    @('I2C', 2, 0x01, 0, 0x00, 0, 255, 255, 0, @(8, 9)), @('IN1,2', 2, 0x01, 0, 0, 0, 255, 255, 0, @(10, 11)),
    @('IN3,4', 2, 0x01, 0, 0, 0, 255, 255, 0, @(12, 13)), @('OUTS', 0, 0x01, 0, 0, 0, 255, 255, 0, @(14, 15, 16)))
# The owner's Recorder on 2026-10-27: sonar A on SONAR1 (auto), telemetry on SONAR2 (115200), sonar B on TELEM
# (115200), navigation on GNSS (auto). A function's settings: baud (0 auto) and its other fields as bytes.
$busFunction = @(1, 3, 2, 4)
$funcBaud = @(0, 0, 115200, 115200, 0)
$funcRest = @($null, [byte[]](0, 1, 1), [byte[]](0, 1, 1), [byte[]](0), [byte[]](0, 0))
$funcVer = @(0, 2, 2, 3, 4)
$rates = @(0, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 2000000, 5000000)
$uartSets = 0   # the fourth SETTING is refused as through a UART (RespErrorRuntime)
function Rate([int] $f) { if ($script:funcBaud[$f] -gt 0) { $script:funcBaud[$f] } elseif ($f -eq 3) { 115200 } else { 57600 } }   # auto: found
# label, MCU pin, bus, role caps, role, timer, param, value
$pins = @(
    @('TX', 'PA2', 0, 0, 1, 0, 0, 0), @('RX', 'PA3', 0, 0, 1, 0, 0, 0), @('TX', 'PC4', 1, 0, 1, 0, 0, 0), @('RX', 'PC5', 1, 0, 1, 0, 0, 0),
    @('TX', 'PA9', 2, 0, 1, 0, 0, 0), @('RX', 'PA10', 2, 0, 1, 0, 0, 0), @('TX', 'PC1', 3, 0, 1, 0, 0, 0), @('RX', 'PC0', 3, 0, 1, 0, 0, 0),
    @('SCL', 'PB10', 4, 0x39, 0, 2, 0, 0), @('SDA', 'PB11', 4, 0x39, 0, 2, 0, 0), @('IN1', 'PB0', 5, 0x79, 3, 1, 0, 1500),
    @('IN2', 'PB1', 5, 0x79, 0, 1, 0, 0), @('IN3', 'PA0', 6, 0x79, 3, 2, 0, 1500), @('IN4', 'PA1', 6, 0x79, 0, 2, 0, 0),
    @('O1', 'PA6', 7, 0x79, 3, 16, 0, 1500), @('O2', 'PB15', 7, 0x39, 0, 15, 0, 0), @('O3', 'PB14', 7, 0x39, 0, 15, 0, 0))
function BusFrame([int] $i) {
    $b = $buses[$i]; $l = New-Object System.Collections.Generic.List[byte]
    $fn = if ($i -lt 4) { $script:busFunction[$i] } else { $b[3] }
    $baud = if ($i -lt 4) { Rate $fn } else { $b[8] }
    $l.Add([byte]$i); $l.Add([byte]$buses.Count); $l.Add([byte]$b[1]); $l.Add([byte]$b[2]); $l.Add([byte]$fn); $l.Add([byte]$fn)
    $l.Add([byte]$b[4]); $l.Add([byte]$b[5]); $l.Add([byte]$b[5]); $l.Add([byte]$b[6]); $l.Add([byte]$b[7])
    $l.AddRange([BitConverter]::GetBytes([uint32]$baud))
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
function FuncFrame([int] $f) {
    $l = New-Object System.Collections.Generic.List[byte]
    $l.Add([byte]$f); $l.AddRange([BitConverter]::GetBytes([uint32]$script:funcBaud[$f])); $l.AddRange($script:funcRest[$f])
    return , (Kp1 1 $script:funcVer[$f] 0x17 $l.ToArray())
}
function Ack([int] $ver, [int] $id, [int] $code, [byte] $ck1, [byte] $ck2) { return , (Kp1 1 $ver $id ([byte[]]($code, $ck1, $ck2)) 0xC0) }

$steps = @('{"op": "waitPorts", "timeoutMs": 30000}', '{"op": "waitPortOp", "timeoutMs": 15000}', '{"op": "portState"}',
           '{"op": "portFunction", "bus": 2, "function": 3}', '{"op": "waitPortOp", "timeoutMs": 15000}', '{"op": "portState"}',
           '{"op": "portBaud", "function": 3, "baud": 921600}', '{"op": "waitPortOp", "timeoutMs": 15000}', '{"op": "portState"}',
           '{"op": "portBaud", "function": 4, "baud": 9600}', '{"op": "waitPortOp", "timeoutMs": 15000}', '{"op": "portState"}',
           '{"op": "portFunction", "bus": 0, "function": 4}', '{"op": "waitPortOp", "timeoutMs": 15000}', '{"op": "portState"}',
           '{"op": "portBaud", "function": 2, "baud": 1000000}', '{"op": "portState"}',
           '{"op": "wait", "ms": 2000}', '{"op": "ports"}',
           '{"op": "devicePage", "recorder": true}', '{"op": "wait", "ms": 3000}',
           '{"op": "probe", "tag": "page", "root": "DeviceSettingsPage"}',
           '{"op": "clickItem", "name": "boardBlock_TELEM"}', '{"op": "wait", "ms": 400}',
           '{"op": "probe", "tag": "telem", "root": "DeviceSettingsPage"}',
           '{"op": "clickItem", "name": "boardBlock_SONAR1"}', '{"op": "wait", "ms": 400}',
           '{"op": "probe", "tag": "sonar1", "root": "DeviceSettingsPage"}',
           '{"op": "clickItem", "name": "boardBlock_TELEM"}', '{"op": "wait", "ms": 300}')
if ($Shot) {
    $steps += '{"op": "scrollTo", "name": "recorderPortsGroup"}'
    $steps += '{"op": "wait", "ms": 600}'
    $steps += ('{"op": "grab", "path": "' + $Shot.Replace('\', '/') + '"}')
    $steps += '{"op": "scrollTo", "name": "boardMenu"}'               # and the TELEM menu, open by now: <shot>_telem.png
    $steps += '{"op": "wait", "ms": 600}'
    $steps += ('{"op": "grab", "path": "' + ($Shot -replace '\.png$', '_telem.png').Replace('\', '/') + '"}')
}
$steps += '{"op": "quit"}'
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
            if (($type -eq 3 -or $type -eq 2) -and ($id -eq 0x16 -or $id -eq 0x17 -or $id -eq 0x23)) {
                $arg = if ($len -ge 1) { [int]$d[$i + 6] } else { -1 }
                $hex = (($d[($i + 6)..($i + 5 + $len)] | ForEach-Object { '{0:x2}' -f $_ }) -join '')
                if ($len -eq 0) { $hex = '' }
                $asks.Add([pscustomobject]@{ T = $t; Type = $type; Id = $id; Ver = $ver; Arg = $arg; Hex = $hex })
                $ck1 = $d[$i + 6 + $len]; $ck2 = $d[$i + 7 + $len]
                $f = $null
                $ok1 = $mode -band 0x80   # the GET asks for an answer: the firmware sends RespOk after the CONTENT
                if ($type -eq 3 -and $id -eq 0x17 -and $ver -eq 0 -and $arg -ge 0 -and $arg -lt $buses.Count) {
                    $f = BusFrame $arg
                    if ($ok1) { [void]$udp.Send($f, $f.Length, $app); $f = Ack 0 0x17 1 $ck1 $ck2 }
                }
                elseif ($type -eq 3 -and $id -eq 0x17 -and $ver -ge 2 -and $ver -le 4) {
                    if ($arg -ge 1 -and $arg -le 4 -and $funcVer[$arg] -eq $ver) {
                        $f = FuncFrame $arg
                        if ($ok1) { [void]$udp.Send($f, $f.Length, $app); $f = Ack $ver 0x17 1 $ck1 $ck2 }
                    } else { $f = Ack $ver 0x17 3 $ck1 $ck2 }
                }
                elseif ($type -eq 2 -and $id -eq 0x17 -and $ver -eq 0) {   # a function onto a connector: a swap
                    $uartSets++
                    $bus = $arg; $fn = if ($len -ge 2) { [int]$d[$i + 7] } else { 0 }; $u = if ($len -ge 3) { [int]$d[$i + 8] } else { 0 }
                    $code = if ($len -ne 3 -or $bus -lt 0 -or $bus -gt 3 -or $fn -lt 1 -or $fn -gt 4 -or $u -ne $buses[$bus][5]) { 3 }
                            elseif ($uartSets -ge 4) { 8 } else { 1 }
                    if ($code -eq 1) {
                        $from = [array]::IndexOf($busFunction, $fn)
                        $busFunction[$from] = $busFunction[$bus]; $busFunction[$bus] = $fn
                    }
                    $f = Ack 0 0x17 $code $ck1 $ck2
                }
                elseif ($type -eq 2 -and $id -eq 0x17 -and $ver -ge 2 -and $ver -le 4) {   # a function's rate
                    $uartSets++
                    $need = @{ 2 = 8; 3 = 6; 4 = 7 }[$ver]
                    $want = if ($len -ge 5) { [BitConverter]::ToUInt32($d, $i + 7) } else { -1 }
                    $code = if ($len -ne $need -or $arg -lt 1 -or $arg -gt 4 -or $funcVer[$arg] -ne $ver -or $rates -notcontains [int]$want -or
                                ($ver -eq 3 -and $d[$i + 11] -ne 0)) { 3 }
                            elseif ($uartSets -ge 4) { 8 } else { 1 }
                    if ($code -eq 1) { $funcBaud[$arg] = $want; $funcRest[$arg] = [byte[]]$d[($i + 11)..($i + 5 + $len)] }
                    $f = Ack $ver 0x17 $code $ck1 $ck2
                }
                elseif ($type -eq 2 -and $id -eq 0x23) { $f = Ack $ver 0x23 $(if ($hex -eq '4a5d6bc9') { 1 } else { 7 }) $ck1 $ck2 }
                elseif ($type -eq 3 -and $id -eq 0x16 -and $ver -eq 0 -and $arg -ge 0 -and $arg -lt $pins.Count) { $f = PinFrame $arg }
                elseif ($type -eq 3 -and $id -eq 0x16 -and $ver -eq 1) { $f = ValuesFrame }
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
$firstFunc = @($all | Where-Object { $_.Id -eq 0x17 -and $_.Ver -ge 2 } | Select-Object -First 1)
$tableEnd = if ($firstFunc.Count) { $firstFunc[0].T } else { [double]::MaxValue }
$busAsks = @($all | Where-Object { $_.Type -eq 3 -and $_.Id -eq 0x17 -and $_.Ver -eq 0 -and $_.T -lt $tableEnd })
$pinAsks = @($all | Where-Object { $_.Type -eq 3 -and $_.Id -eq 0x16 -and $_.Ver -eq 0 })
$v1 = @($all | Where-Object { $_.Type -eq 3 -and $_.Id -eq 0x16 -and $_.Ver -eq 1 })
$pageAt = if ($v1.Count) { $v1[0].T } else { [double]::MaxValue }
$busIds = ($busAsks | ForEach-Object { $_.Arg }) -join ','
$pinIds = ($pinAsks | ForEach-Object { $_.Arg }) -join ','
Expect ($busIds -eq '0,1,2,3,4,5,6,7' -and $pinIds -eq (0..16 -join ',')) 'ask' ("buses {0}; pins {1}" -f $busIds, $pinIds)
# The page opens at the devicePage step: the log line's time against the stand's clock is not shared, so the boundary
# is the first v1: none may come before the table is read plus the script's 2 s wait.
$tableDone = if ($pinAsks.Count) { $pinAsks[-1].T } else { 0 }
$early = @($v1 | Where-Object { $_.T -lt $tableDone + 1800 })
Expect ($early.Count -eq 0) 'quiet' ("{0} ID_PIN v1 within 1.8 s after the table was read (the page not open yet)" -f $early.Count)
$span = if ($v1.Count -ge 2) { ($v1[-1].T - $v1[0].T) / 1000.0 } else { 0 }
$rate = if ($span -gt 0) { ($v1.Count - 1) / $span } else { 0 }
Expect ($v1.Count -ge 10 -and $rate -ge 3.5 -and $rate -le 5.6) 'live' ("{0} ID_PIN v1 over {1:F1} s, {2:F1} a second" -f $v1.Count, $span, $rate)
$json = Join-Path $Out 'ui-page.json'
$cards = @(); $cut = @(); $texts = @(); $groupShown = $false; $blockNodes = @(); $paneNode = $null; $layoutNode = $null
if (Test-Path $json) {
    $doc = [System.IO.File]::ReadAllText($json, $utf8) | ConvertFrom-Json
    $stack = New-Object System.Collections.Stack
    $stack.Push($doc.root)
    while ($stack.Count) {
        $n = $stack.Pop()
        if ($n.name -eq 'recorderPortsGroup') { $groupShown = [bool]$n.visible }
        if ($n.name -and ([string]$n.name).StartsWith('boardBlock_') -and $n.visible) { $cards += ([string]$n.name).Substring(11); $blockNodes += $n }
        if ($n.name -eq 'boardDrawing') { $paneNode = $n }
        if ($n.name -eq 'boardLayout') { $layoutNode = $n }
        if ($n.text -and $n.visible) { $texts += [string]$n.text }
        if ($n.truncated -and $n.visible) { $cut += [string]$n.text }
        foreach ($c in @($n.children)) { if ($c) { $stack.Push($c) } }
    }
}
$pwmTexts = @($texts | Where-Object { $_ -match '1500' -and $_ -match 'OUT1' })
Expect ($groupShown -and $cards.Count -eq 8 -and $pwmTexts.Count -eq 3 -and $cut.Count -eq 0) 'page' (
    "group shown {0}; blocks {1}; PWM texts {2}; cut {3}" -f $groupShown, $cards.Count, $pwmTexts.Count, $(if ($cut.Count) { $cut -join ' | ' } else { 'none' }))
# Each block inside the drawing's width: the layout is scaled (its height over the pane's gives the factor), and the
# probe's w is the unscaled one. 2026-10-27 the right column ran past a ~540 px panel.
$outside = @()
if ($paneNode) {
    $k = if ($layoutNode -and $layoutNode.h -gt 0) { $paneNode.h / $layoutNode.h } else { 1 }   # a build before the scale: 1
    $right = $paneNode.sceneX + $paneNode.w + 0.5
    foreach ($b in $blockNodes) { if ($b.sceneX -lt $paneNode.sceneX - 0.5 -or $b.sceneX + $b.w * $k -gt $right) { $outside += ([string]$b.name).Substring(11) } }
}
Expect ($paneNode -and $blockNodes.Count -eq 8 -and $outside.Count -eq 0) 'fit' (
    "pane {0} px, layout {1} px, scale {2:F3}; outside: {3}" -f $(if ($paneNode) { $paneNode.w } else { '-' }), $(if ($layoutNode) { $layoutNode.w } else { '-' }),
    $(if ($paneNode -and $layoutNode -and $layoutNode.h) { $paneNode.h / $layoutNode.h } else { 0 }), $(if ($outside.Count) { $outside -join ', ' } else { 'none' }))
$portsLine = @($mine | Where-Object { $_ -match 'echoedit script: ports of ' } | Select-Object -Last 1)
Expect ($portsLine.Count -eq 1 -and $portsLine[0] -match 'OUT pins \[IN1, IN3, O1 \|') 'servo' ($(if ($portsLine.Count) { $portsLine[0] -replace '^.*OUT pins ', '' } else { '-' }))
# The UART connectors' settings, in the order they went out (the live poll's v0 reads come only with the page open,
# after all this): see the description.
$uartOut = @($all | Where-Object { ($_.Id -eq 0x17 -and ($_.Ver -ge 2 -or $_.Type -eq 2 -or $_.T -gt $tableEnd)) -or $_.Id -eq 0x23 } |
             Where-Object { $_.T -lt $pageAt } |
             ForEach-Object { '{0}{1:x2}v{2}:{3}' -f $(if ($_.Type -eq 2) { 'set' } else { 'get' }), $_.Id, $_.Ver, $_.Hex })
$uartWant = @('get17v2:01', 'get17v2:02', 'get17v3:03', 'get17v4:04',
              'set17v0:020301', 'set23v0:4a5d6bc9', 'get17v0:00', 'get17v0:01', 'get17v0:02', 'get17v0:03',
              'set17v3:0300100e0000', 'set23v0:4a5d6bc9', 'get17v3:03', 'get17v0:02',
              'set17v4:04802500000000', 'set23v0:4a5d6bc9', 'get17v4:04', 'get17v0:03',
              'set17v0:000402')
Expect (($uartOut -join ' ') -eq ($uartWant -join ' ')) 'uart out' ($uartOut -join ' ')
$uartLines = @($mine | Where-Object { $_ -match 'echoedit script: port(State|Function|Baud) of ' } | ForEach-Object { $_ -replace '^.*echoedit script: ', '' })
$s0 = 'buses SONAR1 f1 57600, SONAR2 f3 115200, TELEM f2 115200, GNSS f4 57600; settings f1 0, f2 115200, f3 115200, f4 0'
$s1 = 'buses SONAR1 f1 57600, SONAR2 f2 115200, TELEM f3 115200, GNSS f4 57600; settings f1 0, f2 115200, f3 115200, f4 0'
$s2 = 'buses SONAR1 f1 57600, SONAR2 f2 115200, TELEM f3 921600, GNSS f4 57600; settings f1 0, f2 115200, f3 921600, f4 0'
$s3 = 'buses SONAR1 f1 57600, SONAR2 f2 115200, TELEM f3 921600, GNSS f4 9600; settings f1 0, f2 115200, f3 921600, f4 9600'
$uartLinesWant = @(
    "portState of Recorder: settable 1, state 0 on bus -1; $s0; network link",
    "portFunction of Recorder: settable 1, state 1 on bus 2; $s0; network link",
    "portState of Recorder: settable 1, state 2 on bus 2; $s1; network link",
    "portBaud of Recorder: settable 1, state 1 on bus 2; $s1; network link",
    "portState of Recorder: settable 1, state 2 on bus 2; $s2; network link",
    "portBaud of Recorder: settable 1, state 1 on bus 3; $s2; network link",
    "portState of Recorder: settable 1, state 2 on bus 3; $s3; network link",
    "portFunction of Recorder: settable 1, state 1 on bus 0; $s3; network link",
    "portState of Recorder: settable 1, state 3 on bus 0; $s3; network link",
    "portBaud of Recorder: settable 1, state 3 on bus 0; $s3; network link",
    "portState of Recorder: settable 1, state 3 on bus 0; $s3; network link")
$names = @($uartLines | ForEach-Object { ($_ -split ' of ')[1] -replace ':.*$', '' } | Select-Object -Unique)
$uartLinesNorm = @($uartLines | ForEach-Object { $_ -replace ' of [^:]+:', ' of Recorder:' })
Expect (($uartLinesNorm -join ' | ') -eq ($uartLinesWant -join ' | ')) 'uart state' (($uartLinesNorm -join "`n      ") + "  (device name: $($names -join ','))")
function ProbeNodes([string] $tag) {
    $nodes = @{}   # names ignore case in PowerShell: a local "out" would hide the logs folder
    $f = Join-Path $Out "ui-$tag.json"
    if (Test-Path $f) {
        $doc = [System.IO.File]::ReadAllText($f, $utf8) | ConvertFrom-Json
        $stack = New-Object System.Collections.Stack
        $stack.Push($doc.root)
        while ($stack.Count) {
            $n = $stack.Pop()
            if ($n.name -and -not $nodes.ContainsKey([string]$n.name)) { $nodes[[string]$n.name] = $n }
            foreach ($c in @($n.children)) { if ($c) { $stack.Push($c) } }
        }
    }
    return $nodes
}
$tn = ProbeNodes 'telem'
$sn = ProbeNodes 'sonar1'
$fc = $tn['portFunctionCombo']; $bc = $tn['portBaudCombo']; $now = $tn['portBaudNow']; $note = $tn['portNote']
$nowText = if ($now) { @($now.children | Where-Object { $_.text } | ForEach-Object { [string]$_.text }) -join ' / ' } else { '-' }
# the TELEM block after the swap: its label, then "telemetry" and the rate in use
$telemHeader = @()
$tb = $tn['boardBlock_TELEM']
if ($tb) {
    $st = New-Object System.Collections.Stack; $st.Push($tb); $bt = @()
    while ($st.Count) { $n = $st.Pop(); if ($n.text -and $n.visible) { $bt += [string]$n.text }; foreach ($c in @($n.children)) { if ($c) { $st.Push($c) } } }
    if (($bt -contains 'TELEM') -and ($bt -contains 'telemetry') -and ($bt -contains '921600')) { $telemHeader = @($bt -join ' ') }
}
$sNote = $sn['portNote']
# A UDP link stands for the Wi-Fi bridge: the choices shown, but off, with the note why; SONAR1 shows its refusal.
$pageOk = $fc -and $fc.visible -and -not $fc.enabled -and $bc -and $bc.visible -and -not $bc.enabled -and $nowText -match '921600' -and
          $note -and $note.visible -and $note.text -and -not $note.truncated -and $telemHeader.Count -ge 1 -and
          $sNote -and $sNote.visible -and $sNote.text -and $sNote.text -ne $note.text
Expect $pageOk 'uart page' ("function {0}/{1}, speed {2}/{3}; now '{4}'; TELEM note '{5}'; header {6}; SONAR1 note '{7}'" -f
    [bool]($fc -and $fc.visible), $(if ($fc) { $fc.enabled } else { '-' }), [bool]($bc -and $bc.visible), $(if ($bc) { $bc.enabled } else { '-' }),
    $nowText, $(if ($note) { $note.text } else { '-' }), $(if ($telemHeader.Count) { $telemHeader[0] } else { '-' }), $(if ($sNote) { $sNote.text } else { '-' }))
$qml = @($mine | Where-Object { $_ -match '\.qml:\d+' -and $_ -match 'Error|TypeError|ReferenceError|Unable to assign|Cannot assign' })
Expect ($qml.Count -eq 0) 'qml' ($(if ($qml.Count) { $qml[0] } else { 'no QML error' }))
"ports live: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
"logs: {0}" -f $Out
exit ([int]($failed -gt 0))

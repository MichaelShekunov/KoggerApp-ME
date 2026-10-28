#requires -Version 5.1
<#
.SYNOPSIS
    The line's search (address 0) against a stand playing the logger (Recorder) with a sonar streaming through it from
    before the app connects: the logger sends nothing by itself, so it must be asked on connecting.
    DeviceLiveness::searchHeld, kSearchFirstAsks (2026-10-28: the logger missing from the device list).

.DESCRIPTION
    One run of KoggerApp on the test desktop (KOGGER_PROFILE reset from the transfer folder's KoggerApp-perf.reg,
    KOGGER_AUTOTEST=UDP:<port>, a live edit script that only waits and quits). This script is the line: the sonar
    (route 2) sends a marked ID_VERSION frame every 20 ms from before the app listens until the end; the logger answers
    each ID_VERSION v0 GETTING to route 0 or 250 with a marked ID_VERSION v0 of board 9 (RecorderMini) from route 250,
    and nothing else.
    Checked:
      asked     the search asks ID_VERSION at route 0 within 2 s of the app's first frame
      found     the app asks the logger for its UART buses (ID_BUS 0x17 to route 250), which it asks of a logger only
      held      no frame of the search (ID_MARK 0x21, ID_VERSION 0x20) to route 0 from 2 s after the app's first
                frame while the sonar streams (the logger's own RECORDER_STATUS and ID_STREAM go there too, as before)
    Exit code 0 when every check holds. A build without the first asks fails "asked" and "found".

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\qml_check\recordersearch_live.ps1 -Exe <build>\KoggerApp.exe -PathPrefix "D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [string] $PathPrefix,
    [string] $Profile = 'perf',
    [int]    $Port = 47633,
    [string] $Out = "$env:TEMP\kogger_recordersearch_live",
    [string] $PerfReg = 'D:\YandexDisk\DelphiProjects\KoggerApp_myEdition_transfer\KoggerApp-perf.reg'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
New-Item -ItemType Directory -Force -Path $Out | Out-Null
if (Test-Path -LiteralPath $PerfReg) {
    # reg.exe reports success on stderr; under 'Stop' PowerShell 5.1 would take that for a failure.
    cmd.exe /c "reg import `"$PerfReg`" >nul 2>&1"
}
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"

function Kp1([int] $route, [int] $type, [int] $ver, [int] $id, [byte[]] $payload) {
    $mode = ($type -band 3) -bor (($ver -band 7) -shl 3) -bor 0x40   # the mark set: "this app's device"
    $body = New-Object System.Collections.Generic.List[byte]
    $body.Add([byte]$route); $body.Add([byte]$mode); $body.Add([byte]$id); $body.Add([byte]$payload.Length)
    $body.AddRange($payload)
    $c1 = 0; $c2 = 0
    foreach ($b in $body) { $c1 = ($c1 + $b) -band 0xFF; $c2 = ($c2 + $c1) -band 0xFF }
    $f = New-Object System.Collections.Generic.List[byte]
    $f.Add(0xBB); $f.Add(0x55); $f.AddRange($body); $f.Add([byte]$c1); $f.Add([byte]$c2)
    return , $f.ToArray()
}
# ID_VERSION v0: the sonar as a Basic2D (12) board, the logger as RecorderMini (9).
$sonar = Kp1 2 1 0 0x20 ([byte[]](0, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4))
$logger = Kp1 250 1 0 0x20 ([byte[]](0, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 6, 7, 8))

$scriptPath = Join-Path $Out 'script.json'
'{"live": true, "steps": [{"op": "wait", "ms": 12000}, {"op": "quit"}]}' | Set-Content -LiteralPath $scriptPath -Encoding ASCII

$udp = New-Object System.Net.Sockets.UdpClient(New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, ($Port + 1)))
[void]$udp.Client.IOControl(-1744830452, [byte[]](0, 0, 0, 0), $null)   # SIO_UDP_CONNRESET off
$udp.Client.ReceiveTimeout = 5
$app = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, $Port)
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
$psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
$psi.EnvironmentVariables['KOGGER_AUTOTEST'] = "UDP:$Port"
$psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $scriptPath
$before = if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
$proc = Start-TestDesktopProcess $psi

$heard = New-Object System.Collections.Generic.List[object]   # the app's frames: time, route, id, ver
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$next = 0.0
$answers = 0
try {
    while (-not $proc.HasExited) {
        $t = $clock.Elapsed.TotalMilliseconds
        if ($t -gt 40000) { $proc.Kill(); throw 'the app did not end in 40 s' }
        if ($t -ge $next) {                                    # the sonar streams from the start, the app or not
            $next = $t + 20
            [void]$udp.Send($sonar, $sonar.Length, $app)
        }
        $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
        try { $d = $udp.Receive([ref]$remote) } catch [System.Net.Sockets.SocketException] { continue }
        $at = $clock.Elapsed.TotalMilliseconds / 1000.0
        $i = 0
        while ($i + 8 -le $d.Length) {
            if ($d[$i] -ne 0xBB -or $d[$i + 1] -ne 0x55) { $i++; continue }
            $len = $d[$i + 5]; $total = 6 + $len + 2
            if ($i + $total -gt $d.Length) { break }
            $route = $d[$i + 2]; $mode = $d[$i + 3]; $id = $d[$i + 4]
            $type = $mode -band 3; $ver = ($mode -shr 3) -band 7
            $heard.Add([pscustomobject]@{ S = $at; Route = $route; Id = $id; Ver = $ver; Type = $type })
            # the logger: only an ID_VERSION v0 GETTING (type 3) to route 0 or 250 gets an answer
            if ($id -eq 0x20 -and $type -eq 3 -and $ver -eq 0 -and ($route -eq 0 -or $route -eq 250)) {
                [void]$udp.Send($logger, $logger.Length, $app)
                $answers++
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
$text -split "`r?`n" | Set-Content -LiteralPath (Join-Path $Out 'kogger.log') -Encoding UTF8
$heard | ForEach-Object { '{0,7:F2} s  route {1,3}  id 0x{2:X2} v{3} type {4}' -f $_.S, $_.Route, $_.Id, $_.Ver, $_.Type } |
    Set-Content -LiteralPath (Join-Path $Out 'from_app.txt') -Encoding ASCII

$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}
$all = $heard.ToArray()
if ($all.Count -eq 0) {
    Expect $false 'app' 'no frame from the app at all'
} else {
    $t0 = $all[0].S
    $to0 = @($all | Where-Object { $_.Route -eq 0 })
    $asked = @($to0 | Where-Object { $_.Id -eq 0x20 -and $_.S -lt $t0 + 2 })
    $bus = @($all | Where-Object { $_.Route -eq 250 -and $_.Id -eq 0x17 })
    $late = @($to0 | Where-Object { $_.S -ge $t0 + 2 -and ($_.Id -eq 0x20 -or $_.Id -eq 0x21) })
    Expect ($asked.Count -ge 1) 'asked' ("{0} ID_VERSION to route 0 in the first 2 s (the app's first frame at {1:F2} s), {2} answered by the logger" -f $asked.Count, $t0, $answers)
    Expect ($bus.Count -ge 1) 'found' ("{0} ID_BUS frames to route 250" -f $bus.Count)
    Expect ($late.Count -eq 0) 'held' ("{0} frames of the search to route 0 from 2 s after the first frame while the sonar streams" -f $late.Count)
}
"recorder search live: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
"logs: {0}" -f $Out
exit $(if ($failed -eq 0) { 0 } else { 1 })

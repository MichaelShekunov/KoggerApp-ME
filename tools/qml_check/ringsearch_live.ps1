#requires -Version 5.1
<#
.SYNOPSIS
    The line's search (address 0) against a stand playing a GODIO ring's two boards over UDP: it must not broadcast
    while the boards answer, and must search again when the line goes quiet. DeviceLiveness::searchHeld;
    L412_GODIO docs/RING_HOLES_2026-10-23.md (each broadcast cost the following board a ping).

.DESCRIPTION
    One run of KoggerApp on the test desktop (KOGGER_PROFILE reset from the transfer folder's KoggerApp-perf.reg,
    KOGGER_AUTOTEST=UDP:<port>, a live edit script that only waits and quits). This script is the line: boards 1 and 2
    send a marked ID_VERSION frame each every 50 ms for 15 s, nothing for 10 s, then again for 10 s. It counts the
    app's frames to route 0 (the search: restart, ID_MARK, ID_VERSION) in each phase.
    Checked:
      answering  after the first 2 s of the boards' frames, no frame to route 0 until they stop
      quiet      the line is searched while it is quiet (the link's own ask-all within ~0.3 s, the search at
                 address 0 from ~3 s after the last frame)
      again      no frame to route 0 from 2 s after the boards come back until they stop at 35 s
      log        the app logs the search waiting and asking again
    Exit code 0 when every check holds. A build without the hold fails "answering" (a broadcast every 5 s).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\qml_check\ringsearch_live.ps1 -Exe <build>\KoggerApp.exe -PathPrefix "D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [string] $PathPrefix,
    [string] $Profile = 'perf',
    [int]    $Port = 47631,
    [string] $Out = "$env:TEMP\kogger_ringsearch_live",
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
# ID_VERSION v0 as a Basic2D (12) board.
$version = [byte[]](0, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4)
$board1 = Kp1 1 1 0 0x20 $version
$board2 = Kp1 2 1 0 0x20 $version

$scriptPath = Join-Path $Out 'script.json'
'{"live": true, "steps": [{"op": "wait", "ms": 37000}, {"op": "quit"}]}' | Set-Content -LiteralPath $scriptPath -Encoding ASCII

$udp = New-Object System.Net.Sockets.UdpClient(New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, ($Port + 1)))
[void]$udp.Client.IOControl(-1744830452, [byte[]](0, 0, 0, 0), $null)   # SIO_UDP_CONNRESET off
$udp.Client.ReceiveTimeout = 10
$app = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, $Port)
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
$psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
$psi.EnvironmentVariables['KOGGER_AUTOTEST'] = "UDP:$Port"
$psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $scriptPath
$before = if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
$proc = Start-TestDesktopProcess $psi

$to0 = New-Object System.Collections.Generic.List[object]   # the app's frames to route 0: time, id
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$next = 0.0
$firstSend = -1.0
try {
    while (-not $proc.HasExited) {
        $t = $clock.Elapsed.TotalMilliseconds
        if ($t -gt 60000) { $proc.Kill(); throw 'the app did not end in 60 s' }
        # the boards' phases, from the first send: 0-15 s on, 15-25 s off, 25-35 s on
        if ($t -ge $next) {
            $next = $t + 50
            if ($firstSend -lt 0) { $firstSend = $t }
            $s = ($t - $firstSend) / 1000.0
            if (($s -lt 15) -or ($s -ge 25 -and $s -lt 35)) {
                [void]$udp.Send($board1, $board1.Length, $app)
                [void]$udp.Send($board2, $board2.Length, $app)
            }
        }
        $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
        try { $d = $udp.Receive([ref]$remote) } catch [System.Net.Sockets.SocketException] { continue }
        $at = ($clock.Elapsed.TotalMilliseconds - $firstSend) / 1000.0
        $i = 0
        while ($i + 8 -le $d.Length) {
            if ($d[$i] -ne 0xBB -or $d[$i + 1] -ne 0x55) { $i++; continue }
            $len = $d[$i + 5]; $total = 6 + $len + 2
            if ($i + $total -gt $d.Length) { break }
            if ($d[$i + 2] -eq 0) { $to0.Add([pscustomobject]@{ S = $at; Id = $d[$i + 4] }) }
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
$to0 | ForEach-Object { '{0,7:F2} s  id 0x{1:X2}' -f $_.S, $_.Id } | Set-Content -LiteralPath (Join-Path $Out 'to_route0.txt') -Encoding ASCII

$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}
$all = $to0.ToArray()
$answering = @($all | Where-Object { $_.S -ge 2 -and $_.S -lt 15 })
$quiet = @($all | Where-Object { $_.S -ge 15 -and $_.S -lt 25 })
$again = @($all | Where-Object { $_.S -ge 27 -and $_.S -lt 35 })
$start = @($all | Where-Object { $_.S -lt 2 })
Expect ($answering.Count -eq 0) 'answering' ("{0} frames to route 0 between 2 and 15 s ({1} in the first 2 s)" -f $answering.Count, $start.Count)
Expect ($quiet.Count -ge 1) 'quiet' (
    "{0} frames to route 0 while the line was quiet, the first at {1}" -f $quiet.Count, $(if ($quiet.Count) { '{0:F2} s' -f $quiet[0].S } else { '-' }))
Expect ($again.Count -eq 0) 'again' ("{0} frames to route 0 between 27 and 35 s" -f $again.Count)
$waits = @($mine | Where-Object { $_ -match 'search: another device of the line answers, the search waits' })
$asks = @($mine | Where-Object { $_ -match 'search: the line is quiet, the search asks again' })
Expect ($waits.Count -ge 2 -and $asks.Count -ge 1) 'log' ("waits logged {0}, asks again {1}" -f $waits.Count, $asks.Count)
"ring search live: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
"logs: {0}" -f $Out
exit ([int]($failed -gt 0))

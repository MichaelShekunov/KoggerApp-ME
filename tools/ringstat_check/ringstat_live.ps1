#requires -Version 5.1
<#
.SYNOPSIS
    A GODIO ring board's sync counters (ID_RING_STAT 0x76) end to end in the running app, against a stand playing three
    ring boards over UDP: board 1, the leader, and board 3, a follower, with firmware 1.17 (they have the counters),
    board 2 with 1.14 (it has not).

.DESCRIPTION
    One run of KoggerApp on the test desktop (KOGGER_PROFILE reset from the transfer folder's KoggerApp-perf.reg,
    KOGGER_AUTOTEST=UDP:<port>, a live edit script, KOGGER_UI_PROBE for dumps of the device page). The stand sends each
    board's ID_VERSION v0 (Basic2D) and v2 (its firmware) every 300 ms, answers GETTING 0x76 v0 from board 1 with its
    state (leader, guard 7.64 ms, BREAK interval 0: a leader does not time its syncs, 2 lost passing on, 5 skipped),
    SETTING 0x76 v0 with the key with RESP 1 and the state zeroed, and GETTING from board 3 with what the firmware's
    session read from a live follower on 1.17 (guard 7640 us, interval 51980 us, 3693 relayed, 1040 BREAKs). The script
    opens board 1's page, reads, resets, opens board 2's page, then board 3's and reads. Checked:
      asked      only on request: one GETTING and one SETTING with the key to board 1, one GETTING to board 3, nothing
                 to board 2
      read       the app's log line and the script's state carry what the board sent
      reset      the state after the reset has the counters at zero
      page       board 1's page shows the counters' island with the values and the interval as not measured (never
                 "0.00 ms"), the counter of frames lost passing on; board 2's page has no such island; board 3's shows
                 a follower with a board before it and its interval
      qml        no QML error in the log
    Exit code 0 when every check holds.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\ringstat_check\ringstat_live.ps1 -Exe <build>\KoggerApp.exe -PathPrefix "D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [string] $PathPrefix,
    [string] $Profile = 'perf',
    [int]    $Port = 47641,
    [string] $Out = "$env:TEMP\kogger_ringstat_live",
    [string] $PerfReg = 'D:\YandexDisk\DelphiProjects\KoggerApp_myEdition_transfer\KoggerApp-perf.reg',
    [int]    $TimeoutS = 90
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
if (Test-Path -LiteralPath $PerfReg) {
    # reg.exe reports success on stderr; under 'Stop' PowerShell 5.1 would take that for a failure.
    cmd.exe /c "reg import `"$PerfReg`" >nul 2>&1"
}
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
$utf8 = New-Object System.Text.UTF8Encoding $false
$key = [uint32]3379256650   # 0xC96B5D4A

function Kp1([int] $route, [int] $type, [int] $ver, [int] $id, [byte[]] $payload, [bool] $resp) {
    $mode = ($type -band 3) -bor (($ver -band 7) -shl 3) -bor 0x40   # the mark set, as the firmware sends
    if ($resp) { $mode = $mode -bor 0x80 }
    $body = New-Object System.Collections.Generic.List[byte]
    $body.Add([byte]$route); $body.Add([byte]$mode); $body.Add([byte]$id); $body.Add([byte]$payload.Length)
    $body.AddRange($payload)
    $c1 = 0; $c2 = 0
    foreach ($b in $body) { $c1 = ($c1 + $b) -band 0xFF; $c2 = ($c2 + $c1) -band 0xFF }
    $f = New-Object System.Collections.Generic.List[byte]
    $f.Add(0xBB); $f.Add(0x55); $f.AddRange($body); $f.Add([byte]$c1); $f.Add([byte]$c2)
    return , $f.ToArray()
}
function StateBytes([int] $addr, [int] $role, [int] $upstream, [uint32[]] $u) {   # $u: guard, interval, relayed, breaks, torn, timeouts, skipped
    $l = New-Object System.Collections.Generic.List[byte]
    $l.Add([byte]$addr); $l.Add([byte]$role); $l.Add([byte]$upstream); $l.Add(0)   # role 0 leader / 1 follower, a board before it, spare
    foreach ($v in $u) { $l.AddRange([BitConverter]::GetBytes([uint32]$v)) }
    return , $l.ToArray()
}
$counters = [uint32[]](7640, 0, 0, 0, 2, 0, 5)
$follower = [uint32[]](7640, 51980, 3693, 1040, 0, 0, 0)   # the live follower on 1.17, as the firmware's session read it
$beats = @(
    (Kp1 1 1 0 0x20 ([byte[]](0, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4)) $false),   # board 1: Basic2D
    (Kp1 1 1 2 0x20 ([byte[]](0, 0, 12, 0, 1, 0, 0, 17, 1)) $false),                             # firmware 1.17
    (Kp1 2 1 0 0x20 ([byte[]](0, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 6, 7, 8)) $false),   # board 2: Basic2D
    (Kp1 2 1 2 0x20 ([byte[]](0, 0, 12, 0, 1, 0, 0, 14, 1)) $false),                             # firmware 1.14
    (Kp1 3 1 0 0x20 ([byte[]](0, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 10, 11, 12)) $false), # board 3: Basic2D
    (Kp1 3 1 2 0x20 ([byte[]](0, 0, 12, 0, 1, 0, 0, 17, 1)) $false))                             # firmware 1.17

$steps = @(
    '{"op": "wait", "ms": 4000}',
    '{"op": "devicePage", "addr": 1}', '{"op": "wait", "ms": 1500}', '{"op": "probe", "tag": "page1", "root": "DeviceSettingsPage"}',
    '{"op": "ringStat", "addr": 1, "do": "read"}', '{"op": "wait", "ms": 1000}', '{"op": "ringStat", "addr": 1, "do": "show"}',
    '{"op": "probe", "tag": "read1", "root": "DeviceSettingsPage"}',
    '{"op": "ringStat", "addr": 1, "do": "reset"}', '{"op": "wait", "ms": 1000}', '{"op": "ringStat", "addr": 1, "do": "show"}',
    '{"op": "probe", "tag": "reset1", "root": "DeviceSettingsPage"}',
    '{"op": "devicePage", "addr": 2}', '{"op": "wait", "ms": 1500}', '{"op": "probe", "tag": "page2", "root": "DeviceSettingsPage"}',
    '{"op": "ringStat", "addr": 2, "do": "show"}',
    '{"op": "devicePage", "addr": 3}', '{"op": "wait", "ms": 1500}',
    '{"op": "ringStat", "addr": 3, "do": "read"}', '{"op": "wait", "ms": 1000}', '{"op": "ringStat", "addr": 3, "do": "show"}',
    '{"op": "probe", "tag": "read3", "root": "DeviceSettingsPage"}', '{"op": "quit"}')
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
$psi.EnvironmentVariables['KOGGER_UI_PROBE_DELAY_MS'] = '600000'   # no dump of its own: only the steps'
$before = if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
$proc = Start-TestDesktopProcess $psi

$got = New-Object System.Collections.Generic.List[object]   # the app's 0x76 frames: route, type, ver, payload
$deadline = (Get-Date).AddSeconds($TimeoutS)
$beat = [DateTime]::MinValue
try {
    while (-not $proc.HasExited) {
        if ((Get-Date) -gt $deadline) { $proc.Kill(); throw "the app did not end in $TimeoutS s" }
        if (((Get-Date) - $beat).TotalMilliseconds -ge 300) {
            $beat = Get-Date
            foreach ($f in $beats) { [void]$udp.Send($f, $f.Length, $app) }
        }
        $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
        try { $d = $udp.Receive([ref]$remote) } catch [System.Net.Sockets.SocketException] { continue }
        $i = 0
        while ($i + 8 -le $d.Length) {
            if ($d[$i] -ne 0xBB -or $d[$i + 1] -ne 0x55) { $i++; continue }
            $len = $d[$i + 5]; $total = 6 + $len + 2
            if ($i + $total -gt $d.Length) { break }
            $route = $d[$i + 2]; $mode = $d[$i + 3]; $type = $mode -band 3; $ver = ($mode -shr 3) -band 7
            if ($d[$i + 4] -eq 0x76) {
                $p = if ($len) { [byte[]]$d[($i + 6)..($i + 5 + $len)] } else { [byte[]]@() }
                $got.Add([pscustomobject]@{ Route = $route; Type = $type; Ver = $ver; Payload = $p })
                if ($route -eq 1 -and $type -eq 3 -and $ver -eq 0) {
                    $f = Kp1 1 1 0 0x76 (StateBytes 1 0 0 $counters) $false
                    [void]$udp.Send($f, $f.Length, $app)
                } elseif ($route -eq 3 -and $type -eq 3 -and $ver -eq 0) {
                    $f = Kp1 3 1 0 0x76 (StateBytes 3 1 1 $follower) $false
                    [void]$udp.Send($f, $f.Length, $app)
                } elseif ($route -eq 1 -and $type -eq 2 -and $ver -eq 0) {
                    $ok = $len -eq 4 -and [BitConverter]::ToUInt32($p, 0) -eq $key
                    $f = Kp1 1 1 0 0x76 ([byte[]]($(if ($ok) { 1 } else { 7 }), $d[$i + $total - 2], $d[$i + $total - 1])) $true
                    [void]$udp.Send($f, $f.Length, $app)
                    if ($ok) {
                        $counters = [uint32[]]($counters[0], $counters[1], 0, 0, 0, 0, 0)
                        $f = Kp1 1 1 0 0x76 (StateBytes 1 0 0 $counters) $false
                        [void]$udp.Send($f, $f.Length, $app)
                    }
                }
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
# The island in a dump: whether it shows, and every text under it.
function Island([string] $tag) {
    $json = Join-Path $Out "ui-$tag.json"
    if (-not (Test-Path $json)) { return $null }
    $doc = [System.IO.File]::ReadAllText($json, $utf8) | ConvertFrom-Json
    $stack = New-Object System.Collections.Stack
    $stack.Push($doc.root)
    while ($stack.Count) {
        $n = $stack.Pop()
        if ($n.name -eq 'ringStatIsland') {
            $texts = New-Object System.Collections.Generic.List[string]
            $s2 = New-Object System.Collections.Stack; $s2.Push($n)
            while ($s2.Count) {
                $m = $s2.Pop()
                if ($m.text -and $m.visible) { $texts.Add([string]$m.text) }
                foreach ($c in @($m.children)) { if ($c) { $s2.Push($c) } }
            }
            return [pscustomobject]@{ Visible = [bool]$n.visible; Texts = $texts.ToArray() }
        }
        foreach ($c in @($n.children)) { if ($c) { $stack.Push($c) } }
    }
    return [pscustomobject]@{ Visible = $false; Texts = @() }
}

$all = $got.ToArray()
$to1 = @($all | Where-Object { $_.Route -eq 1 })
$to2 = @($all | Where-Object { $_.Route -eq 2 })
$to3 = @($all | Where-Object { $_.Route -eq 3 })
$gets = @($to1 | Where-Object { $_.Type -eq 3 })
$sets = @($to1 | Where-Object { $_.Type -eq 2 -and $_.Payload.Length -eq 4 -and [BitConverter]::ToUInt32($_.Payload, 0) -eq $key })
$gets3 = @($to3 | Where-Object { $_.Type -eq 3 })
Expect ($gets.Count -eq 1 -and $sets.Count -eq 1 -and $to1.Count -eq 2 -and $to2.Count -eq 0 -and $gets3.Count -eq 1 -and $to3.Count -eq 1) 'asked' (
    "to board 1: {0} GETTING, {1} SETTING with the key ({2} frames); to board 2: {3}; to board 3: {4} GETTING ({5} frames)" -f $gets.Count, $sets.Count, $to1.Count, $to2.Count, $gets3.Count, $to3.Count)
$readLog = @($mine | Where-Object { $_ -match 'ring-stat: address 1, leader, guard 7\.64 ms, BREAK interval not measured; relayed 0, BREAKs 0, lost passing on 2, leads on a timeout 0, skipped 5' })
$readLog3 = @($mine | Where-Object { $_ -match 'ring-stat: address 3, follower \(a board before it\), guard 7\.64 ms, BREAK interval 51\.98 ms; relayed 3693, BREAKs 1040, lost passing on 0' })
$shows = @($mine | Where-Object { $_ -match 'echoedit script: ringStat show at 1:' })
Expect ($readLog.Count -ge 1 -and $readLog3.Count -ge 1 -and $shows.Count -ge 1 -and $shows[0] -match 'supported 1, known 1, role 0, skipped 5, torn 2, error ""') 'read' (
    $(if ($shows.Count) { $shows[0] -replace '^.*ringStat ', '' } else { 'no state logged' }))
Expect ($shows.Count -ge 2 -and $shows[1] -match 'known 1, role 0, skipped 0, torn 0, error ""') 'reset' (
    $(if ($shows.Count -ge 2) { $shows[1] -replace '^.*ringStat ', '' } else { 'no state after the reset' }))
$p1 = Island 'page1'; $r1 = Island 'read1'; $z1 = Island 'reset1'; $p2 = Island 'page2'; $r3 = Island 'read3'
$readTexts = if ($r1) { $r1.Texts -join ' | ' } else { '' }
$resetTexts = if ($z1) { $z1.Texts -join ' | ' } else { '' }
$read3Texts = if ($r3) { $r3.Texts -join ' | ' } else { '' }
Expect ($p1 -and $p1.Visible -and $r1 -and $r1.Visible -and $readTexts -match 'Ring counters' -and $readTexts -match '7\.64 ms' -and
        $readTexts -match '(^| \| )not measured( \| |$)' -and $readTexts -notmatch '0\.00 ms' -and
        $readTexts -match 'Frames lost passing on' -and $readTexts -match '(^| \| )5( \| |$)' -and
        $readTexts -match 'A sync came later than usual' -and
        $z1 -and $resetTexts -match '(^| \| )0( \| |$)' -and $p2 -and -not $p2.Visible -and
        $r3 -and $r3.Visible -and $read3Texts -match 'Follower: pings on the leader''s sync, a board before it' -and
        $read3Texts -match '51\.98 ms' -and $read3Texts -match '(^| \| )3693( \| |$)') 'page' (
    "board 1: shown {0}, after a read: {1}; board 2: shown {2}; board 3 after a read: {3}" -f $(if ($p1) { $p1.Visible } else { 'no dump' }), $readTexts, $(if ($p2) { $p2.Visible } else { 'no dump' }), $read3Texts)
$qml = @($mine | Where-Object { $_ -match '\.qml:\d+' -and $_ -match 'Error|TypeError|ReferenceError|Unable to assign|Cannot assign' })
Expect ($qml.Count -eq 0) 'qml' ($(if ($qml.Count) { $qml[0] } else { 'no QML error' }))
"ring stat live: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
"logs: {0}" -f $Out
exit ([int]($failed -gt 0))

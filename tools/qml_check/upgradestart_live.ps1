#requires -Version 5.1
<#
.SYNOPSIS
    A firmware update against a stand playing a board with a bootloader, and another device that answers the update's
    packets too: the app calls it flashed only when the board answers from its firmware afterwards, and the progress
    shown never falls back. DevDriver::finishUpgradeStart, DevDriver::receivedUpdate.

.DESCRIPTION
    2026-10-29: a USBL behind the logger acknowledged every packet, was told to start (ID_BOOT v1), stayed in its
    bootloader, and the app said "successfully flashed"; the progress jumped between 0 % and its value, as the logger
    refused each packet sent to route 0. Three runs of KoggerApp on the test desktop (KOGGER_PROFILE reset from the
    transfer folder's KoggerApp-perf.reg, KOGGER_AUTOTEST=UDP:<port>, a live edit script: wait, "upgrade" route 0,
    "waitUpgrade", quit). This script is the line:
      board   route 0, Basic2D (12): ID_VERSION v0 and v2 (boot mode 0 in its firmware, 1 in its bootloader); ID_BOOT v0
              into the bootloader, which acknowledges each ID_UPDATE packet (ID_UPGRADE_V0, type 0); after ID_BOOT v1 it
              starts its firmware ("start"), stays in the bootloader ("stay") or says nothing more ("silent")
      other   route 250, Basic2D: ID_VERSION v0 and v2; each ID_UPDATE on route 0 it refuses (a response, error ID), as
              the logger does
    Checked for each run:
      start   the update ends "flashed" (101), the log says the board answers from its firmware
      stay    it ends "failed (-3)", the board still in its bootloader; the warning says it stayed there
      silent  it ends "failed (-2)" after the wait for an answer
      replug  the link closed once the image is in, the board quiet for 2 s (a board on USB starting anew, the logger
              2026-11-03); the app opens the link again by itself, and the new device waits in the old one's place:
              it ends "flashed"
      gone    the link deleted once the image is in: it ends "failed (-2)" after the wait, not after the session's
              3 minutes
      bar     the progress shown never went back ("fw-upgrade: progress back" not logged)
    Exit code 0 when every check holds.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\qml_check\upgradestart_live.ps1 -Exe <build>\KoggerApp.exe -PathPrefix "D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [string] $PathPrefix,
    [string[]] $Runs = @('start', 'stay', 'silent', 'replug', 'gone'),
    [string] $Profile = 'perf',
    [int]    $Port = 47635,
    [string] $Out = "$env:TEMP\kogger_upgradestart_live",
    [string] $PerfReg = 'D:\YandexDisk\DelphiProjects\KoggerApp_myEdition_transfer\KoggerApp-perf.reg'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"

# KP1: BB 55 | route mode id len | payload | Fletcher; mode: type bits 0-1, version bits 3-5, the mark 0x40, response 0x80
function Kp1([int] $route, [int] $type, [int] $ver, [int] $id, [byte[]] $payload, [bool] $resp = $false) {
    $mode = ($type -band 3) -bor (($ver -band 7) -shl 3) -bor 0x40 -bor $(if ($resp) { 0x80 } else { 0 })
    $body = New-Object System.Collections.Generic.List[byte]
    $body.Add([byte]$route); $body.Add([byte]$mode); $body.Add([byte]$id); $body.Add([byte]$payload.Length)
    $body.AddRange($payload)
    $c1 = 0; $c2 = 0
    foreach ($b in $body) { $c1 = ($c1 + $b) -band 0xFF; $c2 = ($c2 + $c1) -band 0xFF }
    $f = New-Object System.Collections.Generic.List[byte]
    $f.Add(0xBB); $f.Add(0x55); $f.AddRange($body); $f.Add([byte]$c1); $f.Add([byte]$c2)
    return , $f.ToArray()
}
$CONTENT = 1; $SETTING = 2; $GETTING = 3
# ID_VERSION v0: board minor 0, board 12 (Basic2D), 12 bytes, serial number; v2: boot mode, minor, board, boot 1.0, 2
# spare, firmware 1.3
function VersionV0([int] $route, [int] $serial) {
    $p = [byte[]](0, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) + [BitConverter]::GetBytes([uint32]$serial)
    return Kp1 $route $CONTENT 0 0x20 $p
}
function VersionV2([int] $route, [int] $bootMode) { return Kp1 $route $CONTENT 2 0x20 ([byte[]]($bootMode, 0, 12, 0, 1, 0, 0, 3, 1)) }

$image = Join-Path $Out 'image.ufw'
[System.IO.File]::WriteAllBytes($image, [byte[]](1..2000 | ForEach-Object { [byte]($_ % 251) }))

$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}

foreach ($run in $Runs) {
    if (Test-Path -LiteralPath $PerfReg) {
        # reg.exe reports success on stderr; under 'Stop' PowerShell 5.1 would take that for a failure.
        cmd.exe /c "reg import `"$PerfReg`" >nul 2>&1"
    }
    $dir = Join-Path $Out $run
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $scriptPath = Join-Path $dir 'script.json'
    $steps = @(@{ op = 'wait'; ms = 3000 }, @{ op = 'upgrade'; path = $image; addr = 0 })
    if ($run -eq 'replug' -or $run -eq 'gone') {   # the link closed once the image is in, as USB does to a board starting anew
        # replug: closed, and the app opens it again by itself; gone: deleted, never back
        $steps += @{ op = 'waitUpgrade'; until = 100; timeoutMs = 30000 }
        $steps += $(if ($run -eq 'replug') { @{ op = 'link'; of = 'upgrade'; close = $true } } else { @{ op = 'link'; of = 'upgrade'; delete = $true } })
    }
    $steps += @{ op = 'waitUpgrade'; timeoutMs = 60000 }, @{ op = 'quit' }
    (@{ live = $true; steps = $steps } | ConvertTo-Json -Depth 5) | Set-Content -LiteralPath $scriptPath -Encoding ASCII

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

    $boot = $false        # the board in its bootloader
    $silent = $false      # the board says nothing more
    $quietUntil = 0.0     # replug: the board says nothing till then (s on the clock), starting anew
    $packets = 0; $refused = 0; $started = $false
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        while (-not $proc.HasExited) {
            if ($clock.Elapsed.TotalSeconds -gt 120) { $proc.Kill(); throw "run $run did not end in 120 s" }
            $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
            try { $d = $udp.Receive([ref]$remote) } catch [System.Net.Sockets.SocketException] { continue }
            $i = 0
            while ($i + 8 -le $d.Length) {
                if ($d[$i] -ne 0xBB -or $d[$i + 1] -ne 0x55) { $i++; continue }
                $len = $d[$i + 5]; $total = 6 + $len + 2
                if ($i + $total -gt $d.Length) { break }
                $route = $d[$i + 2]; $mode = $d[$i + 3]; $id = $d[$i + 4]
                $type = $mode -band 3; $ver = ($mode -shr 3) -band 7
                $payload = [byte[]]$d[($i + 6)..($i + 5 + [Math]::Max(1, $len))]
                $replies = @()
                if ($route -eq 0 -and -not $silent -and $clock.Elapsed.TotalSeconds -ge $quietUntil) {   # the board
                    if ($id -eq 0x20 -and $type -eq $GETTING -and $ver -eq 0) { $replies += , (VersionV0 0 1001) }
                    if ($id -eq 0x20 -and $type -eq $GETTING -and $ver -eq 2) { $replies += , (VersionV2 0 $(if ($boot) { 1 } else { 0 })) }
                    if ($id -eq 0x24 -and $type -eq $SETTING -and $ver -eq 0) { $boot = $true }
                    if ($id -eq 0x24 -and $type -eq $SETTING -and $ver -eq 1) {
                        $started = $true
                        if ($run -eq 'start' -or $run -eq 'replug') { $boot = $false }
                        if ($run -eq 'replug') { $quietUntil = $clock.Elapsed.TotalSeconds + 2.0 }
                        if ($run -eq 'silent' -or $run -eq 'gone') { $silent = $true }
                    }
                    if ($boot -and $id -eq 0x25 -and $type -eq $SETTING -and $ver -eq 0) {
                        $packets++
                        $num = [BitConverter]::ToUInt16($payload, 0)
                        $ack = [BitConverter]::GetBytes([uint16]$num) + [BitConverter]::GetBytes([uint32](($num) * 96)) + [byte[]](0, 1)
                        $replies += , (Kp1 0 $CONTENT 0 0x25 $ack)
                    }
                }
                if ($route -eq 250 -and $id -eq 0x20 -and $type -eq $GETTING -and $ver -eq 0) { $replies += , (VersionV0 250 2002) }
                if ($route -eq 250 -and $id -eq 0x20 -and $type -eq $GETTING -and $ver -eq 2) { $replies += , (VersionV2 250 0) }
                if ($route -eq 0 -and $id -eq 0x25 -and $type -eq $SETTING) {   # the other device: route 0 is its own too
                    $refused++
                    $replies += , (Kp1 250 $CONTENT 0 0x25 ([byte[]](2, 0, 0)) $true)   # a response: error ID
                }
                foreach ($r in $replies) { [void]$udp.Send($r, $r.Length, $app) }
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
    $mine | Set-Content -LiteralPath (Join-Path $dir 'kogger.log') -Encoding UTF8

    $end = @($mine | Where-Object { $_ -match 'echoedit script: waitUpgrade, \d+ ms: ' } | ForEach-Object { $_ -replace '^.*ms: ', '' } |
        Select-Object -Last 1) -join ''
    $upgrade = @($mine | Where-Object { $_ -match 'Upgrade: ' } | ForEach-Object { $_ -replace '^.*Upgrade: ', '' })
    $back = @($mine | Where-Object { $_ -match 'fw-upgrade: progress back' })
    $notes = @($mine | Where-Object { $_ -match 'notify: ' } | ForEach-Object { $_ -replace '^.*notify: ', '' })
    $want = @{ start = 'flashed'; stay = 'failed (-3)'; silent = 'failed (-2)'; replug = 'flashed'; gone = 'failed (-2)' }[$run]
    $line = @{ start = 'the board answers from its firmware'; stay = 'still in its bootloader'; silent = 'has not answered'
               replug = 'the board is back on the link'; gone = 'no device of the board came back' }[$run]
    Expect ($end -eq $want -and @($mine | Where-Object { $_ -match $line }).Count -ge 1) $run (
        "{0} packets acknowledged, told to start {1}; ends '{2}'; {3}" -f $packets, $started, $end, (($upgrade | Select-Object -Last 2) -join ' | '))
    Expect ($back.Count -eq 0 -and $refused -gt 0) "$run bar" ("{0} packets refused by the other device, the bar back {1} times" -f $refused, $back.Count)
    "    notifications: {0}" -f (($notes | Select-Object -Last 2) -join ' | ')
}
"upgrade start live: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
"logs: {0}" -f $Out
exit $(if ($failed -eq 0) { 0 } else { 1 })

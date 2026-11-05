#requires -Version 5.1
<#
.SYNOPSIS
    The Wi-Fi module's profiles (ID_WIFI_PROFILE, firmware 0.22) end to end in the running app, against a stand that
    plays the module over UDP: no module needed.

.DESCRIPTION
    KoggerApp runs on the test desktop with a test profile of its own (KOGGER_PROFILE, KOGGER_SETTINGS_INI written
    fresh; the owner's settings and profile files are not touched) and KOGGER_AUTOTEST=UDP:<port>, driven by a live edit
    script (wifiProfile steps: what the page's Profile section calls). This script is the module, by the contract (the
    module project's docs/SBP_WIFI.md 3g): ID_VERSION as board 87 with firmware 0.22, its settings (ID_WIFI v0/v4/v7,
    ID_WIFI_NET v0/v1/v2/v3 of 21 bytes/v6/v7 with the app's requests through X1), and ID_WIFI_PROFILE: the status,
    the upload (begin: key and length; pieces: in order, each answered; apply: key, everything there, the CRC-32), and
    after an OK: CONTENT v3 {0}, the new status, 200 ms, then silent for -RebootMs, back with the profile's settings, at
    the profile's address, without the app's mark. A profile named "refuse-lines" is refused as the module refuses two
    UDP lines on one port (ERR_PAYLOAD, CONTENT v3 {13}).
    Checked:
      list      the three built-in profiles
      preview   a built-in profile without its passwords is not ready (the access point's asked); with it, it is; what
                it changes against the stand's settings; after a load, the same profile changes nothing (the module's
                settings read back as the profile left them)
      load      the head unit's profile at the same address: the block the stand got = the app's block of that profile
                with those passwords (the offline check proves that one against the module's encoder), the pieces in
                order, back after the reboot with the name; a refusal shown with its reason; the boat (GODIO) profile at
                address 88: found again as another device, carrying the profile
      capture   the module at 88 saved as a file: the file holds what the stand runs, with the password typed
      page      the Profile section's texts in each state; no QML warning from the page
    -Shots <folder>: one more run in Russian that only takes pictures of the section there (the module as found, a
    profile chosen without its password, the head unit's profile loaded); nothing is checked in it.
    Exit code 0 when every check holds, 1 when one fails.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\wifi022_check\profile_live.ps1 -Exe <build>\KoggerApp.exe
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [string] $Check = '',
    [string] $Profile = 'profcheck',
    [int]    $Port = 47641,
    [int]    $RebootMs = 2000,
    [string] $Out = "$env:TEMP\kogger_profile_live",
    [int]    $TimeoutS = 150,
    [string] $Shots = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
if (-not $Check) { $Check = Join-Path (Split-Path (Split-Path $PSScriptRoot)) 'build\wifi022_check\wifi022_check.exe' }
if (-not (Test-Path -LiteralPath $Check)) { throw "run tools\wifi022_check\run.ps1 first: no $Check" }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$utf8 = New-Object System.Text.UTF8Encoding $false
$ini = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile.ini"
$dataDir = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile"
$log = Join-Path $dataDir 'AppLogs\kogger.log'
$profDir = Join-Path $dataDir 'wifi_profiles'
$keyBytes = [byte[]](0x4A, 0x5D, 0x6B, 0xC9)
$failed = 0
$boshka = -join [char[]](0x411, 0x43E, 0x448, 0x43A, 0x430)
$lodka = -join [char[]](0x41B, 0x43E, 0x434, 0x43A, 0x430)

Add-Type -TypeDefinition @'
public static class StandCrc {
    static readonly uint[] T = Make();
    static uint[] Make() {
        var t = new uint[256];
        for (uint i = 0; i < 256; i++) { uint c = i; for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
        return t;
    }
    public static uint Crc32(byte[] d) { uint c = 0xFFFFFFFFu; foreach (byte b in d) c = T[(c ^ b) & 0xFF] ^ (c >> 8); return c ^ 0xFFFFFFFFu; }
}
'@

function Hex([byte[]] $b) { if (-not $b) { return '' }; return (($b | ForEach-Object { $_.ToString('x2') }) -join '') }
function U16([byte[]] $b, [int] $i) { return [int]$b[$i] + 256 * [int]$b[$i + 1] }
function U32([byte[]] $b, [int] $i) { return [uint32]([BitConverter]::ToUInt32($b, $i)) }

# The module's settings, as it runs them now; the head unit's module of the owner's bench before any profile.
function New-Module {
    return @{
        Addr = 87; Role = 0; X1 = [uint32]2000000; X2 = [uint32]921600; Radio = [byte[]](60, 7, 1, 0)
        ApCh = 6; ApHidden = 0; ApMax = 4; ApAuth = 1; ApSsid = 'KoggerBridge'
        Ip = [byte[]](10, 0, 0, 10, 255, 255, 255, 0, 1, 10, 0, 0, 11, 10, 0, 0, 30, 120, 0, 0)
        L0 = [byte[]](1, 0, 10, 0, 0, 10, 0x6C, 0x38, 0x6C, 0x38); L1 = [byte[]](0, 1, 0, 0, 0, 0, 0x6D, 0x38, 0x6D, 0x38)
        Tx = 5; Rx = 4; Xlat = 0; Nets = @('KoggerBoat', 'KoggerBridge')
        Flags = 0; Name = ''; Mark = $true; Silent = [DateTime]::MinValue; Pending = $null
        UpLen = -1; UpCrc = [uint32]0; Up = (New-Object System.Collections.Generic.List[byte]); Reason = 0
    }
}

# The block (format 1) into the stand's settings; $null when it does not read.
function Read-Blob([byte[]] $b) {
    try {
        $i = 0
        if ($b[$i++] -ne 1) { return $null }
        $n = $b[$i++]; $name = $utf8.GetString($b, $i, $n); $i += $n
        $p = @{ Name = $name; Role = $b[$i]; Addr = $b[$i + 1]; X1 = (U32 $b ($i + 2)) }; $i += 6
        $p.Radio = [byte[]]$b[$i..($i + 3)]; $i += 4
        $p.ApCh = $b[$i]; $p.ApHidden = $b[$i + 1]; $p.ApMax = $b[$i + 2]; $p.ApAuth = $b[$i + 3]; $i += 4
        $n = $b[$i++]; $p.ApSsid = $utf8.GetString($b, $i, $n); $i += $n
        $n = $b[$i++]; $p.ApPass = $utf8.GetString($b, $i, $n); $i += $n
        $p.Ip = [byte[]]$b[$i..($i + 19)]; $i += 20
        $p.L0 = [byte[]]$b[$i..($i + 9)]; $i += 10
        $p.L1 = [byte[]]$b[$i..($i + 9)]; $i += 10
        $p.X2 = (U32 $b $i); $i += 4
        $p.Tx = [int]$b[$i]; if ($p.Tx -gt 127) { $p.Tx -= 256 }
        $p.Rx = [int]$b[$i + 1]; if ($p.Rx -gt 127) { $p.Rx -= 256 }
        $p.Xlat = $b[$i + 2]; $i += 3
        $nn = $b[$i++]; $p.Nets = @(); $p.NetPass = @()
        for ($k = 0; $k -lt $nn; $k++) {
            $n = $b[$i++]; $p.Nets += $utf8.GetString($b, $i, $n); $i += $n
            $n = $b[$i++]; $p.NetPass += $utf8.GetString($b, $i, $n); $i += $n
        }
        if ($i -ne $b.Length) { return $null }
        return $p
    } catch { return $null }
}

function Run-App([string] $name, [string[]] $steps, [string] $code = 'en') {
    $dir = Join-Path $Out $name
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    if (Test-Path -LiteralPath $profDir) { Remove-Item -Recurse -Force $profDir }
    $seed = @('[General]', 'schemaVersion=8', '', '[main]', 'ui\welcomeShown=true', 'ui\instrumentsGradeList=2',
              "appLanguageCode=$code", '', '[deviceSettingsPage]', 'openSections="wifi.profile"', '')
    if (Test-Path -LiteralPath $ini) { Remove-Item -LiteralPath $ini -Force }
    New-Item -ItemType Directory -Force -Path (Split-Path $ini) | Out-Null
    [System.IO.File]::WriteAllText($ini, ($seed -join "`r`n"), $utf8)
    $sp = Join-Path $dir 'steps.json'
    [System.IO.File]::WriteAllText($sp, ('{"live": true, "steps": [' + ($steps -join ",`n") + ']}'), $utf8)

    $udp = New-Object System.Net.Sockets.UdpClient(New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, ($Port + 1)))
    [void]$udp.Client.IOControl(-1744830452, [byte[]](0, 0, 0, 0), $null)
    $app = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, $Port)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
    $psi.EnvironmentVariables['PATH'] = 'D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin;' + $env:PATH
    $psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
    $psi.EnvironmentVariables['KOGGER_SETTINGS_INI'] = '1'
    $psi.EnvironmentVariables['KOGGER_AUTOTEST'] = "UDP:$Port"
    $psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $sp
    $psi.EnvironmentVariables['KOGGER_UI_PROBE'] = $dir
    $psi.EnvironmentVariables['KOGGER_UI_PROBE_DELAY_MS'] = '600000'
    $proc = Start-TestDesktopProcess $psi

    $M = New-Module
    $got = New-Object System.Collections.Generic.List[object]
    $blobs = New-Object System.Collections.Generic.List[object]
    $deadline = (Get-Date).AddSeconds($TimeoutS)
    $beat = [DateTime]::MinValue
    function Kp1([int] $type, [int] $ver, [int] $id, [byte[]] $payload, [bool] $resp) {
        $mode = ($type -band 3) -bor (($ver -band 7) -shl 3)
        if ($M.Mark) { $mode = $mode -bor 0x40 }
        if ($resp) { $mode = $mode -bor 0x80 }
        $body = New-Object System.Collections.Generic.List[byte]
        $body.Add([byte]$M.Addr); $body.Add([byte]$mode); $body.Add([byte]$id); $body.Add([byte]$payload.Length)
        $body.AddRange($payload)
        $c1 = 0; $c2 = 0
        foreach ($x in $body) { $c1 = ($c1 + $x) -band 0xFF; $c2 = ($c2 + $c1) -band 0xFF }
        $f = New-Object System.Collections.Generic.List[byte]
        $f.Add(0xBB); $f.Add(0x55); $f.AddRange($body); $f.Add([byte]$c1); $f.Add([byte]$c2)
        return , $f.ToArray()
    }
    function Send([byte[]] $f) { if ((Get-Date) -ge $M.Silent) { [void]$udp.Send($f, $f.Length, $app) } }
    function Ack([int] $id, [int] $ver, [int] $code, [byte[]] $ck) { Send (Kp1 1 $ver $id ([byte[]]($code, $ck[0], $ck[1])) $true) }
    function StatusFrame() {
        $p = New-Object System.Collections.Generic.List[byte]
        $p.Add($(if ($M.Role -eq 1) { 4 } else { 3 })); $p.Add(0x01)
        $p.Add([byte](256 - 50)); $p.Add(6); $p.Add(3); $p.Add(0); $p.Add(0); $p.Add(0)
        $p.AddRange([byte[]](0x24, 0x6F, 0x28, 0xAA, 0xBB, 0xCC))
        $p.AddRange([byte[]](10, 0, 0, 11, 255, 255, 255, 0, 10, 0, 0, 10, 0, 0, 0, 0))
        $p.AddRange([byte[]](0, 0, 0, 0, 0, 0, 0, 0))
        $p.Add(4); $p.AddRange([byte[]](0x42, 0x4F, 0x41, 0x54))
        return , (Kp1 1 0 0x57 $p.ToArray() $false)
    }
    function PortFrame([int] $port) {
        $p = New-Object byte[] 116
        $p[0] = $port; $p[1] = 0
        $p[2] = 0x01; if ($port -eq 0) { $p[2] = 0x05 }                    # X1: the app's requests come through it
        $baud = $(if ($port -eq 0) { $M.X1 } else { $M.X2 })
        $b = [BitConverter]::GetBytes([uint32]$baud); [Array]::Copy($b, 0, $p, 3, 4); [Array]::Copy($b, 0, $p, 7, 4)
        $p[11] = 2
        for ($i = 20; $i -lt 24; $i++) { $p[$i] = 0xFF }
        return , (Kp1 1 7 0x58 $p $false)
    }
    function LineFrame([int] $line) {
        $l = $(if ($line -eq 0) { $M.L0 } else { $M.L1 })
        $p = New-Object System.Collections.Generic.List[byte]
        $p.Add($line); $p.AddRange([byte[]]$l)
        $p.AddRange([BitConverter]::GetBytes([uint32]$(if ($line -eq 0) { $M.X1 } else { $M.X2 })))
        if ($line -eq 0) { $p.Add([byte]255); $p.Add([byte]255) } else { $p.Add([byte]($M.Tx -band 0xFF)); $p.Add([byte]($M.Rx -band 0xFF)) }
        $p.Add(1)                                                          # the UART runs
        $p.Add($(if ($line -eq 1) { $M.Xlat } else { 0 })); $p.Add($(if ($line -eq 1) { $M.Xlat } else { 0 })); $p.Add(1)   # 0.21: the translator in the build
        return , (Kp1 1 3 0x58 $p.ToArray() $false)
    }
    function ProfileStatus() {
        $n = $utf8.GetBytes($M.Name)
        $p = New-Object System.Collections.Generic.List[byte]
        $p.Add($M.Flags); $p.AddRange([byte[]](0x78, 0x56, 0x34, 0x12))
        $got16 = $(if ($M.UpLen -ge 0) { $M.Up.Count } else { 0 }); $len16 = $(if ($M.UpLen -ge 0) { $M.UpLen } else { 0 })
        $p.Add($got16 -band 0xFF); $p.Add($got16 -shr 8); $p.Add($len16 -band 0xFF); $p.Add($len16 -shr 8)
        $p.Add($n.Length); $p.AddRange($n)
        return , (Kp1 1 0 0x5B $p.ToArray() $false)
    }
    function Apply-Profile($p) {
        foreach ($k in 'Role', 'Addr', 'X1', 'Radio', 'ApCh', 'ApHidden', 'ApMax', 'ApAuth', 'ApSsid', 'Ip', 'L0', 'L1', 'X2', 'Tx', 'Rx', 'Xlat', 'Nets') { $M[$k] = $p[$k] }
        if ($M.Radio[0] -gt 80) { $M.Radio[0] = 80 }                       # 21 dBm is saved as 20
        $M.Flags = 1; $M.Name = $p.Name
    }
    try {
        while (-not $proc.HasExited) {
            $now = Get-Date
            if ($now -gt $deadline) { $proc.Kill(); throw "the app did not end in $TimeoutS s ($name)" }
            if ($M.Pending -and $now -ge $M.Pending.At) {                 # the reboot 200 ms after the answer
                Apply-Profile $M.Pending.Profile
                $M.Pending = $null; $M.Mark = $false; $M.UpLen = -1; $M.Up.Clear()
                $M.Silent = $now.AddMilliseconds($RebootMs)
            }
            if ($now -ge $M.Silent -and ($now - $beat).TotalMilliseconds -ge 300) {
                $beat = $now
                Send (Kp1 1 0 0x20 ([byte[]](0, 87, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4)) $false)   # VERSION v0: board 87
                Send (Kp1 1 2 0x20 ([byte[]](0, 0, 87, 0, 1, 0, 0, 22, 0)) $false)                              # v2: firmware 0.22
            }
            if ($udp.Available -le 0) { Start-Sleep -Milliseconds 10; continue }
            $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
            $d = $udp.Receive([ref]$remote)
            if ($now -lt $M.Silent) { continue }                           # rebooting: nothing arrives
            $i = 0
            while ($i + 8 -le $d.Length) {
                if ($d[$i] -ne 0xBB -or $d[$i + 1] -ne 0x55) { $i++; continue }
                $len = $d[$i + 5]; $total = 6 + $len + 2
                if ($i + $total -gt $d.Length) { break }
                $route = $d[$i + 2]; $mode = $d[$i + 3]; $type = $mode -band 3; $ver = ($mode -shr 3) -band 7; $id = $d[$i + 4]
                $payload = [byte[]]@()
                if ($len -gt 0) { $payload = [byte[]]$d[($i + 6)..($i + 5 + $len)] }
                $ck = [byte[]]($d[$i + $total - 2], $d[$i + $total - 1])
                $i += $total
                if ($route -ne 0 -and $route -ne 255 -and $route -ne $M.Addr) { continue }   # for another address
                if ($id -eq 0x21 -and $type -eq 2) { Ack 0x21 $ver 1 $ck; $M.Mark = $true; continue }   # ID_MARK
                if ($id -eq 0x20 -and $type -eq 3) {
                    if ($ver -eq 2) { Send (Kp1 1 2 0x20 ([byte[]](0, 0, 87, 0, 1, 0, 0, 22, 0)) $false) }
                    else { Send (Kp1 1 0 0x20 ([byte[]](0, 87, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4)) $false) }
                    continue
                }
                if ($id -eq 0x57 -and $type -eq 3) {
                    if ($ver -eq 0) { Send (StatusFrame) }
                    elseif ($ver -eq 4) {
                        if ($M.Nets.Count -eq 0) { Send (Kp1 1 4 0x57 ([byte[]](0, 0)) $false) }
                        for ($k = 0; $k -lt $M.Nets.Count; $k++) {
                            $s = $utf8.GetBytes($M.Nets[$k])
                            Send (Kp1 1 4 0x57 ([byte[]](@($k, $M.Nets.Count, $s.Length) + $s)) $false)
                        }
                    }
                    elseif ($ver -eq 7) { Send (Kp1 1 7 0x57 ([byte[]]($M.Radio[0], $M.Radio[1], $M.Radio[2], $M.Radio[3], $M.Radio[0], 4, $M.Radio[1], $M.Radio[2])) $false) }
                } elseif ($id -eq 0x58 -and $type -eq 3) {
                    if ($ver -eq 0) { Send (Kp1 1 0 0x58 ([byte[]]($M.Role, $M.Role)) $false) }
                    elseif ($ver -eq 1) { $s = $utf8.GetBytes($M.ApSsid); Send (Kp1 1 1 0x58 ([byte[]](@($M.ApCh, $M.ApHidden, $M.ApMax, $M.ApAuth, $s.Length) + $s + @(0))) $false) }
                    elseif ($ver -eq 2) { Send (Kp1 1 2 0x58 $M.Ip $false) }
                    elseif ($ver -eq 3) { if ($payload.Length -ge 1) { Send (LineFrame $payload[0]) } else { Send (LineFrame 0); Send (LineFrame 1) } }
                    elseif ($ver -eq 6) { Send (Kp1 1 6 0x58 ([byte[]]($M.Addr)) $false) }
                    elseif ($ver -eq 7 -and $payload.Length -eq 0) { Send (PortFrame 0); Send (PortFrame 1) }
                } elseif ($id -eq 0x5B) {
                    $got.Add([pscustomobject]@{ Type = $type; Ver = $ver; Payload = $payload; At = $now; Route = $route })
                    if ($type -eq 3 -and $ver -eq 0) { Send (ProfileStatus) }
                    elseif ($type -eq 3 -and $ver -eq 3) { Send (Kp1 1 3 0x5B ([byte[]]($M.Reason)) $false) }
                    elseif ($type -eq 2 -and $ver -eq 1) {
                        if ($payload.Length -ne 10 -or (Hex $payload[0..3]) -ne (Hex $keyBytes)) { Ack 0x5B 1 $(if ($payload.Length -ne 10) { 3 } else { 7 }) $ck; continue }
                        $n = U16 $payload 4
                        if ($n -eq 0 -or $n -gt 1024) { Ack 0x5B 1 3 $ck; continue }
                        $M.UpLen = $n; $M.UpCrc = U32 $payload 6; $M.Up.Clear()
                        Ack 0x5B 1 1 $ck
                    } elseif ($type -eq 2 -and $ver -eq 2) {
                        if ($M.UpLen -lt 0) { Ack 0x5B 2 8 $ck; continue }
                        $off = U16 $payload 0; $part = [byte[]]$payload[2..($payload.Length - 1)]
                        if ($off -eq $M.Up.Count - $part.Length -and $off -ge 0) { Ack 0x5B 2 1 $ck; continue }   # a repeat
                        if ($off -ne $M.Up.Count -or $off + $part.Length -gt $M.UpLen) { Ack 0x5B 2 3 $ck; continue }
                        $M.Up.AddRange($part); Ack 0x5B 2 1 $ck
                    } elseif ($type -eq 2 -and $ver -eq 3) {
                        $blob = $M.Up.ToArray()
                        $reason = 0
                        if ($M.UpLen -lt 0 -or $blob.Length -ne $M.UpLen) { $reason = 1 }
                        elseif ([StandCrc]::Crc32($blob) -ne $M.UpCrc) { $reason = 2 }
                        $p = $(if ($reason -eq 0) { Read-Blob $blob } else { $null })
                        if ($reason -eq 0 -and -not $p) { $reason = 3 }
                        if ($reason -eq 0 -and $p.Name -eq 'refuse-lines') { $reason = 13 }
                        $blobs.Add([pscustomobject]@{ Blob = $blob; Profile = $p; Reason = $reason })
                        $M.Reason = $reason
                        if ($reason -ne 0) { Ack 0x5B 3 3 $ck; Send (Kp1 1 3 0x5B ([byte[]]($reason)) $false); continue }
                        Ack 0x5B 3 1 $ck
                        Send (Kp1 1 3 0x5B ([byte[]](0)) $false)
                        $M.Flags = 1; $M.Name = $p.Name; $M.UpLen = -1; $M.Up.Clear()
                        Send (ProfileStatus)
                        $M.Pending = @{ At = $now.AddMilliseconds(200); Profile = $p }
                    }
                } elseif ($id -eq 0x58 -and $type -eq 2 -and $ver -eq 1 -and $payload.Length -ge 9) {
                    # the access point set on its own: a setting a profile holds, so "changed since" (bit1)
                    $M.ApCh = $payload[4]; $M.ApHidden = $payload[5]; $M.ApMax = $payload[6]; $M.ApAuth = $payload[7]
                    $M.ApSsid = $utf8.GetString($payload, 9, $payload[8])
                    if ($M.Flags -band 1) { $M.Flags = $M.Flags -bor 2 }
                    Ack 0x58 1 1 $ck
                    $M.ApSetAt = $now
                } elseif ($type -eq 2) {
                    Ack $id $ver 1 $ck                                     # any other setting: taken
                }
            }
        }
    } finally {
        $udp.Close()
        if (-not $proc.HasExited) { $proc.Kill() }
    }
    $lines = Get-Content -LiteralPath $log -Encoding UTF8
    $start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
    $mine = @($lines[($start - 1)..($lines.Count - 1)])
    $mine | Set-Content -LiteralPath (Join-Path $dir 'kogger.log') -Encoding UTF8
    $got | ForEach-Object { 'route {0} type {1} v{2} [{3} bytes]' -f $_.Route, $_.Type, $_.Ver, $_.Payload.Length } |
        Set-Content -LiteralPath (Join-Path $dir 'stand.txt') -Encoding ASCII
    return @{ Log = $mine; Got = $got; Blobs = $blobs; Dir = $dir; Module = $M }
}

function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}
function Lines($r, [string] $kind) { return @($r.Log | Where-Object { $_ -match "echoedit script: wifiProfile $kind" } | ForEach-Object { $_ -replace '^.*echoedit script: wifiProfile ', '' }) }
function Texts([string] $json) {
    $out = @{}
    if (-not (Test-Path $json)) { return $out }
    $doc = [System.IO.File]::ReadAllText($json, $utf8) | ConvertFrom-Json
    $stack = New-Object System.Collections.Stack
    $stack.Push(@($doc.root, '', $true))
    while ($stack.Count) {
        $e = $stack.Pop(); $n = $e[0]; $path = $e[1]; $room = $e[2]
        $shown = $room -and $n.visible
        if ($n.text) { $out[$path] = @{ Text = [string]$n.text; Shown = $shown } }
        if ($n.children) {
            $inner = $room -and ($path -eq '' -or $n.h -gt 0.5)
            for ($i = 0; $i -lt $n.children.Count; $i++) { $stack.Push(@($n.children[$i], "$path/$i", $inner)) }
        }
    }
    return $out
}
function HasText($texts, [string] $pattern) { foreach ($v in $texts.Values) { if ($v.Shown -and $v.Text -match $pattern) { return $true } }; return $false }

# The refused profile: the boat's, renamed (the stand refuses it by its name).
$refuse = Join-Path $Out 'refuse-lines.json'
$boatJson = '{"name": "refuse-lines", "role": "ap", "address": 88, "x1_baud": 921600, "radio": {"power_dbm": 20, "mode": "bgn", "bw": 20, "ps": "none"}, "ap": {"ssid": "KoggerBoat", "password": "boatpass1", "channel": 10, "auth": "wpa2", "hidden": false, "max_clients": 4}, "ip": {"ip": "10.0.0.10", "mask": "255.255.255.0", "dhcp": true, "pool_start": "10.0.0.11", "pool_end": "10.0.0.30", "lease_min": 120, "offer_gw": false, "offer_dns": false}, "line0": {"mode": "udp", "dest": "senders", "ip": "0.0.0.0", "rport": 14444, "lport": 14444}, "line1": {"mode": "udp", "dest": "senders", "ip": "0.0.0.0", "rport": 14444, "lport": 14444, "baud": 921600, "tx_pin": 5, "rx_pin": 4, "xlat": "none"}, "networks": []}'
[System.IO.File]::WriteAllText($refuse, $boatJson, $utf8)
$headPw = '{"ap": "bridgepw1", "net0": "boatpass1", "net1": "bridgepw1"}'
$steps = @(
    '{"op": "wait", "ms": 5000}', '{"op": "wifiProfile"}', '{"op": "wifiProfile", "list": true}',
    '{"op": "wifiPage"}', '{"op": "wait", "ms": 2000}', '{"op": "probe", "tag": "start", "root": "DeviceSettingsPage"}',
    '{"op": "wifiProfile", "preview": "builtin:boat_kogger"}',
    '{"op": "wifiProfile", "preview": "builtin:boat_kogger", "passwords": {"ap": "boatpass1"}}',
    ('{"op": "wifiProfile", "load": "builtin:headunit", "passwords": ' + $headPw + '}'),
    '{"op": "wait", "ms": 7000}', '{"op": "wifiProfile"}', '{"op": "probe", "tag": "loaded", "root": "DeviceSettingsPage"}',
    ('{"op": "wifiProfile", "preview": "builtin:headunit", "passwords": ' + $headPw + '}'),
    ('{"op": "wifiProfile", "import": "' + ($refuse -replace '\\', '/') + '"}'),
    '{"op": "wifiProfile", "load": "file:refuse-lines.json"}',
    '{"op": "wait", "ms": 2500}', '{"op": "wifiProfile"}', '{"op": "probe", "tag": "refused", "root": "DeviceSettingsPage"}',
    '{"op": "wifiProfile", "load": "builtin:boat_godio", "passwords": {"ap": "bridgepw1"}}',
    '{"op": "wait", "ms": 9000}', '{"op": "wifiProfile", "addr": 87}', '{"op": "wifiProfile", "addr": 88}',
    '{"op": "wifiProfile", "addr": 88, "capture": true}', '{"op": "wait", "ms": 3500}',
    '{"op": "wifiProfile", "addr": 88, "save": "Test capture", "passwords": {"ap": "bridgepw1"}}',
    '{"op": "wifiProfile", "list": true}',
    '{"op": "wifiProfile", "addr": 88, "setAp": {"channel": 11, "hidden": false, "max": 4, "auth": 1, "ssid": "KoggerBridge"}}',
    '{"op": "wait", "ms": 1000}', '{"op": "wifiProfile", "addr": 88}', '{"op": "quit"}')

$r = Run-App 'profiles' $steps
$st = @(Lines $r 'state')
$pv = @(Lines $r 'preview')
$ld = @(Lines $r 'load')
Expect ($st.Count -ge 1 -and $st[0] -match '^state: support 1 known 1 name "" loaded 0 ') 'found: firmware 0.22, no profile yet' ($st | Select-Object -First 1)
$list = @(Lines $r 'list')
Expect ($list.Count -ge 1 -and $list[0] -match '^list: builtin:headunit builtin:boat_kogger builtin:boat_godio$') 'list: the three built in' ($list | Select-Object -First 1)
Expect ($pv.Count -ge 1 -and $pv[0] -match 'ready 0 own 0 newBaud 5000000 .*changes \[role ap networks address x1 line0 line1 radio\] problems \["the access point''s password: 8 to 63 characters"\]') 'preview: no password, not ready; what changes' $pv[0]
Expect ($pv.Count -ge 2 -and $pv[1] -match 'ready 1 .*problems \[\] blocked ""') 'preview: with the password, ready' $pv[1]
Expect ($ld.Count -ge 1 -and $ld[0] -match '^load builtin:headunit: started') 'load: the head unit started' ($ld | Select-Object -First 1)

$b0 = $r.Blobs | Select-Object -First 1
$headJson = Join-Path $Out 'headunit_test.json'
$hj = '{"name": "' + $boshka + '", "role": "sta", "address": 87, "x1_baud": 2000000, "radio": {"power_dbm": 20, "mode": "bgn", "bw": 20, "ps": "none"}, "ap": {"ssid": "KoggerBridge", "password": "bridgepw1", "channel": 6, "auth": "wpa2", "hidden": false, "max_clients": 4}, "ip": {"ip": "10.0.0.10", "mask": "255.255.255.0", "dhcp": true, "pool_start": "10.0.0.11", "pool_end": "10.0.0.30", "lease_min": 120, "offer_gw": false, "offer_dns": false}, "line0": {"mode": "udp", "dest": "fixed", "ip": "10.0.0.10", "rport": 14444, "lport": 14444}, "line1": {"mode": "off", "dest": "senders", "ip": "0.0.0.0", "rport": 14445, "lport": 14445, "baud": 921600, "tx_pin": 5, "rx_pin": 4, "xlat": "none"}, "networks": [{"ssid": "KoggerBoat", "password": "boatpass1"}, {"ssid": "KoggerBridge", "password": "bridgepw1"}]}'
[System.IO.File]::WriteAllText($headJson, $hj, $utf8)
$env:PATH = 'D:\Qt\6.8.3\llvm-mingw_64\bin;' + $env:PATH     # the check's Qt
$want = (& $Check --blob $headJson | Out-String).Trim()
Expect ($b0 -and $b0.Reason -eq 0 -and (Hex $b0.Blob) -eq $want) 'load: the block the module got = the head unit profile with those passwords' ("{0} bytes, reason {1}" -f $(if ($b0) { $b0.Blob.Length } else { 0 }), $(if ($b0) { $b0.Reason } else { '-' }))
$sets = @($r.Got | Where-Object { $_.Type -eq 2 } | Select-Object -First 3 | ForEach-Object { 'v{0}' -f $_.Ver })
Expect (($sets -join ' ') -eq 'v1 v2 v3') 'load: begin, one piece, apply' ($sets -join ' ')
$back = @($r.Log | Where-Object { $_ -match ('wifi-profile: back with "' + $boshka + '" after \d+ ms$') })
Expect ($back.Count -eq 1 -and $st.Count -ge 2 -and $st[1] -match ('^state: support 1 known 1 name "' + $boshka + '" loaded 1 modified 0 step 0 .*bad 0 addr 87')) 'load: back after the reboot with the name' ($(if ($st.Count -ge 2) { $st[1] } else { 'no state' }))
Expect ($pv.Count -ge 3 -and $pv[2] -match 'changes \[\] problems \[\]') 'preview again: nothing changes (the settings read back as the profile left them)' $(if ($pv.Count -ge 3) { $pv[2] } else { '' })
$imp = @(Lines $r 'import')
Expect ($imp.Count -eq 1 -and $imp[0] -match 'added$' -and $ld.Count -ge 2 -and $ld[1] -match 'started') 'refusal: the file added and loaded' ("{0} | {1}" -f ($imp -join ''), $(if ($ld.Count -ge 2) { $ld[1] } else { '' }))
Expect ($st.Count -ge 3 -and $st[2] -match 'bad 1 .*result "The module refused "refuse-lines": both UDP lines on one port\. Nothing changed in it\."') 'refusal: its reason in words' $(if ($st.Count -ge 3) { $st[2] } else { '' })
$moved = @($r.Log | Where-Object { $_ -match ('wifi-profile: "' + $lodka + ' GODIO" taken, the module reboots into it \(address 88, was 87\)') })
Expect ($moved.Count -eq 1 -and $st.Count -ge 4 -and $st[3] -match 'result "The module took ".* GODIO" and reboots into it\. It comes back at address 88') 'address 88: the old device says where to look' $(if ($st.Count -ge 4) { $st[3] } else { '' })
Expect ($st.Count -ge 5 -and $st[4] -match ('name "' + $lodka + ' GODIO" loaded 1 modified 0 .*addr 88')) 'address 88: found again, carrying the profile' $(if ($st.Count -ge 5) { $st[4] } else { '' })
$saved = @(Lines $r 'save')
$file = Join-Path $profDir 'Test capture.json'
$cap = $null
if (Test-Path -LiteralPath $file) { $cap = [System.IO.File]::ReadAllText($file, $utf8) | ConvertFrom-Json }
Expect ($saved.Count -eq 1 -and $saved[0] -eq 'save: saved' -and $cap -and $cap.name -eq 'Test capture' -and $cap.role -eq 'ap' -and $cap.address -eq 88 -and $cap.x1_baud -eq 921600 -and $cap.ap.ssid -eq 'KoggerBridge' -and $cap.ap.password -eq 'bridgepw1' -and $cap.line1.xlat -eq 'telemetry' -and $cap.line1.baud -eq 9600 -and $cap.line1.tx_pin -eq 10 -and @($cap.networks).Count -eq 0) 'capture: the module at 88 kept as a file, with the password typed' ($saved -join '')
Expect ($list.Count -ge 2 -and $list[1] -match 'file:Test capture\.json' -and $list[1] -match 'file:refuse-lines\.json') 'capture: listed' $(if ($list.Count -ge 2) { $list[1] } else { '' })
# A setting a profile holds, sent on its own: the status asked right after the module's answer, not at the page's 5 s
# (the page open is the module at 87's, so nothing else asks the one at 88).
$asks = @()
if ($r.Module.ApSetAt) {
    $asks = @($r.Got | Where-Object { $_.Type -eq 3 -and $_.Ver -eq 0 -and $_.Route -eq 88 -and $_.At -gt $r.Module.ApSetAt } |
              ForEach-Object { [int]($_.At - $r.Module.ApSetAt).TotalMilliseconds })
}
Expect ($asks.Count -ge 1 -and $asks[0] -ge 200 -and $asks[0] -le 900 -and $st.Count -ge 6 -and $st[5] -match 'loaded 1 modified 1 .*addr 88') 'a setting of the profile: the status asked 300 ms after it, "changed" shown' ("asked {0} ms after; {1}" -f $(if ($asks.Count) { $asks[0] } else { 'never' }), $(if ($st.Count -ge 6) { $st[5] } else { 'no state' }))

$t = Texts (Join-Path $r.Dir 'ui-start.json')
Expect ((HasText $t '^Profile$') -and (HasText $t '^In the module$') -and (HasText $t '^It was not loaded with a profile\.$') -and (HasText $t ('^' + $boshka + '$')) -and (HasText $t '^Save the module as a profile$')) 'page: the section, its rows, the built-in profiles' 'Profile, In the module, the list, Save the module as a profile'
$t = Texts (Join-Path $r.Dir 'ui-loaded.json')
Expect ((HasText $t ('^"' + $boshka + '", its settings as the profile left them\.$')) -and (HasText $t ('^Profile "' + $boshka + '" loaded: the module is back in [0-9.]+ s\.$'))) 'page: loaded' 'what the module carries, the result'
$t = Texts (Join-Path $r.Dir 'ui-refused.json')
Expect (HasText $t '^The module refused "refuse-lines": both UDP lines on one port\. Nothing changed in it\.$') 'page: refused' 'the reason shown'
$pw = @($r.Log | Where-Object { $_ -match 'DeviceSettingsPage\.qml:\d+' })
Expect ($pw.Count -eq 0) 'page: no QML warning' ($(if ($pw.Count) { $pw[0] } else { 'none' }))

if ($Shots) {
    New-Item -ItemType Directory -Force -Path $Shots | Out-Null
    $g = { param($n) '{"op": "grab", "path": "' + (Join-Path $Shots "$n.png").Replace('\', '/') + '"}' }
    $top = '{"op": "scrollTo", "name": "wifiProfileSection"}'
    $shotSteps = @(
        '{"op": "wait", "ms": 5000}', '{"op": "wifiPage"}', '{"op": "wait", "ms": 2500}', $top, '{"op": "wait", "ms": 500}', (& $g 'profile_1_found'),
        '{"op": "clickItem", "name": "wifiProfileRow", "index": 1}', '{"op": "wait", "ms": 800}', $top, '{"op": "wait", "ms": 500}', (& $g 'profile_2_chosen'),
        '{"op": "clickItem", "name": "wifiProfileRow", "index": 1}',
        ('{"op": "wifiProfile", "load": "builtin:headunit", "passwords": ' + $headPw + '}'),
        '{"op": "wait", "ms": 7000}', $top, '{"op": "wait", "ms": 500}', (& $g 'profile_3_loaded'), '{"op": "quit"}')
    [void](Run-App 'shots_ru' $shotSteps 'ru')
    "pictures: {0}" -f $Shots
}

if (Test-Path -LiteralPath $profDir) { Remove-Item -Recurse -Force $profDir }   # the test's files hold made-up passwords only; gone all the same
"profile live: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
"runs: {0}" -f $Out
exit ([int]($failed -gt 0))

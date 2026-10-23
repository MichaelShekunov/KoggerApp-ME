#requires -Version 5.1
<#
.SYNOPSIS
    The Wi-Fi module's radio switched off (ID_WIFI_RF, firmware 0.20) end to end in the running app, against a stand
    that plays the module over UDP: no module needed.

.DESCRIPTION
    Runs of KoggerApp on the test desktop (a test profile of its own, KOGGER_SETTINGS_INI, written fresh; the
    owner's settings are not touched) with KOGGER_AUTOTEST=UDP:<port> and a live edit script. This script is the
    module: ID_VERSION as board 87 with firmware 0.20 and the mark set, its status (ID_WIFI v0, flag bit3 while the
    radio is off), its radio settings (v7), its role and a port summary (ID_WIFI_NET v0, v7: the app's requests came
    through X1 -- "over a wire" -- or through no port -- "over the network", -Network), and ID_WIFI_RF as the
    contract has it (the module project's docs/SBP_WIFI.md, ID_WIFI_RF): the confirmation {code, checksum} and then
    CONTENT v0 {on, seconds left}; over the network no "off" without a timer or past 600 s (ERR_PAYLOAD); "off" for
    7 s is refused as busy (ERR_RUNTIME), to see a refusal; a timer that runs out switches the radio on and tells
    the app (CONTENT {1, 0} and the status). Over the network the module falls silent 200 ms after its answer and
    speaks again RelinkMs after its timer, its CONTENT {1, 0} lost on the way: only its status says it is back.
    The edit script opens the module's page, switches the radio off and on through DevDriver as the page does
    (wifiRadio steps) and pictures the page (UI probe). Checked:
      wired    until on (0 s), on, 3 s and back by the timer, 7 s refused as busy: the frames the stand got byte by
               byte, the state the app reports after each, its log lines
      network  0 s and 601 s not sent at all, 2 s sent; back on by the status alone
      page     the page's texts in each state (en); in each language of -Codes no text of it cut where English is
               not; no QML warning from the page
    Exit code 0 when every check holds, 1 when one fails.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\wifi020_check\rf_live.ps1 -Exe <build>\KoggerApp.exe [-Codes ru,zh,es,hi]
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [string[]] $Codes = @(),
    [string]   $Profile = 'rfcheck',
    [int]      $Port = 47631,
    [int]      $RelinkMs = 1500,
    [string]   $Out = "$env:TEMP\kogger_rf_live",
    [int]      $TimeoutS = 120
)

$ErrorActionPreference = 'Stop'
$Codes = @($Codes | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$utf8 = New-Object System.Text.UTF8Encoding $false
$ini = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile.ini"
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
$keyBytes = [byte[]](0x4A, 0x5D, 0x6B, 0xC9)
$dot = [string][char]0xB7
$failed = 0

function Kp1([int] $type, [int] $ver, [int] $id, [byte[]] $payload, [bool] $resp) {
    $mode = ($type -band 3) -bor (($ver -band 7) -shl 3) -bor 0x40   # the mark always set: "this app's device"
    if ($resp) { $mode = $mode -bor 0x80 }
    $body = New-Object System.Collections.Generic.List[byte]
    $body.Add(0); $body.Add([byte]$mode); $body.Add([byte]$id); $body.Add([byte]$payload.Length)
    $body.AddRange($payload)
    $c1 = 0; $c2 = 0
    foreach ($b in $body) { $c1 = ($c1 + $b) -band 0xFF; $c2 = ($c2 + $c1) -band 0xFF }
    $f = New-Object System.Collections.Generic.List[byte]
    $f.Add(0xBB); $f.Add(0x55); $f.AddRange($body); $f.Add([byte]$c1); $f.Add([byte]$c2)
    return , $f.ToArray()
}
function Hex([byte[]] $b) { if (-not $b) { return '' }; return (($b | ForEach-Object { $_.ToString('X2') }) -join ' ') }

# ID_WIFI v0, 39 bytes: connected to "BOAT" (or idle while the radio is off), flag bit3 = the radio off.
function StatusFrame([bool] $off) {
    $p = New-Object System.Collections.Generic.List[byte]
    if ($off) { $p.Add(0); $p.Add(0x09) } else { $p.Add(3); $p.Add(0x01) }   # state; flags: auto-connect (+ bit3)
    $p.Add([byte](256 - 50)); $p.Add(6); $p.Add(3); $p.Add(0); $p.Add(0); $p.Add(0)
    $p.AddRange([byte[]](0x24, 0x6F, 0x28, 0xAA, 0xBB, 0xCC))
    $p.AddRange([byte[]](192, 168, 4, 2, 255, 255, 255, 0, 192, 168, 4, 1, 0, 0, 0, 0))
    $p.AddRange([byte[]](0, 0, 0, 0, 0, 0, 0, 0))
    $p.Add(4); $p.AddRange([byte[]](0x42, 0x4F, 0x41, 0x54))
    return , (Kp1 1 0 0x57 $p.ToArray() $false)
}
# ID_WIFI_NET v7 page 0 of a port, 116 bytes; bit2 = the request came through this port.
function PortFrame([int] $port, [bool] $asked) {
    $p = New-Object byte[] 116
    $p[0] = $port; $p[1] = 0
    $p[2] = 0x01; if ($asked) { $p[2] = 0x05 }
    $b = [BitConverter]::GetBytes([uint32]921600); [Array]::Copy($b, 0, $p, 3, 4); [Array]::Copy($b, 0, $p, 7, 4)
    $p[11] = 2
    for ($i = 20; $i -lt 24; $i++) { $p[$i] = 0xFF }
    return , (Kp1 1 7 0x58 $p $false)
}
# ID_WIFI_SURVEY CONTENT v1 (firmware 0.20): one other end, 40 frames at 11n MCS7, a bound, 3 of them came again.
function RateFrame() {
    $p = New-Object System.Collections.Generic.List[byte]
    $p.AddRange([byte[]](0, 1, 0x24, 0x6F, 0x28, 0xAA, 0xBB, 0xCC, 200, 0, 40, 0, [byte](256 - 55), 4, 1, 0))
    $p.AddRange([byte[]](3, 7, 1, 0x08, 0x1A, 0x01, 0x00, 40, 0))   # 11n MCS7 SGI, 72200 kbit/s, 40 frames
    $p.AddRange([byte[]](0, 0, 0, 0))                                # 0.19: the bound, none measured
    $p.AddRange([byte[]](3, 0))                                      # 0.20: came again
    return , (Kp1 1 1 0x59 $p.ToArray() $false)
}
function RfFrame([bool] $on, [int] $left) {
    return , (Kp1 1 0 0x5A ([byte[]]($(if ($on) { 1 } else { 0 }), ($left -band 0xFF), (($left -shr 8) -band 0xFF))) $false)
}

function Run-App([string] $name, [string] $code, [bool] $network, [string[]] $steps) {
    $dir = Join-Path $Out $name
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    # the toolset "Maximum" (every settings group), the Antenna swap and Wi-Fi sections open (the page's fold memory)
    $seed = @('[General]', 'schemaVersion=8', '', '[main]', 'ui\welcomeShown=true', 'ui\instrumentsGradeList=2',
              "appLanguageCode=$code", '', '[deviceSettingsPage]', 'openSections="wifi.radioOff,wifi.net"', '')
    if (Test-Path -LiteralPath $ini) { Remove-Item -LiteralPath $ini -Force }
    New-Item -ItemType Directory -Force -Path (Split-Path $ini) | Out-Null
    [System.IO.File]::WriteAllText($ini, ($seed -join "`r`n"), $utf8)
    $sp = Join-Path $dir 'steps.json'
    [System.IO.File]::WriteAllText($sp, ('{"live": true, "steps": [' + ($steps -join ",`n") + ']}'), $utf8)

    $udp = New-Object System.Net.Sockets.UdpClient(New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, ($Port + 1)))
    # an ICMP "port unreachable" of an earlier send (the app not listening yet) would reset the next Receive
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
    $psi.EnvironmentVariables['KOGGER_UI_PROBE_DELAY_MS'] = '600000'   # no dump of its own: only the steps'
    $proc = Start-TestDesktopProcess $psi

    # the module's radio
    $st = @{ Off = $false; Until = [DateTime]::MaxValue; SilentFrom = [DateTime]::MaxValue; SilentTo = [DateTime]::MinValue; Back = $false }
    $got = New-Object System.Collections.Generic.List[object]
    $deadline = (Get-Date).AddSeconds($TimeoutS)
    $beat = [DateTime]::MinValue
    function Send([byte[]] $f) {
        $now = Get-Date
        if ($now -ge $st.SilentFrom -and $now -lt $st.SilentTo) { return }   # its radio is off: nothing reaches the app
        [void]$udp.Send($f, $f.Length, $app)
    }
    function Left() { if (-not $st.Off -or $st.Until -eq [DateTime]::MaxValue) { return 0 }; return [int][Math]::Max(0, [Math]::Ceiling(($st.Until - (Get-Date)).TotalSeconds)) }
    try {
        while (-not $proc.HasExited) {
            $now = Get-Date
            if ($now -gt $deadline) { $proc.Kill(); throw "the app did not end in $TimeoutS s ($name)" }
            # the timer runs out: the radio is back
            if ($st.Off -and $now -ge $st.Until) {
                $st.Off = $false; $st.Until = [DateTime]::MaxValue
                if ($network) { $st.Back = $true }   # its CONTENT {1, 0} is lost: the link is still down
                else { Send (RfFrame $true 0); Send (StatusFrame $false) }
            }
            # over the network the link returns RelinkMs after the radio: the status is all the app gets
            if ($st.Back -and $now -ge $st.SilentTo) { $st.Back = $false; Send (StatusFrame $false) }
            if (($now - $beat).TotalMilliseconds -ge 300) {
                $beat = $now
                Send (Kp1 1 0 0x20 ([byte[]](0, 87, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4)) $false)   # VERSION v0: board 87
                Send (Kp1 1 2 0x20 ([byte[]](0, 0, 87, 0, 1, 0, 0, 20, 0)) $false)                              # v2: firmware 0.20
            }
            if ($udp.Available -le 0) { Start-Sleep -Milliseconds 15; continue }
            $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
            $d = $udp.Receive([ref]$remote)
            if ($now -ge $st.SilentFrom -and $now -lt $st.SilentTo) { continue }   # its radio is off: nothing arrives
            $i = 0
            while ($i + 8 -le $d.Length) {
                if ($d[$i] -ne 0xBB -or $d[$i + 1] -ne 0x55) { $i++; continue }
                $len = $d[$i + 5]; $total = 6 + $len + 2
                if ($i + $total -gt $d.Length) { break }
                $mode = $d[$i + 3]; $type = $mode -band 3; $ver = ($mode -shr 3) -band 7; $id = $d[$i + 4]
                $payload = [byte[]]@()
                if ($len -gt 0) { $payload = [byte[]]$d[($i + 6)..($i + 5 + $len)] }
                $ck = [byte[]]($d[$i + $total - 2], $d[$i + $total - 1])
                if ($id -eq 0x57 -and $type -eq 3) {
                    if ($ver -eq 0) { Send (StatusFrame $st.Off) }
                    elseif ($ver -eq 4) { Send (Kp1 1 4 0x57 ([byte[]](0, 0)) $false) }
                    elseif ($ver -eq 7) { Send (Kp1 1 7 0x57 ([byte[]](80, 7, 1, 0, $(if ($st.Off) { 0 } else { 80 }), 4, 7, 1)) $false) }
                } elseif ($id -eq 0x57 -and $type -eq 2) {
                    Send (Kp1 1 $ver 0x57 ([byte[]](1, $ck[0], $ck[1])) $true)
                } elseif ($id -eq 0x58 -and $type -eq 3) {
                    if ($ver -eq 0) { Send (Kp1 1 0 0x58 ([byte[]](0, 0)) $false) }
                    elseif ($ver -eq 7) { Send (PortFrame 0 (-not $network)); Send (PortFrame 1 $false) }
                } elseif ($id -eq 0x59 -and $type -eq 3 -and $ver -eq 1) {
                    Send (RateFrame)
                } elseif ($id -eq 0x5A) {
                    $got.Add([pscustomobject]@{ Type = $type; Ver = $ver; Payload = $payload; At = $now })
                    if ($type -eq 3 -and $ver -eq 0) {
                        Send (RfFrame (-not $st.Off) (Left))
                    } elseif ($type -eq 2 -and $ver -eq 0) {
                        $code = 1
                        if ($payload.Length -ne 7 -or $payload[4] -gt 1) { $code = 3 }
                        elseif ((Compare-Object $payload[0..3] $keyBytes -SyncWindow 0) -ne $null) { $code = 7 }
                        else {
                            $on = $payload[4] -eq 1
                            $s = [int]$payload[5] + 256 * [int]$payload[6]
                            if (-not $on -and $network -and ($s -eq 0 -or $s -gt 600)) { $code = 3 }
                            elseif (-not $on -and $s -eq 7) { $code = 8 }                        # busy, to see a refusal
                        }
                        Send (Kp1 1 0 0x5A ([byte[]]($code, $ck[0], $ck[1])) $true)
                        if ($code -eq 1) {
                            if ($on) { $st.Off = $false; $st.Until = [DateTime]::MaxValue }
                            else {
                                $st.Off = $true
                                $st.Until = $(if ($s -gt 0) { $now.AddSeconds($s) } else { [DateTime]::MaxValue })
                            }
                        }
                        Send (RfFrame (-not $st.Off) (Left))
                        if ($code -eq 1 -and -not $network) { Send (StatusFrame $st.Off) }
                        if ($code -eq 1 -and $network -and $st.Off) {
                            $st.SilentFrom = $now.AddMilliseconds(200)
                            $st.SilentTo = $st.Until.AddMilliseconds($RelinkMs)
                        }
                    }
                }
                $i += $total
            }
        }
    } finally {
        $udp.Close()
        if (-not $proc.HasExited) { $proc.Kill() }   # the stand failed: no app left behind
    }
    $lines = Get-Content -LiteralPath $log -Encoding UTF8
    $start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
    $mine = @($lines[($start - 1)..($lines.Count - 1)])
    $mine | Set-Content -LiteralPath (Join-Path $dir 'kogger.log') -Encoding UTF8
    $got | ForEach-Object { 'type {0} v{1} [{2}]' -f $_.Type, $_.Ver, (Hex $_.Payload) } |
        Set-Content -LiteralPath (Join-Path $dir 'stand.txt') -Encoding ASCII
    return @{ Log = $mine; Got = $got; Dir = $dir }
}

function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}
function States($r) { return @($r.Log | Where-Object { $_ -match 'echoedit script: wifiRadio state: ' } | ForEach-Object { $_ -replace '^.*wifiRadio state: ', '' }) }
function Settings($r) { return @($r.Got | Where-Object { $_.Type -eq 2 -and $_.Ver -eq 0 } | ForEach-Object { Hex $_.Payload }) }
# every Text of a dump: path -> {text, shown, cut}. Shown: visible and no item above it of no height (a closed
# KIslandRow keeps its children, visible, at height 0).
function Texts([string] $json) {
    $out = @{}
    if (-not (Test-Path $json)) { return $out }
    $doc = [System.IO.File]::ReadAllText($json, $utf8) | ConvertFrom-Json
    $stack = New-Object System.Collections.Stack
    $stack.Push(@($doc.root, '', $true))
    while ($stack.Count) {
        $e = $stack.Pop(); $n = $e[0]; $path = $e[1]; $room = $e[2]
        $shown = $room -and $n.visible
        if ($n.text) { $out[$path] = @{ Text = [string]$n.text; Shown = $shown; Cut = ($shown -and $n.truncated -eq $true) } }
        if ($n.children) {
            $inner = $room -and ($path -eq '' -or $n.h -gt 0.5)
            for ($i = 0; $i -lt $n.children.Count; $i++) { $stack.Push(@($n.children[$i], "$path/$i", $inner)) }
        }
    }
    return $out
}
function HasText($texts, [string] $pattern) { foreach ($v in $texts.Values) { if ($v.Shown -and $v.Text -match $pattern) { return $true } }; return $false }
function PageWarnings($r) { return @($r.Log | Where-Object { $_ -match 'DeviceSettingsPage\.qml:\d+' }) }

$wiredSteps = @(
    '{"op": "wait", "ms": 5000}', '{"op": "wifiRadio"}',
    '{"op": "wifiPage"}', '{"op": "wait", "ms": 2000}', '{"op": "probe", "tag": "on", "root": "DeviceSettingsPage"}',
    '{"op": "wifiRadio", "off": 0}', '{"op": "wait", "ms": 800}', '{"op": "wifiRadio"}',
    '{"op": "probe", "tag": "untilon", "root": "DeviceSettingsPage"}',
    '{"op": "wifiRadio", "on": true}', '{"op": "wait", "ms": 800}', '{"op": "wifiRadio"}',
    '{"op": "wifiRadio", "off": 3}', '{"op": "wait", "ms": 600}', '{"op": "wifiRadio"}',
    '{"op": "probe", "tag": "timed", "root": "DeviceSettingsPage"}',
    '{"op": "wait", "ms": 3500}', '{"op": "wifiRadio"}',
    '{"op": "wifiRadio", "off": 7}', '{"op": "wait", "ms": 800}', '{"op": "wifiRadio"}',
    '{"op": "probe", "tag": "busy", "root": "DeviceSettingsPage"}',
    '{"op": "quit"}')

# ---- over a wire, English
$w = Run-App 'wired_en' 'en' $false $wiredSteps
$s = @(States $w)
$set = @(Settings $w)
Expect ($s.Count -eq 6 -and $s[0] -match '^support 1 off 0 .*wired 1') 'wired: found' ("{0} states; {1}" -f $s.Count, ($s | Select-Object -First 1))
Expect ($set.Count -eq 4 -and $set[0] -eq '4A 5D 6B C9 00 00 00' -and $set[1] -eq '4A 5D 6B C9 01 00 00' -and $set[2] -eq '4A 5D 6B C9 00 03 00' -and $set[3] -eq '4A 5D 6B C9 00 07 00') 'wired: the frames' ($set -join ' | ')
Expect ($s.Count -ge 2 -and $s[1] -match '^support 1 off 1 timed 0 untilOn 1 left 0 wired 1 error ""') 'wired: until on' $s[1]
Expect ($s.Count -ge 3 -and $s[2] -match '^support 1 off 0 timed 0 untilOn 0 left 0') 'wired: on' $s[2]
Expect ($s.Count -ge 4 -and $s[3] -match '^support 1 off 1 timed 1 untilOn 0 left [23] ') 'wired: 3 s' $s[3]
Expect ($s.Count -ge 5 -and $s[4] -match '^support 1 off 0 timed 0') 'wired: back by the timer' $s[4]
Expect ($s.Count -ge 6 -and $s[5] -match '^support 1 off 0 .*error "the radio is busy') 'wired: 7 s refused as busy' $s[5]
$logRf = @($w.Log | Where-Object { $_ -match 'wifi-rf: ' } | ForEach-Object { $_ -replace '^.*wifi-rf: ', '' })
$want = @('switching the radio off until on, over a wire', 'radio off until switched on or a reboot', 'switching the radio on',
          'radio on', 'switching the radio off for 3 s, over a wire', 'radio off, on again in [23] s', 'radio on',
          'switching the radio off for 7 s, over a wire', 'refused: the radio is busy', 'radio on')
$k = 0
foreach ($l in $logRf) { if ($k -lt $want.Count -and $l -match ('^' + $want[$k])) { $k++ } }
Expect ($k -eq $want.Count) 'wired: the log' ("{0} of {1} lines in order; {2}" -f $k, $want.Count, ($logRf -join ' | '))
$t = Texts (Join-Path $w.Dir 'ui-on.json')
Expect ((HasText $t '^Antenna swap$') -and (HasText $t '^Switch the radio off$') -and (HasText $t '^Until on$') -and (HasText $t '^Radio$') -and -not (HasText $t '^The radio is off$')) 'page: on' 'Antenna swap, Switch the radio off, Until on; no off state'
$t = Texts (Join-Path $w.Dir 'ui-untilon.json')
Expect ((HasText $t '^The radio is off$') -and (HasText $t '^Until you switch it on, or until the module restarts\.$') -and (HasText $t '^Switch the radio on$') -and (HasText $t '^Radio off$') -and -not (HasText $t '^Switch the radio off$')) 'page: until on' 'the state, its caption, Switch the radio on, the badge; no Switch the radio off'
$t = Texts (Join-Path $w.Dir 'ui-timed.json')
Expect ((HasText $t ('^Radio off ' + $dot + ' [23] s$')) -and (HasText $t '^On again in [23] s, by the module''s timer\.$')) 'page: timed' 'the badge and the state count the seconds'
$t = Texts (Join-Path $w.Dir 'ui-busy.json')
Expect (HasText $t '^the radio is busy: a channel survey') 'page: busy' 'the refusal shown'
$t = Texts (Join-Path $w.Dir 'ui-on.json')
$rate = @($w.Log | Where-Object { $_ -match 'wifi-rate: .*came again 3, over the last \d+ reports' })
Expect ((HasText $t '^Retransmissions$') -and (HasText $t '^in: 7\.5 % \(\d+ of \d+\)$') -and $rate.Count -ge 1) 'page: retransmissions' 'in: 7.5 % (3 of 40 a report, summed), logged'
$pw = PageWarnings $w
Expect ($pw.Count -eq 0) 'page: no QML warning' ($(if ($pw.Count) { $pw[0] } else { 'none' }))

# ---- over the network
$netSteps = @(
    '{"op": "wait", "ms": 5000}', '{"op": "wifiRadio"}',
    '{"op": "wifiPage"}', '{"op": "wait", "ms": 1500}',
    '{"op": "wifiRadio", "off": 0}', '{"op": "wifiRadio", "off": 601}',
    '{"op": "wifiRadio", "off": 2}', '{"op": "wait", "ms": 600}', '{"op": "wifiRadio"}',
    '{"op": "probe", "tag": "timed", "root": "DeviceSettingsPage"}',
    ('{"op": "wait", "ms": ' + (2500 + $RelinkMs + 2000) + '}'), '{"op": "wifiRadio"}',
    '{"op": "quit"}')
$n = Run-App 'network_en' 'en' $true $netSteps
$s = @(States $n)
$set = @(Settings $n)
$notSent = @($n.Log | Where-Object { $_ -match 'echoedit script: wifiRadio off (0|601): not sent' })
Expect ($s.Count -ge 1 -and $s[0] -match '^support 1 off 0 .*wired 0') 'network: found' ($s | Select-Object -First 1)
Expect ($notSent.Count -eq 2 -and $set.Count -eq 1 -and $set[0] -eq '4A 5D 6B C9 00 02 00') 'network: 0 s and 601 s not sent, 2 s sent' ($set -join ' | ')
Expect ($s.Count -eq 3 -and $s[1] -match '^support 1 off 1 timed 1 untilOn 0 left [12] wired 0') 'network: off for 2 s' $s[1]
$t = Texts (Join-Path $n.Dir 'ui-timed.json')
Expect ((HasText $t '^This module is reached through its radio') -and -not (HasText $t '^Switch the radio on$')) 'network: no Switch on' 'the reason shown instead'
$back = @($n.Log | Where-Object { $_ -match 'wifi-rf: the status says the radio is back on' })
Expect ($back.Count -ge 1 -and $s.Count -eq 3 -and $s[2] -match '^support 1 off 0 ') 'network: back by the status' ($s | Select-Object -Last 1)

# ---- the languages: the page's texts not cut where English is not
if ($Codes.Count) {
    $base = @{}
    foreach ($tag in 'on', 'untilon', 'timed', 'busy') { $base[$tag] = Texts (Join-Path $w.Dir "ui-$tag.json") }
    foreach ($code in $Codes) {
        $r = Run-App "wired_$code" $code $false $wiredSteps
        $cut = @()
        foreach ($tag in 'on', 'untilon', 'timed', 'busy') {
            $tx = Texts (Join-Path $r.Dir "ui-$tag.json")
            if (-not $tx.Count) { $cut += "${tag}: no dump"; continue }
            foreach ($p in $tx.Keys) {
                if ($tx[$p].Cut -and -not ($base[$tag].ContainsKey($p) -and $base[$tag][$p].Cut)) { $cut += "$tag | $($tx[$p].Text)" }
            }
        }
        $pw = PageWarnings $r
        Expect ($cut.Count -eq 0 -and $pw.Count -eq 0) "page: $code" ($(if ($cut.Count) { ($cut | Select-Object -Unique) -join ' || ' } elseif ($pw.Count) { $pw[0] } else { 'nothing cut that English shows whole' }))
    }
}

"rf live: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
"runs: {0}" -f $Out
exit ([int]($failed -gt 0))

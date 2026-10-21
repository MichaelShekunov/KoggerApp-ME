#requires -Version 5.1
<#
.SYNOPSIS
    The sonar presets (ID_PRESET) end to end in the running app, against a stand that plays a nanoSSS with
    firmware 11.21 over UDP: no board needed.

.DESCRIPTION
    One run of KoggerApp (KOGGER_PROFILE, reset from the transfer folder's KoggerApp-perf.reg first, so the
    user's settings are not touched) with KOGGER_AUTOTEST=UDP:<port> and a live edit script. This script is
    the sonar: it answers ID_VERSION as board 17 (nanoSSS) 11.21 with the mark set, sends its eight preset
    slots when asked (slot 0 "Narrow beam" in Russian, 815 kHz, slot 1 "Wide beam", 450 kHz, factory; 2..7
    empty), answers a recall (RESP 1, or 3 for an empty slot), a save (RESP 7 for slots 0..1 without the key,
    else RESP 1 and the slot's new frame) and a delete, the way the contract (the l412_adc project's
    docs/PRESETS_CONCEPT_2026-10-21.md, section 5) says. The edit script waits for the presets, logs them,
    recalls 1 and 5 (empty), saves into 0 and 3, logs them again and quits. Checked:
      ask        the app asked for the presets (GET v0)
      slots      it read them: the log names slot 0 and 1 with their frequencies and names
      recall     SETTING v0 [1] came; the empty 5 was refused (code 3) and the app said so
      save       SETTING v1 [0] with the key and the name padded to 24 bytes; [3] with zeros for the key
      after      the app shows the new names of 0 and 3
    Exit code 0 when every check holds.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\preset_check\preset_live.ps1 -Exe <build>\KoggerApp.exe -PathPrefix "D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [string] $PathPrefix,
    [string] $Profile = 'perf',
    [int]    $Port = 47611,
    [string] $Out = "$env:TEMP\kogger_preset_live",
    [string] $PerfReg = 'D:\YandexDisk\DelphiProjects\KoggerApp_myEdition_transfer\KoggerApp-perf.reg',
    [int]    $TimeoutS = 120
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Out | Out-Null
if (Test-Path -LiteralPath $PerfReg) {
    # reg.exe reports success on stderr; under 'Stop' PowerShell 5.1 would take that for a failure.
    cmd.exe /c "reg import `"$PerfReg`" >nul 2>&1"
}
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
$utf8 = [Text.Encoding]::UTF8
$key = [uint32]3379256650   # 0xC96B5D4A
function S([int[]] $codes) { return -join ($codes | ForEach-Object { [char]$_ }) }
$narrow = S @(0x0423, 0x0437, 0x043A, 0x0438, 0x0439, 0x20, 0x043B, 0x0443, 0x0447)
$wide = S @(0x0428, 0x0438, 0x0440, 0x043E, 0x043A, 0x0438, 0x0439, 0x20, 0x043B, 0x0443, 0x0447)
$newName = S @(0x0422, 0x0435, 0x0441, 0x0442)   # what the script saves into slot 0

# ---- the sonar's state
$slots = @{}
for ($i = 0; $i -lt 8; $i++) { $slots[$i] = @{ Used = $false; Factory = $false; Freq = 0; Pulses = 0; Name = '' } }
$slots[0] = @{ Used = $true; Factory = $true; Freq = 815; Pulses = 25; Name = $narrow }
$slots[1] = @{ Used = $true; Factory = $true; Freq = 450; Pulses = 30; Name = $wide }

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
function Le([System.Collections.Generic.List[byte]] $l, [uint32] $v, [int] $n) { for ($i = 0; $i -lt $n; $i++) { $l.Add([byte](($v -shr (8 * $i)) -band 0xFF)) } }
function SlotFrame([int] $i) {
    $s = $slots[$i]
    $p = New-Object System.Collections.Generic.List[byte]
    $p.Add([byte]$i)
    $flags = 0; if ($s.Used) { $flags = $flags -bor 1 }; if ($s.Factory) { $flags = $flags -bor 2 }
    $p.Add([byte]$flags)
    Le $p $s.Freq 2; $p.Add([byte]$s.Pulses); $p.Add(0); $p.Add(0)   # boost, DSP
    Le $p 4000 2; Le $p 10 2; Le $p 0 2; Le $p 50 4                   # cells, mm, offset, period
    $nb = $utf8.GetBytes($s.Name)
    for ($k = 0; $k -lt 24; $k++) { if ($k -lt $nb.Length) { $p.Add($nb[$k]) } else { $p.Add(0) } }
    return , (Kp1 1 0 0x7C $p.ToArray() $false)
}

# ---- the run
$steps = @(
    '{"op": "waitPresets", "timeoutMs": 30000}', '{"op": "presets"}',
    '{"op": "presetRecall", "slot": 1}', '{"op": "wait", "ms": 800}',
    '{"op": "presetRecall", "slot": 5}', '{"op": "wait", "ms": 800}',
    '{"op": "presetSave", "slot": 0, "name": "\u0422\u0435\u0441\u0442"}', '{"op": "wait", "ms": 800}',
    '{"op": "presetSave", "slot": 3, "name": "user 3"}', '{"op": "wait", "ms": 800}',
    '{"op": "presets"}', '{"op": "quit"}')
$scriptPath = Join-Path $Out 'script.json'
('{"live": true, "steps": [' + ($steps -join ",`n") + ']}') | Set-Content -LiteralPath $scriptPath -Encoding ASCII

$udp = New-Object System.Net.Sockets.UdpClient(New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, ($Port + 1)))
# Windows reports an ICMP "port unreachable" of an earlier send (the app not listening yet) as a reset of the
# next Receive; SIO_UDP_CONNRESET off keeps the stand going.
[void]$udp.Client.IOControl(-1744830452, [byte[]](0, 0, 0, 0), $null)
$app = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, $Port)
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
$psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
$psi.EnvironmentVariables['KOGGER_AUTOTEST'] = "UDP:$Port"
$psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $scriptPath
$proc = [System.Diagnostics.Process]::Start($psi)

$got = New-Object System.Collections.Generic.List[object]   # what the app sent: type, ver, id, payload
$deadline = (Get-Date).AddSeconds($TimeoutS)
$beat = [DateTime]::MinValue
function Send([byte[]] $f) { [void]$udp.Send($f, $f.Length, $app) }
try {
    while (-not $proc.HasExited) {
        if ((Get-Date) -gt $deadline) { $proc.Kill(); throw "the app did not end in $TimeoutS s" }
        if (((Get-Date) - $beat).TotalMilliseconds -ge 300) {
            $beat = Get-Date
            Send (Kp1 1 0 0x20 ([byte[]](0, 17, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4)) $false)   # VERSION v0: board 17
            Send (Kp1 1 2 0x20 ([byte[]](0, 0, 17, 0, 1, 0, 0, 21, 11)) $false)                             # v2: firmware 11.21
        }
        if ($udp.Available -le 0) { Start-Sleep -Milliseconds 20; continue }
        $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
        $d = $udp.Receive([ref]$remote)
        $i = 0
        while ($i + 8 -le $d.Length) {
            if ($d[$i] -ne 0xBB -or $d[$i + 1] -ne 0x55) { $i++; continue }
            $len = $d[$i + 5]; $total = 6 + $len + 2
            if ($i + $total -gt $d.Length) { break }
            $mode = $d[$i + 3]; $type = $mode -band 3; $ver = ($mode -shr 3) -band 7; $id = $d[$i + 4]
            $payload = [byte[]]($d[($i + 6)..($i + 5 + [Math]::Max($len, 1))] | Select-Object -First $len)
            $ck = [byte[]]($d[$i + $total - 2], $d[$i + $total - 1])
            if ($id -eq 0x7C) {
                $got.Add([pscustomobject]@{ Type = $type; Ver = $ver; Payload = $payload })
                if ($type -eq 3 -and $ver -eq 0) {
                    for ($s = 0; $s -lt 8; $s++) { Send (SlotFrame $s) }
                } elseif ($type -eq 2) {
                    $slot = $payload[0]; $code = 1
                    if ($ver -eq 0) {
                        if ($slot -gt 7 -or -not $slots[[int]$slot].Used) { $code = 3 }
                    } elseif ($ver -eq 1) {
                        $k = [BitConverter]::ToUInt32($payload, 1)
                        if ($slot -le 1 -and $k -ne $key) { $code = 7 }
                        else {
                            $n = $payload[5..28]; $z = [Array]::IndexOf([byte[]]$n, [byte]0); if ($z -lt 0) { $z = 24 }
                            $name = if ($z -gt 0) { $utf8.GetString([byte[]]$n, 0, $z) } else { '' }
                            $slots[[int]$slot] = @{ Used = $true; Factory = ($slot -le 1); Freq = 777; Pulses = 20; Name = $name }
                        }
                    } elseif ($ver -eq 2) {
                        $slots[[int]$slot] = @{ Used = $false; Factory = $false; Freq = 0; Pulses = 0; Name = '' }
                    }
                    Send (Kp1 1 $ver 0x7C ([byte[]]($code)) $true)   # one byte, the code, as firmware 11.21 sends it
                    # after a save or a delete that went through, that slot's frame (firmware 11.21; a recall reports TRANSC & co.)
                    if (($ver -eq 1 -or $ver -eq 2) -and $code -eq 1) { Send (SlotFrame $slot) }
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
$mine | Set-Content -LiteralPath (Join-Path $Out 'kogger.log') -Encoding UTF8
$got | ForEach-Object { 'type {0} v{1} [{2}]' -f $_.Type, $_.Ver, $(if ($_.Payload) { ($_.Payload | ForEach-Object { $_.ToString('x2') }) -join ' ' } else { '' }) } |
    Set-Content -LiteralPath (Join-Path $Out 'stand.txt') -Encoding ASCII

$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}
$gets = @($got | Where-Object { $_.Type -eq 3 -and $_.Ver -eq 0 })
Expect ($gets.Count -ge 1) 'ask' ("GET v0 for ID_PRESET came {0} time(s)" -f $gets.Count)
$s0 = @($mine | Where-Object { $_ -like '*preset: slot 0: 815 kHz, 25 pulses*' -and $_.Contains('"' + $narrow + '"') })
$s1 = @($mine | Where-Object { $_ -like '*preset: slot 1: 450 kHz, 30 pulses*' -and $_.Contains('"' + $wide + '"') })
Expect ($s0.Count -ge 1 -and $s1.Count -ge 1 -and ($mine -match 'preset: the sonar keeps presets').Count -ge 1) 'slots' 'slots 0 and 1 logged with their frequency and name'
$recall1 = @($got | Where-Object { $_.Type -eq 2 -and $_.Ver -eq 0 -and $_.Payload.Length -eq 1 -and $_.Payload[0] -eq 1 })
$recall5 = @($got | Where-Object { $_.Type -eq 2 -and $_.Ver -eq 0 -and $_.Payload.Length -eq 1 -and $_.Payload[0] -eq 5 })
$toasts = @($mine | Where-Object { $_ -match 'notify: The preset was not applied' })
Expect ($recall1.Count -eq 1 -and $recall5.Count -eq 1 -and ($mine -match 'preset: SETTING v0 refused, code 3').Count -ge 1 -and $toasts.Count -eq 1) 'recall' (
    "SETTING v0 [1] {0}, [5] {1}; the refusal of 5 logged, shown {2} time(s)" -f $recall1.Count, $recall5.Count, $toasts.Count)
$save0 = @($got | Where-Object { $_.Type -eq 2 -and $_.Ver -eq 1 -and $_.Payload.Length -eq 29 -and $_.Payload[0] -eq 0 })
$save3 = @($got | Where-Object { $_.Type -eq 2 -and $_.Ver -eq 1 -and $_.Payload.Length -eq 29 -and $_.Payload[0] -eq 3 })
$want0 = [byte[]]($utf8.GetBytes($newName) + (New-Object byte[] (24 - $utf8.GetBytes($newName).Length)))
$ok0 = $save0.Count -eq 1 -and [BitConverter]::ToUInt32($save0[0].Payload, 1) -eq $key -and ((Compare-Object $save0[0].Payload[5..28] $want0 -SyncWindow 0) -eq $null)
$ok3 = $save3.Count -eq 1 -and [BitConverter]::ToUInt32($save3[0].Payload, 1) -eq 0
Expect ($ok0 -and $ok3) 'save' ("slot 0: the key and the name padded to 24 bytes {0}; slot 3: no key {1}" -f $ok0, $ok3)
$after = @($mine | Where-Object { $_ -match 'echoedit script: presets of ' } | Select-Object -Last 1)
$afterOk = $after.Count -eq 1 -and $after[0].Contains('0: 777 kHz 20 pulses "' + $newName + '"') -and $after[0].Contains('3: 777 kHz 20 pulses "user 3"')
Expect $afterOk 'after' ($(if ($after.Count) { $after[0] -replace '^\S+ \S+ \S+ \[\w+\] default: ', '' } else { 'no line' }))
"preset live: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
"logs: {0}, {1}" -f (Join-Path $Out 'kogger.log'), (Join-Path $Out 'stand.txt')
exit ([int]($failed -gt 0))

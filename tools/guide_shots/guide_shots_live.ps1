#requires -Version 5.1
<#
.SYNOPSIS
    Pictures of KoggerApp ME for the nanoSSS customer's guide: the window and the rectangles of what the guide points at.

.DESCRIPTION
    The owner, 2026-11-03: a setting the guide names is shown in a picture with its place framed. The guide is the
    l412_adc project's docs/nanoSSS_GUIDE_RU.html; its tools/guide_shots.py crops and frames from docs/img/shots/shots.json.
    One run of the app on the test desktop, its own profile "guide" (a copy of the test profile "perf"; the owner's
    profile is not touched) with a remembered serial port COM3 that is never opened (the run stops if COM3 exists),
    Russian, the main window 1600x1000, toasts kept out, against a UDP stand playing a nanoSSS 11.21: its settings
    (TRANSC, DSP, CHART_SETUP, DATASET, SND_SPEED, DIST_SETUP, UART) answered and taken, presets 0 "Narrow beam" 700 kHz
    and 1 "Wide beam" 450 kHz, the sonar at 600 kHz at first. Each state: raw/<id>.png and items/<id>.txt (windowItems:
    every visible item, its class, objectName, rectangle and the text it shows). make_marks.py then writes shots.json.
    Ids: connect, settings_freq, settings_dsp, settings_chart, settings_period, settings_soundspeed, settings_dataset,
    settings_baud, settings_flash, upgrade, beam_menu, beam_save, beam_active.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\guide_shots\guide_shots_live.ps1 -Exe <build>\KoggerApp.exe -PathPrefix "D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin"
    python -E tools\guide_shots\make_marks.py %TEMP%\kogger_guide_shots %TEMP%\kogger_guide_shots\shots.json %TEMP%\kogger_guide_shots\sheet.png "<build>"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [string] $PathPrefix,
    [string] $Out = "$env:TEMP\kogger_guide_shots",
    [int]    $Port = 47641,
    [int]    $TimeoutS = 240,
    [string] $PerfReg = 'D:\YandexDisk\DelphiProjects\KoggerApp_myEdition_transfer\KoggerApp-perf.reg'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own
$Profile = 'guide'
New-Item -ItemType Directory -Force -Path (Join-Path $Out 'raw'), (Join-Path $Out 'items') | Out-Null
$eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
if (Test-Path -LiteralPath $PerfReg) { cmd.exe /c "reg import `"$PerfReg`" >nul 2>&1" }
& reg.exe delete "HKCU\Software\KOGGER\KoggerApp-$Profile" /f *> $null
& reg.exe copy 'HKCU\Software\KOGGER\KoggerApp-perf' "HKCU\Software\KOGGER\KoggerApp-$Profile" /s /f *> $null
$ErrorActionPreference = $eap
# A remembered serial port for the connection picture: COM3, manual, closed; the app never opens it by itself.
if ([System.IO.Ports.SerialPort]::GetPortNames() -contains 'COM3') { throw 'COM3 exists: the picture would show a real port' }
$cfg = Join-Path $env:LOCALAPPDATA "KOGGER\KoggerApp-$Profile"
New-Item -ItemType Directory -Force -Path $cfg | Out-Null
@'
<?xml version="1.0" encoding="UTF-8"?>
<pinned_links>
    <link>
        <uuid>{6b1d3c52-0a4e-4c1b-9d55-3f7e2a90c0de}</uuid>
        <control_type>0</control_type>
        <port_name>COM3</port_name>
        <custom_name></custom_name>
        <baudrate>921600</baudrate>
        <parity>false</parity>
        <link_type>1</link_type>
        <address></address>
        <source_port>0</source_port>
        <destination_port>0</destination_port>
        <is_pinned>true</is_pinned>
        <is_hided>false</is_hided>
        <is_not_available>false</is_not_available>
        <connection_status>false</connection_status>
        <auto_speed_selection>false</auto_speed_selection>
    </link>
</pinned_links>
'@ | Set-Content -LiteralPath (Join-Path $cfg 'pinned_links.xml') -Encoding UTF8
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
$utf8 = [Text.Encoding]::UTF8
$key = [uint32]3379256650   # 0xC96B5D4A
function S([int[]] $codes) { return -join ($codes | ForEach-Object { [char]$_ }) }
$narrow = S @(0x0423, 0x0437, 0x043A, 0x0438, 0x0439, 0x20, 0x043B, 0x0443, 0x0447)
$wide = S @(0x0428, 0x0438, 0x0440, 0x043E, 0x043A, 0x0438, 0x0439, 0x20, 0x043B, 0x0443, 0x0447)

# ---- the sonar
$st = @{ Freq = 600; Pulses = 25; Boost = 0; Smooth = 5; Count = 2000; Res = 10; Offset = 0; Period = 50; Mask = 2
         Speed = 1500000; Baud = 921600; DistOffset = 0; DistMax = 50000; Conf = 20 }
$slots = @{}
for ($i = 0; $i -lt 8; $i++) { $slots[$i] = @{ Used = $false; Factory = $false; Freq = 0; Pulses = 0; Name = '' } }
$slots[0] = @{ Used = $true; Factory = $true; Freq = 700; Pulses = 25; Name = $narrow }
$slots[1] = @{ Used = $true; Factory = $true; Freq = 450; Pulses = 30; Name = $wide }

function Kp1([int] $type, [int] $ver, [int] $id, [byte[]] $payload, [bool] $resp) {
    $mode = ($type -band 3) -bor (($ver -band 7) -shl 3) -bor 0x40
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
function P([scriptblock] $fill) { $l = New-Object System.Collections.Generic.List[byte]; & $fill $l; return , $l.ToArray() }
function SlotFrame([int] $i) {
    $s = $slots[$i]
    $p = New-Object System.Collections.Generic.List[byte]
    $p.Add([byte]$i)
    $flags = 0; if ($s.Used) { $flags = $flags -bor 1 }; if ($s.Factory) { $flags = $flags -bor 2 }
    $p.Add([byte]$flags)
    Le $p $s.Freq 2; $p.Add([byte]$s.Pulses); $p.Add(0); $p.Add(0)
    Le $p 2000 2; Le $p 10 2; Le $p 0 2; Le $p 50 4
    $nb = $utf8.GetBytes($s.Name)
    for ($k = 0; $k -lt 24; $k++) { if ($k -lt $nb.Length) { $p.Add($nb[$k]) } else { $p.Add(0) } }
    return , (Kp1 1 0 0x7C $p.ToArray() $false)
}
function Transc { return , (Kp1 1 0 0x14 (P { param($l) Le $l $st.Freq 2; $l.Add([byte]$st.Pulses); $l.Add([byte]$st.Boost) }) $false) }
function Dsp { return , (Kp1 1 0 0x13 ([byte[]]($st.Smooth)) $false) }
function ChartSetup { return , (Kp1 1 0 0x12 (P { param($l) Le $l $st.Count 2; Le $l $st.Res 2; Le $l $st.Offset 2 }) $false) }
function Dataset([int] $ch) { return , (Kp1 1 0 0x10 (P { param($l) $l.Add([byte]$ch); Le $l $(if ($ch -eq 1) { $st.Period } else { 0 }) 4; Le $l $(if ($ch -eq 1) { $st.Mask } else { 0 }) 4 }) $false) }
function SoundSpeed { return , (Kp1 1 0 0x15 (P { param($l) Le $l $st.Speed 4 }) $false) }
function DistV1 { return , (Kp1 1 1 0x11 (P { param($l) Le $l $st.DistOffset 4; Le $l $st.DistMax 4 }) $false) }
function DistV2 { return , (Kp1 1 2 0x11 ([byte[]]($st.Conf)) $false) }
function Uart([int] $u) { return , (Kp1 1 0 0x18 (P { param($l) Le $l $key 4; $l.Add([byte]$u); Le $l $st.Baud 4 }) $false) }

# ---- the script: each state a picture and its items
$steps = New-Object System.Collections.Generic.List[object]
function Shot([string] $id) {
    $steps.Add(@{ op = 'wait'; ms = 700 })
    $steps.Add(@{ op = 'grab'; path = (Join-Path $Out "raw\$id.png") })
    $steps.Add(@{ op = 'windowItems'; name = 'main'; path = (Join-Path $Out "items\$id.txt") })
}
$steps.Add(@{ op = 'wait'; ms = 3000 })
$steps.Add(@{ op = 'tr'; apply = 'ru'; context = 'HotActionsPanel'; source = 'Save' })
$steps.Add(@{ op = 'windowSize'; w = 1600; h = 1000 })
$steps.Add(@{ op = 'itemSet'; has = 'maxCardWidth'; set = @{ suppressed = $true } })   # no toasts in the pictures
$steps.Add(@{ op = 'wait'; ms = 1500 })
$steps.Add(@{ op = 'storeCall'; method = 'openConnectionsSettings' }); $steps.Add(@{ op = 'wait'; ms = 1000 }); Shot 'connect'
$steps.Add(@{ op = 'devicePage'; addr = 0 }); $steps.Add(@{ op = 'wait'; ms = 1500 })
$steps.Add(@{ op = 'clickText'; text = [regex]::Unescape('\u0420\u0430\u0441\u0448\u0438\u0440\u0435\u043d\u043d\u044b\u0435 \u043d\u0430\u0441\u0442\u0440\u043e\u0439\u043a\u0438') }); $steps.Add(@{ op = 'wait'; ms = 1000 })
foreach ($p in @(@('settings_freq', [regex]::Unescape('\u041a\u043e\u043b\u0438\u0447\u0435\u0441\u0442\u0432\u043e \u0438\u043c\u043f\u0443\u043b\u044c\u0441\u043e\u0432')),
                 @('settings_dsp', [regex]::Unescape('\u0413\u043e\u0440. \u0441\u0433\u043b\u0430\u0436\u0438\u0432\u0430\u043d\u0438\u0435')),
                 @('settings_chart', [regex]::Unescape('\u0420\u0430\u0437\u0440\u0435\u0448\u0435\u043d\u0438\u0435, \u043c\u043c')),
                 @('settings_period', [regex]::Unescape('\u041f\u0435\u0440\u0438\u043e\u0434, \u043c\u0441'), 1),
                 @('settings_soundspeed', [regex]::Unescape('\u0421\u043a\u043e\u0440\u043e\u0441\u0442\u044c \u0437\u0432\u0443\u043a\u0430, \u043c/\u0441')),
                 @('settings_dataset', [regex]::Unescape('\u041d\u0430\u0431\u043e\u0440 \u0434\u0430\u043d\u043d\u044b\u0445')),
                 @('settings_baud', [regex]::Unescape('921600')),
                 @('settings_flash', [regex]::Unescape('\u0414\u0435\u0439\u0441\u0442\u0432\u0438\u044f')),
                 @('upgrade', [regex]::Unescape('\u041f\u0420\u041e\u0428\u0418\u0422\u042c')))) {
    if ($p[0] -eq 'upgrade') { $steps.Add(@{ op = 'typeInto'; near = [regex]::Unescape('\u0412\u0432\u0435\u0434\u0438\u0442\u0435 \u043f\u0443\u0442\u044c'); text = 'nanoSSS_v11.21.ufw' }) }
    $steps.Add(@{ op = 'showText'; text = $p[1]; margin = 120; index = $(if ($p.Count -gt 2) { $p[2] } else { 0 }) }); Shot $p[0]
}
$steps.Add(@{ op = 'storeCall'; set = @{ settingsPanelOpen = $false; settingsSubPageActive = $false } }); $steps.Add(@{ op = 'wait'; ms = 1200 })
$steps.Add(@{ op = 'clickItem'; name = 'devBadgeButton'; index = 0 }); $steps.Add(@{ op = 'wait'; ms = 1000 }); Shot 'beam_menu'
$steps.Add(@{ op = 'drag'; fromClass = 'FreqPresetButton'; fromIndex = 0; by = @(0, 0); steps = 1; holdMs = 1500 }); Shot 'beam_save'
$steps.Add(@{ op = 'clickText'; text = [regex]::Unescape('\u041e\u0442\u043c\u0435\u043d\u0430') }); $steps.Add(@{ op = 'wait'; ms = 1200 })
$steps.Add(@{ op = 'presetRecall'; slot = 0 }); $steps.Add(@{ op = 'wait'; ms = 1500 })
$steps.Add(@{ op = 'clickItem'; name = 'devBadgeButton'; index = 0 }); $steps.Add(@{ op = 'wait'; ms = 1000 }); Shot 'beam_active'
$steps.Add(@{ op = 'quit' })
$scriptPath = Join-Path $Out 'script.json'
[System.IO.File]::WriteAllText($scriptPath, (@{ live = $true; steps = $steps.ToArray() } | ConvertTo-Json -Depth 6), (New-Object System.Text.UTF8Encoding $false))

$udp = New-Object System.Net.Sockets.UdpClient(New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, ($Port + 1)))
[void]$udp.Client.IOControl(-1744830452, [byte[]](0, 0, 0, 0), $null)
$app = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, $Port)
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
$psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
$psi.EnvironmentVariables['KOGGER_AUTOTEST'] = "UDP:$Port"
$psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $scriptPath
$psi.EnvironmentVariables['KOGGER_UI_PROBE'] = $Out   # gives the probe its window: "grab" needs it
$before = if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
$proc = Start-TestDesktopProcess $psi
$seen = @{}
$deadline = (Get-Date).AddSeconds($TimeoutS)
$beat = [DateTime]::MinValue
function Send([byte[]] $f) { [void]$udp.Send($f, $f.Length, $app) }
try {
    while (-not $proc.HasExited) {
        if ((Get-Date) -gt $deadline) { $proc.Kill(); throw "the app did not end in $TimeoutS s" }
        if (((Get-Date) - $beat).TotalMilliseconds -ge 300) {
            $beat = Get-Date
            Send (Kp1 1 0 0x20 ([byte[]](0, 17, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4)) $false)
            Send (Kp1 1 2 0x20 ([byte[]](0, 0, 17, 0, 1, 0, 0, 21, 11)) $false)
        }
        if ($udp.Available -le 0) { Start-Sleep -Milliseconds 15; continue }
        $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
        $d = $udp.Receive([ref]$remote)
        $i = 0
        while ($i + 8 -le $d.Length) {
            if ($d[$i] -ne 0xBB -or $d[$i + 1] -ne 0x55) { $i++; continue }
            $len = $d[$i + 5]; $total = 6 + $len + 2
            if ($i + $total -gt $d.Length) { break }
            $mode = $d[$i + 3]; $type = $mode -band 3; $ver = ($mode -shr 3) -band 7; $id = $d[$i + 4]
            $payload = [byte[]]($d[($i + 6)..($i + 5 + [Math]::Max($len, 1))] | Select-Object -First $len)
            $seen["0x{0:x2} t{1} v{2}" -f $id, $type, $ver] = 1 + [int]$seen["0x{0:x2} t{1} v{2}" -f $id, $type, $ver]
            if ($type -eq 2) {   # a setting: taken, reported back
                switch ($id) {
                    0x14 { if ($len -ge 4) { $st.Freq = [BitConverter]::ToUInt16($payload, 0); $st.Pulses = $payload[2]; $st.Boost = $payload[3] }; Send (Transc) }
                    0x13 { if ($len -ge 1) { $st.Smooth = $payload[0] }; Send (Dsp) }
                    0x12 { if ($len -ge 6) { $st.Count = [BitConverter]::ToUInt16($payload, 0); $st.Res = [BitConverter]::ToUInt16($payload, 2); $st.Offset = [BitConverter]::ToUInt16($payload, 4) }; Send (ChartSetup) }
                    0x10 { if ($len -ge 9 -and $payload[0] -eq 1) { $st.Period = [BitConverter]::ToUInt32($payload, 1); $st.Mask = [BitConverter]::ToUInt32($payload, 5) }; Send (Dataset 1) }
                    0x15 { if ($len -ge 4) { $st.Speed = [BitConverter]::ToUInt32($payload, 0) }; Send (SoundSpeed) }
                }
                if ($id -eq 0x7C) {
                    $slot = $payload[0]; $code = 1
                    if ($ver -eq 0) {
                        if ($slot -gt 7 -or -not $slots[[int]$slot].Used) { $code = 3 }
                        else { $st.Freq = $slots[[int]$slot].Freq; $st.Pulses = $slots[[int]$slot].Pulses }
                    } elseif ($ver -eq 1) {
                        $n = $payload[5..28]; $z = [Array]::IndexOf([byte[]]$n, [byte]0); if ($z -lt 0) { $z = 24 }
                        $name = if ($z -gt 0) { $utf8.GetString([byte[]]$n, 0, $z) } else { '' }
                        $slots[[int]$slot] = @{ Used = $true; Factory = ($slot -le 1); Freq = $st.Freq; Pulses = $st.Pulses; Name = $name }
                    }
                    Send (Kp1 1 $ver 0x7C ([byte[]]($code)) $true)
                    if ($ver -eq 0 -and $code -eq 1) { Send (Transc) }
                    if (($ver -eq 1 -or $ver -eq 2) -and $code -eq 1) { Send (SlotFrame $slot) }
                }
            } elseif ($type -eq 3) {   # a question
                switch ($id) {
                    0x14 { Send (Transc) }
                    0x13 { Send (Dsp) }
                    0x12 { Send (ChartSetup) }
                    0x10 { Send (Dataset 1); Send (Dataset 2) }
                    0x15 { Send (SoundSpeed) }
                    0x11 { if ($ver -eq 1) { Send (DistV1) } elseif ($ver -eq 2) { Send (DistV2) } }
                    0x18 { if ($ver -eq 0) { Send (Uart 1) } }
                    0x7C { if ($ver -eq 0) { for ($s = 0; $s -lt 8; $s++) { Send (SlotFrame $s) } } }
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
$text -split "`r?`n" | Set-Content -LiteralPath (Join-Path $Out 'kogger.log') -Encoding UTF8
$seen.GetEnumerator() | Sort-Object Name | ForEach-Object { '{0} x{1}' -f $_.Name, $_.Value } | Set-Content -LiteralPath (Join-Path $Out 'asked.txt') -Encoding ASCII
@($text -split "`r?`n" | Where-Object { $_ -match 'echoedit script: (grab|windowItems|showText|clickText|clickItem|storeCall|devicePage|drag|tr|windowSize|presetRecall)' } |
    ForEach-Object { $_ -replace '^.*echoedit script: ', '' })

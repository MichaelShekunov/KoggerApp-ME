#requires -Version 5.1
<#
.SYNOPSIS
    The servo's manual mode end to end in the running app (DevDriver's pacing, read-back and refusal), against a
    stand that plays a Recorder's servo control over UDP: no device needed. docs/SERVO_MANUAL.md.

.DESCRIPTION
    Two runs of KoggerApp (KOGGER_PROFILE, reset from the transfer folder's KoggerApp-perf.reg first, so the user's
    settings are not touched) with KOGGER_AUTOTEST=UDP:<port> and a live edit script, on the test desktop. This
    script is the device: it answers ID_VERSION, sends its ID_SERVO_CONTROL state when asked (scanning, center
    15.00, angle range 180, step 4.5, range 90), takes a SETTING the way the contract says and acknowledges it
    (CONTENT v0 with the response bit: the code and the frame's two checksum bytes).

    Run "manual" (the firmware has manual mode). The script: manual on, a 1 s drag from 10 to -40 with a move
    every 5 ms, 12.34 typed, manual off. Checked:
      in         a SETTING with bit 1 and center_deg the scan's 15.00
      readback   a GET after it, and the app shows manual mode as supported
      pace       during the drag the SETTINGs come at least 20 ms apart (25 ms slots; the last one, the release, may
                 come sooner) and far fewer than the moves the script made
      release    the drag's last SETTING is -40.00; the typed 12.34 goes out as 1234
      out        a SETTING without bit 1 and center_deg 15.00 again
      held       the device never shown as without servo control (no lost acknowledgement)
    Run "old" (a firmware without manual mode: a SETTING with bit 1 gets RespErrorPayload). Checked:
      refused    the app logs the refusal, shows manual off and manual support -1, and sends no more bit 1
    Exit code 0 when every check holds.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\servo_check\servo_live.ps1 -Exe <build>\KoggerApp.exe -PathPrefix "D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [string] $PathPrefix,
    [string] $Profile = 'perf',
    [int]    $Port = 47621,
    [string] $Out = "$env:TEMP\kogger_servo_live",
    [string] $PerfReg = 'D:\YandexDisk\DelphiProjects\KoggerApp_myEdition_transfer\KoggerApp-perf.reg',
    [int]    $TimeoutS = 90
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"

function Kp1([int] $type, [int] $ver, [int] $id, [byte[]] $payload, [bool] $resp) {
    $mode = ($type -band 3) -bor (($ver -band 7) -shl 3) -bor 0x40   # the mark set: "this app's device"
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
function S16([byte[]] $p, [int] $at) { return [int][BitConverter]::ToInt16($p, $at) }
function U16([byte[]] $p, [int] $at) { return [int][BitConverter]::ToUInt16($p, $at) }
function StateBytes($st) {
    $l = New-Object System.Collections.Generic.List[byte]
    foreach ($v in @($st.General, $st.Options, $st.PwmMin, $st.PwmMax, $st.AngleRange, $st.Step, $st.Range, $st.Center)) {
        $l.AddRange([BitConverter]::GetBytes([int16]$v))   # every field here fits 16 bits signed
    }
    return , $l.ToArray()
}

function Invoke-Run([string] $kind, [string[]] $steps) {
    if (Test-Path -LiteralPath $PerfReg) {
        # reg.exe reports success on stderr; under 'Stop' PowerShell 5.1 would take that for a failure.
        cmd.exe /c "reg import `"$PerfReg`" >nul 2>&1"
    }
    $scriptPath = Join-Path $Out "script_$kind.json"
    ('{"live": true, "steps": [' + ($steps -join ",`n") + ']}') | Set-Content -LiteralPath $scriptPath -Encoding ASCII
    $st = @{ General = 1; Options = 0; PwmMin = 500; PwmMax = 2500; AngleRange = 18000; Step = 450; Range = 9000; Center = 1500 }

    $udp = New-Object System.Net.Sockets.UdpClient(New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, ($Port + 1)))
    # Windows reports an ICMP "port unreachable" of an earlier send (the app not listening yet) as a reset of the
    # next Receive; SIO_UDP_CONNRESET off keeps the stand going.
    [void]$udp.Client.IOControl(-1744830452, [byte[]](0, 0, 0, 0), $null)
    $udp.Client.ReceiveTimeout = 20   # a blocking receive: a frame is timed when it comes, not when a sleep ends
    $app = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, $Port)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
    if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
    $psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
    $psi.EnvironmentVariables['KOGGER_AUTOTEST'] = "UDP:$Port"
    $psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $scriptPath
    $before = if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
    $proc = Start-TestDesktopProcess $psi

    $got = New-Object System.Collections.Generic.List[object]   # what the app sent for 0x27: time, type, state
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    $deadline = (Get-Date).AddSeconds($TimeoutS)
    $beat = [DateTime]::MinValue
    try {
        while (-not $proc.HasExited) {
            if ((Get-Date) -gt $deadline) { $proc.Kill(); throw "run $kind`: the app did not end in $TimeoutS s" }
            if (((Get-Date) - $beat).TotalMilliseconds -ge 300) {
                $beat = Get-Date
                $f = Kp1 1 0 0x20 ([byte[]](0, 17, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4)) $false   # VERSION v0
                [void]$udp.Send($f, $f.Length, $app)
            }
            $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
            try { $d = $udp.Receive([ref]$remote) } catch [System.Net.Sockets.SocketException] { continue }
            $t = $clock.Elapsed.TotalMilliseconds
            $i = 0
            while ($i + 8 -le $d.Length) {
                if ($d[$i] -ne 0xBB -or $d[$i + 1] -ne 0x55) { $i++; continue }
                $len = $d[$i + 5]; $total = 6 + $len + 2
                if ($i + $total -gt $d.Length) { break }
                $mode = $d[$i + 3]; $type = $mode -band 3; $ver = ($mode -shr 3) -band 7; $id = $d[$i + 4]
                if ($id -eq 0x27 -and $ver -eq 0) {
                    $p = [byte[]]($d[($i + 6)..($i + 5 + [Math]::Max($len, 1))] | Select-Object -First $len)
                    if ($type -eq 3) {
                        $got.Add([pscustomobject]@{ T = $t; Type = 3; General = -1; Center = 0 })
                        $f = Kp1 1 0 0x27 (StateBytes $st) $false
                        [void]$udp.Send($f, $f.Length, $app)
                    } elseif ($type -eq 2 -and $len -eq 16) {
                        $g = U16 $p 0
                        $got.Add([pscustomobject]@{ T = $t; Type = 2; General = $g; Center = (S16 $p 14) })
                        $code = 1
                        if (($g -band 2) -and $kind -eq 'old') { $code = 3 }   # RespErrorPayload: no manual mode here
                        elseif ((U16 $p 4) -ge (U16 $p 6) -or (S16 $p 8) -le 0 -or (S16 $p 12) -lt 0 -or (S16 $p 10) -eq 0) { $code = 3 }
                        else {
                            $st = @{ General = $g; Options = (U16 $p 2); PwmMin = (U16 $p 4); PwmMax = (U16 $p 6)
                                     AngleRange = (S16 $p 8); Step = (S16 $p 10); Range = (S16 $p 12); Center = (S16 $p 14) }
                        }
                        $f = Kp1 1 0 0x27 ([byte[]]($code, $d[$i + $total - 2], $d[$i + $total - 1])) $true
                        [void]$udp.Send($f, $f.Length, $app)
                    }
                }
                $i += $total
            }
        }
    } finally {
        $udp.Close()
        if (-not $proc.HasExited) { $proc.Kill() }   # the stand failed: no app left behind
    }
    $fs = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); [void]$fs.Seek($before, 'Begin')
    $text = (New-Object System.IO.StreamReader($fs, [Text.Encoding]::UTF8)).ReadToEnd(); $fs.Close()
    $mine = @($text -split "`r?`n")
    $mine | Set-Content -LiteralPath (Join-Path $Out "kogger_$kind.log") -Encoding UTF8
    $got | ForEach-Object { '{0,9:F1} ms  {1}  general {2}  center {3}' -f $_.T, $(if ($_.Type -eq 3) { 'GET    ' } else { 'SETTING' }), $_.General, $_.Center } |
        Set-Content -LiteralPath (Join-Path $Out "stand_$kind.txt") -Encoding ASCII
    $r = New-Object psobject
    $r | Add-Member -NotePropertyName Got -NotePropertyValue ($got.ToArray())
    $r | Add-Member -NotePropertyName Log -NotePropertyValue $mine
    return $r
}

$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}

# ---- run "manual"
$a = Invoke-Run 'manual' @(
    '{"op": "waitServo", "timeoutMs": 30000}', '{"op": "wait", "ms": 500}', '{"op": "servo"}',
    '{"op": "servoManual", "on": true}', '{"op": "wait", "ms": 700}', '{"op": "servo"}',
    '{"op": "servoDrag", "from": 10, "to": -40, "ms": 1000, "everyMs": 5}', '{"op": "wait", "ms": 700}', '{"op": "servo"}',
    '{"op": "servoSet", "deg": 12.34}', '{"op": "wait", "ms": 700}', '{"op": "servo"}',
    '{"op": "servoManual", "on": false}', '{"op": "wait", "ms": 700}', '{"op": "servo"}', '{"op": "quit"}')
$sets = @($a.Got | Where-Object { $_.Type -eq 2 })
$manualSets = @($sets | Where-Object { $_.General -band 2 })
$in = @($manualSets | Select-Object -First 1)
Expect ($in.Count -eq 1 -and $in[0].Center -eq 1500) 'in' ("first SETTING with bit 1: {0}" -f $(if ($in.Count) { "center $($in[0].Center)" } else { 'none' }))
$getAfter = @($a.Got | Where-Object { $_.Type -eq 3 -and $in.Count -and $_.T -gt $in[0].T })
$servoLines = @($a.Log | Where-Object { $_ -match 'echoedit script: servo of ' })
Expect ($getAfter.Count -ge 1 -and $servoLines.Count -ge 2 -and $servoLines[1] -match 'manual 1 at 15\.00, manual support 1') 'readback' (
    "GETs after it: {0}; {1}" -f $getAfter.Count, $(if ($servoLines.Count -ge 2) { $servoLines[1] -replace '^.*servo of ', '' } else { 'no line' }))
$dragLine = @($a.Log | Where-Object { $_ -match 'echoedit script: servoDrag .*: (\d+) moves' } | Select-Object -Last 1)
$moves = if ($dragLine.Count -and $dragLine[0] -match ': (\d+) moves') { [int]$Matches[1] } else { 0 }
# The drag's frames: bit 1, between the entry and the typed 12.34.
$typed = @($manualSets | Where-Object { $_.Center -eq 1234 } | Select-Object -First 1)
$drag = @($manualSets | Where-Object { $in.Count -and $typed.Count -and $_.T -gt $in[0].T -and $_.T -lt $typed[0].T })
# One slot is 25 ms (Windows' coarse timer makes it ~31): at most 4 in any 100 ms, 5 with the release. Counted in a
# window, not as the closest two: a frame the stand reads late shortens the next gap without the app sending sooner.
$most = 0
for ($k = 0; $k -lt $drag.Count; $k++) {
    $n = @($drag | Where-Object { $_.T -ge $drag[$k].T -and $_.T -lt $drag[$k].T + 100 }).Count
    if ($n -gt $most) { $most = $n }
}
$gaps = @(for ($k = 1; $k -lt $drag.Count - 1; $k++) { $drag[$k].T - $drag[$k - 1].T })   # the release not counted
$median = if ($gaps.Count) { @($gaps | Sort-Object)[[int]($gaps.Count / 2)] } else { 0 }
Expect ($drag.Count -ge 10 -and $drag.Count -le 42 -and $most -le 5 -and $moves -gt 2 * $drag.Count) 'pace' (
    "{0} SETTINGs for {1} moves; at most {2} in 100 ms; the median gap {3:F1} ms" -f $drag.Count, $moves, $most, $median)
$lastDrag = @($drag | Select-Object -Last 1)
Expect ($lastDrag.Count -eq 1 -and $lastDrag[0].Center -eq -4000 -and $typed.Count -eq 1) 'release' (
    "the drag's last {0}; 12.34 typed: {1}" -f $(if ($lastDrag.Count) { $lastDrag[0].Center } else { 'none' }), $(if ($typed.Count) { 'center 1234' } else { 'not sent as 1234' }))
$outSet = @($sets | Where-Object { $typed.Count -and $_.T -gt $typed[0].T -and -not ($_.General -band 2) } | Select-Object -First 1)
Expect ($outSet.Count -eq 1 -and $outSet[0].Center -eq 1500 -and $servoLines.Count -ge 5 -and $servoLines[4] -match 'manual 0 at ') 'out' (
    "SETTING without bit 1: {0}" -f $(if ($outSet.Count) { "center $($outSet[0].Center)" } else { 'none' }))
$held = @($servoLines | Where-Object { $_ -match 'supported 1,' })
Expect ($servoLines.Count -eq 5 -and $held.Count -eq 5) 'held' ("{0} of {1} state lines with servo control" -f $held.Count, $servoLines.Count)

# ---- run "old"
$b = Invoke-Run 'old' @(
    '{"op": "waitServo", "timeoutMs": 30000}', '{"op": "wait", "ms": 500}',
    '{"op": "servoManual", "on": true}', '{"op": "wait", "ms": 900}', '{"op": "servo"}',
    '{"op": "servoDrag", "from": 0, "to": 30, "ms": 300}', '{"op": "wait", "ms": 300}', '{"op": "quit"}')
$bSets = @($b.Got | Where-Object { $_.Type -eq 2 -and ($_.General -band 2) })
$refusedLog = @($b.Log | Where-Object { $_ -match 'servo: manual mode refused' })
$bLine = @($b.Log | Where-Object { $_ -match 'echoedit script: servo of ' } | Select-Object -Last 1)
Expect ($bSets.Count -eq 1 -and $refusedLog.Count -eq 1 -and $bLine.Count -eq 1 -and $bLine[0] -match 'manual 0 at .*manual support -1') 'refused' (
    "SETTINGs with bit 1: {0}; refusal logged {1}; {2}" -f $bSets.Count, $refusedLog.Count, $(if ($bLine.Count) { $bLine[0] -replace '^.*servo of ', '' } else { 'no line' }))

"servo live: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
"logs: {0}" -f $Out
exit ([int]($failed -gt 0))

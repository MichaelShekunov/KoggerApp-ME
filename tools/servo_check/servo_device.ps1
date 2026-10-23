#requires -Version 5.1
<#
.SYNOPSIS
    The servo's manual mode on a real Recorder with firmware feat/servo-pwm-x7 on a serial port: the servo turns.
    docs/SERVO_MANUAL.md.

.DESCRIPTION
    One run of KoggerApp on the test desktop (a test profile of its own, KOGGER_SETTINGS_INI, written fresh; the
    owner's settings are not touched) with KOGGER_AUTOTEST=<port> and a live edit script: manual on, 45, -45, a 1.5 s
    drag from -45 to 30 with a move every 5 ms, 0, manual off, quit. The servo moves: run it only with the owner's
    word. It is left as the scan had it (manual off, the scan's center sent back).
    Checked from the app's log, each a read-back the device sent (DevDriver's "servo: the device reports ..."):
      control    the app found the servo control
      in         the device reports manual 1, holding the scan's center; manual support 1
      angles     then 45.00, -45.00, 30.00 (the drag's end), 0.00, in that order
      out        then manual 0 with the scan's center as before
      answered   every setting sent got RespOk: answered >= manual frames sent
    Exit code 0 when every check holds.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\servo_check\servo_device.ps1 -Exe <build>\KoggerApp.exe -Port COM17
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [Parameter(Mandatory = $true)] [string] $Port,
    [string] $Profile = 'servodevice',
    [string] $Out = "$env:TEMP\kogger_servo_device",
    [int]    $TimeoutS = 120
)

$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding $false
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$ini = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile.ini"
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
$seed = @('[General]', 'schemaVersion=8', '', '[main]', 'ui\welcomeShown=true', 'ui\instrumentsGradeList=2', '')
if (Test-Path -LiteralPath $ini) { Remove-Item -LiteralPath $ini -Force }
New-Item -ItemType Directory -Force -Path (Split-Path $ini) | Out-Null
[System.IO.File]::WriteAllText($ini, ($seed -join "`r`n"), $utf8)

$steps = @(
    '{"op": "waitServo", "timeoutMs": 40000}', '{"op": "wait", "ms": 1500}', '{"op": "servo"}',
    '{"op": "servoManual", "on": true}', '{"op": "wait", "ms": 1000}', '{"op": "servo"}',
    '{"op": "servoSet", "deg": 45}', '{"op": "wait", "ms": 1000}',
    '{"op": "servoSet", "deg": -45}', '{"op": "wait", "ms": 1000}',
    '{"op": "servoDrag", "from": -45, "to": 30, "ms": 1500, "everyMs": 5}', '{"op": "wait", "ms": 1000}',
    '{"op": "servoSet", "deg": 0}', '{"op": "wait", "ms": 1000}',
    '{"op": "servoManual", "on": false}', '{"op": "wait", "ms": 1000}', '{"op": "servo"}', '{"op": "quit"}')
$sp = Join-Path $Out 'steps.json'
[System.IO.File]::WriteAllText($sp, ('{"live": true, "steps": [' + ($steps -join ",`n") + ']}'), $utf8)

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
$psi.EnvironmentVariables['PATH'] = 'D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin;' + $env:PATH
$psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
$psi.EnvironmentVariables['KOGGER_SETTINGS_INI'] = '1'
$psi.EnvironmentVariables['KOGGER_AUTOTEST'] = $Port
$psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $sp
$proc = Start-TestDesktopProcess $psi
if (-not $proc.WaitForExit($TimeoutS * 1000)) { $proc.Kill(); throw "the app did not end in $TimeoutS s" }

$lines = Get-Content -LiteralPath $log -Encoding UTF8
$start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
$mine = @($lines[($start - 1)..($lines.Count - 1)])
$mine | Set-Content -LiteralPath (Join-Path $Out 'kogger.log') -Encoding UTF8

$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}
$reports = @($mine | Where-Object { $_ -match 'servo: the device reports manual' } | ForEach-Object {
    if ($_ -match 'reports manual (\d) at (-?[\d.]+), scan center (-?[\d.]+); manual frames sent (\d+), settings answered (\d+)') {
        [pscustomobject]@{ Manual = [int]$Matches[1]; Deg = [double]$Matches[2]; Center = [double]$Matches[3]
                           Sent = [int]$Matches[4]; Answered = [int]$Matches[5] }
    } })
$states = @($mine | Where-Object { $_ -match 'echoedit script: servo of ' })
Expect ((@($mine | Where-Object { $_ -match 'echoedit script: waitServo, \d+ ms$' })).Count -eq 1) 'control' ($(if ($states.Count) { $states[0] -replace '^.*servo of ', '' } else { 'no servo state logged' }))
$first = @($reports | Select-Object -First 1)
$center0 = if ($first.Count) { $first[0].Center } else { [double]::NaN }
Expect ($first.Count -eq 1 -and $first[0].Manual -eq 1 -and [Math]::Abs($first[0].Deg - [Math]::Max(-90, [Math]::Min(90, $center0))) -lt 0.006 -and
        $states.Count -ge 2 -and $states[1] -match 'manual 1 at .*manual support 1') 'in' (
    $(if ($first.Count) { "manual {0} at {1:F2}, scan center {2:F2}" -f $first[0].Manual, $first[0].Deg, $first[0].Center } else { 'no read-back' }))
# The read-backs in order, each angle once at least: a step's read-back may come twice (the drag's end and its set).
$want = @(45.0, -45.0, 30.0, 0.0)
$k = 0
foreach ($r in $reports) { if ($k -lt $want.Count -and $r.Manual -eq 1 -and [Math]::Abs($r.Deg - $want[$k]) -lt 0.006) { $k++ } }
Expect ($k -eq $want.Count) 'angles' (($reports | Where-Object { $_.Manual -eq 1 } | ForEach-Object { '{0:F2}' -f $_.Deg }) -join ', ')
$last = @($reports | Select-Object -Last 1)
Expect ($last.Count -eq 1 -and $last[0].Manual -eq 0 -and [Math]::Abs($last[0].Center - $center0) -lt 0.006) 'out' (
    $(if ($last.Count) { "manual {0}, scan center {1:F2}" -f $last[0].Manual, $last[0].Center } else { 'no read-back' }))
Expect ($last.Count -eq 1 -and $last[0].Answered -ge $last[0].Sent -and $last[0].Sent -gt 5) 'answered' (
    $(if ($last.Count) { "manual frames sent {0}, settings answered {1}" -f $last[0].Sent, $last[0].Answered } else { '-' }))
$drag = @($mine | Where-Object { $_ -match 'echoedit script: servoDrag' } | ForEach-Object { $_ -replace '^.*echoedit script: ', '' })
"drag: {0}" -f $(if ($drag.Count) { $drag[0] } else { 'not logged' })
"servo device: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
"log: {0}" -f (Join-Path $Out 'kogger.log')
exit ([int]($failed -gt 0))

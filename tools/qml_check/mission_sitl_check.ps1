#requires -Version 5.1
<#
.SYNOPSIS
    The missions (upstream's planner, transfer and run tracker) against ArduRover SITL, checked by another MAVLink.

.DESCRIPTION
    1. SITL: ArduRover 4.4.0, motorboat (WSL ~/sitl, as docs/BOAT_MISSION.md sets it up), started fresh (--wipe) by
       the caller (below); stopped at the end.
    2. The app on the test desktop, a test profile of its own written fresh, KOGGER_AUTOTEST=TCP:5760 (a hidden TCP link
       to SITL's serial 0), the edit script's mission steps: the autopilot bound; a plan of three waypoints, a rally
       point and an inclusion fence, 1.5 m/s, made in the planner; uploaded; downloaded again (the planner says
       whether what came back matches the plan; MavVehicle has the tracker's route for its ghost); then armed, AUTO
       for a while (MISSION_CURRENT must move on), HOLD, disarmed.
    3. The other MAVLink: pymavlink in WSL on SITL's serial 2 (TCP 5762) reads what the autopilot holds - the mission,
       the fence, the rally points, FENCE_ENABLE/TYPE/ACTION - and that is compared with the plan of step 2.
    Exit code 0 when every check holds, 1 when one fails, 2 when SITL or pymavlink is not there.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\qml_check\mission_sitl_check.ps1 -Exe <build>\KoggerApp.exe
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [string] $Profile = 'missionsitl',
    [int]    $TimeoutS = 240
)
$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding $false
$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}
function ToWsl([string] $winPath) {
    $p = $winPath.Replace('\', '/')
    return '/mnt/' + $p.Substring(0, 1).ToLower() + $p.Substring(2)
}

# The plan, near SITL's home (40.35, 45.0): what step 2 makes and step 3 expects.

$points = @(@(40.3505, 45.0005), @(40.3510, 45.0012), @(40.3503, 45.0018))
$rally = ,@(40.3497, 44.9995)   # the comma: one pair, not unrolled into two numbers
$fence = @(@(40.3480, 44.9970), @(40.3530, 44.9970), @(40.3530, 45.0040), @(40.3480, 45.0040))
$speed = 1.5
function JsonPairs($list) { '[' + (($list | ForEach-Object { '[' + $_[0] + ', ' + $_[1] + ']' }) -join ', ') + ']' }

$work = Join-Path ([System.IO.Path]::GetTempPath()) 'kogger_mission_sitl'
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force -Path $work | Out-Null

# --- 1. SITL ---------------------------------------------------------------------------------------------------------
# Started by the caller (it has to be: started from PowerShell or as a detached task, SITL quits right after loading its
# defaults, 2026-10-23; from Git Bash it runs), e.g. in Git Bash, the check in the same call:
#   (MSYS_NO_PATHCONV=1 wsl -e bash -c 'cd ~/sitl && ./ardurover --model motorboat --home 40.35,45.0,1900,0 #        --defaults rover.parm,motorboat.parm -I0 --speedup 5 --wipe < /dev/null > ~/sitl/sitl.out 2>&1 & sleep 600' &)
#   powershell -ExecutionPolicy Bypass -File tools/qml_check/mission_sitl_check.ps1 -Exe <build>/KoggerApp.exe
# No connection to look at it first: SITL takes the first one for its serial 0.
$running = & wsl.exe -e pgrep -x ardurover 2>$null
if (-not $running) { "no SITL running (ardurover in WSL); start it as above"; exit 2 }

try {
    # --- 2. the app ----------------------------------------------------------------------------------------------------
    . (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')
    $ini = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile.ini"
    $log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
    $seed = @('[General]', 'schemaVersion=8', '', '[main]', 'ui\welcomeShown=true', '')
    if (Test-Path -LiteralPath $ini) { Remove-Item -LiteralPath $ini -Force }
    [System.IO.File]::WriteAllText($ini, ($seed -join "`r`n"), $utf8)
    $steps = @(
        '{"op": "waitAutopilot", "timeoutMs": 60000}',
        '{"op": "wait", "ms": 3000}',
        '{"op": "missionState"}',
        ('{"op": "missionPlan", "home": [40.35, 45.0], "points": ' + (JsonPairs $points) + ', "rally": ' + (JsonPairs $rally) + ', "fence": ' + (JsonPairs $fence) + ', "speed": ' + $speed + '}'),
        '{"op": "missionUpload"}',
        '{"op": "waitMissionTransfer", "timeoutMs": 60000}',
        '{"op": "missionState"}',
        '{"op": "missionDownload"}',
        '{"op": "waitMissionTransfer", "timeoutMs": 60000}',
        '{"op": "wait", "ms": 1000}',
        '{"op": "missionState"}',
        '{"op": "wait", "ms": 15000}',
        '{"op": "autopilotArm", "on": true}',
        '{"op": "wait", "ms": 3000}',
        '{"op": "autopilotMode", "mode": 10}',
        '{"op": "wait", "ms": 25000}',
        '{"op": "missionState"}',
        '{"op": "autopilotMode", "mode": 4}',
        '{"op": "wait", "ms": 2000}',
        '{"op": "autopilotArm", "on": false}',
        '{"op": "wait", "ms": 2000}',
        '{"op": "missionState"}',
        '{"op": "quit"}')
    $sp = Join-Path $work 'steps.json'
    [System.IO.File]::WriteAllText($sp, ('{"live": true, "steps": [' + ($steps -join ', ') + ']}'), $utf8)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
    $psi.EnvironmentVariables['PATH'] = 'D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin;' + $env:PATH
    $psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
    $psi.EnvironmentVariables['KOGGER_SETTINGS_INI'] = '1'
    $psi.EnvironmentVariables['KOGGER_AUTOTEST'] = 'TCP:5760'
    $psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $sp
    $app = Start-TestDesktopProcess $psi
    if (-not $app.WaitForExit($TimeoutS * 1000)) { $app.Kill(); Expect $false 'the app ended' "not in $TimeoutS s" }
    $lines = @(Get-Content -LiteralPath $log -Encoding UTF8)
    $start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
    $lines = $lines[($start - 1)..($lines.Count - 1)]
    [System.IO.File]::WriteAllLines((Join-Path $work 'app.log'), [string[]]$lines, $utf8)
    $states = @($lines | ForEach-Object { if ($_ -match 'echoedit script: missionState (\{.*\})$') { $Matches[1] | ConvertFrom-Json } })
    $transfers = @($lines | Where-Object { $_ -match 'echoedit script: mission transfer: ' } | ForEach-Object { ($_ -split 'mission transfer: ')[1] })
    $received = $lines | Where-Object { $_ -match 'echoedit script: mission received: ' } | Select-Object -Last 1
    Expect ($states.Count -ge 1 -and $states[0].online) 'the autopilot bound' $(if ($states.Count) { "mode $($states[0].modeName)" } else { 'no missionState' })
    Expect (@($lines -match 'missionPlan, 5 added: items 3, rally 1, fence 1').Count -gt 0) 'the plan made' (($lines | Where-Object { $_ -match 'echoedit script: missionPlan' }) -join '')
    Expect ($transfers.Count -ge 1 -and $transfers[0] -match '^upload ok') 'uploaded' ($transfers -join '; ')
    Expect ($transfers.Count -ge 2 -and $transfers[1] -match '^download ok') 'downloaded' ($transfers -join '; ')
    Expect ($received -match 'empty 0, matches the plan 1, route points \d+, fence 1, rally 1, skipped 0') 'what came back matches the plan' "$received"
    $read = if ($states.Count -ge 3) { $states[2] } else { $null }
    Expect ($read -and $read.runNav -ge 5 -and $read.boatRoute -eq $read.runNav) 'the ghost has the vehicle''s route (tracker -> MavVehicle)' `
        $(if ($read) { "tracker $($read.runNav) positioned items, MavVehicle $($read.boatRoute)" } else { 'no state' })
    $run = if ($states.Count -ge 4) { $states[3] } else { $null }
    Expect ($run -and $run.armed -eq 1 -and $run.modeName -match 'AUTO|Auto' -and $run.missionSeq -ge 2) 'the mission runs (armed, AUTO, MISSION_CURRENT moved on)' `
        $(if ($run) { "armed $($run.armed), mode $($run.modeName), seq $($run.missionSeq) of $($run.missionTotal)" } else { 'no state' })
    $end = if ($states.Count -ge 5) { $states[4] } else { $null }
    Expect ($end -and $end.armed -eq 0 -and $end.modeName -match 'HOLD|Hold') 'held and disarmed' $(if ($end) { "armed $($end.armed), mode $($end.modeName)" } else { 'no state' })
    $errors = @($lines | Where-Object { $_ -match 'qrc:/qml.*(TypeError|ReferenceError)' })
    Expect ($errors.Count -eq 0) 'no QML errors' $(if ($errors) { $errors[0] } else { 'none' })

    # --- 3. the other MAVLink -------------------------------------------------------------------------------------------
    $py = Join-Path $work 'read.py'
    [System.IO.File]::WriteAllText($py, @'
import json, sys, time
from pymavlink import mavutil
m = mavutil.mavlink_connection('tcp:127.0.0.1:5762', source_system=250, source_component=191)
m.wait_heartbeat(timeout=20)
def items(kind):
    m.mav.mission_request_list_send(m.target_system, m.target_component, kind)
    c = m.recv_match(type='MISSION_COUNT', blocking=True, timeout=5)
    if c is None:
        return None
    out = []
    for i in range(c.count):
        m.mav.mission_request_int_send(m.target_system, m.target_component, i, kind)
        it = m.recv_match(type='MISSION_ITEM_INT', blocking=True, timeout=5)
        if it is None:
            return None
        out.append({'seq': it.seq, 'cmd': it.command, 'lat': it.x / 1e7, 'lon': it.y / 1e7, 'p1': it.param1, 'p2': it.param2})
    m.mav.mission_ack_send(m.target_system, m.target_component, 0, kind)
    return out
def param(name):
    m.mav.param_request_read_send(m.target_system, m.target_component, name.encode(), -1)
    t = time.time()
    while time.time() - t < 5:
        p = m.recv_match(type='PARAM_VALUE', blocking=True, timeout=5)
        if p is not None and p.param_id == name:
            return p.param_value
    return None
print(json.dumps({'mission': items(0), 'fence': items(1), 'rally': items(2),
                  'FENCE_ENABLE': param('FENCE_ENABLE'), 'FENCE_TYPE': param('FENCE_TYPE'), 'FENCE_ACTION': param('FENCE_ACTION')}))
'@, $utf8)
    $out = & wsl.exe -e python3 (ToWsl $py) 2>&1
    $json = ($out | Where-Object { $_ -match '^\{' } | Select-Object -Last 1)
    if (-not $json) { Expect $false 'read by pymavlink' (($out | Select-Object -Last 3) -join ' '); throw 'no read' }
    $held = $json | ConvertFrom-Json
    [System.IO.File]::WriteAllText((Join-Path $work 'held.json'), $json, $utf8)
    $near = { param($a, $b) [Math]::Abs($a - $b) -lt 2e-7 }
    $wps = @($held.mission | Where-Object { $_.cmd -eq 16 -and $_.seq -gt 0 })   # MAV_CMD_NAV_WAYPOINT, not the home slot
    # The planner's route: the start point, the points, and (the end action by default, "return to start") the start again
    $route = @(,@(40.35, 45.0)) + $points + @(,@(40.35, 45.0))
    $okWps = $wps.Count -eq $route.Count
    for ($i = 0; $okWps -and $i -lt $route.Count; $i++) { $okWps = (& $near $wps[$i].lat $route[$i][0]) -and (& $near $wps[$i].lon $route[$i][1]) }
    Expect $okWps 'the waypoints on the autopilot' (($wps | ForEach-Object { "$($_.lat),$($_.lon)" }) -join ' ')
    $speedItem = @($held.mission | Where-Object { $_.cmd -eq 178 }) | Select-Object -First 1   # DO_CHANGE_SPEED
    Expect ($speedItem -and [Math]::Abs($speedItem.p2 - $speed) -lt 0.01) 'the speed on the autopilot' $(if ($speedItem) { "$($speedItem.p2) m/s" } else { 'no DO_CHANGE_SPEED' })
    $fv = @($held.fence | Where-Object { $_.cmd -eq 5001 })   # MAV_CMD_NAV_FENCE_POLYGON_VERTEX_INCLUSION
    $okFence = $fv.Count -eq $fence.Count
    for ($i = 0; $okFence -and $i -lt $fence.Count; $i++) { $okFence = (& $near $fv[$i].lat $fence[$i][0]) -and (& $near $fv[$i].lon $fence[$i][1]) }
    Expect $okFence 'the fence on the autopilot' "$($fv.Count) inclusion vertices"
    $rp = @($held.rally | Where-Object { $_.cmd -eq 5100 })   # MAV_CMD_NAV_RALLY_POINT
    Expect ($rp.Count -eq 1 -and (& $near $rp[0].lat $rally[0][0]) -and (& $near $rp[0].lon $rally[0][1])) 'the rally point on the autopilot' "$($rp.Count)"
    Expect ($held.FENCE_ENABLE -eq 1 -and (([int]$held.FENCE_TYPE) -band 4) -ne 0) 'the fence switched on' "FENCE_ENABLE $($held.FENCE_ENABLE), FENCE_TYPE $($held.FENCE_TYPE), FENCE_ACTION $($held.FENCE_ACTION)"
}
finally {
    & wsl.exe -e pkill -x ardurover 2>$null | Out-Null   # a fresh SITL for each run: the next one wipes its own
    Remove-Item -LiteralPath $ini -Force -ErrorAction SilentlyContinue
}
"mission sitl check: {0} (logs in {1})" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" })), $work
exit ([int]($failed -gt 0))

#requires -Version 5.1
<#
.SYNOPSIS
    Drags the 3D view (a left-button pan, as a finger does on the head unit) for a while and samples the
    stacks of the app's threads meanwhile: where a drag's time goes, and how smooth it is.

.DESCRIPTION
    The owner, 2026-10-22: dragging the map in the 3D window stutters on the head unit, the CPU no higher
    than 20 %. The app opens -File (as a double click does, in a test profile, on the test desktop); once
    the bottom track is quiet the edit script's "drag3d" step drags the view through its triggers, as the
    QML pointer handlers do (Windows' own mouse input cannot be posted: Qt reads the buttons with
    GetAsyncKeyState), for -DragSeconds: the pointer circles -Radius px about the view's middle, one turn
    in -TurnSeconds, -MoveHz moves a second. As the step starts ("drag3d start" in kogger.log),
    sampler.exe (sampler\, built as profile_open.ps1 builds it) samples every -IntervalMs for as long.
    Printed: the step's line (moves, frames and the longest gap between them, the GUI thread's time per
    move, how late the moves' timer fired), the app's "health:" lines, the sampler's report. -NoDrag
    samples the same stretch without a drag (a "wait"), for a baseline. -Action runs another edit-script step in
    the drag's place (a "relayout": the docks' cost), the sampler starting at the log line -Mark.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\perf\drag_profile.ps1 -Exe <build>\KoggerApp.exe -File D:\Temp\hu_field\2026.10.22_07.17.03.klf -PathPrefix "D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [Parameter(Mandatory = $true)] [string] $File,
    [string] $PathPrefix,
    [string] $Profile = 'perf',
    [double] $DragSeconds = 20,
    [int]    $MoveHz = 100,
    [int]    $Radius = 120,
    [double] $TurnSeconds = 2,
    [int]    $IntervalMs = 2,
    [string] $Focus = '',
    [switch] $NoDrag,
    [switch] $NoSampler,
    [switch] $SettingsIni,         # KOGGER_SETTINGS_INI: the profile's settings from %APPDATA%\KOGGER\KoggerApp-<Profile>.ini
                                   # (a head unit's KoggerApp.conf put there: its layout and widgets)
    [int]    $BurstMs = 0,         # > 0: the GUI thread's busy stretches of at least this long, each with what it ran
                                   # (the sampler's timeline): a jerk the totals hide
    [int]    $LinkTrack = 0,       # > 0: the boat's track by link quality filled with this many samples first, as a long
                                   # session leaves it (the edit script's "linkTrack", by the plan's first point)
    [int]    $LinkTrackRun = 6,    # its quality changing every this many samples
    [string] $Action = '',         # an edit-script step (JSON) run instead of the drag, sampled for -DragSeconds
    [string] $Mark = '',           # with -Action: the text of the log line it starts with ("echoedit script: relayout start")
    [int]    $TimeoutS = 400,
    [string] $Toolchain = 'D:\Qt\Tools\llvm-mingw1706_64\bin',
    [string] $Out = "$env:TEMP\kogger_drag_profile"
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
New-Item -ItemType Directory -Force -Path $Out | Out-Null

$sampler = Join-Path $Out 'sampler.exe'
$syms = $null
if (-not $NoSampler) {
    $source = Join-Path $PSScriptRoot 'sampler\sampler.cpp'
    if (-not (Test-Path $sampler) -or (Get-Item $source).LastWriteTime -gt (Get-Item $sampler).LastWriteTime) {
        & (Join-Path $Toolchain 'x86_64-w64-mingw32-clang++.exe') -O2 -std=c++17 $source -o $sampler -ldbghelp -lwinmm
        if ($LASTEXITCODE -ne 0) { throw 'building the sampler failed' }
    }
    $syms = Join-Path $Out ('syms_' + (Get-FileHash -LiteralPath $Exe -Algorithm MD5).Hash.Substring(0, 12) + '.txt')
    if (-not (Test-Path $syms)) {
        & (Join-Path $Toolchain 'llvm-nm.exe') --defined-only -C $Exe | Set-Content -LiteralPath $syms -Encoding ASCII
        if ($LASTEXITCODE -ne 0) { throw 'listing the symbols failed' }
    }
}

$inv = [Globalization.CultureInfo]::InvariantCulture
$act = if ($Action) { $Action }
       elseif ($NoDrag) { '{"op": "wait", "ms": ' + [int]($DragSeconds * 1000) + '}' }
       else { ('{{"op": "drag3d", "seconds": {0}, "hz": {1}, "radius": {2}, "turnSeconds": {3}}}' -f $DragSeconds.ToString($inv), $MoveHz, $Radius, $TurnSeconds.ToString($inv)) }
$steps = @('{"op": "settle", "quietMs": 3000, "timeoutMs": 180000}')
if ($LinkTrack -gt 0) { $steps += @(('{{"op": "linkTrack", "points": {0}, "run": {1}}}' -f $LinkTrack, $LinkTrackRun), '{"op": "wait", "ms": 2000}') }
$steps += @($act, '{"op": "wait", "ms": 1000}', '{"op": "quit"}')
$scriptPath = Join-Path $Out 'steps.json'
('{"steps": [' + ($steps -join ",`n") + ']}') | Set-Content -LiteralPath $scriptPath -Encoding ASCII

$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
$before = if (Test-Path $log) { @(Get-Content -LiteralPath $log -Encoding UTF8).Count } else { 0 }
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
$psi.Arguments = '"' + $File + '"'
if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
$psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
$psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $scriptPath
if ($SettingsIni) { $psi.EnvironmentVariables['KOGGER_SETTINGS_INI'] = '1' }
$app = Start-TestDesktopProcess $psi

# The sampler goes as the drag (or the wait in its place) begins: the settle step's line comes just before.
$mark = if ($Action) { $(if ($Mark) { $Mark } else { 'echoedit script: ' }) }
        elseif ($NoDrag) { 'echoedit script: settle,' } else { 'echoedit script: drag3d start' }
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$started = $false
while (-not $app.HasExited -and $clock.Elapsed.TotalSeconds -lt $TimeoutS) {
    $now = if (Test-Path -LiteralPath $log) { @(Get-Content -LiteralPath $log -Encoding UTF8) } else { @() }   # a new profile has none yet
    if ($now.Count -lt $before) { $before = 0 }   # a new file
    if (@($now[$before..($now.Count - 1)] | Select-String -SimpleMatch $mark).Count -gt 0) { $started = $true; break }
    Start-Sleep -Milliseconds 100
}
if (-not $started) { if (-not $app.HasExited) { $app.Kill() }; throw "no '$mark' in the log" }
$report = Join-Path $Out 'report.txt'
$sp = $null
if (-not $NoSampler) {
    $spi = New-Object System.Diagnostics.ProcessStartInfo
    $spi.FileName = $sampler; $spi.UseShellExecute = $false
    $timeline = Join-Path $Out 'timeline.txt'
    $spi.Arguments = "$($app.Id) $DragSeconds $IntervalMs `"$syms`" KoggerApp.exe $(if ($Focus) { $Focus } else { '-' })" +
                     $(if ($BurstMs -gt 0) { " `"$timeline`"" } else { '' })
    $sp = Start-TestDesktopProcess $spi -StdOut $report -StdErr (Join-Path $Out 'sampler.err')
}
if (-not $app.WaitForExit([int](($TimeoutS - $clock.Elapsed.TotalSeconds) * 1000))) { $app.Kill(); throw 'the app did not quit' }
if ($sp -and -not $sp.WaitForExit(60000)) { $sp.Kill(); throw 'the sampler did not end' }

$lines = Get-Content -LiteralPath $log -Encoding UTF8
$start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
$mine = $lines[($start - 1)..($lines.Count - 1)]
"--- the step"
$mine | Select-String -Pattern 'echoedit script: (drag3d|settle|wait done|linkTrack|relayout)' | ForEach-Object { $_.Line -replace '^.*echoedit script: ', '' }
"--- health lines of the run"
$mine | Select-String -Pattern 'health:' | ForEach-Object { $_.Line -replace '^.*health: ', '' }
if ($sp -and $BurstMs -gt 0 -and (Test-Path -LiteralPath $timeline)) {
    # The GUI thread (its stacks hold qMain): samples come only while it runs, so a stretch of samples ~IntervalMs
    # apart is one busy stretch. For each of at least -BurstMs: when, how long, and the functions most on its stacks.
    $idle = 'NtUserMsgWaitForMultipleObjectsEx'
    $skip = '^(ntdll!|KERNEL32!|KERNELBASE!|win32u!|USER32!|main$|qMain|__tmainCRTStartup|\.l_startw|Qt6Core!ZN16QCoreApplication4exec|Qt6Core!ZN10QEventLoop4exec|Qt6Core!ZN21QEventDispatcherWin3213processEvents|Qt6Gui!ZN26QWindowsGuiEventDispatcher13processEvents)'
    $recs = foreach ($l in [System.IO.File]::ReadLines($timeline)) {
        $p = $l.Split(' ', 3)
        if ($p.Count -eq 3) { [pscustomobject]@{ Tid = $p[0]; Ms = [double]::Parse($p[1], $inv); Fs = $p[2].Split(';') } }
    }
    $gui = ($recs | Where-Object { $_.Fs -match 'qMain' } | Group-Object Tid | Sort-Object Count -Descending | Select-Object -First 1).Name
    $mine = @($recs | Where-Object { $_.Tid -eq $gui -and $_.Fs[0] -notmatch $idle } | Sort-Object Ms)
    "--- the GUI thread ($gui): busy stretches of $BurstMs ms or more ($($mine.Count) busy samples)"
    $bursts = New-Object System.Collections.Generic.List[object]
    $cur = $null
    foreach ($r in $mine) {
        if ($cur -and $r.Ms - $cur.Last -le 3 * $IntervalMs) { $cur.Last = $r.Ms; $cur.Recs.Add($r) }
        else { $cur = [pscustomobject]@{ First = $r.Ms; Last = $r.Ms; Recs = (New-Object System.Collections.Generic.List[object]) }; $cur.Recs.Add($r); $bursts.Add($cur) }
    }
    foreach ($b in $bursts) {
        $len = $b.Last - $b.First + $IntervalMs
        if ($len -lt $BurstMs) { continue }
        $count = @{}
        foreach ($r in $b.Recs) { foreach ($f in ($r.Fs | Select-Object -Unique)) { if ($f -notmatch $skip) { $count[$f] = 1 + [int]$count[$f] } } }
        "{0,8:N0} ms: {1,4:N0} ms, {2} samples" -f $b.First, $len, $b.Recs.Count
        $count.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 14 |
            ForEach-Object { "      {0,3}  {1}" -f $_.Value, $(if ($_.Key.Length -gt 150) { $_.Key.Substring(0, 150) } else { $_.Key }) }
    }
}
if ($sp) {
    "--- sampler report: $report"
    Get-Content -LiteralPath $report
}

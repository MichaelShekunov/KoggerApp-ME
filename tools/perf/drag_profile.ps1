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
    samples the same stretch without a drag (a "wait"), for a baseline.

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
$act = if ($NoDrag) { '{"op": "wait", "ms": ' + [int]($DragSeconds * 1000) + '}' }
       else { ('{{"op": "drag3d", "seconds": {0}, "hz": {1}, "radius": {2}, "turnSeconds": {3}}}' -f $DragSeconds.ToString($inv), $MoveHz, $Radius, $TurnSeconds.ToString($inv)) }
$steps = @('{"op": "settle", "quietMs": 3000, "timeoutMs": 180000}', $act, '{"op": "wait", "ms": 1000}', '{"op": "quit"}')
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
$app = Start-TestDesktopProcess $psi

# The sampler goes as the drag (or the wait in its place) begins: the settle step's line comes just before.
$mark = if ($NoDrag) { 'echoedit script: settle,' } else { 'echoedit script: drag3d start' }
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$started = $false
while (-not $app.HasExited -and $clock.Elapsed.TotalSeconds -lt $TimeoutS) {
    $now = @(Get-Content -LiteralPath $log -Encoding UTF8)
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
    $spi.Arguments = "$($app.Id) $DragSeconds $IntervalMs `"$syms`" KoggerApp.exe $Focus"
    $sp = Start-TestDesktopProcess $spi -StdOut $report -StdErr (Join-Path $Out 'sampler.err')
}
if (-not $app.WaitForExit([int](($TimeoutS - $clock.Elapsed.TotalSeconds) * 1000))) { $app.Kill(); throw 'the app did not quit' }
if ($sp -and -not $sp.WaitForExit(60000)) { $sp.Kill(); throw 'the sampler did not end' }

$lines = Get-Content -LiteralPath $log -Encoding UTF8
$start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
$mine = $lines[($start - 1)..($lines.Count - 1)]
"--- the step"
$mine | Select-String -Pattern 'echoedit script: (drag3d|settle|wait done)' | ForEach-Object { $_.Line -replace '^.*echoedit script: ', '' }
"--- health lines of the run"
$mine | Select-String -Pattern 'health:' | ForEach-Object { $_.Line -replace '^.*health: ', '' }
if ($sp) {
    "--- sampler report: $report"
    Get-Content -LiteralPath $report
}

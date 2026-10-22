#requires -Version 5.1
<#
.SYNOPSIS
    An echogram's bottom-track line (and the rangefinder's) as the user left it, after a restart of the app.

.DESCRIPTION
    The owner, 2026-10-22: the bottom-track line came back after every restart. The panel's "Visibility" was read
    back from the plot's visibility, which is on while the depth value is on (the default). Six runs of KoggerApp
    over one recording, in one test profile (KOGGER_PROFILE, reset from the transfer folder's KoggerApp-perf.reg
    before the first run only, so each run starts where the last one left off), driven by the edit script's
    "plotState" step (it changes the panel state as the panel's controls do and logs it):
      off      line off (bottom track and rangefinder), quit  ->  next run: both lines still off, the plot's
               bottom-track theme 0
      points   line on as points, quit                        ->  next run: on, the type Points, the plot's theme 2
      hidden   line off again, quit                           ->  next run: off, and the type still Points
    Exit code 0 when every check holds.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\qml_check\echostate_persist_check.ps1 -Exe <build>\KoggerApp.exe -File D:\Temp\klf_check\00004_2x4.KLF -PathPrefix "D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [Parameter(Mandatory = $true)] [string] $File,
    [string] $PathPrefix,
    [string] $Profile = 'perf',
    [string] $PerfReg = 'D:\YandexDisk\DelphiProjects\KoggerApp_myEdition_transfer\KoggerApp-perf.reg',
    [string] $Out = "$env:TEMP\kogger_echostate_persist",
    [int]    $TimeoutS = 300
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
if (Test-Path -LiteralPath $PerfReg) {
    # reg.exe reports success on stderr; under 'Stop' PowerShell 5.1 would take that for a failure.
    cmd.exe /c "reg import `"$PerfReg`" >nul 2>&1"
}

function Invoke-Run([string] $name, [string[]] $steps) {
    $scriptPath = Join-Path $Out "$name.json"
    $all = @('{"op": "settle", "quietMs": 2000, "timeoutMs": 180000}') + $steps + @('{"op": "plotState"}', '{"op": "quit"}')
    ('{"steps": [' + ($all -join ",`n") + ']}') | Set-Content -LiteralPath $scriptPath -Encoding ASCII
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
    $psi.Arguments = '"' + $File + '"'
    if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
    $psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
    $psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $scriptPath
    $app = [System.Diagnostics.Process]::Start($psi)
    if (-not $app.WaitForExit($TimeoutS * 1000)) { $app.Kill(); throw "run '$name': the app did not end in $TimeoutS s" }
    $lines = Get-Content -LiteralPath $log -Encoding UTF8
    $start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
    $mine = @($lines[($start - 1)..($lines.Count - 1)])
    $mine | Set-Content -LiteralPath (Join-Path $Out "$name.log") -Encoding UTF8
    # The last plotState line of each echogram on screen.
    $re = 'echoedit script: plotState plot (\d+): bottomTrackLine (\w+) bottomTrackValue (\w+) bottomTrackTheme (\d+) rangefinderLine (\w+); plot: bottom track visible (\w+) theme (-?\d+), rangefinder theme (-?\d+)'
    $state = @{}
    foreach ($l in $mine) {
        if ($l -match $re) {
            $state[[int]$Matches[1]] = [pscustomobject]@{
                # QString::arg(bool) writes 1 / 0
                Line = $Matches[2] -in '1', 'true'; Value = $Matches[3] -in '1', 'true'; Type = [int]$Matches[4]
                RfLine = $Matches[5] -in '1', 'true'; PlotVisible = $Matches[6] -in '1', 'true'; PlotTheme = [int]$Matches[7]; RfTheme = [int]$Matches[8] }
        }
    }
    if ($state.Count -eq 0) { throw "run '$name': no echogram on screen logged its state (log: $(Join-Path $Out "$name.log"))" }
    return $state
}

$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}
function Show($st) { ($st.Keys | Sort-Object | ForEach-Object { $s = $st[$_]; "plot $_ line $($s.Line) type $($s.Type) plot theme $($s.PlotTheme), rangefinder line $($s.RfLine) theme $($s.RfTheme)" }) -join '; ' }

$null = Invoke-Run 'off' @('{"op": "plotState", "set": {"bottomTrackLine": false, "rangefinderLine": false}}', '{"op": "wait", "ms": 1500}')
$st = Invoke-Run 'after-off' @()
$ok = $true; foreach ($s in $st.Values) { $ok = $ok -and -not $s.Line -and $s.PlotTheme -eq 0 -and -not $s.RfLine -and $s.RfTheme -eq 0 }
Expect $ok 'off' (Show $st)

$null = Invoke-Run 'points' @('{"op": "plotState", "set": {"bottomTrackTheme": 1, "bottomTrackLine": true}}', '{"op": "wait", "ms": 1500}')
$st = Invoke-Run 'after-points' @()
$ok = $true; foreach ($s in $st.Values) { $ok = $ok -and $s.Line -and $s.Type -eq 1 -and $s.PlotTheme -eq 2 }
Expect $ok 'points' (Show $st)

$null = Invoke-Run 'hidden' @('{"op": "plotState", "set": {"bottomTrackLine": false}}', '{"op": "wait", "ms": 1500}')
$st = Invoke-Run 'after-hidden' @()
$ok = $true; foreach ($s in $st.Values) { $ok = $ok -and -not $s.Line -and $s.PlotTheme -eq 0 -and $s.Type -eq 1 }
Expect $ok 'hidden' (Show $st)

"echostate persist: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
exit ([int]($failed -gt 0))

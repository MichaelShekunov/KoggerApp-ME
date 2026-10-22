#requires -Version 5.1
<#
.SYNOPSIS
    The app started through test_desktop.ps1 works as usual and stays off the desktop of whoever is at the machine.

.DESCRIPTION
    One run of KoggerApp over a recording on the test desktop, in a test profile, driven by an edit script
    (settle, a grab of its window, the echograms' plotState, quit). Every 50 ms while it runs, from this
    script's desktop: whose the foreground window is, and how many visible windows of the app are here.
    Holds when
      - the foreground never belonged to the app, and no window of it was ever visible here,
      - the app had its window on the test desktop,
      - it ended by itself with exit code 0, its grab is a picture (not one flat colour), and the echograms
        logged their state.
    The detector itself is checked first: it must see the visible windows of the shell (explorer) here.
    Exit code 0 when everything holds.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\common\test_desktop_check.ps1 -Exe <build>\KoggerApp.exe -File D:\Temp\klf_check\00004_2x4.KLF -PathPrefix "D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [Parameter(Mandatory = $true)] [string] $File,
    [string] $PathPrefix,
    [string] $Profile = 'perf',
    [string] $Out = "$env:TEMP\kogger_test_desktop_check",
    [int]    $TimeoutS = 300
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test_desktop.ps1')
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
$png = Join-Path $Out 'grab.png'
Remove-Item -LiteralPath $png -ErrorAction SilentlyContinue

$failed = 0
function Expect([bool] $ok, [string] $what, [string] $detail) {
    if (-not $ok) { $script:failed++ }
    "{0} {1}: {2}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $what, $detail
}

$shell = @(Get-Process -Name explorer -ErrorAction SilentlyContinue)
$seen = 0; foreach ($s in $shell) { $seen += [KoggerTestDesktop]::VisibleWindowsHere($s.Id) }
Expect ($seen -gt 0) 'the detector sees windows on this desktop' "$seen visible window(s) of explorer"

$scriptPath = Join-Path $Out 'steps.json'
$steps = @('{"op": "settle", "quietMs": 2000, "timeoutMs": 180000}',
           ('{"op": "grab", "path": "' + $png.Replace('\', '/') + '"}'),
           '{"op": "plotState"}', '{"op": "quit"}')
('{"steps": [' + ($steps -join ",`n") + ']}') | Set-Content -LiteralPath $scriptPath -Encoding ASCII

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
$psi.Arguments = '"' + $File + '"'
if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
$psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
$psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $scriptPath
$psi.EnvironmentVariables['KOGGER_UI_PROBE'] = $Out              # gives the probe its window: "grab" needs it

$app = Start-TestDesktopProcess $psi
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$samples = 0; $foregroundTaken = 0; $shownHere = 0; $onTestDesktop = $false
while (-not $app.HasExited -and $clock.Elapsed.TotalSeconds -lt $TimeoutS) {
    ++$samples
    if ([KoggerTestDesktop]::ForegroundPid() -eq $app.Id) { ++$foregroundTaken }
    if ([KoggerTestDesktop]::VisibleWindowsHere($app.Id) -gt 0) { ++$shownHere }
    if (-not $onTestDesktop) { $onTestDesktop = [KoggerTestDesktop]::MainWindow($app.Id) -ne [System.IntPtr]::Zero }
    Start-Sleep -Milliseconds 50
}
if (-not $app.HasExited) { $app.Kill(); throw "the app did not end in $TimeoutS s" }
$app.WaitForExit()

Expect ($foregroundTaken -eq 0 -and $shownHere -eq 0) 'nothing on this desktop' `
    ("{0} samples over {1:N1} s: the foreground the app's in {2}, a window of it visible here in {3}" -f $samples, $clock.Elapsed.TotalSeconds, $foregroundTaken, $shownHere)
Expect $onTestDesktop 'its window on the test desktop' "$onTestDesktop"
Expect ($app.ExitCode -eq 0) 'exit code' "$($app.ExitCode)"

$picture = $false; $detail = 'no grab'
if (Test-Path -LiteralPath $png) {
    Add-Type -AssemblyName System.Drawing
    $bmp = [System.Drawing.Bitmap]::FromFile($png)
    try {
        $colours = @{}
        for ($y = 0; $y -lt $bmp.Height; $y += 8) { for ($x = 0; $x -lt $bmp.Width; $x += 8) { $colours[$bmp.GetPixel($x, $y).ToArgb()] = 1 } }
        $picture = $colours.Count -gt 50
        $detail = "{0}x{1}, {2} colours on an 8-px grid" -f $bmp.Width, $bmp.Height, $colours.Count
    } finally { $bmp.Dispose() }
}
Expect $picture 'the grab' $detail

$lines = Get-Content -LiteralPath $log -Encoding UTF8
$start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
$state = @($lines[($start - 1)..($lines.Count - 1)] | Select-String -Pattern 'echoedit script: plotState plot')
Expect ($state.Count -gt 0) 'the echograms logged their state' "$($state.Count) line(s)"

"test desktop: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
exit ([int]($failed -gt 0))

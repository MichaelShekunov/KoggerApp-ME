#requires -Version 5.1
<#
.SYNOPSIS
    The echogram's horizontal scale in the running app: test spikes put into the bottom track at known pings
    must be drawn in the columns plot2D_columns.h gives them at x1, x4, 1/4 and 1/16.

.DESCRIPTION
    Three runs of KoggerApp over a recording (KOGGER_PROFILE, so the user's settings are not touched), each
    driven by the edit script (KOGGER_ECHOEDIT_SCRIPT: spike, hscale, view, grab, quit):
      spikes   the first channel's bottom 1.6 times deeper at pings P..P+3 and at P+15
      control  no spike: what the recording's own bottom line looks like there
      four     only P..P+3
    At each scale the view puts ping P in canvas column 360 (x1: the right edge at P+359, x4: P+89, 1/4: the
    group of P in column 360, 1/16 the same). In each grab the columns where the first channel's line (green,
    the upper half of the echogram) jumps up are found; the spikes' columns are those of a run and not of the
    control. Expected (the line's slope widens a jump by up to two columns each side):
      x1    P..P+3 in 360..363, P+15 in 375
      x4    P..P+3 in 360..375, P+15 in 420..423
      1/4   P+3 (the newest of P's group) in 360, P+15 in 363
      1/16  P+15 (the newest of the group P..P+15) in 360; with P..P+3 alone nothing: a compressed column
            shows the newest ping of its group, and a target shorter than the group can drop out
    P must be a multiple of 16, the recording longer than P + 5800 pings and -Pings its ping count (checked
    against the app's log). The echogram must take the
    right half of the window (-CanvasX0 otherwise) and the first channel's line be drawn as a line.
    Exit code 0 when every expectation holds.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\echoscale_check\live_check.ps1 -Exe <build>\KoggerApp.exe -File D:\Temp\perf\00014_03.05.2025.KLF -PathPrefix "D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [Parameter(Mandatory = $true)] [string] $File,
    [string] $Profile = 'perf',
    [string] $PathPrefix,
    [int]    $Ping = 50000,
    [int]    $Pings = 57079,               # the recording's ping count: 57079 for 00014_03.05.2025.KLF
    [int]    $CanvasX0 = -1,
    [string] $Out = "$env:TEMP\kogger_echoscale_live",
    [int]    $TimeoutS = 400
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
if ($Ping % 16 -ne 0) { throw "-Ping must be a multiple of 16 (the groups of 1/16 start there)" }
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"

# The views as heads (Plot2D: the timeline position times the ping count; the newest ping shown is head - 2),
# P in column 360 at each scale. The script's "view" takes an epoch e and sets the position e / (n - 1), which
# the plot turns into round(position * n) in float: the epoch is the one that gives the head wanted.
function Epoch-For([long] $head) {
    for ($e = $head - 2; $e -le $head + 1; $e++) {
        $pos = [float]([double]$e / [double]($Pings - 1))
        $h = [Math]::Round([double][float]([double]$pos * [double][float]$Pings), [MidpointRounding]::AwayFromZero)
        if ($h -eq $head) { return $e }
    }
    throw "no epoch gives the head $head over $Pings pings"
}
$views = [ordered]@{
    x1  = @{ Scale = 1;      Epoch = (Epoch-For ($Ping + 361)) }
    x4  = @{ Scale = 4;      Epoch = (Epoch-For ($Ping + 91)) }
    q4  = @{ Scale = 0.25;   Epoch = (Epoch-For ($Ping + 1441)) }
    q16 = @{ Scale = 0.0625; Epoch = (Epoch-For ($Ping + 5761)) }
}

function Run-App([string] $tag, [object[]] $spikes) {
    $dir = Join-Path $Out $tag
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $steps = New-Object System.Collections.Generic.List[string]
    $steps.Add('{"op": "settle", "quietMs": 3000, "timeoutMs": 180000}')
    foreach ($s in $spikes) { $steps.Add(('{{"op": "spike", "from": {0}, "width": {1}, "factor": 1.6}}' -f $s[0], $s[1])) }
    foreach ($name in $views.Keys) {
        $v = $views[$name]
        $png = (Join-Path $dir "$name.png") -replace '\\', '/'
        $steps.Add(('{{"op": "hscale", "scale": {0}}}' -f $v.Scale.ToString([Globalization.CultureInfo]::InvariantCulture)))
        $steps.Add(('{{"op": "view", "epoch": {0}}}' -f $v.Epoch))
        $steps.Add('{"op": "wait", "ms": 1500}')
        $steps.Add(('{{"op": "grab", "path": "{0}"}}' -f $png))
    }
    $steps.Add('{"op": "hscale", "scale": 1}')
    $steps.Add('{"op": "quit"}')
    $scriptPath = Join-Path $dir 'script.json'
    ('{"steps": [' + ($steps -join ",`n") + ']}') | Set-Content -LiteralPath $scriptPath -Encoding ASCII

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
    $psi.Arguments = '"' + $File + '"'
    if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
    $psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
    $psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $scriptPath
    $psi.EnvironmentVariables['KOGGER_UI_PROBE'] = $dir            # gives the probe its window: "grab" needs it
    $app = Start-TestDesktopProcess $psi
    if (-not $app.WaitForExit($TimeoutS * 1000)) { $app.Kill(); throw "run $tag did not end in $TimeoutS s" }
    $lines = Get-Content -LiteralPath $log -Encoding UTF8
    $start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
    $mine = $lines[($start - 1)..($lines.Count - 1)]
    $hs = @($mine | Where-Object { $_ -match 'echoedit script: hscale [0-9.]+ on \d+ echogram\(s\), \d+ visible; \d+ pings' })
    if ($hs.Count -eq 0 -or ($hs[0] -match ', 0 visible')) { throw "run ${tag}: no visible echogram took the scale (log: $log)" }
    $n = [int]([regex]::Match($hs[0], '; (\d+) pings').Groups[1].Value)
    if ($n -ne $Pings) { throw "the recording has $n pings, not ${Pings}: run again with -Pings $n" }
    foreach ($name in $views.Keys) {
        if (-not (Test-Path (Join-Path $dir "$name.png"))) { throw "run ${tag}: no grab $name.png" }
    }
    Write-Host ("run {0}: exit {1}" -f $tag, $app.ExitCode)
    return $dir
}

# The columns where the first channel's line jumps up: its topmost green pixel well above the median of
# the 81 columns around.
function Jump-Columns([string] $png) {
    $bmp = New-Object System.Drawing.Bitmap $png
    try {
        $x0 = if ($CanvasX0 -ge 0) { $CanvasX0 } else { [int]($bmp.Width / 2) }
        $w = $bmp.Width - $x0
        $h = [int]($bmp.Height / 2)
        $rect = New-Object System.Drawing.Rectangle 0, 0, $bmp.Width, $bmp.Height
        $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $bytes = New-Object byte[] ($data.Stride * $bmp.Height)
        [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
        $bmp.UnlockBits($data)
        $ys = New-Object int[] $w
        for ($c = 0; $c -lt $w; $c++) {
            $ys[$c] = -1
            for ($y = 0; $y -lt $h; $y++) {
                $i = $y * $data.Stride + ($x0 + $c) * 4
                if ($bytes[$i + 1] -gt 180 -and $bytes[$i + 2] -lt 140 -and $bytes[$i] -lt 140) { $ys[$c] = $y; break }
            }
        }
    } finally { $bmp.Dispose() }
    $jumps = New-Object System.Collections.Generic.List[int]
    for ($c = 0; $c -lt $w; $c++) {
        if ($ys[$c] -lt 0) { continue }
        $around = New-Object System.Collections.Generic.List[int]
        for ($k = [Math]::Max(0, $c - 40); $k -le [Math]::Min($w - 1, $c + 40); $k++) { if ($ys[$k] -ge 0) { $around.Add($ys[$k]) } }
        $around.Sort()
        if ($ys[$c] -lt $around[[int]($around.Count / 2)] - 15) { $jumps.Add($c) }
    }
    return ,$jumps.ToArray()
}

$runs = [ordered]@{
    spikes  = Run-App 'spikes'  @(@($Ping, 4), @(($Ping + 15), 1))
    control = Run-App 'control' @()
    four    = Run-App 'four'    @(,@($Ping, 4))   # the comma keeps one pair a pair
}
$cols = @{}
foreach ($r in $runs.Keys) { foreach ($v in $views.Keys) { $cols["$r/$v"] = Jump-Columns (Join-Path $runs[$r] "$v.png") } }

$failed = 0
# A run's own jumps (not the control's) must cover `must` and stay inside `within`.
function Expect([string] $run, [string] $view, [int[]] $must, [int[]] $within, [string] $what) {
    $control = $cols["control/$view"]
    $own = @($cols["$run/$view"] | Where-Object { $control -notcontains $_ })
    $missing = @($must | Where-Object { $own -notcontains $_ })
    $stray = @($own | Where-Object { $within -notcontains $_ })
    $ok = $missing.Count -eq 0 -and $stray.Count -eq 0
    if (-not $ok) { $script:failed++ }
    "{0} {1,-7} {2,-4} {3}: columns {4}{5}{6}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $run, $view, $what,
        ($(if ($own.Count) { ($own -join ',') } else { 'none' })),
        ($(if ($missing.Count) { '; missing ' + ($missing -join ',') } else { '' })),
        ($(if ($stray.Count) { '; outside ' + ($stray -join ',') } else { '' }))
}

Expect 'spikes' 'x1'  (@(360..363) + 375)         (@(358..365) + @(373..377)) 'P..P+3 in 360..363, P+15 in 375'
Expect 'spikes' 'x4'  (@(360..375) + @(420..423)) (@(358..377) + @(418..425)) 'P..P+3 in 360..375, P+15 in 420..423'
Expect 'spikes' 'q4'  @(360, 363)                 @(358..365)                 'P+3 in 360, P+15 in 363'
Expect 'spikes' 'q16' @(360)                      @(358..362)                 'P+15 in 360'
Expect 'four'   'q4'  @(360)                      @(358..362)                 'P+3 in 360'
Expect 'four'   'q16' @()                         @()                         'none: P+15 is shown, not P..P+3'
"echoscale live: {0}" -f ($(if ($failed -eq 0) { 'PASS' } else { "$failed FAIL" }))
exit ([int]($failed -gt 0))

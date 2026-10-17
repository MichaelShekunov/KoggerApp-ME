#requires -Version 5.1
<#
.SYNOPSIS
    Opens a file twice, with the background zoom precompute and without it (KOGGER_MOSAIC_PRECOMPUTE=0),
    zooms with the mouse wheel, and compares the mosaic on screen after the zoom tile by tile.

.DESCRIPTION
    Each run is open_bench.ps1 with KOGGER_MOSAIC_DIGEST and KOGGER_MOSAIC_DUMP: once tiles stop coming
    the app logs a hash of the tiles on screen and dumps them. Printed per run: when the last tile of
    the new zoom came, from the zoom change ("mosaic zoom: A -> B" to the digest, less the digest's
    1.5 s wait), and the precompute's lines. Then mosaic_dump_compare.ps1 on the last dump of each.

    PASS: the same tiles, the same heights and marks, and at most -MaxPixels pixels painted with
    another value, none more than -MaxApart apart -- a judgement, not an exact match: on the test file
    of 2026-10-17 seven pixels of 6.1 million came out 1 to 3 apart, in three tiles at the edge of the
    survey, the cause not found (docs/HANDOFF.md). FAIL otherwise, or when a run has no zoom or digest.

    The window grabs differ between two runs alike too (the camera after the wheel), so they are not
    compared. A run takes about -Seconds plus the start; keep the mouse off its window.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\perf\precompute_check.ps1 -Exe <build>\KoggerApp.exe -File D:\Temp\perf\a.KLF -PathPrefix 'D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [Parameter(Mandatory = $true)] [string] $File,
    [string] $PathPrefix,
    [string] $Profile = 'perf',
    [double] $Seconds = 34,
    [double] $WheelAt = 20,
    [int]    $Wheel = 6,
    [string] $Out = "$env:TEMP\kogger_precompute_check",
    [int]    $MaxPixels = 64,
    [int]    $MaxApart = 8
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$bench = Join-Path $here 'open_bench.ps1'
$compare = Join-Path $here 'mosaic_dump_compare.ps1'
New-Item -ItemType Directory -Force -Path $Out | Out-Null

$keep = @{}
foreach ($name in 'KOGGER_MOSAIC_DIGEST', 'KOGGER_MOSAIC_PRECOMPUTE', 'KOGGER_MOSAIC_DUMP') {
    $keep[$name] = [Environment]::GetEnvironmentVariable($name)
}
$dumps = @{}
$failed = $false
try {
    foreach ($mode in 'on', 'off') {
        $dir = Join-Path $Out $mode
        if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
        $env:KOGGER_MOSAIC_DIGEST = '1'
        $env:KOGGER_MOSAIC_PRECOMPUTE = $(if ($mode -eq 'on') { '1' } else { '0' })
        $env:KOGGER_MOSAIC_DUMP = $dir
        $benchArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $bench, '-Exe', $Exe, '-File', $File, '-Profile', $Profile,
                  '-Seconds', $Seconds, '-WheelAt', $WheelAt, '-Wheel', $Wheel, '-Out', (Join-Path $Out "grab_$mode"),
                  '-Pattern', 'mosaic zoom|mosaic digest|mosaic precompute')
        if ($PathPrefix) { $benchArgs += @('-PathPrefix', $PathPrefix) }
        $lines = & powershell @benchArgs 2>&1 | ForEach-Object { "$_" }
        "== precompute $mode"
        $zoomAt = $null; $digestAt = $null; $zoomLine = ''
        foreach ($l in $lines) {
            if ($l -notmatch '^\s*([\d.,]+) s  (.*)$') { continue }
            $t = [double]::Parse($Matches[1], [Globalization.CultureInfo]::CurrentCulture)
            $text = $Matches[2]
            if ($text -match 'mosaic precompute') { "   $text" }
            if ($text -match 'mosaic zoom: (\d+) -> (\d+)') { $zoomAt = $t; $digestAt = $null; $zoomLine = "$($Matches[1]) -> $($Matches[2])" }
            elseif ($null -ne $zoomAt -and $null -eq $digestAt -and $text -match 'mosaic digest: (.*)$') { $digestAt = $t; $digest = $Matches[1] }
        }
        if ($null -eq $zoomAt -or $null -eq $digestAt) {
            "   no zoom change or no digest after it: the wheel did not reach the 3D view?"
            $failed = $true
            continue
        }
        "   zoom $zoomLine, the last tile {0:N2} s after it; digest $digest" -f ($digestAt - $zoomAt - 1.5)
        $last = Get-ChildItem -Path $dir -Filter 'digest_*.bin' | Sort-Object { [int]($_.BaseName -replace '\D', '') } | Select-Object -Last 1
        if (-not $last) { "   no dump in $dir"; $failed = $true; continue }
        $dumps[$mode] = $last.FullName
    }
}
finally {
    foreach ($name in $keep.Keys) { [Environment]::SetEnvironmentVariable($name, $keep[$name]) }
}

if ($failed -or $dumps.Count -ne 2) { 'FAIL: a run gave nothing to compare'; exit 1 }
"== tiles, with the precompute (A) and without (B): $($dumps['on']) vs $($dumps['off'])"
$res = & powershell -NoProfile -ExecutionPolicy Bypass -File $compare -A $dumps['on'] -B $dumps['off'] -Picture (Join-Path $Out 'diff.png') 2>&1 | ForEach-Object { "$_" }
$res
$sum = $res | Select-Object -First 1
if ($sum -notmatch '(\d+) only in A, (\d+) only in B; of \d+ pixels: painted only in A (\d+), only in B (\d+), another value (\d+) \(at most (\d+) apart\); heights (\d+), marks (\d+)') {
    'FAIL: no summary from mosaic_dump_compare.ps1'; exit 1
}
$m = $Matches
$ok = ([int]$m[1] -eq 0) -and ([int]$m[2] -eq 0) -and ([int]$m[3] -eq 0) -and ([int]$m[4] -eq 0) -and ([int]$m[5] -le $MaxPixels) -and
      ([int]$m[6] -le $MaxApart) -and ([int]$m[7] -eq 0) -and ([int]$m[8] -eq 0)
if ($ok) { "PASS: the same tiles, heights and marks; {0} pixels of another value (at most {1}), at most {2} apart (at most {3})" -f $m[5], $MaxPixels, $m[6], $MaxApart; exit 0 }
"FAIL: beyond the tolerance (tiles or coverage or heights differ, or more than $MaxPixels pixels, or more than $MaxApart apart)"
exit 1

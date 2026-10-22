#requires -Version 5.1
<#
.SYNOPSIS
    End-to-end check of echogram editing on a recording: KoggerApp opens it up to three times, a script
    of edits (KOGGER_ECHOEDIT_SCRIPT, src/echogram_edit/echo_edit_script.h) works on it, and the dumps it
    leaves are judged here. docs/echogram-editing.md.

.DESCRIPTION
    Up to three windows of the app, each closing itself (a minute or two each on a 350 MB recording):

    Run 1. As read: the bottom and the mosaic ("base"); the pencil's magnet started 0.3 m below and above
    the bottom track's bottom; a CSV export. The start trimmed where the boat is in the water ("trim").
    A stretch marked bad, one drawn, one erased, one raised, one lowered ("all"); all undone ("undone");
    all redone ("redone"); a CSV export; the edits saved next to the recording; screenshots.
    Run 2. The recording again, its file of edits read ("reload"); every edit reset ("reset"); the reset
    undone ("unreset"); reset again and saved, which removes the file.
    Run 3, the filters. Spikes put into the bottom track's bottom (1, 2 and 3 pings, 1.6 times deeper),
    the start trimmed ("f-base"); a stretch smoothed with three sweeps of the brush ("f-smooth"); a stretch
    erased and its gap filled ("f-fill"); one side of a stretch left out of the mosaic ("f-side"); the
    spikes removed over the recording ("f-despike"); the turns left out of the mosaic ("f-turns"); the
    filters undone ("f-undone") and redone ("f-redone"); reset, which removes the file.

    Checked:
      base     nothing left out; the CSV has a row for every ping
      magnet   from 0.3 m below and above: lands within 10 cm of the bottom track for most pings
      trim     the pings before the boat is in the water left out (the count, the CSV rows); the mosaic
               loses the pixels only they painted and gains none
      all      each edit where it was made: the marked range, the drawn depth, no bottom where erased,
               the bottom no deeper than the raise limit and no nearer than the lower one; the CSV rows
      undone   no edit left, the bottom of every ping as in base (bit for bit), the mosaic as in base
      redone   the edits and the bottom as in all (bit for bit), the mosaic as in all
      file     the file of edits written, and what it holds is what all held
      reload   the edits as in all; the bottom as in all except next to the limits (a run over a stretch
               starts with a shorter window than one over the recording)
      reset    no edit left; the bottom as in base except next to the edited stretches
      unreset  the edits and the bottom as in reload (bit for bit)
      cleanup  no file of edits left next to the recording
      f-smooth   only the brushed pings changed, each now a point set by hand within the depths around
                 it, the stretch less rough
      f-fill     the erased pings on the straight line between the bottoms around the gap, no other
      f-side     the side (the first channel's sub-channel) and its range kept in the edits, every depth
                 as it was; the mosaic loses pixels, gains none
      f-despike  the spikes put in given the bottom around them (within 15 cm or 3 %); the pings it
                 finds in the recording itself few (at most 1 % of those in use), counted
      f-turns    the turns found counted; their sides out of the mosaic, every depth as it was
      f-undone   the bottom and the mosaic as before the filters (bit for bit)
      f-redone   the bottom, the edits and the mosaic as after them
      f-cleanup  no file of edits left

    The recording is not written; a file of edits that was there before is kept aside and put back.
    Exit code 0 when every check passes.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\perf\echoedit_e2e.ps1 -Exe D:\Temp\ka_echoedit\build\Desktop_Qt_6_8_3_llvm_mingw_64_bit-Release\KoggerApp.exe -File D:\Temp\perf\00014_03.05.2025.KLF -TrimStart 2924 -PathPrefix D:\Qt\6.8.3\llvm-mingw_64\bin
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [Parameter(Mandatory = $true)] [string] $File,
    [Parameter(Mandatory = $true)] [int] $TrimStart,   # the first ping with the boat in the water
    [ValidateSet('all', 'edits', 'filters')] [string] $Runs = 'all',   # edits: runs 1 and 2; filters: run 3
    [string] $Profile = 'echoedit',
    [string] $Out = "$env:TEMP\kogger_echoedit_e2e",
    [string] $PathPrefix,
    [int] $WaitMs = 10000,           # after an edit, for the mosaic to be painted again
    [int] $TimeoutS = 600
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
$File = (Resolve-Path -LiteralPath $File).Path
$name = [System.IO.Path]::GetFileNameWithoutExtension($File)
$work = Join-Path $Out $name
if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
New-Item -ItemType Directory -Force -Path $work | Out-Null
$compare = Join-Path $PSScriptRoot 'mosaic_dump_compare.ps1'
$kedit = "$File.kedit"
$kept = "$File.kedit.e2e-kept"
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"

$script:pass = 0
$script:fail = 0
function Check([bool] $ok, [string] $what, [string] $detail = '') {
    if ($ok) { $script:pass++; Write-Host "PASS $what $(if ($detail) { "($detail)" })" }
    else { $script:fail++; Write-Host "FAIL $what $(if ($detail) { ": $detail" })" }
}
function J([string] $path) { return Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json }
function Bottom([string] $dump) {
    $b = [System.IO.File]::ReadAllBytes("$dump.bottom.bin")
    $f = New-Object float[] ($b.Length / 4)
    [System.Buffer]::BlockCopy($b, 0, $f, 0, $b.Length)
    return , $f
}
# Epochs where two bottoms differ (NaN equals NaN), as a list.
function Differ([float[]] $a, [float[]] $b) {
    $d = New-Object System.Collections.Generic.List[int]
    for ($i = 0; $i -lt [Math]::Min($a.Length, $b.Length); $i++) {
        $x = $a[$i]; $y = $b[$i]
        if (([float]::IsNaN($x) -and [float]::IsNaN($y)) -or $x -eq $y) { continue }
        $d.Add($i)
    }
    return , $d
}
function Median([float[]] $v, [int] $from, [int] $to) {
    $s = @(); for ($i = $from; $i -lt $to; $i++) { if (-not [float]::IsNaN($v[$i])) { $s += $v[$i] } }
    if ($s.Count -eq 0) { return [double]::NaN }
    $s = $s | Sort-Object; return [double]$s[[int]($s.Count / 2)]
}
function EditsText($dump) {   # what the file of edits holds, without the recording's name
    $e = $dump.edits
    return (@($e.trim | ConvertTo-Json -Compress), ($e.bad | ConvertTo-Json -Compress), ($e.bottom | ConvertTo-Json -Depth 5 -Compress),
              ($e.limitMin | ConvertTo-Json -Depth 5 -Compress), ($e.limitMax | ConvertTo-Json -Depth 5 -Compress),
              ($e.mosaicOut | ConvertTo-Json -Depth 5 -Compress)) -join '|'
}

function Run-App([string] $tag, [object[]] $steps) {
    $dir = Join-Path $work $tag
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $scriptPath = Join-Path $dir 'script.json'
    (@{ steps = $steps } | ConvertTo-Json -Depth 6) | Set-Content -LiteralPath $scriptPath -Encoding ASCII
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
    $psi.Arguments = '"' + $File + '"'
    if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
    $psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
    $psi.EnvironmentVariables['KOGGER_ECHOEDIT_SCRIPT'] = $scriptPath
    $psi.EnvironmentVariables['KOGGER_MOSAIC_DIGEST'] = '1'
    $psi.EnvironmentVariables['KOGGER_MOSAIC_DUMP'] = (Join-Path $dir 'mosaic')
    $psi.EnvironmentVariables['KOGGER_MOSAIC_PRECOMPUTE'] = '0'   # one painter: the dumps compare
    $psi.EnvironmentVariables['KOGGER_UI_PROBE'] = $dir            # gives the probe its window: "grab" needs it
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    $app = Start-TestDesktopProcess $psi
    if (-not $app.WaitForExit($TimeoutS * 1000)) {
        $app.Kill()
        throw "run $tag did not end in $TimeoutS s"
    }
    Write-Host ("run {0}: {1:N0} s, exit {2}" -f $tag, $clock.Elapsed.TotalSeconds, $app.ExitCode)
    # This run's lines of the log: from its last opening on.
    $lines = Get-Content -LiteralPath $log -Encoding UTF8
    $start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
    $mine = $lines[($start - 1)..($lines.Count - 1)]
    $mine | Set-Content -LiteralPath (Join-Path $dir 'kogger.log') -Encoding UTF8
    # Each dump with the mosaic digest written last before it (digest_<n>.bin, n counted in the log).
    $digests = 0
    $map = @{}
    foreach ($l in $mine) {
        if ($l -match 'mosaic digest:') { $digests++ }
        if ($l -match 'echoedit script: dump (\S+\.json):') { $map[[System.IO.Path]::GetFileNameWithoutExtension($Matches[1])] = $digests }
    }
    $mine | Where-Object { $_ -match 'echoedit script:|echogram edit|echogram edits' } | ForEach-Object { '    ' + ($_ -replace '^\S+ \S+ ', '') } | Write-Host
    return $map
}
function Mosaic([string] $tag, [hashtable] $map, [string] $dump) {
    $n = $map[$dump]
    if (-not $n) { return $null }
    return (Join-Path $work "$tag\mosaic\digest_$n.bin")
}
function CompareMosaic([string] $a, [string] $b, [switch] $Judge) {
    if (-not $a -or -not $b -or -not (Test-Path $a) -or -not (Test-Path $b)) { return $null }
    # A blank tile (no pixel, no height) shows nothing: a rebuild posts one that had only heights of the
    # pings left out (two on 00008), and that is no difference on screen.
    $cmpArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $compare, '-A', $a, '-B', $b, '-Quiet', '-IgnoreBlankTiles')
    if ($Judge) { $cmpArgs += '-Judge' }
    $text = & powershell @cmpArgs
    return ($text -join "`n")
}
# Mosaic B is mosaic A without some pings: it loses pixels, gains none (but a few), keeps the rest (but
# a few that those pings had painted over: at most -OtherMax of the painted pixels). Whether it does,
# and the numbers.
function Loses([string] $a, [string] $b, [double] $OtherMax = 0.01) {
    $c = CompareMosaic $a $b
    if (-not ($c -and $c -match 'tiles: (\d+) in both, (\d+) only in A, (\d+) only in B; of (\d+) pixels \((\d+) painted in B\): painted only in A (\d+), only in B (\d+), another value (\d+)')) {
        return @($false, "no digests ($a, $b): $c")
    }
    $tilesOnlyA = [double]$Matches[2]; $tilesOnlyB = [double]$Matches[3]
    $painted = [double]$Matches[5]; $onlyA = [double]$Matches[6]; $onlyB = [double]$Matches[7]; $other = [double]$Matches[8]
    $ok = ($onlyA -gt 0 -or $tilesOnlyA -gt 0) -and $tilesOnlyB -eq 0 -and $onlyB / [Math]::Max(1, $painted) -le 0.001 -and $other / [Math]::Max(1, $painted) -le $OtherMax
    return @($ok, ("pixels only before {0}, only after {1}, another value {2} of {3} painted; tiles only before {4}, only after {5}" -f $onlyA, $onlyB, $other, $painted, $tilesOnlyA, $tilesOnlyB))
}

# The recording's own file of edits, if any, aside for the time of the check.
# A file kept aside by a run that did not finish is the recording's own edits (or a test's): moved over, it
# would be lost. Stop and say so.
if (Test-Path -LiteralPath $kept) {
    throw "a run that did not finish left $kept -- the recording's own edits, or a test's; put it back as $kedit by hand (or remove it), then run again"
}
if (Test-Path -LiteralPath $kedit) { Move-Item -LiteralPath $kedit -Destination $kept; Write-Host "kept aside: $kedit" }
try {
    $T = $TrimStart
    $wait = @{ op = 'wait'; ms = $WaitMs }
    $settle = @{ op = 'settle'; quietMs = 3000 }
    $shots = @()

    if ($Runs -ne 'filters') {
        $r1 = Join-Path $work 'r1'; $r2 = Join-Path $work 'r2'
        $steps1 = @(
            $settle, $wait,
            @{ op = 'magnetProbe'; name = 'below'; from = $T + 500; toFrac = 0.95; offset = 0.3; radius = 0.5 },
            @{ op = 'magnetProbe'; name = 'above'; from = $T + 500; toFrac = 0.95; offset = -0.3; radius = 0.5 },
            @{ op = 'exportCsv'; dir = "$r1\csv_base" },
            @{ op = 'dump'; path = "$r1\base.json" },
            @{ op = 'trimStart'; epoch = $T }, $settle, $wait,
            @{ op = 'exportCsv'; dir = "$r1\csv_trim" },
            @{ op = 'dump'; path = "$r1\trim.json" },
            @{ op = 'view'; epoch = $T + 300; palette = $true; tool = 7 }, @{ op = 'wait'; ms = 1500 },
            @{ op = 'grab'; path = "$r1\trim.png" },
            @{ op = 'mark'; fromFrac = 0.40; toFrac = 0.42 },
            @{ op = 'draw'; fromFrac = 0.50; toFrac = 0.51; depthRel = 0.8 },
            @{ op = 'erase'; fromFrac = 0.60; toFrac = 0.605 },
            @{ op = 'raise'; fromFrac = 0.70; toFrac = 0.71; depthRel = 0.85 },
            @{ op = 'lower'; fromFrac = 0.80; toFrac = 0.81; depthRel = 1.15 },
            $settle, $wait,
            @{ op = 'exportCsv'; dir = "$r1\csv_all" },
            @{ op = 'dump'; path = "$r1\all.json" },
            @{ op = 'view'; epochFrac = 0.505; tool = 1 }, @{ op = 'wait'; ms = 1500 },
            @{ op = 'grab'; path = "$r1\draw.png" },
            @{ op = 'view'; epochFrac = 0.41; tool = 5 }, @{ op = 'wait'; ms = 1500 },
            @{ op = 'grab'; path = "$r1\mark.png" },
            @{ op = 'view'; epochFrac = 0.705; tool = 2 }, @{ op = 'wait'; ms = 1500 },
            @{ op = 'grab'; path = "$r1\raise.png" },
            @{ op = 'undoAll' }, $settle, $wait,
            @{ op = 'dump'; path = "$r1\undone.json" },
            @{ op = 'redoAll' }, $settle, $wait,
            @{ op = 'dump'; path = "$r1\redone.json" },
            @{ op = 'view'; palette = $false; tool = 0 },
            @{ op = 'flush' },
            @{ op = 'quit' }
        )
        $map1 = Run-App 'r1' $steps1
        $keditAfter1 = Test-Path -LiteralPath $kedit
        $keditText = if ($keditAfter1) { Get-Content -LiteralPath $kedit -Raw -Encoding UTF8 } else { '' }

        $steps2 = @(
            $settle, $wait,
            @{ op = 'dump'; path = "$r2\reload.json" },
            @{ op = 'view'; epoch = $T + 300; palette = $true; tool = 0 }, @{ op = 'wait'; ms = 1500 },
            @{ op = 'grab'; path = "$r2\reload.png" },
            @{ op = 'resetAll' }, $settle, $wait,
            @{ op = 'dump'; path = "$r2\reset.json" },
            @{ op = 'undo' }, $settle, $wait,
            @{ op = 'dump'; path = "$r2\unreset.json" },
            @{ op = 'resetAll' }, $settle,
            @{ op = 'flush' },
            @{ op = 'quit' }
        )
        $map2 = Run-App 'r2' $steps2
        $keditAfter2 = Test-Path -LiteralPath $kedit

        # ---- judging runs 1 and 2 ----
        $base = J "$r1\base.json"; $trim = J "$r1\trim.json"; $all = J "$r1\all.json"
        $undone = J "$r1\undone.json"; $redone = J "$r1\redone.json"
        $reload = J "$r2\reload.json"; $reset = J "$r2\reset.json"; $unreset = J "$r2\unreset.json"
        $n = [int]$base.epochs
        $w = [int]$base.windowSize
        $bBase = Bottom "$r1\base.json"; $bAll = Bottom "$r1\all.json"; $bUndone = Bottom "$r1\undone.json"
        $bRedone = Bottom "$r1\redone.json"; $bReload = Bottom "$r2\reload.json"; $bReset = Bottom "$r2\reset.json"
        $bUnreset = Bottom "$r2\unreset.json"
        # As the script runner rounds a share of the recording (std::lround): half away from zero, not to even.
        $at = { param($f) [int][Math]::Round($f * $n, [MidpointRounding]::AwayFromZero) }
        $m0 = & $at 0.40; $m1 = & $at 0.42; $d0 = & $at 0.50; $d1 = & $at 0.51; $e0 = & $at 0.60; $e1 = & $at 0.605
        $ra0 = & $at 0.70; $ra1 = & $at 0.71; $lo0 = & $at 0.80; $lo1 = & $at 0.81
        Write-Host ("{0}: {1} pings, bottom-track window {2}" -f $name, $n, $w)

        Check ($base.excluded -eq 0 -and $base.export.rows -eq $n) 'base: nothing left out, a CSV row for every ping' ("rows {0}" -f $base.export.rows)
        foreach ($p in 'below', 'above') {
            $pr = $base.probe.$p
            Check ($pr.considered -gt 1000 -and $pr.within10cm -ge 0.7 -and $pr.medianAbsM -le 0.08) "magnet from 0.3 m $p the bottom: lands on it" `
                ("{0} pings, {1:P0} within 10 cm, median {2:N3} m, 90% {3:N3} m, {4} stayed" -f $pr.considered, $pr.within10cm, $pr.medianAbsM, $pr.p90AbsM, $pr.stayed)
        }

        Check ($trim.excluded -eq $T -and $trim.edits.trim.start -eq $T -and $trim.export.rows -eq ($n - $T)) 'trim: the pings before the boat is in the water left out, also of the CSV' `
            ("left out {0}, rows {1}" -f $trim.excluded, $trim.export.rows)
        $mBase = Mosaic 'r1' $map1 'base'; $mTrim = Mosaic 'r1' $map1 'trim'
        $l = Loses $mBase $mTrim
        Check $l[0] 'trim: the mosaic loses what only those pings painted, gains nothing, keeps the rest' $l[1]

        $bad = @($all.edits.bad)
        Check ($bad.Count -eq 1 -and [int]$bad[0][0] -eq $m0 -and [int]$bad[0][1] -eq $m1) 'all: the marked stretch kept as one range' (($bad | ConvertTo-Json -Compress))
        Check ($all.excluded -eq ($T + $m1 - $m0) -and $all.export.rows -eq ($n - $all.excluded)) 'all: trimmed and marked pings left out, also of the CSV' ("left out {0}, rows {1}" -f $all.excluded, $all.export.rows)
        $drawn = @(); for ($i = $d0; $i -lt $d1; $i++) { $drawn += $bAll[$i] }
        $drawDepth = $drawn[0]
        Check ((@($drawn | Where-Object { $_ -ne $drawDepth }).Count -eq 0) -and -not [float]::IsNaN($drawDepth)) 'all: the drawn stretch at the drawn depth' ("{0} pings at {1:N3} m" -f $drawn.Count, $drawDepth)
        $erased = 0; for ($i = $e0; $i -lt $e1; $i++) { if ([float]::IsNaN($bAll[$i])) { $erased++ } }
        Check ($erased -eq ($e1 - $e0)) 'all: no bottom where erased' ("{0} of {1}" -f $erased, ($e1 - $e0))
        $maxLim = [double]$all.edits.limitMax[0].values[0]; $minLim = [double]$all.edits.limitMin[0].values[0]
        $raiseOver = 0; for ($i = $ra0; $i -lt $ra1; $i++) { if ($bAll[$i] -gt $maxLim * 1.05 + 0.05) { $raiseOver++ } }
        $lowerUnder = 0; for ($i = $lo0; $i -lt $lo1; $i++) { if ($bAll[$i] -lt $minLim * 0.95 - 0.05) { $lowerUnder++ } }
        Check ($raiseOver -eq 0 -and (Median $bAll $ra0 $ra1) -lt (Median $bBase $ra0 $ra1)) 'all: raised, the bottom no deeper than the limit' `
            ("limit {0:N2} m, median {1:N2} -> {2:N2} m, {3} over" -f $maxLim, (Median $bBase $ra0 $ra1), (Median $bAll $ra0 $ra1), $raiseOver)
        Check ($lowerUnder -eq 0 -and (Median $bAll $lo0 $lo1) -gt (Median $bBase $lo0 $lo1)) 'all: lowered, the bottom no nearer than the limit' `
            ("limit {0:N2} m, median {1:N2} -> {2:N2} m, {3} under" -f $minLim, (Median $bBase $lo0 $lo1), (Median $bAll $lo0 $lo1), $lowerUnder)

        $du = Differ $bBase $bUndone
        Check ($undone.excluded -eq 0 -and -not $undone.hasEdits -and $du.Count -eq 0 -and -not $undone.canUndo -and $undone.canRedo) `
            'undone: no edit left, every bottom as read (bit for bit)' ("{0} pings differ" -f $du.Count)
        $c = CompareMosaic $mBase (Mosaic 'r1' $map1 'undone') -Judge
        Check ($c -and $c -match 'PASS') 'undone: the mosaic as read' ($c -split "`n" | Select-Object -Last 1)

        $dr = Differ $bAll $bRedone
        Check ((EditsText $redone) -eq (EditsText $all) -and $dr.Count -eq 0) 'redone: the edits and every bottom as before the undo' ("{0} pings differ" -f $dr.Count)
        $c = CompareMosaic (Mosaic 'r1' $map1 'all') (Mosaic 'r1' $map1 'redone') -Judge
        Check ($c -and $c -match 'PASS') 'redone: the mosaic as before the undo' ($c -split "`n" | Select-Object -Last 1)

        $fileOk = $false
        if ($keditAfter1) {
            $kf = $keditText | ConvertFrom-Json
            $fileOk = $kf.kind -eq 'KoggerApp echogram edits' -and [int]$kf.epochs -eq $n -and [int]$kf.trim.start -eq $T -and @($kf.bad).Count -eq 1
        }
        Check $fileOk 'file: the edits written next to the recording' $kedit

        Check ((EditsText $reload) -eq (EditsText $all) -and $reload.excluded -eq $all.excluded -and $reload.editsFileExists) 'reload: the edits read back as they were'
        $dl = Differ $bAll $bReload
        $near = { param($i, $ranges) foreach ($r in $ranges) { if ($i -ge $r[0] - $w -and $i -lt $r[1] + $w) { return $true } }; return $false }
        $limits = @(@($ra0, $ra1), @($lo0, $lo1))
        $far = @($dl | Where-Object { -not (& $near $_ $limits) })
        Check ($far.Count -eq 0) 'reload: every bottom as in run 1, but next to the limits' ("{0} differ, {1} of them away from the limits" -f $dl.Count, $far.Count)

        $edited = @(@($d0, $d1), @($e0, $e1), @($ra0, $ra1), @($lo0, $lo1))
        $ds = Differ $bBase $bReset
        $far = @($ds | Where-Object { -not (& $near $_ $edited) })
        Check ($reset.excluded -eq 0 -and -not $reset.hasEdits -and $far.Count -eq 0) 'reset: no edit left, the bottom as read but next to the edited stretches' `
            ("{0} differ, {1} of them away from the edits" -f $ds.Count, $far.Count)
        $dn = Differ $bReload $bUnreset
        Check ((EditsText $unreset) -eq (EditsText $reload) -and $dn.Count -eq 0) 'unreset: the reset undone, edits and bottom as read back' ("{0} pings differ" -f $dn.Count)
        Check (-not $keditAfter2) 'cleanup: a reset recording leaves no file of edits'
        $shots += "$r1\trim.png", "$r1\draw.png", "$r1\mark.png", "$r1\raise.png", "$r2\reload.png"
    }

    if ($Runs -ne 'edits') {
        if (Test-Path -LiteralPath $kedit) { Remove-Item -LiteralPath $kedit -Force }   # what run 2 failed to clean up
        $r3 = Join-Path $work 'r3'
        $steps3 = @(
            $settle, $wait,
            @{ op = 'dump'; path = "$r3\raw.json" },
            @{ op = 'spike'; fromFrac = 0.45; width = 1; factor = 1.6 },
            @{ op = 'spike'; fromFrac = 0.46; width = 2; factor = 1.6 },
            @{ op = 'spike'; fromFrac = 0.47; width = 3; factor = 1.6 },
            @{ op = 'trimStart'; epoch = $T }, $settle, $wait,
            @{ op = 'dump'; path = "$r3\base.json" },
            @{ op = 'smooth'; fromFrac = 0.30; toFrac = 0.31; passes = 3 }, $settle, $wait,
            @{ op = 'dump'; path = "$r3\smooth.json" },
            @{ op = 'erase'; fromFrac = 0.33; toFrac = 0.332 },
            @{ op = 'fillGaps'; fromFrac = 0.329; toFrac = 0.333 }, $settle, $wait,
            @{ op = 'dump'; path = "$r3\fill.json" },
            @{ op = 'mosaicOut'; fromFrac = 0.36; toFrac = 0.38; side = 0 }, $settle, $wait,
            @{ op = 'dump'; path = "$r3\side.json" },
            @{ op = 'view'; epochFrac = 0.37; palette = $true; tool = 11 }, @{ op = 'wait'; ms = 1500 },
            @{ op = 'grab'; path = "$r3\side.png" },
            @{ op = 'despike' }, $settle, $wait,
            @{ op = 'dump'; path = "$r3\despike.json" },
            @{ op = 'view'; epochFrac = 0.46; tool = 0 }, @{ op = 'wait'; ms = 1500 },
            @{ op = 'grab'; path = "$r3\despike.png" },
            @{ op = 'turns' }, $settle, $wait,
            @{ op = 'dump'; path = "$r3\turns.json" },
            @{ op = 'grab'; path = "$r3\turns.png" },
            @{ op = 'undoTo'; steps = 1 }, $settle, $wait,
            @{ op = 'dump'; path = "$r3\undone.json" },
            @{ op = 'redoAll' }, $settle, $wait,
            @{ op = 'dump'; path = "$r3\redone.json" },
            @{ op = 'view'; palette = $false; tool = 0 },
            @{ op = 'resetAll' }, $settle,
            @{ op = 'flush' },
            @{ op = 'quit' }
        )
        $map3 = Run-App 'r3' $steps3
        $keditAfter3 = Test-Path -LiteralPath $kedit

        # ---- judging run 3 ----
        $f = @{}; $fb = @{}
        foreach ($k in 'raw', 'base', 'smooth', 'fill', 'side', 'despike', 'turns', 'undone', 'redone') {
            $f[$k] = J "$r3\$k.json"
            $fb[$k] = Bottom "$r3\$k.json"
        }
        $n3 = [int]$f.raw.epochs
        $at3 = { param($q) [int][Math]::Round($q * $n3, [MidpointRounding]::AwayFromZero) }
        $s0 = & $at3 0.30; $s1 = & $at3 0.31
        $g0 = & $at3 0.33; $g1 = & $at3 0.332; $b0 = & $at3 0.329; $b1 = & $at3 0.333
        $x0 = & $at3 0.36; $x1 = & $at3 0.38
        $sp = @()
        foreach ($q in @(@(0.45, 1), @(0.46, 2), @(0.47, 3))) { $p0 = & $at3 $q[0]; for ($i = 0; $i -lt $q[1]; $i++) { $sp += $p0 + $i } }

        # Smoothing: three sweeps of a median of +-4 pings, each depth from those around it within 12.
        $chg = Differ $fb.base $fb.smooth
        $outside = @($chg | Where-Object { $_ -lt $s0 -or $_ -ge $s1 })
        $rough = { param($v)
            $r = 0.0
            for ($i = $s0 + 1; $i -lt $s1 - 1; $i++) {
                $a = $v[$i - 1]; $b = $v[$i]; $c = $v[$i + 1]
                if (-not ([float]::IsNaN($a) -or [float]::IsNaN($b) -or [float]::IsNaN($c))) { $r += [Math]::Abs($c - 2 * $b + $a) }
            }
            return $r
        }
        $rb = & $rough $fb.base; $rs = & $rough $fb.smooth
        $outOfRange = 0
        foreach ($i in $chg) {
            $lo = [double]::MaxValue; $hi = [double]::MinValue
            for ($j = [Math]::Max(0, $i - 12); $j -le [Math]::Min($n3 - 1, $i + 12); $j++) {
                $v = $fb.base[$j]; if (-not [float]::IsNaN($v)) { $lo = [Math]::Min($lo, $v); $hi = [Math]::Max($hi, $v) }
            }
            if ($fb.smooth[$i] -lt $lo -or $fb.smooth[$i] -gt $hi) { $outOfRange++ }
        }
        # The points set by hand, from the edits: every changed ping is a new one, and every new one is under
        # the brush (a ping a sweep moved and a later one put back where it was stays the hand's).
        $handOf = { param($dump)
            $s = New-Object 'System.Collections.Generic.HashSet[int]'
            foreach ($run in @($dump.edits.bottom)) {
                if (-not $run) { continue }
                for ($v = 0; $v -lt @($run.values).Count; $v++) { [void]$s.Add([int]$run.from + $v) }
            }
            return , $s
        }
        $handBase = & $handOf $f.base
        $newHand = @((& $handOf $f.smooth) | Where-Object { -not $handBase.Contains($_) })
        $newSet = New-Object 'System.Collections.Generic.HashSet[int]'; foreach ($e in $newHand) { [void]$newSet.Add($e) }
        $notHand = @($chg | Where-Object { -not $newSet.Contains($_) })
        $handOutside = @($newHand | Where-Object { $_ -lt $s0 -or $_ -ge $s1 })
        Check ($chg.Count -gt 0 -and $outside.Count -eq 0 -and $rs -lt $rb -and $outOfRange -eq 0 -and $notHand.Count -eq 0 -and $handOutside.Count -eq 0) `
            'f-smooth: only the brushed pings changed, set by hand, each within the depths around it, less rough' `
            ("{0} of {1} changed, roughness {2:N3} -> {3:N3} m, {4} outside the brush, {5} out of range; {6} new hand points, {7} changed not by hand, {8} outside the brush" -f $chg.Count, ($s1 - $s0), $rb, $rs, $outside.Count, $outOfRange, $newHand.Count, $notHand.Count, $handOutside.Count)

        # Gap filling: the bottom as the brush saw it (the stretch erased), the gaps on a straight line.
        $er = [float[]]$fb.smooth.Clone(); for ($i = $g0; $i -lt $g1; $i++) { $er[$i] = [float]::NaN }
        $chgF = Differ $fb.smooth $fb.fill
        $offLine = 0; $outsideF = 0; $worstF = 0.0
        foreach ($i in $chgF) {
            if ($i -lt $b0 -or $i -ge $b1 -or -not [float]::IsNaN($er[$i])) { $outsideF++; continue }
            if ([float]::IsNaN($fb.fill[$i])) { continue }   # erased and left open
            $lft = $i - 1; while ($lft -ge 0 -and [float]::IsNaN($er[$lft])) { $lft-- }
            $rgt = $i + 1; while ($rgt -lt $n3 -and [float]::IsNaN($er[$rgt])) { $rgt++ }
            if ($lft -lt 0 -or $rgt -ge $n3) { $offLine++; continue }
            $exp = $er[$lft] + ($er[$rgt] - $er[$lft]) * ($i - $lft) / ($rgt - $lft)
            $dev = [Math]::Abs($fb.fill[$i] - $exp)
            $worstF = [Math]::Max($worstF, $dev)
            if ($dev -gt 1e-4 * [Math]::Max(1.0, [Math]::Abs($exp))) { $offLine++ }
        }
        $stillOpen = 0; for ($i = $g0; $i -lt $g1; $i++) { if ([float]::IsNaN($fb.fill[$i])) { $stillOpen++ } }
        Check ($stillOpen -eq 0 -and $offLine -eq 0 -and $outsideF -eq 0) 'f-fill: the erased stretch closed on the straight line between the bottoms around it, nothing else' `
            ("{0} pings set, {1} of {2} erased left open, {3} off the line (worst {4:E1} m), {5} other pings changed" -f $chgF.Count, $stillOpen, ($g1 - $g0), $offLine, $worstF, $outsideF)

        # A side out of the mosaic: its range in the edits, the depths as they were.
        $mo = @($f.side.edits.mosaicOut)
        $side0 = @($f.side.channels)[0]
        $moOk = $mo.Count -eq 1 -and [int]$mo[0].address -eq [int]$side0.address -and [int]$mo[0].sub -eq [int]$side0.sub `
            -and @($mo[0].ranges).Count -eq 1 -and [int]$mo[0].ranges[0][0] -eq $x0 -and [int]$mo[0].ranges[0][1] -eq $x1
        $dSide = Differ $fb.fill $fb.side
        Check ($moOk -and $dSide.Count -eq 0 -and $f.side.excluded -eq $f.fill.excluded) 'f-side: the first channel''s side out of the mosaic over the stretch, every depth kept' `
            ("{0}; {1} depths changed; channels {2}" -f ($mo | ConvertTo-Json -Depth 5 -Compress), $dSide.Count, (@($f.side.channels) | ConvertTo-Json -Compress))
        # Where the side painted over another line's swath, that swath shows again: "another value".
        $l = Loses (Mosaic 'r3' $map3 'fill') (Mosaic 'r3' $map3 'side') -OtherMax 1.0
        Check $l[0] 'f-side: the mosaic loses that side''s pixels, gains none' $l[1]

        # Spikes: the ones put in, and what it finds in the recording itself.
        $chgD = Differ $fb.side $fb.despike
        $found = @($chgD | Where-Object { $sp -notcontains $_ })
        $badFix = 0; $worstD = 0.0
        foreach ($p in $sp) {
            $orig = $fb.raw[$p]; $now = $fb.despike[$p]
            $dev = if ([float]::IsNaN($now)) { [double]::PositiveInfinity } else { [Math]::Abs($now - $orig) }
            $worstD = [Math]::Max($worstD, $dev)
            if ($dev -gt [Math]::Max(0.15, 0.03 * $orig)) { $badFix++ }
        }
        $usable = [int]$f.side.usable
        $handD = [int]$f.despike.hand - [int]$f.side.hand
        Check ([int]$f.base.results.spiked -eq $sp.Count -and $badFix -eq 0 -and $handD -eq $chgD.Count -and [int]$f.despike.results.despike -eq $chgD.Count -and $found.Count -le 0.01 * $usable) `
            'f-despike: the spikes put in given the bottom around them; few others found' `
            ("{0} put in ({1} given a bottom off by more than 15 cm or 3 %, worst {2:N3} m); {3} found in the recording, {4:P2} of {5} pings in use" -f $sp.Count, $badFix, $worstD, $found.Count, ($found.Count / [Math]::Max(1, $usable)), $usable)
        if ($found.Count -gt 0) {
            $show = ($found | Select-Object -First 12 | ForEach-Object { "{0}: {1:N2} -> {2:N2} m" -f $_, $fb.side[$_], $fb.despike[$_] }) -join '; '
            Write-Host "    found in the recording (the first 12): $show"
        }

        # Turns: counted; their sides out of the mosaic, every depth as it was.
        $nt = [int]$f.turns.results.turns
        $dT = Differ $fb.despike $fb.turns
        $moT = @($f.turns.edits.mosaicOut)
        $rangesT = 0; $beforeTrim = 0
        foreach ($sd in $moT) {
            foreach ($rg in @($sd.ranges)) { $rangesT++; if ([int]$rg[0] -lt $T) { $beforeTrim++ } }
        }
        if ($nt -gt 0) {
            $l = Loses (Mosaic 'r3' $map3 'despike') (Mosaic 'r3' $map3 'turns') -OtherMax 1.0
            Check ($dT.Count -eq 0 -and $beforeTrim -eq 0 -and $l[0]) 'f-turns: the turns'' sides out of the mosaic (pings in use only), every depth kept; the mosaic loses pixels, gains none' `
                ("{0} pings, {1} ranges over the sides, {2} of them on trimmed pings; {3} depths changed; {4}" -f $nt, $rangesT, $beforeTrim, $dT.Count, $l[1])
        } else {
            Check ($dT.Count -eq 0 -and [int]$f.turns.undoCount -eq [int]$f.despike.undoCount) 'f-turns: none found, nothing changed' ("{0} depths changed" -f $dT.Count)
        }

        # Undone to the trim, and redone.
        $dU = Differ $fb.base $fb.undone
        Check ($dU.Count -eq 0 -and (EditsText $f.undone) -eq (EditsText $f.base) -and [int]$f.undone.undoCount -eq 1) 'f-undone: the filters taken back, every bottom as before them (bit for bit)' `
            ("{0} pings differ, {1} steps left" -f $dU.Count, $f.undone.undoCount)
        $c = CompareMosaic (Mosaic 'r3' $map3 'base') (Mosaic 'r3' $map3 'undone') -Judge
        Check ($c -and $c -match 'PASS') 'f-undone: the mosaic as before the filters' ($c -split "`n" | Select-Object -Last 1)
        $dR = Differ $fb.turns $fb.redone
        Check ($dR.Count -eq 0 -and (EditsText $f.redone) -eq (EditsText $f.turns)) 'f-redone: the edits and every bottom as after the filters' ("{0} pings differ" -f $dR.Count)
        $c = CompareMosaic (Mosaic 'r3' $map3 'turns') (Mosaic 'r3' $map3 'redone') -Judge
        Check ($c -and $c -match 'PASS') 'f-redone: the mosaic as after the filters' ($c -split "`n" | Select-Object -Last 1)
        Check (-not $keditAfter3) 'f-cleanup: a reset recording leaves no file of edits'
        $shots += "$r3\side.png", "$r3\despike.png", "$r3\turns.png"
    }

    foreach ($png in $shots) {
        Write-Host ("screenshot: {0} {1}" -f $png, $(if (Test-Path $png) { '' } else { '(missing)' }))
    }
}
finally {
    if (Test-Path -LiteralPath $kept) {
        if (Test-Path -LiteralPath $kedit) { Remove-Item -LiteralPath $kedit -Force }
        Move-Item -LiteralPath $kept -Destination $kedit -Force
        Write-Host "put back: $kedit"
    } elseif (Test-Path -LiteralPath $kedit) {
        Remove-Item -LiteralPath $kedit -Force   # the check's own, if a run stopped half way
    }
}
Write-Host ("echoedit_e2e {0}: {1} passed, {2} failed" -f $name, $script:pass, $script:fail)
exit ([int]($script:fail -gt 0))

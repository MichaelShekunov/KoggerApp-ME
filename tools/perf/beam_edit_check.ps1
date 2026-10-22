#requires -Version 5.1
<#
.SYNOPSIS
    The mosaic's brightness model after an echogram edit: a recording edited in the session paints as the
    same recording reopened with its edits. docs/mosaic-quality.md, docs/echogram-editing.md.

.DESCRIPTION
    Three windows of the app, each closing itself (20 to 40 s each on a 350 MB recording), the edits made by
    a script (KOGGER_ECHOEDIT_SCRIPT, src/echogram_edit/echo_edit_script.h); the background precompute off in
    all three, so one painter paints what is dumped:

    Run 1, "session". The recording as read; once its bottom track is quiet and the mosaic painted, pings
    -MarkFrom up to (not including) -MarkTo are marked bad; after the rebuild the mosaic is dumped; the edits
    are saved next to the recording.
    Run 2, "reopen". The recording again, its file of edits read before the model is measured; the mosaic
    dumped once painted.
    Run 3, "off". As run 1 from a recording with no edits, but the correction ("Even out brightness") is
    turned off through a settings widget before the edit and on again after it.

    Checked:
      edited       run 1 measured the model again after the edit, from fewer pings than before it
      model        the model run 1 painted with after the edit is run 2's: its log line, the timing and the
                   reason left out, the same
      mosaic       run 1's mosaic after the edit against run 2's: no tile in one only, and
                   mosaic_dump_compare.ps1 -Judge on the rest
      off-edited   run 3 turned the correction off and on, and measured the model after the edit meanwhile
      off-model    the model run 3 measured last is run 2's
      off-mosaic   run 3's mosaic once the correction is back on against run 2's, as "mosaic"
    A build without the re-measure fails "edited", "model" and "mosaic"; one that skips it while the
    correction is off fails the three "off" checks. Each run starts from a fresh copy of the test profile
    "perf" (reg import it first, as for the other checks) into -Profile. The recording's own file of edits, if any, is put aside
    and back; a file put aside by a run that did not finish stops the check, to be put back by hand.
    Exit code 0 when all pass, 1 when a check fails.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\perf\beam_edit_check.ps1 -Exe <build>\KoggerApp.exe -File D:\Temp\perf\00014_03.05.2025.KLF -PathPrefix D:\Qt\6.8.3\llvm-mingw_64\bin
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [Parameter(Mandatory = $true)] [string] $File,
    [int] $MarkFrom = 20000,
    [int] $MarkTo = 45000,
    [string] $Profile = 'beamedit',
    [string] $Out = "$env:TEMP\kogger_beam_edit",
    [string] $PathPrefix,
    [int] $WaitMs = 10000,           # after the settle, for the mosaic to be painted
    [int] $TimeoutS = 600
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\common\test_desktop.ps1')   # the app on a desktop of its own: no window, no focus taken
$File = (Resolve-Path -LiteralPath $File).Path
$work = Join-Path $Out ([System.IO.Path]::GetFileNameWithoutExtension($File))
if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
New-Item -ItemType Directory -Force -Path $work | Out-Null
$compare = Join-Path $PSScriptRoot 'mosaic_dump_compare.ps1'
$kedit = "$File.kedit"
$kept = "$File.kedit.beamcheck-kept"
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"
$correctionKey = 'app:mosaic.beamCorrection'

$script:fail = 0
function Check([bool] $ok, [string] $what, [string] $detail = '') {
    if ($ok) { Write-Host "PASS $what $(if ($detail) { "($detail)" })" }
    else { $script:fail++; Write-Host "FAIL $what $(if ($detail) { ": $detail" })" }
}

function Run-App([string] $tag, [object[]] $steps) {
    # Each run from the test profile as it is ("perf"): run 3 pins a setting, and a pin left in the profile
    # would make the next "pin" step take it off.
    $key = "HKCU\Software\KOGGER\KoggerApp-$Profile"
    $eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'   # reg.exe's messages are not errors here
    & reg.exe delete $key /f *> $null                                      # absent the first time
    & reg.exe copy 'HKCU\Software\KOGGER\KoggerApp-perf' $key /s /f *> $null
    $rc = $LASTEXITCODE
    $ErrorActionPreference = $eap
    if ($rc -ne 0) { throw "could not copy the test profile perf to $Profile" }
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
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    $app = Start-TestDesktopProcess $psi
    if (-not $app.WaitForExit($TimeoutS * 1000)) {
        $app.Kill()
        throw "run $tag did not end in $TimeoutS s"
    }
    Write-Host ("run {0}: {1:N0} s, exit {2}" -f $tag, $clock.Elapsed.TotalSeconds, $app.ExitCode)
    $lines = Get-Content -LiteralPath $log -Encoding UTF8
    $start = ($lines | Select-String -Pattern 'log opened' | Select-Object -Last 1).LineNumber
    $mine = @($lines[($start - 1)..($lines.Count - 1)])
    $mine | Set-Content -LiteralPath (Join-Path $dir 'kogger.log') -Encoding UTF8
    # Each dump with the mosaic digest written last before it (digest_<n>.bin, n counted in the log).
    $digests = 0
    $map = @{}
    foreach ($l in $mine) {
        if ($l -match 'mosaic digest:') { $digests++ }
        if ($l -match 'echoedit script: dump (\S+\.json):') { $map[[System.IO.Path]::GetFileNameWithoutExtension($Matches[1])] = $digests }
    }
    $beam = @($mine | Where-Object { $_ -match 'mosaic beam: measured' } | ForEach-Object { $_ -replace '^.*?mosaic beam: ', '' })
    $beam | ForEach-Object { Write-Host "    $_" }
    $quality = @($mine | Where-Object { $_ -match 'mosaic quality: correction (on|off)' } | ForEach-Object { $Matches[1] })
    return [pscustomobject]@{ Map = $map; Beam = $beam; Quality = $quality; Dir = $dir }
}
function Mosaic($run, [string] $dump) {
    $n = $run.Map[$dump]
    if (-not $n) { return $null }
    return (Join-Path $run.Dir "mosaic\digest_$n.bin")
}
# A model's line without what differs between two measurements of the same pings: the time it took and why.
function ModelOf([string] $line) {
    return (($line -replace ' after an echogram edit', '') -replace ' in \d+ ms;', ';')
}
function PingsOf([string] $line) {
    return @([regex]::Matches($line, '(\d+) pings') | ForEach-Object { [int]$_.Groups[1].Value })
}
# Two mosaics of the same pings: no tile in one only (both runs paint with one painter, so a tile in one only
# is paint the other lost or never had), and -Judge on the tiles in both.
function SameMosaic([string] $what, [string] $a, [string] $b) {
    if (-not ($a -and $b -and (Test-Path $a) -and (Test-Path $b))) { Check $false $what "no digests ($a, $b)"; return }
    $text = & powershell -NoProfile -ExecutionPolicy Bypass -File $compare -A $a -B $b -Quiet -IgnoreBlankTiles -Judge
    $counts = @($text | Where-Object { $_ -match '^tiles:' } | Select-Object -First 1)
    $verdict = @($text | Where-Object { $_ -match '^(PASS|FAIL)' } | Select-Object -Last 1)
    $ok = $counts.Count -eq 1 -and $counts[0] -match '^tiles: (\d+) in both, (\d+) only in A, (\d+) only in B' `
        -and [int]$Matches[1] -gt 0 -and [int]$Matches[2] -eq 0 -and [int]$Matches[3] -eq 0 `
        -and $verdict.Count -eq 1 -and $verdict[0] -like 'PASS*'
    Check $ok $what ("{0} {1}" -f ($counts -join ''), ($verdict -join ''))
}

if (Test-Path -LiteralPath $kept) {
    throw "a run that did not finish left $kept -- the recording's own edits, or this check's; put it back as $kedit by hand (or remove it), then run again"
}
if (Test-Path -LiteralPath $kedit) { Move-Item -LiteralPath $kedit -Destination $kept; Write-Host "kept aside: $kedit" }
$hadBak = Test-Path -LiteralPath "$kedit.bak"
try {
    $settle = @{ op = 'settle'; quietMs = 3000 }
    $wait = @{ op = 'wait'; ms = $WaitMs }
    $mark = @{ op = 'mark'; from = $MarkFrom; to = $MarkTo }
    $r1 = Run-App 'session' @(
        $settle, $wait,
        @{ op = 'dump'; path = (Join-Path $work 'session\before.json') },
        $mark, $settle, $wait,
        @{ op = 'dump'; path = (Join-Path $work 'session\edited.json') },
        @{ op = 'flush' },
        @{ op = 'quit' })
    Check (Test-Path -LiteralPath $kedit) 'file' "the edits saved next to the recording"
    $r2 = Run-App 'reopen' @(
        $settle, $wait,
        @{ op = 'dump'; path = (Join-Path $work 'reopen\reopened.json') },
        @{ op = 'quit' })
    if (Test-Path -LiteralPath $kedit) { Remove-Item -LiteralPath $kedit }   # this check's, made by run 1
    $use = @{ op = 'pinuse'; key = $correctionKey; index = 0 }
    $r3 = Run-App 'off' @(
        $settle, $wait,
        @{ op = 'pin'; key = $correctionKey; label = 'Even out brightness' }, @{ op = 'wait'; ms = 1500 },
        $use, $settle, $wait,
        $mark, $settle, $wait,
        $use, $settle, $wait,
        @{ op = 'dump'; path = (Join-Path $work 'off\edited.json') },
        @{ op = 'quit' })

    $before = @($r1.Beam | Where-Object { $_ -notmatch 'after an echogram edit' })
    $after = @($r1.Beam | Where-Object { $_ -match 'after an echogram edit' })
    $edited = $after.Count -ge 1 -and $before.Count -ge 1
    $detail = "measured $($before.Count) time(s) before the edit, $($after.Count) after it"
    if ($edited) {
        $pb = PingsOf $before[-1]; $pa = PingsOf $after[-1]
        $edited = $pa.Count -eq 2 -and $pb.Count -eq 2 -and $pa[0] -lt $pb[0] -and $pa[1] -lt $pb[1]
        $detail = "pings left/right $($pb -join '/') before the edit, $($pa -join '/') after"
    }
    Check $edited 'edited' $detail

    $reopened = if ($r2.Beam.Count) { ModelOf $r2.Beam[-1] } else { '' }
    $inUse = if ($after.Count) { $after[-1] } elseif ($before.Count) { $before[-1] } else { '' }
    Check ($inUse -and $reopened -and (ModelOf $inUse) -eq $reopened) 'model' "session: $(ModelOf $inUse) | reopen: $reopened"
    SameMosaic 'mosaic' (Mosaic $r1 'edited') (Mosaic $r2 'reopened')

    $toggled = ($r3.Quality -join ',') -match 'on,off,on$'
    $offAfter = @($r3.Beam | Where-Object { $_ -match 'after an echogram edit' })
    Check ($toggled -and $offAfter.Count -ge 1) 'off-edited' "correction $($r3.Quality -join ' -> '); measured after the edit $($offAfter.Count) time(s)"
    $offLast = if ($r3.Beam.Count) { ModelOf $r3.Beam[-1] } else { '' }
    Check ($offLast -and $offLast -eq $reopened) 'off-model' "off: $offLast | reopen: $reopened"
    SameMosaic 'off-mosaic' (Mosaic $r3 'edited') (Mosaic $r2 'reopened')
}
finally {
    if (Test-Path -LiteralPath $kedit) { Remove-Item -LiteralPath $kedit -Force }   # this check's
    if (-not $hadBak -and (Test-Path -LiteralPath "$kedit.bak")) { Remove-Item -LiteralPath "$kedit.bak" -Force }
    if (Test-Path -LiteralPath $kept) { Move-Item -LiteralPath $kept -Destination $kedit; Write-Host "put back: $kedit" }
}
Write-Host ("beam_edit_check: {0}" -f $(if ($script:fail) { "$($script:fail) failed" } else { 'all passed' }))
exit ([int]($script:fail -gt 0))

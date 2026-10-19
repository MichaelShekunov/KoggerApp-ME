#requires -Version 5.1
<#
.SYNOPSIS
    The mosaic's brightness model after an echogram edit: a recording edited in the session paints as the
    same recording reopened with its edits. docs/mosaic-quality.md, docs/echogram-editing.md.

.DESCRIPTION
    Two windows of the app, each closing itself (about a minute each on a 350 MB recording), the edits made
    by a script (KOGGER_ECHOEDIT_SCRIPT, src/echogram_edit/echo_edit_script.h):

    Run 1, "session". The recording as read; once its bottom track is quiet and the mosaic painted, a long
    stretch is marked bad (-MarkFrom .. -MarkTo); after the rebuild the mosaic is dumped; the edits are
    saved next to the recording.
    Run 2, "reopen". The recording again, its file of edits read before the model is measured; the mosaic
    dumped once painted.

    Checked:
      edited   run 1 measured the model again after the edit (a "mosaic beam: measured after an echogram
               edit" line), and from fewer pings than before it
      model    the model run 1 painted with after the edit is the one run 2 measured: its line in the log, the
               timing left out, the same
      mosaic   run 1's mosaic after the edit against run 2's: mosaic_dump_compare.ps1 -Judge (shares, as two
               paintings of the same pings by different jobs differ a little)
    A build without the re-measure fails "edited" and "model", and "mosaic" by the brightness the left-out
    pings put into the model. The recording's own file of edits, if any, is put aside and back.
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
$File = (Resolve-Path -LiteralPath $File).Path
$work = Join-Path $Out ([System.IO.Path]::GetFileNameWithoutExtension($File))
if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
New-Item -ItemType Directory -Force -Path $work | Out-Null
$compare = Join-Path $PSScriptRoot 'mosaic_dump_compare.ps1'
$kedit = "$File.kedit"
$kept = "$File.kedit.beamcheck-kept"
$log = Join-Path $env:APPDATA "KOGGER\KoggerApp-$Profile\AppLogs\kogger.log"

$script:fail = 0
function Check([bool] $ok, [string] $what, [string] $detail = '') {
    if ($ok) { Write-Host "PASS $what $(if ($detail) { "($detail)" })" }
    else { $script:fail++; Write-Host "FAIL $what $(if ($detail) { ": $detail" })" }
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
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    $app = [System.Diagnostics.Process]::Start($psi)
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
    return [pscustomobject]@{ Map = $map; Beam = $beam; Dir = $dir; Lines = $mine }
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
    $m = [regex]::Matches($line, '(\d+) pings')
    return @($m | ForEach-Object { [int]$_.Groups[1].Value })
}

if (Test-Path -LiteralPath $kedit) { Move-Item -LiteralPath $kedit -Destination $kept -Force; Write-Host "kept aside: $kedit" }
$hadBak = Test-Path -LiteralPath "$kedit.bak"
try {
    $settle = @{ op = 'settle'; quietMs = 3000 }
    $wait = @{ op = 'wait'; ms = $WaitMs }
    $r1 = Run-App 'session' @(
        $settle, $wait,
        @{ op = 'dump'; path = (Join-Path $work 'session\before.json') },
        @{ op = 'mark'; from = $MarkFrom; to = $MarkTo }, $settle, $wait,
        @{ op = 'dump'; path = (Join-Path $work 'session\edited.json') },
        @{ op = 'flush' },
        @{ op = 'quit' })
    Check (Test-Path -LiteralPath $kedit) 'file' "the edits saved next to the recording"
    $r2 = Run-App 'reopen' @(
        $settle, $wait,
        @{ op = 'dump'; path = (Join-Path $work 'reopen\reopened.json') },
        @{ op = 'quit' })

    $before = @($r1.Beam | Where-Object { $_ -notmatch 'after an echogram edit' })
    $after = @($r1.Beam | Where-Object { $_ -match 'after an echogram edit' })
    $edited = $after.Count -ge 1 -and $before.Count -ge 1
    $detail = if ($edited) {
        $pb = PingsOf $before[-1]; $pa = PingsOf $after[-1]
        $edited = $pa.Count -eq 2 -and $pb.Count -eq 2 -and $pa[0] -lt $pb[0] -and $pa[1] -lt $pb[1]
        "pings left/right $($pb -join '/') before the edit, $($pa -join '/') after"
    } else { "measured $($before.Count) time(s) before the edit, $($after.Count) after it" }
    Check $edited 'edited' $detail

    $inUse = if ($after.Count) { $after[-1] } elseif ($before.Count) { $before[-1] } else { '' }
    $reopened = if ($r2.Beam.Count) { $r2.Beam[-1] } else { '' }
    Check ($inUse -and $reopened -and (ModelOf $inUse) -eq (ModelOf $reopened)) 'model' `
        "session: $(ModelOf $inUse) | reopen: $(ModelOf $reopened)"

    $a = Mosaic $r1 'edited'
    $b = Mosaic $r2 'reopened'
    if ($a -and $b -and (Test-Path $a) -and (Test-Path $b)) {
        $text = & powershell -NoProfile -ExecutionPolicy Bypass -File $compare -A $a -B $b -Quiet -IgnoreBlankTiles -Judge
        $verdict = @($text | Where-Object { $_ -match '^(PASS|FAIL)' } | Select-Object -Last 1)
        $counts = @($text | Where-Object { $_ -match '^tiles:' } | Select-Object -First 1)
        Check ($verdict.Count -eq 1 -and $verdict[0] -like 'PASS*') 'mosaic' ("{0} {1}" -f ($counts -join ''), ($verdict -join ''))
    } else {
        Check $false 'mosaic' "no digests ($a, $b)"
    }
}
finally {
    if (Test-Path -LiteralPath $kedit) { Remove-Item -LiteralPath $kedit -Force }
    if (-not $hadBak -and (Test-Path -LiteralPath "$kedit.bak")) { Remove-Item -LiteralPath "$kedit.bak" -Force }
    if (Test-Path -LiteralPath $kept) { Move-Item -LiteralPath $kept -Destination $kedit -Force; Write-Host "put back: $kedit" }
}
Write-Host ("beam_edit_check: {0}" -f $(if ($script:fail) { "$($script:fail) failed" } else { 'all passed' }))
exit ([int]($script:fail -gt 0))

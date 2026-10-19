#requires -Version 5.1
<#
.SYNOPSIS
    Builds and runs tools/echoedit_check, the offline check of the echogram edits' model
    (src/echogram_edit/echo_edit_model.*; docs/echogram-editing.md), with the toolchain of
    scripts\win_build_release.ps1.

.DESCRIPTION
    Exit code 0 when every check passes. With -Mutants it also builds the check against copies of the
    model with one fault each and expects every one of them to fail it (a check that a fault does not
    fail would not notice that fault come back); exit code 1 when one passes.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\echoedit_check\run.ps1 -Mutants
#>
[CmdletBinding()]
param(
    [string] $QtRoot    = 'D:\Qt',
    [string] $QtVersion = '6.8.3',
    [switch] $Mutants
)

$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot
$repo = Split-Path -Parent (Split-Path -Parent $src)
$qtPrefix = "$QtRoot\$QtVersion\llvm-mingw_64"
$llvm = "$QtRoot\Tools\llvm-mingw1706_64"
foreach ($p in @($qtPrefix, $llvm)) {
    if (-not (Test-Path -LiteralPath $p)) { throw "not found: $p (see docs\build-windows-cli.md)" }
}

function Resolve-Tool {
    param([string] $Exe, [string[]] $Fallbacks)
    $onPath = Get-Command $Exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    foreach ($f in $Fallbacks) { if (Test-Path -LiteralPath $f) { return $f } }
    throw "$Exe not found on PATH and not at any known fallback location."
}
$vsCMake = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake'
$cmake = Resolve-Tool 'cmake.exe' @("$vsCMake\CMake\bin\cmake.exe")
$ninja = Resolve-Tool 'ninja.exe' @("$vsCMake\Ninja\ninja.exe", "$QtRoot\Tools\Ninja\ninja.exe")

$env:PATH = "$llvm\bin;$qtPrefix\bin;$env:PATH"
function ToCMakePath { param([string] $p) return ($p -replace '\\', '/') }
function Invoke-Native {
    param([string] $Exe, [string[]] $Arguments, [string] $What)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Exe @Arguments | Out-Host } finally { $ErrorActionPreference = $prev }
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit $LASTEXITCODE)" }
}
function Build-Check {
    param([string] $BuildDir, [string] $ModelDir)
    $cfg = @(
        '-S', (ToCMakePath $src), '-B', (ToCMakePath $BuildDir), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
        ('-DCMAKE_MAKE_PROGRAM=' + (ToCMakePath $ninja)),
        ('-DCMAKE_PREFIX_PATH=' + (ToCMakePath $qtPrefix)),
        ('-DCMAKE_CXX_COMPILER=' + (ToCMakePath "$llvm\bin\clang++.exe")))
    if ($ModelDir) { $cfg += ('-DECHOEDIT_SRC=' + (ToCMakePath $ModelDir)) }
    Invoke-Native $cmake $cfg 'cmake configure'
    Invoke-Native $cmake @('--build', (ToCMakePath $BuildDir)) 'build'
    return (Join-Path $BuildDir 'echoedit_check.exe')
}

$exe = Build-Check (Join-Path $repo 'build\echoedit_check') ''
& $exe
$code = $LASTEXITCODE
if ($code -ne 0 -or -not $Mutants) { exit $code }

# One fault each, in a copy of the model. The text to replace must be there: a mutant that changes
# nothing would pass and look like a gap in the check.
$faults = @(
    @{ Name = 'NaN is not NaN';                 From = 'return (std::isnan(a) && std::isnan(b)) || a == b;'; To = 'return a == b;' },
    @{ Name = 'a new step keeps the redo side';  From = "    redo_.clear();`r`n    undo_.append(std::move(c));"; To = '    undo_.append(std::move(c));' },
    @{ Name = 'undo forgets the step';           From = "    Command c = undo_.takeLast();`r`n    redo_.append(c);"; To = '    Command c = undo_.takeLast();' },
    @{ Name = 'an unchanged trim takes points';  From = 'if (to.start > from.start || to.end < from.end)'; To = 'if (to.start >= from.start || to.end < from.end)' },
    @{ Name = 'the magnet without the t1 scale'; From = 'return ((bestI + s.initWinScaled + 1.0f) * c.t1) * resolution;'; To = 'return ((bestI + s.initWinScaled + 1.0f)) * resolution;' },
    @{ Name = 'the magnet looks everywhere';     From = "    int start = fromM/c.t1/resolution;`r`n    int stop = toM/c.t1/resolution;"; To = "    int start = 0;`r`n    int stop = int(sum.size());" },
    @{ Name = 'the magnet without a threshold';  From = 'qint64 best = qint64(threshold_int) * pings;'; To = 'qint64 best = 0;' },
    @{ Name = 'a newer file is read';            From = 'if (version > EditFile::kVersion)'; To = 'if (false && version > EditFile::kVersion)' },
    @{ Name = 'a gap does not break a run';      From = 'if (r.from + r.values.size() == epoch)'; To = 'if (true)' },
    @{ Name = 'the newest step can go';          From = 'while (undo_.size() > 1 &&'; To = 'while (!undo_.isEmpty() &&' },
    @{ Name = 'an undo tracks again everywhere'; From = 'if (undo ? (limited && limitsChanged)'; To = 'if (undo ? true' },
    @{ Name = 'the brush gives a missing bottom one'; From = 'if (epoch < 0 || epoch >= d.size() || !std::isfinite(d[epoch]))'; To = 'if (epoch < 0 || epoch >= d.size())' },
    @{ Name = 'a gap over the limit is filled';  From = "`r`n                         && right - left - 1 <= maxGap;"; To = ';' },
    @{ Name = 'the gap line starts a ping late'; From = 'float(e - left) / float(right - left)'; To = 'float(e - i) / float(right - left)' },
    @{ Name = 'spikes without the robust spread'; From = 'std::max(k * 1.4826f * mad, minAbs + minRel * std::fabs(med))'; To = '(minAbs + minRel * std::fabs(med))' },
    @{ Name = 'a hand point is judged a spike';   From = 'if (!std::isfinite(d[i]) || skipped(i))'; To = 'if (!std::isfinite(d[i]))' },
    @{ Name = 'a spike keeps its depth';         From = 'out.append(Spike{ i, med });'; To = 'out.append(Spike{ i, d[i] });' },
    @{ Name = 'the heading not unwrapped';       From = 'std::fabs(unwrapped[k] - unwrapped[j])'; To = 'std::fabs(double(headingDeg[k]) - double(headingDeg[j]))' },
    @{ Name = 'turns judged without the range';  From = 'ds < double(radiusFactor) * double(rangeM[i]) * dpsi'; To = 'ds < double(radiusFactor) * dpsi' },
    @{ Name = 'turns without their margin';      From = 'if (valid(c) && dist(a, c) > marginM)'; To = 'if (true)' },
    @{ Name = 'a side going back counts as out'; From = 'if ((b.mosaicOut & ~before) != 0)'; To = 'if (b.mosaicOut != before)' },
    @{ Name = 'a wobble kept as a turn';         From = 'if (length < double(minRunM) && std::fabs(unwrapped[b] - unwrapped[a]) < double(minTurnDeg))'; To = 'if (false)' },
    @{ Name = 'a side out is not rebuilt';       From = "            if ((b.mosaicOut & ~before) != 0) {`r`n                return true;"; To = "            if ((b.mosaicOut & ~before) != 0) {`r`n                return false;" }
)

$utf8 = New-Object System.Text.UTF8Encoding $false
$missed = 0
$i = 0
foreach ($m in $faults) {
    ++$i
    $dir = Join-Path $repo ("build\echoedit_mutant_{0}\model" -f $i)
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item (Join-Path $repo 'src\echogram_edit\echo_edit_model.h') $dir -Force
    $text = [System.IO.File]::ReadAllText((Join-Path $repo 'src\echogram_edit\echo_edit_model.cpp'), $utf8)
    if (-not $text.Contains($m.From)) {
        $text = $text.Replace("`r`n", "`n")
        $from = $m.From.Replace("`r`n", "`n")
        if (-not $text.Contains($from)) { throw "mutant '$($m.Name)': its text is not in echo_edit_model.cpp" }
        $text = $text.Replace($from, $m.To.Replace("`r`n", "`n"))
    } else {
        $text = $text.Replace($m.From, $m.To)
    }
    [System.IO.File]::WriteAllText((Join-Path $dir 'echo_edit_model.cpp'), $text, $utf8)
    $mexe = Build-Check (Join-Path $repo ("build\echoedit_mutant_{0}\build" -f $i)) $dir
    $out = & $mexe
    $failed = @($out | Where-Object { $_ -like 'FAIL *' })
    if ($LASTEXITCODE -ne 0 -and $failed.Count -gt 0) {
        Write-Host ("mutant {0} ({1}): caught by {2} check(s), first: {3}" -f $i, $m.Name, $failed.Count, $failed[0])
    } else {
        Write-Host ("mutant {0} ({1}): MISSED" -f $i, $m.Name)
        ++$missed
    }
}
Write-Host ("echoedit_check mutants: {0} of {1} caught" -f ($faults.Count - $missed), $faults.Count)
exit ([int]($missed -gt 0))

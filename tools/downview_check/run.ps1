#requires -Version 5.1
<#
.SYNOPSIS
    Builds and runs tools/downview_check, the offline check of the down view's arithmetic (src/down_view.h), with the
    toolchain of scripts\win_build_release.ps1.

.DESCRIPTION
    Exit code 0 when every check passes. With -Mutants it also builds the check against copies of down_view.h with one
    fault each and expects every one of them to fail it; exit code 1 when one passes.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\downview_check\run.ps1 -Mutants
#>
[CmdletBinding()]
param(
    [string] $QtRoot = 'D:\Qt',
    [switch] $Mutants
)

$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot
$repo = Split-Path -Parent (Split-Path -Parent $src)
$llvm = "$QtRoot\Tools\llvm-mingw1706_64"
if (-not (Test-Path -LiteralPath $llvm)) { throw "not found: $llvm (see docs\build-windows-cli.md)" }

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

$env:PATH = "$llvm\bin;$env:PATH"
function ToCMakePath { param([string] $p) return ($p -replace '\\', '/') }
function Invoke-Native {
    param([string] $Exe, [string[]] $Arguments, [string] $What)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Exe @Arguments | Out-Host } finally { $ErrorActionPreference = $prev }
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit $LASTEXITCODE)" }
}
function Build-Check {
    param([string] $BuildDir, [string] $SrcDir)
    $cfg = @(
        '-S', (ToCMakePath $src), '-B', (ToCMakePath $BuildDir), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
        ('-DCMAKE_MAKE_PROGRAM=' + (ToCMakePath $ninja)),
        ('-DCMAKE_CXX_COMPILER=' + (ToCMakePath "$llvm\bin\clang++.exe")))
    if ($SrcDir) { $cfg += ('-DDOWNVIEW_SRC=' + (ToCMakePath $SrcDir)) }
    Invoke-Native $cmake $cfg 'cmake configure'
    Invoke-Native $cmake @('--build', (ToCMakePath $BuildDir)) 'build'
    return (Join-Path $BuildDir 'downview_check.exe')
}

$exe = Build-Check (Join-Path $repo 'build\downview_check') ''
& $exe
$code = $LASTEXITCODE
if ($code -ne 0 -or -not $Mutants) { exit $code }

# One fault each, in a copy of down_view.h. The text to replace must be there: a mutant that changes nothing would
# pass and look like a gap in the check.
$faults = @(
    @{ Name = 'energy as the amplitudes'' mean, not their squares'''; From = 'std::exp2(2.0 * i / 15.0) + std::exp2(2.0 * j / 15.0)) / 2.0;'; To = 'std::exp2(1.0 * i / 15.0) + std::exp2(1.0 * j / 15.0)) / 2.0;' },
    @{ Name = 'energy back to bytes at 15, not 7.5';         From = 'std::lround(7.5 * std::log2(p))'; To = 'std::lround(15.0 * std::log2(p))' },
    @{ Name = 'energy truncated, not rounded';               From = 'const long v = std::lround(7.5 * std::log2(p));'; To = 'const long v = long(7.5 * std::log2(p));' },
    @{ Name = 'energy as the louder side alone';             From = 'default:         return energy(a, b);'; To = 'default:         return a > b ? a : b;' },
    @{ Name = 'the mean rounded down';                       From = 'return uint8_t((unsigned(a) + unsigned(b) + 1u) / 2u);'; To = 'return uint8_t((unsigned(a) + unsigned(b)) / 2u);' },
    @{ Name = 'min as the larger byte';                      From = 'case Mode::Min:  return a < b ? a : b;'; To = 'case Mode::Min:  return a > b ? a : b;' },
    @{ Name = 'a row reads side 0 twice';                    From = 'out[i] = combine(mode, side0[i], side1[i]);'; To = 'out[i] = combine(mode, side0[i], side0[i]);' },
    @{ Name = 'mean and min sub-channels swapped';           From = 'return sub == kSubMean ? Mode::Mean : sub == kSubMin ? Mode::Min : Mode::Energy;'; To = 'return sub == kSubMean ? Mode::Min : sub == kSubMin ? Mode::Mean : Mode::Energy;' },
    @{ Name = 'a side taken for a down view';                From = 'return sub >= kSubEnergy && sub <= kSubMin;'; To = 'return sub <= kSubMin;' }
)

$utf8 = New-Object System.Text.UTF8Encoding $false
$header = [System.IO.File]::ReadAllText((Join-Path $repo 'src\down_view.h'), $utf8).Replace("`r`n", "`n")
$missed = 0
$i = 0
foreach ($m in $faults) {
    ++$i
    if (-not $header.Contains($m.From)) { throw "mutant '$($m.Name)': its text is not in down_view.h" }
    $dir = Join-Path $repo ("build\downview_mutant_{0}\src" -f $i)
    New-Item -ItemType Directory -Force $dir | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $dir 'down_view.h'), $header.Replace($m.From, $m.To), $utf8)
    $mexe = Build-Check (Join-Path $repo ("build\downview_mutant_{0}\build" -f $i)) $dir
    $out = & $mexe
    $failed = @($out | Where-Object { $_ -like 'FAIL *' })
    if ($LASTEXITCODE -ne 0 -and $failed.Count -gt 0) {
        Write-Host ("mutant {0} ({1}): caught by {2} check(s), first: {3}" -f $i, $m.Name, $failed.Count, $failed[0])
    } else {
        Write-Host ("mutant {0} ({1}): MISSED" -f $i, $m.Name)
        ++$missed
    }
}
Write-Host ("downview_check mutants: {0} of {1} caught" -f ($faults.Count - $missed), $faults.Count)
exit ([int]($missed -gt 0))

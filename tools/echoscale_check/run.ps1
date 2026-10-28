#requires -Version 5.1
<#
.SYNOPSIS
    Builds and runs tools/echoscale_check, the offline check of the echogram's horizontal scale
    (src/scene2d/plot2D_columns.h: the horizontal scale's steps), with the toolchain of scripts\win_build_release.ps1.

.DESCRIPTION
    Exit code 0 when every check passes. With -Mutants it also builds the check against copies of the
    header with one fault each and expects every one of them to fail it (a check that a fault does not
    fail would not notice that fault come back); exit code 1 when one passes.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\echoscale_check\run.ps1 -Mutants
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
    param([string] $BuildDir, [string] $HeaderDir)
    $cfg = @(
        '-S', (ToCMakePath $src), '-B', (ToCMakePath $BuildDir), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
        ('-DCMAKE_MAKE_PROGRAM=' + (ToCMakePath $ninja)),
        ('-DCMAKE_CXX_COMPILER=' + (ToCMakePath "$llvm\bin\clang++.exe")))
    if ($HeaderDir) { $cfg += ('-DECHOSCALE_SRC=' + (ToCMakePath $HeaderDir)) }
    Invoke-Native $cmake $cfg 'cmake configure'
    Invoke-Native $cmake @('--build', (ToCMakePath $BuildDir)) 'build'
    return (Join-Path $BuildDir 'echoscale_check.exe')
}

$exe = Build-Check (Join-Path $repo 'build\echoscale_check') ''
& $exe
$code = $LASTEXITCODE
if ($code -ne 0 -or -not $Mutants) { exit $code }

# One fault each, in a copy of the header. The text to replace must be there: a mutant that changes
# nothing would pass and look like a gap in the check.
$faults = @(
    @{ Name = 'nearest by difference';                 From = 'std::fabs(std::log(scale / kScales[i]))'; To = 'std::fabs(scale - kScales[i])' },
    @{ Name = 'a bad scale is the smallest';           From = "        return kScaleOne;`n    }`n    int best"; To = "        return 0;`n    }`n    int best" },
    @{ Name = 'the most compressed step out of reach'; From = 'std::clamp(scaleIndex(scale) + steps, 0, kScaleCount - 1)'; To = 'std::clamp(scaleIndex(scale) + steps, 1, kScaleCount - 1)' },
    @{ Name = 'a step develop''s snap moves';          From = '{ 0.1f, 0.2f, 0.5f,'; To = '{ 0.1f, 0.25f, 0.5f,' },
    @{ Name = 'one is not at kScaleOne';               From = 'kScaleOne = 3;'; To = 'kScaleOne = 4;' },
    @{ Name = 'the steps stop at 8';                   From = '6.0f, 8.0f, 10.0f }'; To = '6.0f, 8.0f, 8.0f }' }
)

$utf8 = New-Object System.Text.UTF8Encoding $false
$header = [System.IO.File]::ReadAllText((Join-Path $repo 'src\scene2d\plot2D_columns.h'), $utf8).Replace("`r`n", "`n")
$missed = 0
$i = 0
foreach ($m in $faults) {
    ++$i
    if (-not $header.Contains($m.From)) { throw "mutant '$($m.Name)': its text is not in plot2D_columns.h" }
    $dir = Join-Path $repo ("build\echoscale_mutant_{0}\header" -f $i)
    New-Item -ItemType Directory -Force $dir | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $dir 'plot2D_columns.h'), $header.Replace($m.From, $m.To), $utf8)
    $mexe = Build-Check (Join-Path $repo ("build\echoscale_mutant_{0}\build" -f $i)) $dir
    $out = & $mexe
    $failed = @($out | Where-Object { $_ -like 'FAIL *' })
    if ($LASTEXITCODE -ne 0 -and $failed.Count -gt 0) {
        Write-Host ("mutant {0} ({1}): caught by {2} check(s), first: {3}" -f $i, $m.Name, $failed.Count, $failed[0])
    } else {
        Write-Host ("mutant {0} ({1}): MISSED" -f $i, $m.Name)
        ++$missed
    }
}
Write-Host ("echoscale_check mutants: {0} of {1} caught" -f ($faults.Count - $missed), $faults.Count)
exit ([int]($missed -gt 0))

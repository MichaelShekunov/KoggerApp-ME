#requires -Version 5.1
<#
.SYNOPSIS
    Builds and runs tools/sonarqa_check, the offline check of the sonar acceptance module (src/sonar_qa,
    docs/SONAR_QA.md) against a simulated nanoSSS, with the toolchain of scripts\win_build_release.ps1.

.DESCRIPTION
    Exit code 0 when every check passes.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\sonarqa_check\run.ps1
#>
[CmdletBinding()]
param(
    [string] $QtRoot = 'D:\Qt'
)

$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot
$repo = Split-Path -Parent (Split-Path -Parent $src)
$llvm = "$QtRoot\Tools\llvm-mingw1706_64"
$qt = "$QtRoot\6.8.3\llvm-mingw_64"
foreach ($d in $llvm, $qt) { if (-not (Test-Path -LiteralPath $d)) { throw "not found: $d (see docs\build-windows-cli.md)" } }

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

$env:PATH = "$qt\bin;$llvm\bin;$env:PATH"
function ToCMakePath { param([string] $p) return ($p -replace '\\', '/') }
function Invoke-Native {
    param([string] $Exe, [string[]] $Arguments, [string] $What)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Exe @Arguments | Out-Host } finally { $ErrorActionPreference = $prev }
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit $LASTEXITCODE)" }
}

$build = Join-Path $repo 'build\sonarqa_check'
Invoke-Native $cmake @(
    '-S', (ToCMakePath $src), '-B', (ToCMakePath $build), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    ('-DCMAKE_MAKE_PROGRAM=' + (ToCMakePath $ninja)),
    ('-DCMAKE_PREFIX_PATH=' + (ToCMakePath $qt)),
    ('-DCMAKE_CXX_COMPILER=' + (ToCMakePath "$llvm\bin\clang++.exe"))) 'cmake configure'
Invoke-Native $cmake @('--build', (ToCMakePath $build)) 'build'
& (Join-Path $build 'sonarqa_check.exe')
exit $LASTEXITCODE

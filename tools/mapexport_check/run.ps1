#requires -Version 5.1
# Builds and runs tools/mapexport_check (the map export's offline check, docs/map-export.md) with the
# same toolchain as scripts\win_build_release.ps1. Exit code 0 when every check passes.
#   powershell -ExecutionPolicy Bypass -File tools\mapexport_check\run.ps1 [-Keep <folder>]
[CmdletBinding()]
param(
    [string] $QtRoot    = 'D:\Qt',
    [string] $QtVersion = '6.8.3',
    [string] $Keep      = ''
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

$build = Join-Path $repo 'build\mapexport_check'
$env:PATH = "$llvm\bin;$qtPrefix\bin;$env:PATH"
function ToCMakePath { param([string] $p) return ($p -replace '\\', '/') }
function Invoke-Native {
    param([string] $Exe, [string[]] $Arguments, [string] $What)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Exe @Arguments } finally { $ErrorActionPreference = $prev }
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit $LASTEXITCODE)" }
}

Invoke-Native $cmake @(
    '-S', (ToCMakePath $src), '-B', (ToCMakePath $build), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    ('-DCMAKE_MAKE_PROGRAM=' + (ToCMakePath $ninja)),
    ('-DCMAKE_PREFIX_PATH=' + (ToCMakePath $qtPrefix)),
    ('-DCMAKE_CXX_COMPILER=' + (ToCMakePath "$llvm\bin\clang++.exe"))
) 'cmake configure'
Invoke-Native $cmake @('--build', (ToCMakePath $build)) 'build'

$env:QT_PLUGIN_PATH = "$qtPrefix\plugins"
$exeArgs = @()
if ($Keep) { $exeArgs = @('--keep', $Keep) }
& (Join-Path $build 'mapexport_check.exe') @exeArgs
exit $LASTEXITCODE

#requires -Version 5.1
[CmdletBinding()]
param(
    [string] $QtRoot    = 'D:\Qt',
    [string] $QtVersion = '6.8.3',
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string] $Config    = 'Release',
    [switch] $Clean,
    [switch] $ConfigureOnly
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

function Require-Path {
    param([string] $Path, [string] $What, [string] $Hint)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$What not found: $Path`n  $Hint"
    }
    return $Path
}

$qtPrefix = Require-Path "$QtRoot\$QtVersion\llvm-mingw_64" 'Qt kit' `
    "python -m aqt install-qt windows desktop $QtVersion win64_llvm_mingw -m qt5compat qtmultimedia qtpositioning qtserialport qtshadertools -O $QtRoot"
$llvm = Require-Path "$QtRoot\Tools\llvm-mingw1706_64" 'LLVM-MinGW toolchain' `
    "python -m aqt install-tool windows desktop tools_llvm_mingw1706 -O $QtRoot"

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

# Build directory name matches scripts\win_deploy_windows_release.bat and the Qt Creator default.
$buildDir = Join-Path $repo ("build\Desktop_Qt_{0}_llvm_mingw_64_bit-{1}" -f ($QtVersion -replace '\.', '_'), $Config)

if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
    Write-Host "removing $buildDir"
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}

$env:PATH = "$llvm\bin;$qtPrefix\bin;$env:PATH"

Write-Host "repo    : $repo"
Write-Host "qt      : $qtPrefix"
Write-Host "toolchain: $llvm"
Write-Host "cmake   : $cmake"
Write-Host "ninja   : $ninja"
Write-Host "build   : $buildDir"

# CMake writes these values verbatim into its own .cmake files, where a backslash
# starts an escape sequence - pass them with forward slashes.
function ToCMakePath { param([string] $p) return ($p -replace '\\', '/') }

# Windows PowerShell 5.1 turns any stderr line from a native exe into a terminating
# error under $ErrorActionPreference='Stop' - CMake writes warnings there. Judge these
# calls by their exit code only.
function Invoke-Native {
    param([string] $Exe, [string[]] $Arguments, [string] $What)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Exe @Arguments } finally { $ErrorActionPreference = $prev }
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit $LASTEXITCODE)" }
}

Invoke-Native $cmake @(
    '-S', (ToCMakePath $repo), '-B', (ToCMakePath $buildDir), '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=$Config",
    ('-DCMAKE_MAKE_PROGRAM=' + (ToCMakePath $ninja)),
    ('-DCMAKE_PREFIX_PATH=' + (ToCMakePath $qtPrefix)),
    ('-DCMAKE_CXX_COMPILER=' + (ToCMakePath "$llvm\bin\clang++.exe")),
    ('-DCMAKE_RC_COMPILER=' + (ToCMakePath "$llvm\bin\windres.exe"))
) 'cmake configure'

if ($ConfigureOnly) { Write-Host 'configure only - stopping here'; exit 0 }

Invoke-Native $cmake @('--build', (ToCMakePath $buildDir)) 'build'

$exe = Join-Path $buildDir 'KoggerApp.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "build reported success but $exe is missing" }
Write-Host ("OK: {0} ({1:N1} MB)" -f $exe, ((Get-Item $exe).Length / 1MB))

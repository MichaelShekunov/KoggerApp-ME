#requires -Version 5.1
<#
.SYNOPSIS
    Parses a log file with src/proto_binnary.h as it is in the tree and as it was at -Base, and fails
    unless both see the same frames: count, bytes, a hash over every frame, the error counters.

.DESCRIPTION
    Builds tools/parse_check twice (CMake + Ninja, the llvm-mingw clang++), each against its copy of the
    header, runs both on -File and compares their output but for the time line, which is printed.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\parse_check\compare.ps1 -File D:\Temp\perf\a.KLF -Base HEAD~1
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $File,
    [string] $Base = 'HEAD',
    [string] $Work = "$env:TEMP\kogger_parse_check",
    [string] $Toolchain = 'D:\Qt\Tools\llvm-mingw1706_64\bin',
    [int]    $Repeats = 3
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$env:PATH = $Toolchain + ';' + $env:PATH
$cmake = (Get-Command cmake.exe -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}

$results = @{}
foreach ($side in 'base', 'tree') {
    $src = Join-Path $Work "$side\src"
    New-Item -ItemType Directory -Force -Path $src | Out-Null
    foreach ($h in 'proto_binnary.h', 'mav_link_conf.h') {
        if ($side -eq 'tree') {
            Copy-Item -LiteralPath (Join-Path $repo "src\$h") -Destination $src -Force
        }
        else {
            $text = & git -C $repo show "${Base}:src/$h"
            if ($LASTEXITCODE -ne 0) { throw "git show ${Base}:src/$h failed" }
            [System.IO.File]::WriteAllText((Join-Path $src $h), ($text -join "`n") + "`n")
        }
    }
    $build = Join-Path $Work "$side\build"
    & $cmake -G Ninja -S (Join-Path $repo 'tools\parse_check') -B $build -DCMAKE_CXX_COMPILER=clang++ `
        -DCMAKE_BUILD_TYPE=Release ("-DPROTO_SRC=" + ($src -replace '\\', '/')) | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "configure ($side) failed" }
    & $cmake --build $build | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "build ($side) failed" }
    $out = & (Join-Path $build 'parse_check.exe') $File $Repeats
    "{0}: {1}" -f $side, ($out -join ' | ')
    $results[$side] = ($out | Where-Object { $_ -notmatch '^parse ' }) -join "`n"
}
if ($results['base'] -ne $results['tree']) { throw 'the two parsers saw different frames' }
'same frames'

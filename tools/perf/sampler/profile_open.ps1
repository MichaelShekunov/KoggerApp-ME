#requires -Version 5.1
<#
.SYNOPSIS
    Opens a file in KoggerApp (as a double click does, in a profile of its own) and samples the stacks
    of all its threads for a while: where the time went, per thread.

.DESCRIPTION
    sampler.cpp (beside this script) is built once into -Work with the llvm-mingw clang++ the app is
    built with; the exe's symbols are listed with llvm-nm (a MinGW exe has no PDB). Every -IntervalMs
    each thread that ran since its last sample is suspended, its stack walked (dbghelp StackWalk64)
    and resumed; the report ranks, per thread, the leaf functions and the functions on the stack.
    -Focus <name part> adds who calls the functions so named. -SampleDelay: seconds from the start
    before sampling (the open starts at once; a zoom later needs open_bench.ps1's -WheelAt and a
    longer delay).

    Used on 2026-10-17 to find where opening a file went (docs/HANDOFF.md, Performance).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\perf\sampler\profile_open.ps1 -Exe D:\Temp\kogger_merge_test\dist\stage\KoggerApp.exe -File D:\Temp\perf\00014_03.05.2025.KLF
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Exe,
    [Parameter(Mandatory = $true)] [string] $File,
    [string] $Profile = 'perf',
    [double] $SampleSeconds = 12,
    [int]    $IntervalMs = 2,
    [double] $SampleDelay = 0.3,
    [string] $Focus = '',
    [string] $PathPrefix,                                        # a build tree's exe needs the Qt bin folder on PATH
    [string] $Toolchain = 'D:\Qt\Tools\llvm-mingw1706_64\bin',   # clang++ and llvm-nm
    [string] $Work = "$env:TEMP\kogger_sampler",
    [string] $Report = "$env:TEMP\kogger_sampler\report.txt"
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$sampler = Join-Path $Work 'sampler.exe'
$source = Join-Path $PSScriptRoot 'sampler.cpp'
if (-not (Test-Path $sampler) -or (Get-Item $source).LastWriteTime -gt (Get-Item $sampler).LastWriteTime) {
    & (Join-Path $Toolchain 'x86_64-w64-mingw32-clang++.exe') -O2 -std=c++17 $source -o $sampler -ldbghelp -lwinmm
    if ($LASTEXITCODE -ne 0) { throw 'building the sampler failed' }
}
$syms = Join-Path $Work ('syms_' + (Get-FileHash -LiteralPath $Exe -Algorithm MD5).Hash.Substring(0, 12) + '.txt')
if (-not (Test-Path $syms)) {
    & (Join-Path $Toolchain 'llvm-nm.exe') --defined-only -C $Exe | Set-Content -LiteralPath $syms -Encoding ASCII
    if ($LASTEXITCODE -ne 0) { throw 'listing the symbols failed' }
}

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe; $psi.UseShellExecute = $false; $psi.WorkingDirectory = Split-Path $Exe
$psi.Arguments = '"' + $File + '"'
if ($PathPrefix) { $psi.EnvironmentVariables['PATH'] = $PathPrefix + ';' + $env:PATH }
$psi.EnvironmentVariables['KOGGER_PROFILE'] = $Profile
$psi.EnvironmentVariables['KOGGER_UI_PROBE'] = Join-Path $Work 'probe'
$psi.EnvironmentVariables['KOGGER_UI_PROBE_DELAY_MS'] = [string][int](($SampleDelay + $SampleSeconds + 3) * 1000)
$psi.EnvironmentVariables['KOGGER_UI_PROBE_QUIT'] = '1'
$app = [System.Diagnostics.Process]::Start($psi)
Start-Sleep -Milliseconds ([int]($SampleDelay * 1000))
& $sampler $app.Id $SampleSeconds $IntervalMs $syms 'KoggerApp.exe' $Focus > $Report
if (-not $app.WaitForExit(120000)) { throw 'the app did not quit' }
"report: $Report"

#requires -Version 5.1
<#
.SYNOPSIS
    Builds and runs tools/codec_check, the offline check of the echogram codecs the app decodes (KEC, echocodec
    V3) against the codec repository's test vectors, with the toolchain of scripts\win_build_release.ps1.

.DESCRIPTION
    Exit code 0 when every check passes. With -Mutants it also builds the check against copies of src/codec with
    one fault each and expects every one of them to fail it; exit code 1 when one passes.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\codec_check\run.ps1 -Mutants
#>
[CmdletBinding()]
param(
    [string] $QtRoot = 'D:\Qt',
    [string] $Vectors = 'D:\YandexDisk\MikroPascalProjects\KoggerEchoCodec\test\app_vectors',
    [switch] $Mutants
)

$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot
$repo = Split-Path -Parent (Split-Path -Parent $src)
$llvm = "$QtRoot\Tools\llvm-mingw1706_64"
if (-not (Test-Path -LiteralPath $llvm)) { throw "not found: $llvm (see docs\build-windows-cli.md)" }
if (-not (Test-Path -LiteralPath $Vectors)) { throw "test vectors not found: $Vectors" }

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
    param([string] $BuildDir, [string] $CodecDir)
    $cfg = @(
        '-S', (ToCMakePath $src), '-B', (ToCMakePath $BuildDir), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
        ('-DCMAKE_MAKE_PROGRAM=' + (ToCMakePath $ninja)),
        ('-DCMAKE_C_COMPILER=' + (ToCMakePath "$llvm\bin\clang.exe")),
        ('-DCMAKE_CXX_COMPILER=' + (ToCMakePath "$llvm\bin\clang++.exe")))
    if ($CodecDir) { $cfg += ('-DCODEC_SRC=' + (ToCMakePath $CodecDir)) }
    Invoke-Native $cmake $cfg 'cmake configure'
    Invoke-Native $cmake @('--build', (ToCMakePath $BuildDir)) 'build'
    return (Join-Path $BuildDir 'codec_check.exe')
}

$exe = Build-Check (Join-Path $repo 'build\codec_check') ''
& $exe $Vectors
$code = $LASTEXITCODE
if ($code -ne 0 -or -not $Mutants) { exit $code }

# One fault each, in a copy of src/codec. The text to replace must be there: a mutant that changes nothing would
# pass and look like a gap in the check.
$faults = @(
    @{ Name = 'blocks one bin short';                  File = 'chart_v3.cpp'; From = 'const int start = blk * CCH_BLOCK_BINS;'; To = 'const int start = blk * (CCH_BLOCK_BINS - 1);' },
    @{ Name = 'no fill from the previous ping';        File = 'chart_v3.cpp'; From = 'if (fill) {'; To = 'if (false) {' },
    @{ Name = 'a late block not recognised';           File = 'chart_v3.cpp'; From = 'if (havePrev_ && k == prevKey_ && !(active_ && k == key_))'; To = 'if (false)' },
    @{ Name = 'progressive left as bins';              File = 'chart_v3.cpp'; From = 'if (bands_.size() <= 1)'; To = 'if (true)' },
    @{ Name = 'bands start at the running sum';        File = 'chart_codec_host.c'; From = 'bands[k++].start_mm = lo;'; To = 'bands[k].start_mm = k ? bands[k - 1].start_mm + (uint32_t)b[k - 1].count * b[k - 1].resol_mm : 0; ++k;' },
    @{ Name = 'an unknown mode taken as progressive';  File = 'chart_codec_host.c'; From = 'if (mode > CHART_MODE_PROGRESSIVE || max <= 0)'; To = 'if (max <= 0)' }
)

$utf8 = New-Object System.Text.UTF8Encoding $false
$codecDir = Join-Path $repo 'src\codec'
$files = @{}
foreach ($f in Get-ChildItem -LiteralPath $codecDir -File) {
    $files[$f.Name] = [System.IO.File]::ReadAllText($f.FullName, $utf8).Replace("`r`n", "`n")
}
$missed = 0
$i = 0
foreach ($m in $faults) {
    ++$i
    if (-not $files[$m.File].Contains($m.From)) { throw "mutant '$($m.Name)': its text is not in $($m.File)" }
    $dir = Join-Path $repo ("build\codec_mutant_{0}\src" -f $i)
    New-Item -ItemType Directory -Force $dir | Out-Null
    foreach ($f in $files.Keys) {
        $text = if ($f -eq $m.File) { $files[$f].Replace($m.From, $m.To) } else { $files[$f] }
        [System.IO.File]::WriteAllText((Join-Path $dir $f), $text, $utf8)
    }
    $mexe = Build-Check (Join-Path $repo ("build\codec_mutant_{0}\build" -f $i)) $dir
    $out = & $mexe $Vectors
    $failed = @($out | Where-Object { $_ -like 'FAIL *' })
    if ($LASTEXITCODE -ne 0 -and $failed.Count -gt 0) {
        Write-Host ("mutant {0} ({1}): caught by {2} check(s), first: {3}" -f $i, $m.Name, $failed.Count, $failed[0])
    } else {
        Write-Host ("mutant {0} ({1}): MISSED" -f $i, $m.Name)
        ++$missed
    }
}
Write-Host ("codec_check mutants: {0} of {1} caught" -f ($faults.Count - $missed), $faults.Count)
exit ([int]($missed -gt 0))

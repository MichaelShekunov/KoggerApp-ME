#requires -Version 5.1
<#
.SYNOPSIS
    Builds and runs tools/linktrack_check, the offline check of src/scene3d/link_track_geometry.h (the triangles of
    the boat's track by link quality over the 3D view), with the toolchain of scripts\win_build_release.ps1.

.DESCRIPTION
    Exit code 0 when every check passes. With -Mutants it also builds the check against copies of the header with one
    fault each and expects every one of them to fail it; exit code 1 when one passes.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\linktrack_check\run.ps1 -Mutants
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
function Build-Check {
    param([string] $BuildDir, [string] $SrcDir)
    $cfg = @(
        '-S', (ToCMakePath $src), '-B', (ToCMakePath $BuildDir), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
        ('-DCMAKE_MAKE_PROGRAM=' + (ToCMakePath $ninja)),
        ('-DCMAKE_PREFIX_PATH=' + (ToCMakePath $qt)),
        ('-DCMAKE_CXX_COMPILER=' + (ToCMakePath "$llvm\bin\clang++.exe")))
    if ($SrcDir) { $cfg += ('-DLINKTRACK_SRC=' + (ToCMakePath $SrcDir)) }
    Invoke-Native $cmake $cfg 'cmake configure'
    Invoke-Native $cmake @('--build', (ToCMakePath $BuildDir)) 'build'
    return (Join-Path $BuildDir 'linktrack_check.exe')
}

$exe = Build-Check (Join-Path $repo 'build\linktrack_check') ''
& $exe
$code = $LASTEXITCODE
if ($code -ne 0 -or -not $Mutants) { exit $code }

# One fault each, in a copy of the header. The text to replace must be there: a mutant that changes nothing would
# pass and look like a gap in the check.
$faults = @(
    @{ Name = 'no dashes for a loss';                From = 'if (q == dashed) {'; To = 'if (false) {' },
    @{ Name = 'the ends not carried half a width';   From = 'band(a - d * half, b + d * half, nrm, c);'; To = 'band(a, b, nrm, c);' },
    @{ Name = 'colours not premultiplied';           From = 'return static_cast<unsigned char>((v * a + 127) / 255);'; To = 'return static_cast<unsigned char>(v);' },
    @{ Name = 'no fringe on one side';               From = 'quad(inner, solid, outer, clear);'; To = '' },
    @{ Name = 'a stretch in its start''s colour';    From = 'const int q = quality[i];'; To = 'const int q = quality[i - 1];' },
    @{ Name = 'nothing culled';                      From = 'const QRectF room = clip.adjusted(-lineWidth, -lineWidth, lineWidth, lineWidth);'; To = 'const QRectF room(-1e9, -1e9, 2e9, 2e9);' },
    @{ Name = 'not culled at the bottom';            From = '|| std::min(a.y(), b.y()) > room.bottom()'; To = '' },
    @{ Name = 'an end not finite drawn';             From = 'if (!std::isfinite(a.x()) || !std::isfinite(a.y()) || !std::isfinite(b.x()) || !std::isfinite(b.y())) {'; To = 'if (false) {' },
    @{ Name = 'culled at the edge, not a width out'; From = 'clip.adjusted(-lineWidth, -lineWidth, lineWidth, lineWidth)'; To = 'clip' },
    @{ Name = 'a dash as long as its gap and more';  From = 'const double dash = 2.0 * lineWidth, step = 4.0 * lineWidth;'; To = 'const double dash = 3.0 * lineWidth, step = 4.0 * lineWidth;' }
)

$utf8 = New-Object System.Text.UTF8Encoding $false
$header = [System.IO.File]::ReadAllText((Join-Path $repo 'src\scene3d\link_track_geometry.h'), $utf8).Replace("`r`n", "`n")
$missed = 0
$i = 0
foreach ($m in $faults) {
    ++$i
    if (-not $header.Contains($m.From)) { throw "mutant '$($m.Name)': its text is not in link_track_geometry.h" }
    $dir = Join-Path $repo ("build\linktrack_mutant_{0}\src" -f $i)
    New-Item -ItemType Directory -Force $dir | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $dir 'link_track_geometry.h'), $header.Replace($m.From, $m.To), $utf8)
    $mexe = Build-Check (Join-Path $repo ("build\linktrack_mutant_{0}\build" -f $i)) $dir
    $out = & $mexe
    $failed = @($out | Where-Object { $_ -like 'FAIL *' })
    if ($LASTEXITCODE -ne 0 -and $failed.Count -gt 0) {
        Write-Host ("mutant {0} ({1}): caught by {2} check(s), first: {3}" -f $i, $m.Name, $failed.Count, $failed[0])
    } else {
        Write-Host ("mutant {0} ({1}): MISSED" -f $i, $m.Name)
        ++$missed
    }
}
Write-Host ("linktrack_check mutants: {0} of {1} caught" -f ($faults.Count - $missed), $faults.Count)
exit ([int]($missed -gt 0))

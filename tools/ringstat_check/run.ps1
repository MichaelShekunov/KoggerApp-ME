#requires -Version 5.1
<#
.SYNOPSIS
    Builds and runs tools/ringstat_check, the offline check of a GODIO ring board's sync counters on the protocol
    side (ID_RING_STAT, IDBinRingStat in src/id_binnary.*), with the toolchain of scripts\win_build_release.ps1.

.DESCRIPTION
    Exit code 0 when every check passes. With -Mutants it also builds the check against copies of
    id_binnary.h/.cpp with one fault each and expects every one of them to fail it; exit code 1 when one passes.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\ringstat_check\run.ps1 -Mutants
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
        ('-DCMAKE_C_COMPILER=' + (ToCMakePath "$llvm\bin\clang.exe")),
        ('-DCMAKE_CXX_COMPILER=' + (ToCMakePath "$llvm\bin\clang++.exe")))
    if ($SrcDir) { $cfg += ('-DRINGSTAT_SRC=' + (ToCMakePath $SrcDir)) }
    Invoke-Native $cmake $cfg 'cmake configure'
    Invoke-Native $cmake @('--build', (ToCMakePath $BuildDir)) 'build'
    return (Join-Path $BuildDir 'ringstat_check.exe')
}

$exe = Build-Check (Join-Path $repo 'build\ringstat_check') ''
& $exe
$code = $LASTEXITCODE
if ($code -ne 0 -or -not $Mutants) { exit $code }

# One fault each, in a copy of id_binnary.h/.cpp. The text to replace must be there: a mutant that changes nothing
# would pass and look like a gap in the check.
$faults = @(
    @{ Name = 'role and upstream read the other way';   File = 'id_binnary.cpp'; From = "s.role = proto.read<U1>();`n    s.upstream = proto.read<U1>();"; To = "s.upstream = proto.read<U1>();`n    s.role = proto.read<U1>();" },
    @{ Name = 'the spare byte not skipped';             File = 'id_binnary.cpp'; From = "    proto.read<U1>();   // spare`n"; To = '' },
    @{ Name = 'the guard read as 16 bits';              File = 'id_binnary.cpp'; From = 's.guardUs = proto.read<U4>();'; To = 's.guardUs = proto.read<U2>();' },
    @{ Name = 'a short frame taken';                    File = 'id_binnary.cpp'; From = 'if (proto.readAvailable() < kFrameBytes) {'; To = 'if (proto.readAvailable() < 28) {' },
    @{ Name = 'any version taken';                      File = 'id_binnary.cpp'; From = "if (proto.ver() != v0) {`n        return respErrorVersion;`n    }`n    if (proto.readAvailable() < kFrameBytes)"; To = 'if (proto.readAvailable() < kFrameBytes)' },
    @{ Name = 'skipped and timeouts read the other way'; File = 'id_binnary.cpp'; From = "s.timeouts = proto.read<U4>();`n    s.skipped = proto.read<U4>();"; To = "s.skipped = proto.read<U4>();`n    s.timeouts = proto.read<U4>();" },
    @{ Name = 'the reset without the key';              File = 'id_binnary.cpp'; From = "    appendKey(id_out);`n    id_out.end();`n    emit binFrameOut(id_out);   // not tracked"; To = "    id_out.end();`n    emit binFrameOut(id_out);   // not tracked" },
    @{ Name = 'the reset as version 1';                 File = 'id_binnary.cpp'; From = "id_out.create(SETTING, v0, id(), m_address);`n    appendKey(id_out);`n    id_out.end();`n    emit binFrameOut(id_out);   // not tracked"; To = "id_out.create(SETTING, v1, id(), m_address);`n    appendKey(id_out);`n    id_out.end();`n    emit binFrameOut(id_out);   // not tracked" },
    @{ Name = 'asked on its own at connect';            File = 'id_binnary.h';   From = 'void requestAll() override {}   // nothing on its own: read() when asked'; To = 'void requestAll() override { simpleRequest(v0); }' }
)

$utf8 = New-Object System.Text.UTF8Encoding $false
$files = @{}
foreach ($f in 'id_binnary.h', 'id_binnary.cpp') {
    $files[$f] = [System.IO.File]::ReadAllText((Join-Path $repo "src\$f"), $utf8).Replace("`r`n", "`n")
}
$missed = 0
$i = 0
foreach ($m in $faults) {
    ++$i
    if (-not $files[$m.File].Contains($m.From)) { throw "mutant '$($m.Name)': its text is not in $($m.File)" }
    $dir = Join-Path $repo ("build\ringstat_mutant_{0}\src" -f $i)
    New-Item -ItemType Directory -Force $dir | Out-Null
    foreach ($f in $files.Keys) {
        $text = if ($f -eq $m.File) { $files[$f].Replace($m.From, $m.To) } else { $files[$f] }
        [System.IO.File]::WriteAllText((Join-Path $dir $f), $text, $utf8)
    }
    $mexe = Build-Check (Join-Path $repo ("build\ringstat_mutant_{0}\build" -f $i)) $dir
    $out = & $mexe
    $failed = @($out | Where-Object { $_ -like 'FAIL *' })
    if ($LASTEXITCODE -ne 0 -and $failed.Count -gt 0) {
        Write-Host ("mutant {0} ({1}): caught by {2} check(s), first: {3}" -f $i, $m.Name, $failed.Count, $failed[0])
    } else {
        Write-Host ("mutant {0} ({1}): MISSED" -f $i, $m.Name)
        ++$missed
    }
}
Write-Host ("ringstat_check mutants: {0} of {1} caught" -f ($faults.Count - $missed), $faults.Count)
exit ([int]($missed -gt 0))

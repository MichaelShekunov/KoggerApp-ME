#requires -Version 5.1
<#
.SYNOPSIS
    Builds and runs tools/servo_check, the offline check of the servo's manual mode on the protocol side
    (ID_SERVO_CONTROL, IDBinServoControl in src/id_binnary.*), with the toolchain of scripts\win_build_release.ps1.

.DESCRIPTION
    Exit code 0 when every check passes. With -Mutants it also builds the check against copies of
    id_binnary.h/.cpp with one fault each and expects every one of them to fail it; exit code 1 when one passes.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\servo_check\run.ps1 -Mutants
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
    if ($SrcDir) { $cfg += ('-DSERVO_SRC=' + (ToCMakePath $SrcDir)) }
    Invoke-Native $cmake $cfg 'cmake configure'
    Invoke-Native $cmake @('--build', (ToCMakePath $BuildDir)) 'build'
    return (Join-Path $BuildDir 'servo_check.exe')
}

$exe = Build-Check (Join-Path $repo 'build\servo_check') ''
& $exe
$code = $LASTEXITCODE
if ($code -ne 0 -or -not $Mutants) { exit $code }

# One fault each, in a copy of id_binnary.h/.cpp. The text to replace must be there: a mutant that changes nothing
# would pass and look like a gap in the check.
$faults = @(
    @{ Name = 'manual on another bit';                    File = 'id_binnary.h';   From = 'GenManual = 0x0002'; To = 'GenManual = 0x0004' },
    @{ Name = 'the angle not sent on the way in';         File = 'id_binnary.cpp'; From = "m_general |= GenManual;`n        m_centerDeg = m_manualDeg;"; To = 'm_general |= GenManual;' },
    @{ Name = 'manual starts at 0, not the scan center';  File = 'id_binnary.cpp'; From = 'm_manualDeg = clampManual(m_centerDeg);'; To = 'm_manualDeg = 0;' },
    @{ Name = 'the scan center not sent on the way out';  File = 'id_binnary.cpp'; From = "m_general &= ~GenManual;`n        m_centerDeg = m_scanCenterDeg;`n    }`n    send(false);"; To = "m_general &= ~GenManual;`n    }`n    send(false);" },
    @{ Name = 'the scan center sent in manual mode';      File = 'id_binnary.h';   From = "m_scanCenterDeg = v;`n            return;"; To = 'm_scanCenterDeg = v;' },
    @{ Name = 'the scan center shown as center_deg';      File = 'id_binnary.h';   From = 'return manual() ? m_scanCenterDeg : m_centerDeg;'; To = 'return m_centerDeg;' },
    @{ Name = 'the angle not cut to the range';           File = 'id_binnary.cpp'; From = 'return static_cast<S2>(qBound(-half, int(deg), half));'; To = 'return deg;' },
    @{ Name = 'manual frames tracked';                    File = 'id_binnary.cpp'; From = "if (tracked) {`n        hashBinFrameOut(id_out);"; To = "if (true) {`n        hashBinFrameOut(id_out);" },
    @{ Name = 'the read-back angle not taken';            File = 'id_binnary.cpp'; From = "if (manual()) {`n            m_manualDeg = m_centerDeg;`n        }"; To = '' },
    @{ Name = 'a refusal leaves the angle as center';     File = 'id_binnary.cpp'; From = "    m_general &= ~GenManual;`n    m_centerDeg = m_scanCenterDeg;`n}"; To = "    m_general &= ~GenManual;`n}" },
    @{ Name = 'the frequency answer not taken';           File = 'id_binnary.cpp'; From = 'm_pwmHz = proto.read<U2>();'; To = 'proto.read<U2>();' },
    @{ Name = 'the frequency sent as v0';                 File = 'id_binnary.cpp'; From = "id_out.create(SETTING, v1, id(), m_address);`n    id_out.write<U2>(hz);"; To = "id_out.create(SETTING, v0, id(), m_address);`n    id_out.write<U2>(hz);" },
    @{ Name = 'the frequency not read back';              File = 'id_binnary.cpp'; From = "emit binFrameOut(id_out);`n`n    simpleRequest(v1);"; To = 'emit binFrameOut(id_out);' },
    @{ Name = 'the frequency not read at the start';      File = 'id_binnary.h';   From = "simpleRequest(v0);`n        simpleRequest(v1);"; To = 'simpleRequest(v0);' },
    @{ Name = 'the period one microsecond long';          File = 'id_binnary.h';   From = 'return hz > 0 ? 1000000 / hz : 0;'; To = 'return hz > 0 ? 1000000 / hz + 1 : 0;' }
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
    $dir = Join-Path $repo ("build\servo_mutant_{0}\src" -f $i)
    New-Item -ItemType Directory -Force $dir | Out-Null
    foreach ($f in $files.Keys) {
        $text = if ($f -eq $m.File) { $files[$f].Replace($m.From, $m.To) } else { $files[$f] }
        [System.IO.File]::WriteAllText((Join-Path $dir $f), $text, $utf8)
    }
    $mexe = Build-Check (Join-Path $repo ("build\servo_mutant_{0}\build" -f $i)) $dir
    $out = & $mexe
    $failed = @($out | Where-Object { $_ -like 'FAIL *' })
    if ($LASTEXITCODE -ne 0 -and $failed.Count -gt 0) {
        Write-Host ("mutant {0} ({1}): caught by {2} check(s), first: {3}" -f $i, $m.Name, $failed.Count, $failed[0])
    } else {
        Write-Host ("mutant {0} ({1}): MISSED" -f $i, $m.Name)
        ++$missed
    }
}
Write-Host ("servo_check mutants: {0} of {1} caught" -f ($faults.Count - $missed), $faults.Count)
exit ([int]($missed -gt 0))

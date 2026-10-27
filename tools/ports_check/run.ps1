#requires -Version 5.1
<#
.SYNOPSIS
    Builds and runs tools/ports_check, the offline check of a Recorder's port table on the protocol side
    (ID_BUS, ID_PIN; IDBinBus, IDBinPin in src/id_binnary.*), with the toolchain of scripts\win_build_release.ps1.

.DESCRIPTION
    Exit code 0 when every check passes. With -Mutants it also builds the check against copies of
    id_binnary.h/.cpp with one fault each and expects every one of them to fail it; exit code 1 when one passes.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\ports_check\run.ps1 -Mutants
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
    if ($SrcDir) { $cfg += ('-DPORTS_SRC=' + (ToCMakePath $SrcDir)) }
    Invoke-Native $cmake $cfg 'cmake configure'
    Invoke-Native $cmake @('--build', (ToCMakePath $BuildDir)) 'build'
    return (Join-Path $BuildDir 'ports_check.exe')
}

$exe = Build-Check (Join-Path $repo 'build\ports_check') ''
& $exe
$code = $LASTEXITCODE
if ($code -ne 0 -or -not $Mutants) { exit $code }

# One fault each, in a copy of id_binnary.h/.cpp. The text to replace must be there: a mutant that changes nothing
# would pass and look like a gap in the check.
$faults = @(
    @{ Name = 'function and its saved one swapped';  File = 'id_binnary.cpp'; From = "b.function = proto.read<U1>();`n    b.functionSaved = proto.read<U1>();"; To = "b.functionSaved = proto.read<U1>();`n    b.function = proto.read<U1>();" },
    @{ Name = 'baud read as 16 bits';                File = 'id_binnary.cpp'; From = 'b.baud = proto.read<U4>();'; To = 'b.baud = proto.read<U2>();' },
    @{ Name = 'a bus without its label taken';       File = 'id_binnary.cpp'; From = "    if (!readLabel(proto, &b.label)) {`n        return respErrorPayload;`n    }"; To = '    readLabel(proto, &b.label);' },
    @{ Name = 'role caps read as 8 bits';            File = 'id_binnary.cpp'; From = 'p.roleCaps = proto.read<U4>();'; To = 'p.roleCaps = proto.read<U1>();' },
    @{ Name = 'v1 entries of 5 bytes';               File = 'id_binnary.cpp'; From = 'const S4 value = proto.read<S4>();'; To = 'const S4 value = proto.read<S2>();' },
    @{ Name = 'v1 invents undescribed pins';         File = 'id_binnary.cpp'; From = "            if (it != m_pins.end()) {   // a pin not described yet waits for its v0`n                it->role = role;`n                it->value = value;`n            }"; To = "            m_pins[id].role = role;`n            m_pins[id].value = value;" },
    @{ Name = 'the pin request without its id';      File = 'id_binnary.cpp'; From = "    id_out.write<U1>(pin);`n"; To = '' },
    @{ Name = 'values asked as v0';                  File = 'id_binnary.cpp'; From = '    simpleRequest(v1);'; To = '    simpleRequest(v0);' },
    @{ Name = 'v2 log and to-host swapped';          File = 'id_binnary.cpp'; From = "            f.log = proto.read<U1>();`n            f.toHost = proto.read<U1>();"; To = "            f.toHost = proto.read<U1>();`n            f.log = proto.read<U1>();" },
    @{ Name = 'a function taken in any version';     File = 'id_binnary.cpp'; From = "        if (functionVersion(function) != ver) {`n            return respErrorPayload;`n        }`n"; To = '' },
    @{ Name = 'v3 moves the rate in use (old meaning)'; File = 'id_binnary.cpp'; From = "        m_func[function] = f;`n        return respOk;"; To = "        m_func[function] = f;`n        for (Bus& b : m_buses) { if (b.function == function) { b.baud = f.baud; } }`n        return respOk;" },
    @{ Name = 'telemetry unread sent with to-host 1'; File = 'id_binnary.cpp'; From = 'id_out.write<U1>(f.valid ? f.toHost : U1(0));'; To = 'id_out.write<U1>(f.toHost);' },
    @{ Name = 'the swap without its UART';           File = 'id_binnary.cpp'; From = "    id_out.write<U1>(function);`n    id_out.write<U1>(uart);`n"; To = "    id_out.write<U1>(function);`n" },
    @{ Name = 'auto not among the rates';            File = 'id_binnary.cpp'; From = '{ 0, 9600, 19200'; To = '{ 9600, 19200' },
    @{ Name = 'settings asked as v0';                File = 'id_binnary.cpp'; From = 'id_out.create(GETTING, functionVersion(function), id(), m_address);'; To = 'id_out.create(GETTING, v0, id(), m_address);' },
    @{ Name = 'navigation rate dropped';             File = 'id_binnary.cpp'; From = "            f.rateHz = proto.read<U1>();`n"; To = '' }
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
    $dir = Join-Path $repo ("build\ports_mutant_{0}\src" -f $i)
    New-Item -ItemType Directory -Force $dir | Out-Null
    foreach ($f in $files.Keys) {
        $text = if ($f -eq $m.File) { $files[$f].Replace($m.From, $m.To) } else { $files[$f] }
        [System.IO.File]::WriteAllText((Join-Path $dir $f), $text, $utf8)
    }
    $mexe = Build-Check (Join-Path $repo ("build\ports_mutant_{0}\build" -f $i)) $dir
    $out = & $mexe
    $failed = @($out | Where-Object { $_ -like 'FAIL *' })
    if ($LASTEXITCODE -ne 0 -and $failed.Count -gt 0) {
        Write-Host ("mutant {0} ({1}): caught by {2} check(s), first: {3}" -f $i, $m.Name, $failed.Count, $failed[0])
    } else {
        Write-Host ("mutant {0} ({1}): MISSED" -f $i, $m.Name)
        ++$missed
    }
}
Write-Host ("ports_check mutants: {0} of {1} caught" -f ($faults.Count - $missed), $faults.Count)
exit ([int]($missed -gt 0))

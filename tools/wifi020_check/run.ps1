#requires -Version 5.1
<#
.SYNOPSIS
    Builds and runs tools/wifi020_check, the offline check of the Wi-Fi module's firmware 0.20 additions in the app
    (the radio switched off: ID_WIFI_RF, IDBinWifiRf, WifiRf; the retransmissions in the link rate report:
    WifiLinkRate, WifiRetryWindow), with the toolchain of scripts\win_build_release.ps1.

.DESCRIPTION
    Exit code 0 when every check passes. With -Mutants it also builds the check against copies of
    id_binnary.h/.cpp and device/wifi_ops.h/.cpp with one fault each and expects every one of them to fail it; exit
    code 1 when one passes.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\wifi020_check\run.ps1 -Mutants
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
    if ($SrcDir) { $cfg += ('-DWIFI020_SRC=' + (ToCMakePath $SrcDir)) }
    Invoke-Native $cmake $cfg 'cmake configure'
    Invoke-Native $cmake @('--build', (ToCMakePath $BuildDir)) 'build'
    return (Join-Path $BuildDir 'wifi020_check.exe')
}

$exe = Build-Check (Join-Path $repo 'build\wifi020_check') ''
& $exe
$code = $LASTEXITCODE
if ($code -ne 0 -or -not $Mutants) { exit $code }

# One fault each, in a copy of the sources. The text to replace must be there: a mutant that changes nothing would
# pass and look like a gap in the check.
$cpp = 'id_binnary.cpp'
$ops = 'device\wifi_ops.h'
$faults = @(
    @{ Name = 'no key in the request';               File = $cpp; From = "    appendKey(out);`n    out.write<U1>(on);"; To = '    out.write<U1>(on);' },
    @{ Name = 'off sent as on';                      File = $cpp; From = "void IDBinWifiRf::switchOff(uint16_t seconds) {`n    send(0, seconds);"; To = "void IDBinWifiRf::switchOff(uint16_t seconds) {`n    send(1, seconds);" },
    @{ Name = 'the seconds big-endian';              File = $cpp; From = "    out.write<U1>(on);`n    out.write<U2>(seconds);"; To = "    out.write<U1>(on);`n    out.write<U1>(U1(seconds >> 8));`n    out.write<U1>(U1(seconds & 0xFF));" },
    @{ Name = 'the request as version 1';            File = $cpp; From = "    out.create(SETTING, v0, id(), m_address);`n    appendKey(out);`n    out.write<U1>(on);"; To = "    out.create(SETTING, v1, id(), m_address);`n    appendKey(out);`n    out.write<U1>(on);" },
    @{ Name = 'the on byte not read';                File = $cpp; From = 'state_.on = on == 1;'; To = 'state_.on = true;' },
    @{ Name = 'a SETTING taken as the state';        File = $cpp; From = "    if (proto.type() != CONTENT)`n        return respErrorType;`n    if (proto.ver() != v0)`n        return respErrorVersion;`n    if (proto.readAvailable() < 3)"; To = "    if (proto.ver() != v0)`n        return respErrorVersion;`n    if (proto.readAvailable() < 3)" },
    @{ Name = 'on = 2 taken';                        File = $cpp; From = "    if (on > 1)`n        return respErrorPayload;`n"; To = '' },
    @{ Name = 'seconds kept while on';               File = $cpp; From = 'state_.secondsLeft = state_.on ? 0 : left;'; To = 'state_.secondsLeft = left;' },
    @{ Name = 'a 2-byte state taken';                File = $cpp; From = "        return respErrorVersion;`n    if (proto.readAvailable() < 3)"; To = "        return respErrorVersion;`n    if (proto.readAvailable() < 2)" },
    @{ Name = 'supported from 0.19';                 File = $ops; From = 'return (fwMajor > 0 || fwMinor >= 20) ? 1 : 0;'; To = 'return (fwMajor > 0 || fwMinor >= 19) ? 1 : 0;' },
    @{ Name = 'over the network up to 601 s';        File = $ops; From = 'seconds > (wired ? kWiredMaxS : kNetworkMaxS))'; To = 'seconds > (wired ? kWiredMaxS : kNetworkMaxS + 1))' },
    @{ Name = 'over the network until on';           File = $ops; From = 'return wired || seconds > 0;'; To = 'return true;' },
    @{ Name = 'the seconds left rounded down';       File = $ops; From = 'return left <= 0 ? 0 : int((left + 999) / 1000);'; To = 'return left <= 0 ? 0 : int(left / 1000);' },
    @{ Name = 'a status within 1 s of the answer';   File = $ops; From = 'if (valid && on == flagOff && sinceMs > kSettleMs)'; To = 'if (valid && on == flagOff)' },
    @{ Name = 'retransmissions big-endian';          File = $ops; From = 'out.retries = u8(16 + 13 * n) | (u8(17 + 13 * n) << 8);'; To = 'out.retries = (u8(16 + 13 * n) << 8) | u8(17 + 13 * n);' },
    @{ Name = 'retransmissions read before the bounds'; File = $ops; From = 'out.retries = u8(16 + 13 * n) | (u8(17 + 13 * n) << 8);'; To = 'out.retries = u8(16 + 9 * n) | (u8(17 + 9 * n) << 8);' },
    @{ Name = 'eleven reports kept';                 File = $ops; From = 'while (e.size() > kKeep ||'; To = 'while (e.size() > kKeep + 1 ||' },
    @{ Name = 'old reports summed';                  File = $ops; From = "            if (nowMs - e.atMs > kMaxAgeMs)`n                continue;`n            s.retries += e.retries;"; To = '            s.retries += e.retries;' },
    @{ Name = 'more retries than frames taken';      File = $ops; From = 'if (frames <= 0 || retries < 0 || retries > frames)'; To = 'if (frames <= 0 || retries < 0)' },
    @{ Name = 'the radio-off flag on bit4';          File = 'id_binnary.h'; From = 'FlagRadioOff = 1 << 3'; To = 'FlagRadioOff = 1 << 4' },
    @{ Name = 'LR 0x1A not taken for 500';           File = $ops; From = 'if (r.kind == KindLr && r.kbps == 0 && r.code == kLrCode500)'; To = 'if (false)' },
    @{ Name = 'any LR code taken for 500';           File = $ops; From = 'r.kbps == 0 && r.code == kLrCode500)'; To = 'r.kbps == 0)' },
    @{ Name = 'code 0x1A of any kind taken for 500'; File = $ops; From = 'if (r.kind == KindLr && r.kbps == 0 && r.code'; To = 'if (r.kbps == 0 && r.code' },
    @{ Name = 'LR shown without its number';         File = $ops; From = "case KindLr:`n            return r.kbps ? mbit"; To = "case KindLr:`n            return false ? mbit" }
)

$utf8 = New-Object System.Text.UTF8Encoding $false
$files = @{}
foreach ($f in 'id_binnary.h', 'id_binnary.cpp', 'device\wifi_ops.h', 'device\wifi_ops.cpp') {
    $files[$f] = [System.IO.File]::ReadAllText((Join-Path $repo "src\$f"), $utf8).Replace("`r`n", "`n")
}
$missed = 0
$i = 0
foreach ($m in $faults) {
    ++$i
    if (-not $files[$m.File].Contains($m.From)) { throw "mutant '$($m.Name)': its text is not in $($m.File)" }
    $dir = Join-Path $repo ("build\wifi020_mutant_{0}\src" -f $i)
    New-Item -ItemType Directory -Force (Join-Path $dir 'device') | Out-Null
    foreach ($f in $files.Keys) {
        $text = if ($f -eq $m.File) { $files[$f].Replace($m.From, $m.To) } else { $files[$f] }
        [System.IO.File]::WriteAllText((Join-Path $dir $f), $text, $utf8)
    }
    $mexe = Build-Check (Join-Path $repo ("build\wifi020_mutant_{0}\build" -f $i)) $dir
    $out = & $mexe
    $failed = @($out | Where-Object { $_ -like 'FAIL *' })
    if ($LASTEXITCODE -ne 0 -and $failed.Count -gt 0) {
        Write-Host ("mutant {0} ({1}): caught by {2} check(s), first: {3}" -f $i, $m.Name, $failed.Count, $failed[0])
    } else {
        Write-Host ("mutant {0} ({1}): MISSED" -f $i, $m.Name)
        ++$missed
    }
}
Write-Host ("wifi020_check mutants: {0} of {1} caught" -f ($faults.Count - $missed), $faults.Count)
exit ([int]($missed -gt 0))

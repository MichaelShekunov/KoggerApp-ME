#requires -Version 5.1
<#
.SYNOPSIS
    Builds and runs tools/wifi022_check, the offline check of the Wi-Fi module's profiles in the app (firmware 0.22,
    ID_WIFI_PROFILE: WifiProfile, IDBinWifiProfile, WifiProfileUpload, WifiProfileDiff, WifiProfileStore), with the
    toolchain of scripts\win_build_release.ps1.

.DESCRIPTION
    Exit code 0 when every check passes.
    -Mutants: also builds the check against copies of the sources with one fault each and expects every one of them to
    fail it; exit code 1 when one passes.
    -ModuleRepo <the module project's folder>: also encodes the module project's three presets (profiles\*.json) with
    the module's own host\sbpframe.py and with the app, and compares the blocks: the app reading the module's file, and
    the app's built-in profile with that file's passwords put back. Only "match" or "DIFFERS" and the length are
    printed: the blocks hold the passwords.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\wifi022_check\run.ps1 -Mutants -ModuleRepo D:\YandexDisk\DelphiProjects\ESP32_WiFi
#>
[CmdletBinding()]
param(
    [string] $QtRoot = 'D:\Qt',
    [switch] $Mutants,
    [string] $ModuleRepo
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
    if ($SrcDir) { $cfg += ('-DWIFI022_SRC=' + (ToCMakePath $SrcDir)) }
    Invoke-Native $cmake $cfg 'cmake configure'
    Invoke-Native $cmake @('--build', (ToCMakePath $BuildDir)) 'build'
    return (Join-Path $BuildDir 'wifi022_check.exe')
}

$exe = Build-Check (Join-Path $repo 'build\wifi022_check') ''
& $exe
$code = $LASTEXITCODE

if ($ModuleRepo) {
    # The module's own encoder against the app's, on the module's own presets. Nothing of a block is printed.
    $hostDir = Join-Path $ModuleRepo 'host'
    if (-not (Test-Path -LiteralPath (Join-Path $hostDir 'sbpframe.py'))) { throw "not the module project: $ModuleRepo" }
    $py = (Get-Command python -ErrorAction Stop).Source
    foreach ($key in 'headunit', 'boat_kogger', 'boat_godio') {
        $file = Join-Path $ModuleRepo "profiles\$key.json"
        $script = "import sys, json; sys.path.insert(0, r'$hostDir'); import sbpframe as S; " +
                  "print(S.profile_blob(json.load(open(r'$file', encoding='utf-8'))).hex())"
        $theirs = (& $py -I -c $script | Out-String).Trim()
        $fromFile = (& $exe --blob $file | Out-String).Trim()
        $builtin = (& $exe --builtin $key $file | Out-String).Trim()
        $n = [int]($theirs.Length / 2)
        $a = if ($fromFile -eq $theirs -and $n -gt 0) { 'match' } else { 'DIFFERS'; $code = 1 }
        $b = if ($builtin -eq $theirs -and $n -gt 0) { 'match' } else { 'DIFFERS'; $code = 1 }
        Write-Host ("module preset {0} ({1} bytes): the app reading its file = profile_blob: {2}; the app's built-in profile with its passwords = profile_blob: {3}" -f $key, $n, $a, $b)
    }
}
if ($code -ne 0 -or -not $Mutants) { exit $code }

# One fault each, in a copy of the sources. The text to replace must be there: a mutant that changes nothing would
# pass and look like a gap in the check.
$cpp = 'id_binnary.cpp'
$prof = 'device\wifi_profile.cpp'
$profh = 'device\wifi_profile.h'
$faults = @(
    @{ Name = 'CRC-32 with another polynomial';       File = $prof; From = '0xEDB88320u'; To = '0xEDB88321u' },
    @{ Name = 'the begin announces a byte more';      File = $prof; From = '    putU16(p, uint16_t(blob.size()));'; To = '    putU16(p, uint16_t(blob.size() + 1));' },
    @{ Name = 'addresses written backwards';          File = $prof; From = "    b.append(char(a >> 24));`n    b.append(char((a >> 16) & 0xFF));"; To = "    b.append(char(a & 0xFF));`n    b.append(char((a >> 16) & 0xFF));" },
    @{ Name = 'the network count left out';           File = $prof; From = "    b.append(char(networks.size()));`n"; To = '' },
    @{ Name = 'power rounded half away from zero';    File = $prof; From = 'std::nearbyint(power.toDouble() * 4.0)'; To = 'std::round(power.toDouble() * 4.0)' },
    @{ Name = 'pieces over 253 bytes';                File = $prof; From = 'size = qBound(1, size, kMaxChunk);'; To = 'size = qMax(1, size);' },
    @{ Name = 'role 2 taken';                         File = $prof; From = "    if (p.role > 1)`n        return ReasonRole;`n"; To = '' },
    @{ Name = 'a late OK taken for the next piece';   File = $prof; From = 'if (resp == 1 && extraOks_ > 0) {'; To = 'if (false) {' },
    @{ Name = 'a held upload not applied again';      File = $prof; From = 'if (!appliedAgain_ && st.announced == total() && st.received == total()) {'; To = 'if (false) {' },
    @{ Name = 'ERR_RUNTIME taken as a refusal';       File = $prof; From = 'runtime_ = resp == 8;'; To = 'runtime_ = false;' },
    @{ Name = '0 asked for taken as saved';           File = $prof; From = 'finish(askedReason_ ? NoAnswer : Saved);'; To = 'finish(Saved);' },
    @{ Name = '64 characters not checked for hex';    File = $prof; From = 'hex = hex && std::isxdigit(uint8_t(c));'; To = 'hex = hex;' },
    @{ Name = '21 dBm compared as 21';                File = $prof; From = 'qMin<int>(p.powerQ, 80)'; To = 'int(p.powerQ)' },
    @{ Name = 'supported from 0.21';                  File = $profh; From = 'return (fwMajor > 0 || fwMinor >= 22) ? 1 : 0;'; To = 'return (fwMajor > 0 || fwMinor >= 21) ? 1 : 0;' },
    @{ Name = 'the CP2105 by the CP2102 id';          File = $profh; From = 'productId == 0xEA70'; To = 'productId == 0xEA60' },
    @{ Name = 'the status name a byte early';         File = $cpp; From = 's.name = QString::fromUtf8(raw.mid(10, d[9]));'; To = 's.name = QString::fromUtf8(raw.mid(9, d[9]));' },
    @{ Name = 'a SETTING taken as a reason';          File = $cpp; From = "    if (proto.type() != CONTENT)                  // a SETTING seen on the link is not the module's word`n        return respErrorType;`n    if (proto.ver() == v0) {"; To = '    if (proto.ver() == v0) {' },
    @{ Name = 'the translator bytes not read';        File = $cpp; From = 'if (proto.readAvailable() >= 3) {         // 0.21'; To = 'if (false) {         // 0.21' }
)

$utf8 = New-Object System.Text.UTF8Encoding $false
$files = @{}
foreach ($f in 'id_binnary.h', 'id_binnary.cpp', 'device\wifi_ops.h', 'device\wifi_ops.cpp', 'device\wifi_profile.h', 'device\wifi_profile.cpp') {
    $files[$f] = [System.IO.File]::ReadAllText((Join-Path $repo "src\$f"), $utf8).Replace("`r`n", "`n")
}
$missed = 0
$i = 0
foreach ($m in $faults) {
    ++$i
    if (-not $files[$m.File].Contains($m.From)) { throw "mutant '$($m.Name)': its text is not in $($m.File)" }
    $dir = Join-Path $repo ("build\wifi022_mutant_{0}\src" -f $i)
    New-Item -ItemType Directory -Force (Join-Path $dir 'device') | Out-Null
    foreach ($f in $files.Keys) {
        $text = if ($f -eq $m.File) { $files[$f].Replace($m.From, $m.To) } else { $files[$f] }
        [System.IO.File]::WriteAllText((Join-Path $dir $f), $text, $utf8)
    }
    $mexe = Build-Check (Join-Path $repo ("build\wifi022_mutant_{0}\build" -f $i)) $dir
    $out = & $mexe
    $failed = @($out | Where-Object { $_ -like 'FAIL *' })
    if ($LASTEXITCODE -ne 0 -and $failed.Count -gt 0) {
        Write-Host ("mutant {0} ({1}): caught by {2} check(s), first: {3}" -f $i, $m.Name, $failed.Count, $failed[0])
    } else {
        Write-Host ("mutant {0} ({1}): MISSED" -f $i, $m.Name)
        ++$missed
    }
}
Write-Host ("wifi022_check mutants: {0} of {1} caught" -f ($faults.Count - $missed), $faults.Count)
exit ([int]($missed -gt 0))

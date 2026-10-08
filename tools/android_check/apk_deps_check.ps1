<#
.SYNOPSIS
  Every shared library in an APK must find what it links against.

.DESCRIPTION
  On 2026-10-02 an APK built, signed and installed, then crashed on start: the
  private plugin of Qt5Compat.GraphicalEffects needs libQt6ShaderTools, the Qt kit
  had no qtshadertools module, and androiddeployqt packaged the plugin without its
  dependency and without a word. On the device that reads as "Type ColorOverlay
  unavailable" and a SIGSEGV in Core::UILoad - far from the cause.

  This check reads the NEEDED entries of every lib/<abi>/*.so in the APK with the
  NDK's llvm-readelf. A dependency is satisfied when the same ABI directory of the
  APK carries it, or when the NDK sysroot has it for that ABI at the minimum API
  level - that is, Android itself provides it. Anything else is reported.

  Libraries loaded with dlopen have no NEEDED entry, so one rule covers the case
  that hit on 2026-10-07: Qt's OpenSSL backend plugin is packaged either way and
  loads libssl_3.so and libcrypto_3.so at run time. Without them every https
  request fails (docs/build-android-cli.md), so they must sit next to it.
  Exit code 0 when nothing is missing, 1 otherwise.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\android_check\apk_deps_check.ps1
  powershell -ExecutionPolicy Bypass -File tools\android_check\apk_deps_check.ps1 -Apk out_android\KoggerApp.apk
#>
[CmdletBinding()]
param(
    [string] $Apk,
    [string] $Ndk    = 'D:\Android\Sdk\ndk\27.3.13750724',
    [int]    $MinSdk = 28
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

if (-not $Apk) {
    $found = Get-ChildItem (Join-Path $repo 'build') -Recurse -Filter *.apk -ErrorAction SilentlyContinue |
             Where-Object { $_.FullName -like '*\outputs\apk\*' } |
             Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $found) { throw 'no APK under build\ - pass -Apk' }
    $Apk = $found.FullName
}
$Apk = (Resolve-Path $Apk).Path

$readelf = Join-Path $Ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe'
if (-not (Test-Path -LiteralPath $readelf)) { throw "llvm-readelf not found: $readelf" }
$sysroot = Join-Path $Ndk 'toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib'

# The sysroot directory that holds the system libraries of each ABI.
$triples = @{
    'arm64-v8a'   = 'aarch64-linux-android'
    'armeabi-v7a' = 'arm-linux-androideabi'
    'x86_64'      = 'x86_64-linux-android'
    'x86'         = 'i686-linux-android'
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$work = Join-Path ([System.IO.Path]::GetTempPath()) ('apk_deps_' + [guid]::NewGuid().ToString('N'))
[System.IO.Directory]::CreateDirectory($work) | Out-Null

$bad = 0
try {
    $zip = [System.IO.Compression.ZipFile]::OpenRead($Apk)
    try {
        $abis = $zip.Entries | Where-Object { $_.FullName -match '^lib/([^/]+)/[^/]+\.so$' } |
                ForEach-Object { $Matches[1] } | Sort-Object -Unique
        foreach ($abi in $abis) {
            $dir = Join-Path $work $abi
            [System.IO.Directory]::CreateDirectory($dir) | Out-Null
            foreach ($e in $zip.Entries) {
                if ($e.FullName -like "lib/$abi/*.so") {
                    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, (Join-Path $dir $e.Name), $true)
                }
            }
        }
    } finally { $zip.Dispose() }

    Write-Host "apk : $Apk"
    foreach ($abi in $abis) {
        $dir = Join-Path $work $abi
        $present = @{}
        foreach ($f in Get-ChildItem $dir -Filter *.so) { $present[$f.Name] = $true }

        $system = @{}
        if ($triples.ContainsKey($abi)) {
            $api = Join-Path (Join-Path $sysroot $triples[$abi]) $MinSdk
            foreach ($f in Get-ChildItem $api -Filter *.so -ErrorAction SilentlyContinue) { $system[$f.Name] = $true }
        }
        if ($system.Count -eq 0) { throw "no NDK system libraries for $abi at API $MinSdk under $sysroot" }

        $missing = @{}
        foreach ($f in Get-ChildItem $dir -Filter *.so) {
            foreach ($line in (& $readelf -d $f.FullName 2>$null)) {
                if ($line -match '\(NEEDED\).*\[(.+?)\]') {
                    $dep = $Matches[1]
                    if (-not $present.ContainsKey($dep) -and -not $system.ContainsKey($dep)) {
                        if (-not $missing.ContainsKey($dep)) { $missing[$dep] = New-Object System.Collections.Generic.List[string] }
                        $missing[$dep].Add($f.Name)
                    }
                }
            }
        }

        if (@($present.Keys | Where-Object { $_ -like 'libplugins_tls_qopensslbackend*' }).Count -gt 0) {
            foreach ($lib in 'libssl_3.so', 'libcrypto_3.so') {
                if (-not $present.ContainsKey($lib)) {
                    if (-not $missing.ContainsKey($lib)) { $missing[$lib] = New-Object System.Collections.Generic.List[string] }
                    $missing[$lib].Add('libplugins_tls_qopensslbackend (dlopen at run time)')
                }
            }
        }

        if ($missing.Count -eq 0) {
            Write-Host ("{0,-12} PASS  {1} libraries, every dependency found" -f $abi, $present.Count)
        } else {
            $bad++
            Write-Host ("{0,-12} FAIL  {1} libraries, {2} dependencies missing:" -f $abi, $present.Count, $missing.Count)
            foreach ($k in ($missing.Keys | Sort-Object)) {
                Write-Host ("    {0}  <- {1}" -f $k, (($missing[$k] | Sort-Object) -join ', '))
            }
        }
    }
} finally {
    [System.IO.Directory]::Delete($work, $true)
}

if ($bad -gt 0) { exit 1 }
exit 0

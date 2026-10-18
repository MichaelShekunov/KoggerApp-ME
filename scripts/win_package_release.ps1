#requires -Version 5.1
<#
.SYNOPSIS
    Builds, deploys and packages KoggerApp ME for Windows: a portable
    .zip and an Inno Setup installer, both written to dist\.

.DESCRIPTION
    Non-interactive on purpose. win_deploy_windows_release.bat asks questions
    and ends in `pause`, which makes it unusable from CI or from any shell
    without a terminal; this script never prompts and fails loudly instead.

    The version stamped on the artifacts is resources\version.txt plus the
    short commit the build came from, so a downloaded file can always be traced
    back to a commit. A dirty working tree is marked as such in the name,
    because then the commit does not describe what was built. The names end
    with the time the build wrote KoggerApp.exe, YYYYMMDD-HHMMSS.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\win_package_release.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\win_package_release.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
    [string] $QtRoot    = 'D:\Qt',
    [string] $QtVersion = '6.8.3',
    [ValidateSet('Release', 'RelWithDebInfo')]
    [string] $Config    = 'Release',
    [string] $InnoRoot  = 'C:\Program Files (x86)\Inno Setup 6',
    [switch] $SkipBuild,
    [switch] $NoInstaller,
    [switch] $NoZip
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

function Require-Path {
    param([string] $Path, [string] $What, [string] $Hint)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$What not found: $Path`n  $Hint"
    }
    return $Path
}

function Write-Step {
    param([string] $Text)
    Write-Host ''
    Write-Host "==> $Text" -ForegroundColor Cyan
}

# ---------------------------------------------------------------- environment

$qtPrefix = Require-Path "$QtRoot\$QtVersion\llvm-mingw_64" 'Qt kit' `
    "python -m aqt install-qt windows desktop $QtVersion win64_llvm_mingw -m qt5compat qtmultimedia qtpositioning qtserialport qtshadertools -O $QtRoot"
$llvm = Require-Path "$QtRoot\Tools\llvm-mingw1706_64" 'LLVM-MinGW toolchain' `
    "python -m aqt install-tool windows desktop tools_llvm_mingw1706 -O $QtRoot"
$windeployqt = Require-Path "$qtPrefix\bin\windeployqt.exe" 'windeployqt' `
    'The Qt kit is incomplete; reinstall it.'

# ------------------------------------------------------------------- version

$version = (Get-Content -LiteralPath "$repo\resources\version.txt" -Raw).Trim()
if (-not $version) { throw 'resources\version.txt is empty' }

$commit = & git -C $repo rev-parse --short=8 HEAD 2>$null
if ($LASTEXITCODE -ne 0) { throw 'not a git checkout, or git is not on PATH' }
$commit = $commit.Trim()

$dirty = & git -C $repo status --porcelain
if ($dirty) {
    Write-Warning 'The working tree has uncommitted changes; build_info.txt says "dirty: yes".'
}

# Windows wants four numeric fields and nothing else.
$numeric = ($version -split '-')[0]
while (($numeric -split '\.').Count -lt 4) { $numeric = "$numeric.0" }

Write-Host "file ver: $numeric"

# --------------------------------------------------------------------- build

$binPath = "$repo\build\Desktop_Qt_6_8_3_llvm_mingw_64_bit-$Config\KoggerApp.exe"

if ($SkipBuild) {
    Write-Step 'Build skipped by request'
} else {
    Write-Step 'Building'
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$repo\scripts\win_build_release.ps1" `
        -QtRoot $QtRoot -QtVersion $QtVersion -Config $Config
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
}

Require-Path $binPath 'KoggerApp.exe' 'Build the Release configuration first, or drop -SkipBuild.' | Out-Null

# The version the build put in the binary, "ME 1.0.3 18.10.2026 10:23" (cmake/kogger_version.cmake),
# for the installer and build_info.txt: the window's title says the same.
$header = "$repo\build\Desktop_Qt_6_8_3_llvm_mingw_64_bit-$Config\generated\kogger_revision.h"
$fullVersion = ''
if (Test-Path -LiteralPath $header) {
    $m = Select-String -LiteralPath $header -Pattern '#define KOGGER_VERSION_FULL "([^"]*)"'
    if ($m) { $fullVersion = $m.Matches[0].Groups[1].Value }
}
if (-not $fullVersion) { throw "no KOGGER_VERSION_FULL in ${header} (build first)" }
Write-Host "version:  $fullVersion"

# Every build says 1.0.3; the time the build wrote KoggerApp.exe tells two apart (owner, 2026-10-07),
# the file names carry no commit (owner, 2026-10-18). With -SkipBuild that is the earlier build.
$built     = (Get-Item -LiteralPath $binPath).LastWriteTime
$builtTag  = $built.ToString('yyyyMMdd-HHmmss')
$baseName  = "KoggerApp-ME-$version"
$zipName   = "$baseName-portable-$builtTag.zip"
$setupBase = "$baseName-setup-$builtTag"
Write-Host "files:    $zipName, $setupBase.exe"

# -------------------------------------------------------------------- deploy

$dist  = "$repo\dist"
$stage = "$dist\stage"

Write-Step "Staging into $stage"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null

# Qt 6.8 dropped -no-virtualkeyboard; the plugin type is skipped by name now.
& $windeployqt $binPath -qmldir "$repo\qml" -dir $stage -no-translations --skip-plugin-types virtualkeyboard
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed (exit $LASTEXITCODE) - the staged tree would be incomplete" }

Copy-Item -LiteralPath $binPath -Destination $stage -Force

# windeployqt does not know about the LLVM-MinGW runtime, and the application
# will not start without it.
foreach ($dll in @('libunwind.dll', 'libc++.dll', 'libwinpthread-1.dll')) {
    $src = "$llvm\bin\$dll"
    if (-not (Test-Path -LiteralPath $src)) { throw "LLVM-MinGW runtime missing: $src" }
    Copy-Item -LiteralPath $src -Destination $stage -Force
}

$buildInfo = @(
    "KoggerApp ME",
    "version:   $fullVersion",
    "commit:    $(& git -C $repo rev-parse HEAD)",
    "dirty:     $(if ($dirty) { 'yes' } else { 'no' })",
    "built:     $($built.ToString('yyyy-MM-dd HH:mm:ss zzz'))",
    "packaged:  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')",
    "qt:        $QtVersion llvm-mingw_64",
    "config:    $Config"
)
Set-Content -LiteralPath "$stage\build_info.txt" -Value $buildInfo -Encoding utf8

$staged = (Get-ChildItem -LiteralPath $stage -Recurse -File | Measure-Object -Property Length -Sum)
Write-Host ("staged: {0} files, {1:N1} MB" -f $staged.Count, ($staged.Sum / 1MB))

# ----------------------------------------------------------------- artifacts

$artifacts = @()

if (-not $NoZip) {
    Write-Step 'Packing the portable zip'
    $zipPath = "$dist\$zipName"
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $stage, $zipPath, [System.IO.Compression.CompressionLevel]::Optimal, $false)
    $artifacts += $zipPath
}

if (-not $NoInstaller) {
    Write-Step 'Building the installer'

    # Inno Setup finalises setup.exe by rewriting its resources, and that fails
    # with "EndUpdateResource failed (110)" if anything holds the file open.
    # dist\ sits inside the Yandex Disk sync root, and the client grabs files
    # the moment they appear — it had just started on the 50 MB zip. So the
    # installer is built outside the sync root and moved in afterwards; a
    # rebuilt artifact has no business being synced mid-write anyway.
    $issOut = Join-Path $env:TEMP "koggerapp-iss-$PID"
    New-Item -ItemType Directory -Path $issOut -Force | Out-Null

    $iscc = "$InnoRoot\ISCC.exe"
    if (-not (Test-Path -LiteralPath $iscc)) {
        $onPath = Get-Command 'ISCC.exe' -ErrorAction SilentlyContinue
        if ($onPath) { $iscc = $onPath.Source }
    }
    Require-Path $iscc 'Inno Setup (ISCC.exe)' `
        'Install Inno Setup 6, or pass -InnoRoot, or pass -NoInstaller to skip.' | Out-Null

    & $iscc `
        "/DMyAppVersion=$fullVersion" `
        "/DMyAppVersionInfo=$numeric" `
        "/DPayloadDir=$stage" `
        "/DOutputDir=$issOut" `
        "/DOutputBase=$setupBase" `
        "/DIconFile=$repo\resources\icons\app\kogger_app.ico" `
        "$repo\packaging\koggerapp.iss"
    if ($LASTEXITCODE -ne 0) { throw "ISCC failed (exit $LASTEXITCODE)" }

    Move-Item -LiteralPath "$issOut\$setupBase.exe" `
              -Destination "$dist\$setupBase.exe" -Force
    Remove-Item -LiteralPath $issOut -Recurse -Force -ErrorAction SilentlyContinue
    $artifacts += "$dist\$setupBase.exe"
}

# -------------------------------------------------------------------- report

Write-Step 'Artifacts'
foreach ($a in $artifacts) {
    Require-Path $a 'artifact' 'The packaging step reported success but produced nothing.' | Out-Null
    $item = Get-Item -LiteralPath $a
    $hash = (Get-FileHash -LiteralPath $a -Algorithm SHA256).Hash
    Write-Host ("{0,-10:N1} MB  {1}" -f ($item.Length / 1MB), $item.Name)
    Write-Host ("            sha256 {0}" -f $hash.ToLower())
}
Write-Host ''
Write-Host "dist: $dist"

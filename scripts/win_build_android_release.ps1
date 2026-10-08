#requires -Version 5.1
[CmdletBinding()]
param(
    [string] $QtRoot    = 'D:\Qt',
    [string] $QtVersion = '6.8.3',
    [string] $AndroidRoot = 'D:\Android',
    [string] $NdkVersion  = '27.3.13750724',
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string] $Config    = 'Release',
    # Side-by-side build: '.myedition' gives the package its own application id and
    # launcher name, so a test-signed build installs next to a release-signed
    # KoggerApp. Empty produces the normal package.
    [string] $AppIdSuffix = '',
    [switch] $Clean,
    [switch] $ConfigureOnly,
    # Take the APK the last build wrote instead of building.
    [switch] $SkipBuild,
    # Signing: all three or none. With them the signed APK goes to $OutDir, without
    # them a copy of the unsigned one goes to $OutDir\unsigned.
    [string] $Keystore,
    [string] $KeyAlias,
    [string] $KeyPassFile,
    [string] $OutDir
)

# Builds the universal APK (armeabi-v7a + arm64-v8a) without Qt Creator, checks
# its libraries and puts it into out_android as
# KoggerApp-<versionName>[-<app id suffix>]-<YYYYMMDD-HHMMSS>.apk, the stamp being
# when the build wrote the APK. The build directory name is the one
# scripts\win_deploy_android_release.bat and the Qt Creator kit both expect.

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

function Require-Path {
    param([string] $Path, [string] $What, [string] $Hint)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$What not found: $Path`n  $Hint"
    }
    return $Path
}

$qtArm64 = Require-Path "$QtRoot\$QtVersion\android_arm64_v8a" 'Qt Android kit (arm64-v8a)' `
    "python -m aqt install-qt windows android $QtVersion android_arm64_v8a -m qtmultimedia qtpositioning qt5compat -O $QtRoot"
$qtArmV7 = Require-Path "$QtRoot\$QtVersion\android_armv7" 'Qt Android kit (armeabi-v7a)' `
    "python -m aqt install-qt windows android $QtVersion android_armv7 -m qtmultimedia qtpositioning qt5compat -O $QtRoot"
$qtHost  = Require-Path "$QtRoot\$QtVersion\llvm-mingw_64" 'Qt host kit' `
    "python -m aqt install-qt windows desktop $QtVersion win64_llvm_mingw -O $QtRoot"
$sdk = Require-Path "$AndroidRoot\Sdk" 'Android SDK' `
    "unpack commandlinetools-win into $AndroidRoot\Sdk\cmdline-tools\latest, then run sdkmanager"
$ndk = Require-Path "$sdk\ndk\$NdkVersion" "Android NDK $NdkVersion" `
    "sdkmanager --sdk_root=$sdk `"ndk;$NdkVersion`""

# The JDK carries its patch level in the directory name - take the newest one.
$jdk = Get-ChildItem "$AndroidRoot\jdk" -Directory -ErrorAction SilentlyContinue |
       Sort-Object Name -Descending | Select-Object -First 1
if (-not $jdk) { throw "JDK not found under $AndroidRoot\jdk (Android Gradle Plugin 8.6 needs JDK 17)" }

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

$buildDir = Join-Path $repo ("build\Qt_{0}_for_Android_Universal-{1}" -f ($QtVersion -replace '\.', '_'), $Config)

$signing = @($Keystore, $KeyAlias, $KeyPassFile | Where-Object { $_ })
if ($signing.Count -ne 0 -and $signing.Count -ne 3) { throw 'signing needs -Keystore, -KeyAlias and -KeyPassFile together' }
if ($signing.Count -eq 3) {
    Require-Path $Keystore 'keystore' 'pass the keystore file with -Keystore' | Out-Null
    Require-Path $KeyPassFile 'password file' 'the first line of -KeyPassFile is the keystore password' | Out-Null
}
if (-not $OutDir) { $OutDir = Join-Path $repo 'out_android' }
$buildTools = Get-ChildItem "$sdk\build-tools" -Directory -ErrorAction SilentlyContinue |
              Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $buildTools) { throw "no build-tools under $sdk\build-tools" }

if ($Clean -and $SkipBuild) { throw '-Clean with -SkipBuild would leave no APK to take' }
if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
    Write-Host "removing $buildDir"
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}

$env:JAVA_HOME        = $jdk.FullName
$env:ANDROID_SDK_ROOT = $sdk
$env:ANDROID_NDK_ROOT = $ndk
$env:PATH             = "$($jdk.FullName)\bin;$env:PATH"
# platform/android/build.gradle reads this; empty means the normal package.
$env:KOGGER_APP_ID_SUFFIX = $AppIdSuffix

Write-Host "repo    : $repo"
Write-Host "qt      : $qtArm64"
Write-Host "          $qtArmV7"
Write-Host "host qt : $qtHost"
Write-Host "sdk     : $sdk"
Write-Host "ndk     : $ndk"
Write-Host "jdk     : $($jdk.FullName)"
Write-Host "build   : $buildDir"

# CMake writes these values verbatim into its own .cmake files, where a backslash
# starts an escape sequence - pass them with forward slashes.
function ToCMakePath { param([string] $p) return ($p -replace '\\', '/') }

# Windows PowerShell 5.1 turns any stderr line from a native exe into a terminating
# error under $ErrorActionPreference='Stop' - CMake and Gradle both write there.
# Judge these calls by their exit code only.
function Invoke-Native {
    param([string] $Exe, [string[]] $Arguments, [string] $What)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Exe @Arguments } finally { $ErrorActionPreference = $prev }
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit $LASTEXITCODE)" }
}

if ($SkipBuild) {
    $apk = Get-ChildItem $buildDir -Recurse -Filter *.apk -ErrorAction SilentlyContinue |
           Where-Object { $_.FullName -like '*\outputs\apk\*' } |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $apk) { throw "no built APK under $buildDir - build first" }
} else {
    Invoke-Native $cmake @(
        '-S', (ToCMakePath $repo), '-B', (ToCMakePath $buildDir), '-G', 'Ninja',
        "-DCMAKE_BUILD_TYPE=$Config",
        ('-DCMAKE_MAKE_PROGRAM=' + (ToCMakePath $ninja)),
        ('-DCMAKE_TOOLCHAIN_FILE=' + (ToCMakePath "$qtArm64\lib\cmake\Qt6\qt.toolchain.cmake")),
        ('-DQT_HOST_PATH=' + (ToCMakePath $qtHost)),
        ('-DANDROID_SDK_ROOT=' + (ToCMakePath $sdk)),
        ('-DANDROID_NDK_ROOT=' + (ToCMakePath $ndk)),
        # One kit builds both ABIs into a single universal APK. The ABI list itself is
        # pinned in CMakeLists.txt; arm64-v8a comes from the toolchain file above, and
        # only the second ABI has to be pointed at its own Qt installation.
        ('-DQT_PATH_ANDROID_ABI_armeabi-v7a=' + (ToCMakePath $qtArmV7))
    ) 'cmake configure'

    if ($ConfigureOnly) { Write-Host 'configure only - stopping here'; exit 0 }

    # Ninja reruns androiddeployqt only when libKoggerApp or the deployment settings
    # change. A Qt module added to the kit changes neither, so the old package would be
    # kept - and a library the new module brings would never reach the APK (this is how
    # libQt6ShaderTools went missing once). Removing the rule's declared output makes
    # every run deploy afresh; Gradle's own up-to-date checks keep that cheap.
    $runStart = Get-Date
    Remove-Item -LiteralPath (Join-Path $buildDir 'android-build\KoggerApp.apk') -ErrorAction SilentlyContinue

    Invoke-Native $cmake @('--build', (ToCMakePath $buildDir), '--target', 'apk') 'apk build'

    # Qt 6.8's own CMake deployment writes to 'android-build'; Qt Creator used to name
    # that directory 'android-build-KoggerApp', which is what win_deploy_android_release.bat
    # still looks for. Accept either - but only a package written by this run.
    $apk = Get-ChildItem $buildDir -Recurse -Filter *.apk -ErrorAction SilentlyContinue |
           Where-Object { $_.LastWriteTime -ge $runStart -and $_.Name -ne 'KoggerApp.apk' } |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $apk) { throw "build reported success but this run wrote no .apk under $buildDir" }
}
Write-Host ("OK: {0} ({1:N1} MB)" -f $apk.FullName, ($apk.Length / 1MB))

# A package missing a library builds, signs and installs, and fails only on the device
# (2026-10-02, 2026-10-07): check it before it leaves the build directory.
Invoke-Native 'powershell' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
    "$repo\tools\android_check\apk_deps_check.ps1", '-Apk', $apk.FullName) 'apk_deps_check (nothing copied)'

# The name repeats what the package says about itself, and ends with when the build
# wrote it: every build says versionName 1.0.3, only the stamp tells two of them apart.
$prev = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $badging = & "$($buildTools.FullName)\aapt2.exe" dump badging $apk.FullName 2>$null | Select-Object -First 1
} finally { $ErrorActionPreference = $prev }
# Case-sensitive, with the space: the same line carries platformBuildVersionName='15'.
if ($badging -cnotmatch "package: name='([^']+)'.*?\sversionName='([^']+)'") { throw "cannot read the package of $($apk.FullName)" }
$appId = $Matches[1]
$variant = if ($appId -like 'org.kogger.koggerapp.*') { '-' + $appId.Substring('org.kogger.koggerapp.'.Length) } else { '' }
$name  = "KoggerApp-$($Matches[2])$variant"
$built = $apk.LastWriteTime.ToString('yyyyMMdd-HHmmss')

if ($signing.Count -eq 3) {
    $dest = Join-Path $OutDir "$name-$built.apk"
    if (Test-Path -LiteralPath $dest) { throw "already there, not overwritten: $dest" }
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    Invoke-Native "$($buildTools.FullName)\zipalign.exe" @('-c', '-p', '4', $apk.FullName) 'zipalign check'
    # The store password covers the key; --key-pass on the same file would read past its end.
    Invoke-Native "$($buildTools.FullName)\apksigner.bat" @(
        'sign', '--ks', $Keystore, '--ks-key-alias', $KeyAlias, '--ks-pass', "file:$KeyPassFile",
        '--v1-signing-enabled', 'true', '--v4-signing-enabled', 'false', '--out', $dest, $apk.FullName
    ) 'apksigner sign'
    Invoke-Native "$($buildTools.FullName)\apksigner.bat" @('verify', $dest) 'apksigner verify'
} else {
    $dest = Join-Path $OutDir "unsigned\$name-unsigned-$built.apk"
    if (Test-Path -LiteralPath $dest) { throw "already there, not overwritten: $dest" }
    New-Item -ItemType Directory -Path (Split-Path $dest) -Force | Out-Null
    Copy-Item -LiteralPath $apk.FullName -Destination $dest
}
Write-Host ("APK: {0}" -f $dest)
Write-Host ("     sha256 {0}" -f (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash.ToLower())

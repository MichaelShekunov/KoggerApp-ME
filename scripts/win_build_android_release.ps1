#requires -Version 5.1
[CmdletBinding()]
param(
    [string] $QtRoot    = 'D:\Qt',
    [string] $QtVersion = '6.8.3',
    [string] $AndroidRoot = 'D:\Android',
    [string] $NdkVersion  = '27.3.13750724',
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string] $Config    = 'Release',
    [switch] $Clean,
    [switch] $ConfigureOnly
)

# Builds the universal APK (armeabi-v7a + arm64-v8a) without Qt Creator.
# The build directory name is the one scripts\win_deploy_android_release.bat
# and the Qt Creator kit both expect.

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

if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
    Write-Host "removing $buildDir"
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}

$env:JAVA_HOME        = $jdk.FullName
$env:ANDROID_SDK_ROOT = $sdk
$env:ANDROID_NDK_ROOT = $ndk
$env:PATH             = "$($jdk.FullName)\bin;$env:PATH"

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

Invoke-Native $cmake @('--build', (ToCMakePath $buildDir), '--target', 'apk') 'apk build'

# Qt 6.8's own CMake deployment writes to 'android-build'; Qt Creator used to name
# that directory 'android-build-KoggerApp', which is what win_deploy_android_release.bat
# still looks for. Accept either.
$apk = Get-ChildItem $buildDir -Recurse -Filter *.apk -ErrorAction SilentlyContinue |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $apk) { throw "build reported success but no .apk under $buildDir" }
Write-Host ("OK: {0} ({1:N1} MB)" -f $apk.FullName, ($apk.Length / 1MB))

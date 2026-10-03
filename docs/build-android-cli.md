# Android build from the command line

`README.md` describes the Qt Creator workflow. This document covers the scripted
alternative: `scripts/win_build_android_release.ps1`, no IDE, no Qt account,
reproducible from a clean machine. It is the Android counterpart of
`build-windows-cli.md`; what the app then does on a tablet is `android.md`.

Provisioned and verified on 2026-10-02 against `myedition` at `ceef7929`
(version 1.0.3) on a machine that had nothing Android on it beforehand.

## Toolchain

| Component | Version | Location |
| --- | --- | --- |
| Qt (target) | 6.8.3, kits `android_arm64_v8a` and `android_armv7` | `D:\Qt\6.8.3\android_*` |
| Qt (host) | 6.8.3, kit `llvm-mingw_64` | `D:\Qt\6.8.3\llvm-mingw_64` |
| JDK | Temurin 17.0.20.1+1 | `D:\Android\jdk\jdk-17.0.20.1+1` |
| Android SDK | cmdline-tools 11076708, platform-tools, platforms 34 and 35, build-tools 34.0.0 | `D:\Android\Sdk` |
| NDK | 27.3.13750724 (2.3 GB unpacked) | `D:\Android\Sdk\ndk\27.3.13750724` |
| CMake | 4.3.1 | on PATH |
| Ninja | 1.13.2 | on PATH |

Three of those versions are pinned by the project rather than chosen:

- **JDK 17.** `platform/android/build.gradle` pins Android Gradle Plugin 8.6.0,
  which refuses to run on anything older. A machine set up for the firmware
  work typically has JDK 8 only, and the failure appears deep inside Gradle.
- **NDK 27.3.13750724.** The version `README.md` states for the released APK.
- **Both Qt Android kits.** `CMakeLists.txt` forces
  `QT_ANDROID_ABIS "armeabi-v7a;arm64-v8a"`, so one build produces the universal
  APK and each ABI needs its own Qt installation. The host kit is a third
  install: `androiddeployqt.exe` lives there, not in the target kits.

## Installing Qt

```
python -m aqt install-qt windows android 6.8.3 android_arm64_v8a -m qtmultimedia qtpositioning qt5compat qtserialport qtshadertools -O D:\Qt
python -m aqt install-qt windows android 6.8.3 android_armv7   -m qtmultimedia qtpositioning qt5compat qtserialport qtshadertools -O D:\Qt
```

479 MB and 402 MB on disk, both together about 30 seconds on a warm connection.

`qtshadertools` is **a runtime dependency on Android**, not only a desktop build
tool: the private plugin of `Qt5Compat.GraphicalEffects`
(`libqml_Qt5Compat_GraphicalEffects_private_qtgraphicaleffectsprivateplugin`)
lists `libQt6ShaderTools` in its `NEEDED`. Without the module the APK builds,
signs and installs, and then dies on start: `ColorOverlay` in
`qml/scene2d/KThemeSwitcher.qml` cannot load, `main.qml` fails, and the app
crashes with SIGSEGV in `Core::UILoad`. The log line that gives it away:

```
Cannot load library .../libqml_Qt5Compat_GraphicalEffects_private_..._arm64-v8a.so:
dlopen failed: library "_data_app_..._libqml_Qt5Compat_..." not found
```

— the mangled name is how Qt reports a dependency it could not satisfy, not a
missing file; the plugin itself is in the APK. To check any APK for this whole
class of problem, list every `NEEDED` of every bundled `.so` with the NDK's
`llvm-readelf -d` and subtract what the APK carries: the result must be empty.

`qtserialport` looks unnecessary — Android serial goes through
`platform/android/src/qtandroidserialport`, a vendored copy that talks to the
Java USB stack, and nothing links `Qt6::SerialPort`. It is required all the same:
the vendored `qserialport.h` includes `<QtSerialPort/qserialportglobal.h>`, which
resolves only out of an installed module. Without it the build stops at

```
qserialport.h:10:10: fatal error: 'QtSerialPort/qserialportglobal.h' file not found
```

## Installing the Android SDK and NDK

```
curl -L -o D:\Android\jdk17.zip "https://api.adoptium.net/v3/binary/latest/17/ga/windows/x64/jdk/hotspot/normal/eclipse"
curl -L -o D:\Android\cmdline-tools.zip "https://dl.google.com/android/repository/commandlinetools-win-11076708_latest.zip"
```

Unpack the JDK under `D:\Android\jdk`, and the command-line tools so that
`sdkmanager.bat` ends up at `D:\Android\Sdk\cmdline-tools\latest\bin` — the
`latest` level is not optional, `sdkmanager` refuses to run without it.

Then, with `JAVA_HOME` pointing at the JDK:

```
sdkmanager.bat --sdk_root=D:\Android\Sdk --licenses < yes.txt
sdkmanager.bat --sdk_root=D:\Android\Sdk platform-tools platforms;android-34 platforms;android-35 build-tools;34.0.0 ndk;27.3.13750724
```

where `yes.txt` is a file of `y` lines.

**The licence trap.** Feeding those `y` answers through a pipe
(`"y`n" * 60 | sdkmanager --licenses`, or `echo y | ...`) does not work from
PowerShell: `sdkmanager` prints the first prompt, never sees the answers, and
exits reporting *"7 of 7 SDK package licenses not accepted"*. The install that
follows then **skips every package and still exits 0** — the only sign of
trouble is a list of names under "can not be installed". File redirection
(`< yes.txt`) works. After a successful run `D:\Android\Sdk\licenses` holds
seven 42-byte hash files; check for them rather than trusting the exit code.

## Building

```
scripts\win_build_android_release.ps1                 # Release, both ABIs
scripts\win_build_android_release.ps1 -ConfigureOnly  # configure, stop before compiling
scripts\win_build_android_release.ps1 -Clean          # wipe the build directory first
```

The script locates the kits, sets `JAVA_HOME`, `ANDROID_SDK_ROOT` and
`ANDROID_NDK_ROOT`, and configures with the Qt Android toolchain file plus
`QT_HOST_PATH` and `QT_PATH_ANDROID_ABI_armeabi-v7a` (arm64-v8a comes from the
toolchain file itself). A correct configure says:

```
-- Configuring 'KoggerApp' for the following Android ABIs: arm64-v8a (default), armeabi-v7a
```

The build directory is `build\Qt_6_8_3_for_Android_Universal-Release`, the name
`scripts\win_deploy_android_release.bat` and the Qt Creator kit both expect.

**The stale-package trap.** Ninja reruns `androiddeployqt` only when
`libKoggerApp` or the deployment settings change. A Qt module added to the kit
changes neither, so a rebuild after `aqt install-qt ... -m <module>` keeps the
old package, prints `ninja: no work to do`, and the new module's libraries never
reach the APK. The script therefore removes the packaging rule's declared output
(`android-build\KoggerApp.apk`) before every build, and accepts only an APK
written by the current run.

CMake prints deprecation warnings from the NDK's own `android.toolchain.cmake`
(it still declares compatibility with CMake 3.6). They come from Google's files,
not from this project, and are harmless.

The APK lands in

```
build\Qt_6_8_3_for_Android_Universal-Release\android-build\build\outputs\apk\release\android-build-release-unsigned.apk
```

Note `android-build`, not `android-build-KoggerApp`: Qt 6.8's own CMake
deployment names that directory after the deployment settings, where Qt Creator
named it after the target. `win_deploy_android_release.bat` still looks for the
old name and will not find this file.

Measured on the first clean run: 73.0 MB unsigned, `arm64-v8a` and `armeabi-v7a`
side by side (22.3 MB and 17.8 MB of `libKoggerApp`, 94 shared objects each),
`versionCode 10003`, `versionName 1.0.3`, `minSdkVersion 28`,
`targetSdkVersion 34`. The Gradle half of the build takes about 3 minutes.

## Checking the APK before it goes on a device

```
powershell -ExecutionPolicy Bypass -File tools\android_check\apk_deps_check.ps1 [-Apk <path>]
```

reads the `NEEDED` entries of every `lib/<abi>/*.so` in the APK with the NDK's
`llvm-readelf` and fails on any dependency that neither the APK nor Android
provides (Android's own set is taken from the NDK sysroot at the minimum API
level). Without `-Apk` it takes the newest APK under `build\`. Proven both ways
on 2026-10-02: the package without `qtshadertools` fails on exactly
`libQt6ShaderTools` for both ABIs, the fixed one passes with 95 libraries per ABI.

## Installing next to a release-signed KoggerApp

A tablet that already has KoggerApp from the official release refuses a
test-signed package with the same application id. Uninstalling it would take its
settings, so the fork can be built as a separate app instead — the arrangement the
PC has, where "KoggerApp myEdition" is installed beside the official one:

```
scripts\win_build_android_release.ps1 -AppIdSuffix .myedition
```

The suffix reaches `platform/android/build.gradle` as `KOGGER_APP_ID_SUFFIX`,
which appends it to the application id (`org.kogger.koggerapp.myedition`) and
names the app "KoggerApp myEdition" on the launcher, through the
`${koggerAppLabel}` placeholder in `AndroidManifest.xml`. The file provider's
authority was already `${applicationId}.qtprovider`, so the two apps do not
collide there either. The Java package stays `org.kogger.koggerapp`; only the
installed identity changes. Without the switch the package is the normal one.

## What the fork had to change

Android is an upstream platform and the app ships for it, but three of this
fork's own files were written without it in mind. All three are now fixed:

- `src/stall_monitor.cpp` — guarded its diagnostics with `Q_OS_LINUX`, and Qt
  counts Android as a Linux. Bionic declares `backtrace()` only from API 33
  while the app targets 28, so the build failed on `backtrace`,
  `backtrace_symbols_fd`. Only the backtrace half is now excluded
  (`KOGGER_STALL_BACKTRACE`); the thread and `/proc` diagnostics still run on
  Android.
- `src/device/usb_tap.h`, `src/device/usb_tap.cpp` — included `<QSerialPort>`
  and `<QSerialPortInfo>` outright. `src/link/link.h` and
  `src/link/link_manager.h` already show the project's convention for this:
  include the vendored `"qserialport.h"` under `#if defined(Q_OS_ANDROID)`.
  The USB tap is a fork addition, which is why it never met Android before.

## Signing

The build produces an **unsigned** release APK, and Android installs nothing
unsigned. `win_deploy_android_release.bat` expects
`android-build-KoggerApp-release-signed.apk` and verifies the signature with
`apksigner` before copying anything, so the release keystore has to be supplied
separately — it is deliberately not in the repository.
`win_install_android_release.bat` pushes the signed APK to a connected device
with `adb`.

For a build that is only going on a test tablet, a throwaway key is enough:

```
keytool -genkeypair -keystore test.keystore -alias test -keyalg RSA -keysize 2048 -validity 3650
apksigner sign --ks test.keystore --out KoggerApp-<version>-universal-testsigned.apk <unsigned>.apk
apksigner verify --verbose KoggerApp-<version>-universal-testsigned.apk
```

`apksigner` signs with scheme v3 only, which Android 9 (API 28, our minimum)
understands, so the result installs on every device the app supports. A tablet
that already carries a release-signed KoggerApp will refuse the test-signed one
over the top — the signatures differ; uninstall first. Never ship a test-signed
APK: a device cannot upgrade from it to a release build either.

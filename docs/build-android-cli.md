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
| OpenSSL for Android | KDAB `android_openssl` b71f147 (2026-01-26), OpenSSL 3, 209 MB | `D:\Android\Sdk\android_openssl` |
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

Then OpenSSL, which `sdkmanager` does not install:

```
git clone --depth 1 https://github.com/KDAB/android_openssl.git D:\Android\Sdk\android_openssl
```

**Without it the APK has no TLS, and nothing fails at build time.** Qt brings no TLS of its own to
Android: its OpenSSL backend plugin is in the APK either way and loads `libssl_3.so`/`libcrypto_3.so` at
run time. `CMakeLists.txt` adds them from `<ANDROID_SDK_ROOT>/android_openssl`; until 2026-10-07 it skipped
them silently when the folder was missing, and the APKs of 2026-10-05/06 went out without them. Every https
request then fails while the network status, which comes from Android, says online. The owner reported
on 2026-10-07 that maps did not load on the phone though the app showed internet (which tile source was
chosen, and whether the default "Limit downloads on metered networks" was holding them back too, was not
established). By the code, OpenStreetMap and the Baidu sources go over https and cannot load without
OpenSSL. Google Satellite goes over plain http (checked: `khm0.google.com` answers 200 with a JPEG, no
redirect). Now a missing folder is a CMake warning, and a found one prints
`-- Android OpenSSL: <the two libraries>`. The rebuilt APK with OpenSSL 3 inside: the owner's phone loaded
Baidu tiles over https the same day.

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
PC has, where "KoggerApp ME" is installed beside the official one:

```
scripts\win_build_android_release.ps1 -AppIdSuffix .me
```

The suffix reaches `platform/android/build.gradle` as `KOGGER_APP_ID_SUFFIX`,
which appends it to the application id (`org.kogger.koggerapp.me`) and
names the app "KoggerApp ME" on the launcher, through the
`${koggerAppLabel}` placeholder in `AndroidManifest.xml`. The file provider's
authority was already `${applicationId}.qtprovider`, so the two apps do not
collide there either. The Java package stays `org.kogger.koggerapp`; only the
installed identity changes. Without the switch the package is the normal one.
Until 2026-10-18 the suffix was `.myedition` ("KoggerApp myEdition"); a phone with that package gets
`.me` as a second app beside it, with settings of its own -- remove the old one by hand.

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

## A second build PC (2026-10-05)

The same toolchain was provisioned on a second PC from scratch, by these steps and the same versions (Temurin
17.0.20.1+1, NDK 27.3.13750724, both Qt Android kits). The first Gradle run there took 12 minutes (Gradle 8.10
and the Android Gradle Plugin's dependencies downloaded); the APK passed `apk_deps_check` (95 libraries per ABI).

That PC has its own throwaway key, `D:\Android\keys\myedition-test.keystore` (alias `myedition-test`, the
password in `myedition-test.password.txt` beside it; outside the repository and the synced folder). A device
keeps accepting updates only from the key it was installed with: the phone installed on 2026-10-05 takes
packages signed there, the tablet those signed with the first PC's key. Copy one key to both PCs to have one.

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
```

`win_build_android_release.ps1` then signs as well (since 2026-10-07), after it has run
`apk_deps_check` on the package:

```
scripts\win_build_android_release.ps1 -AppIdSuffix .myedition -Keystore D:\Android\keys\myedition-test.keystore -KeyAlias myedition-test -KeyPassFile D:\Android\keys\myedition-test.password.txt
scripts\win_build_android_release.ps1 -SkipBuild -Keystore ... -KeyAlias ... -KeyPassFile ...   # sign the last build again
```

The signed APK goes to `out_android\KoggerApp-<versionName>[-<suffix>]-<YYYYMMDD-HHMMSS>.apk`: name and
version come from the package itself (`aapt2 dump badging`), the stamp is when the build wrote the APK
(the owner's rule: every build says 1.0.3). Without the three key arguments a copy of the unsigned APK goes
to `out_android\unsigned\KoggerApp-...-unsigned-<stamp>.apk`. An existing file is never overwritten.
Checked 2026-10-07: `-SkipBuild` with the test key gives a byte-identical copy of the APK signed by hand
(SHA-256 `a6083aa7...`); a full build named its APK `...-20261007-115216.apk`; a repeat run and a key given
without its alias are refused. The password file is read by `apksigner` only (`--ks-pass file:`), never
printed. The Windows packages follow the same rule: `win_package_release.ps1` names them
`KoggerApp-ME-<version>-portable|setup-<stamp>` after the time of `KoggerApp.exe` (until 2026-10-18
`KoggerApp-<version>-myedition-<commit>[-dirty]-...`; the commit and "dirty" are in `build_info.txt`),
and `win_deploy_windows_release.bat` / `win_deploy_android_release.bat` stamp their archive and APK the same way.

`apksigner` signs with schemes v2 and v3 by default (v1 only when minSdk is
below 24; `--v1-signing-enabled true` adds it), and every device the app supports
takes that. Mind the check: `apksigner verify --verbose` verifies only what the
APK's own minSdk (28) needs and prints v1 and v2 as `false` even when they are
there; `--min-sdk-version 24` shows v2 too. A package with no signature at all
(the `-unsigned` output) is refused by every installer, and a phone's own
installer says so as a parse error — a Xiaomi one on 2026-10-06: "problem
parsing the package (33), packageInfo is null". A tablet
that already carries a release-signed KoggerApp will refuse the test-signed one
over the top — the signatures differ; uninstall first. Never ship a test-signed
APK: a device cannot upgrade from it to a release build either.

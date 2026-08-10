# Windows build from the command line

`README.md` describes the Qt Creator workflow. This document covers the scripted
alternative used by this checkout: no IDE, no Qt account, reproducible from a
clean machine.

Verified on 2026-08-09 against `develop` at `455b049e` (version 1.0.2), and again on
2026-08-10 on a second machine provisioned from nothing by exactly these steps —
581/581 targets, `sonifier_check` PASS on both frequency scales.

## Toolchain

| Component | Version | Location |
| --- | --- | --- |
| Qt | 6.8.3, kit `llvm-mingw_64` | `D:\Qt\6.8.3\llvm-mingw_64` |
| Compiler | Clang 17.0.6 (LLVM-MinGW) | `D:\Qt\Tools\llvm-mingw1706_64` |
| CMake | 4.2.3 | bundled with Visual Studio Build Tools |
| Ninja | 1.12.1 | bundled with Visual Studio Build Tools |

MSVC cannot be used: the Windows branch of `CMakeLists.txt` links the prebuilt
`third_party/freetype/lib/llvm-mingw-x64/libfreetype.a`, which is a MinGW archive.

## Installing Qt

[`aqtinstall`](https://github.com/miurahr/aqtinstall) fetches the official Qt
binaries from `download.qt.io` without a Qt account, which makes the install
scriptable and repeatable.

```
pip install aqtinstall
python -m aqt install-qt windows desktop 6.8.3 win64_llvm_mingw -m qt5compat qtmultimedia qtpositioning qtserialport qtshadertools -O D:\Qt
python -m aqt install-tool windows desktop tools_llvm_mingw1706 -O D:\Qt
```

Roughly 1.3 GB on disk (measured, `aqtinstall` 3.3.0, both commands together about
75 seconds on a warm connection). Every module in that list is required:

- `qtpositioning`, `qtserialport`, `qtmultimedia` — `find_package` components in
  `CMakeLists.txt`. `qtmultimedia` is an addition of this fork, required by
  `docs/echogram-sonification.md`.
- `qt5compat` — the QML tree imports `Qt5Compat.GraphicalEffects`. It is not a
  `find_package` component, so leaving it out still configures and builds; the
  app then fails at runtime instead.
- `qtshadertools` — `qtgraphicaleffectsprivateplugin.dll` imports
  `Qt6ShaderTools.dll`. Without it the plugin fails to load, the QML root object
  is never created, and `Core::UILoad` dereferences the null `QObject*` it is
  handed, crashing with `0xC0000005`.

## Building

```
powershell -ExecutionPolicy Bypass -File scripts\win_build_release.ps1
```

Parameters: `-QtRoot` (default `D:\Qt`), `-QtVersion` (default `6.8.3`),
`-Config` (`Release` / `Debug` / `RelWithDebInfo`), `-Clean`, `-ConfigureOnly`.

Output goes to `build\Desktop_Qt_6_8_3_llvm_mingw_64_bit-<Config>\KoggerApp.exe`,
the path `scripts\win_deploy_windows_release.bat` expects.

Two Windows-specific details the script handles:

- Path values passed as `-D...` use forward slashes. CMake writes them verbatim
  into its own generated `.cmake` files, where `\Q` and `\T` are invalid escapes
  and abort the configure step.
- Native tools are invoked through a helper that judges them by exit code only.
  Windows PowerShell 5.1 turns any stderr line from a native executable into a
  terminating error under `$ErrorActionPreference = 'Stop'`, and CMake writes
  ordinary warnings to stderr.

## Portable package

```
scripts\win_deploy_windows_release.bat
```

Produces `out_x64\` (139 MB, 60 entries), which runs with no Qt on `PATH`. Set the
`llvm_mingw` environment variable to override the toolchain path baked into the
script.

Two things the script does not do for you. It expects `windeployqt` already on
`PATH` — unlike `win_build_release.ps1`, which puts the toolchain there itself —
and it is interactive, asking about archiving through `set /p` and ending on
`pause`. Driven from a script it therefore needs both supplied:

```
export PATH="/d/Qt/6.8.3/llvm-mingw_64/bin:/d/Qt/Tools/llvm-mingw1706_64/bin:$PATH"
printf 'n\n\n' | cmd //c "scripts\win_deploy_windows_release.bat"
```

Check the result by the timestamp on `out_x64\KoggerApp.exe`, not by the exit
code: the script reports success even when every copy inside it failed. On a
checkout inside a sync folder that is a live hazard, not a hypothetical — see
`docs/HANDOFF.md`, "The sync client fights the deploy".

## Debugging a startup failure

The executable is built for the GUI subsystem, so Qt sends warnings to the
debugger rather than to stderr and a failed launch produces no output at all.
LLDB ships with the toolchain:

```
set PATH=D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin;%PATH%
lldb -b -o "settings set target.env-vars QT_ASSUME_STDERR_HAS_CONSOLE=1" -o "process launch" -o "bt all" -- build\Desktop_Qt_6_8_3_llvm_mingw_64_bit-Release\KoggerApp.exe
```

`QT_LOGGING_RULES=qt.qml.import.debug=true` additionally traces QML module and
plugin resolution, which is what identifies a missing Qt module.

# myEdition handoff

Everything needed to pick this checkout up on a different machine without
re-deriving it. Read this first; `build-windows-cli.md`,
`echogram-sonification.md` and `echogram-deconvolution.md` are the depth.
Accepted-but-unstarted ideas live in `backlog.md`.

Written 2026-08-09.

## What this checkout is

A private fork of [KoggerTech/KoggerApp](https://github.com/KoggerTech/KoggerApp),
cloned from branch `develop` at `455b049e485e792b766323d489d19c544f5d0277`
("Merge remote-tracking branch 'remotes/origin/master' into develop"),
`resources/version.txt` = 1.0.2. No submodules.

**Merged upstream `v1.0.3`** (`3a7f5266`, released 2026-09-07) on 2026-09-25 as merge commit
`f95f9845`; `resources/version.txt` is 1.0.3 since then. What that brought and how the ten
conflicts were resolved is under "Staying in sync with upstream".

It has **two remotes, and only one of them accepts a push**:

```
origin  https://github.com/KoggerTech/KoggerApp.git             (fetch)
origin  DISABLED_no_push_myEdition                              (push)
mine    git@github.com:MichaelShekunov/KoggerApp-myEdition.git  (fetch, push)
```

`origin` is upstream, fetch-only. Its push URL is deliberately broken so that a
stray `push` fails loudly instead of quietly publishing this fork to a public
repository. Undo only if that is what you actually want:

```
git remote set-url --push origin https://github.com/KoggerTech/KoggerApp.git
```

`mine` is a **private** repository under a personal account. It is where branch
`myedition` lives and where it is backed up. Private is a deliberate choice, not
an oversight — see "Why a private repository and not a GitHub fork" below.

It is reached over **SSH**, on a key dedicated to it: `~/.ssh/id_ed25519_github`,
pinned to `github.com` by `~/.ssh/config` with `IdentitiesOnly yes`. HTTPS works
just as well by hand, but it asks for credentials interactively, and anything
running without a terminal cannot answer that prompt. The key is deliberately
not the one that opens the Repka board — one key, one trust domain. Revoke it
under Settings → SSH keys.

A private remote does not change what may be written here. Keep firmware-derived
code, calibration data, and anything a customer gave in confidence out of this
tree: private today is not private forever, a repository is opened with one
click, and the history goes with it.

### Working from another machine

The checkout lives under `D:\YandexDisk\`, a Yandex Disk sync root replicated to
both machines, so there is nothing to transfer — open the same directory and carry
on. A third machine, or a rebuild after a disk failure, clones from `mine`.

It is a *sync folder*, not a network share. The distinction matters: a background
client (`YandexDisk2.exe`) holds files open while it uploads them, which breaks
the deploy step — see "The sync client fights the deploy" in the field notes.

Done for real on 2026-08-10. The second machine already had Visual Studio Build
Tools 18 at the exact path `CMakeCache.txt` pins, so only Qt was missing;
`aqtinstall` supplied it in about 75 seconds and the existing build tree stayed
valid without a reconfigure. Budget one Qt install, not an afternoon.

Three consequences of that arrangement:

**The build tree is machine-specific; the source tree is not.** `build/` is in
`.gitignore` but it is still on the shared drive, and its `CMakeCache.txt` pins
absolute paths from the machine that configured it:

```
CMAKE_MAKE_PROGRAM   C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/.../ninja.exe
CMAKE_COMMAND        C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/.../cmake.exe
CMAKE_CXX_COMPILER   D:/Qt/Tools/llvm-mingw1706_64/bin/clang++.exe
CMAKE_PREFIX_PATH    D:/Qt/6.8.3/llvm-mingw_64
```

If the other machine has a different Build Tools version, or Qt anywhere but
`D:\Qt`, reconfigure from scratch rather than trusting the cache:

```
powershell -ExecutionPolicy Bypass -File scripts\win_build_release.ps1 -Clean -QtRoot <your Qt root>
```

`out_x64/` is a build product too — regenerate it, never edit it.

**Binaries in `build/` do not travel; `out_x64/` does.** Both syncing machines see
the same `build/` tree, but everything in it links Qt from `D:\Qt` and finds
nothing if that kit is absent. On a machine without Qt,
`build\sonifier_check\sonifier_check.exe` fails with
`Qt6Core.dll: cannot open shared object file` and exit 127 — the binary is
present and useless. Only `out_x64/`, which `windeployqt` fills with its own
copies of the Qt DLLs, runs anywhere. If you want to hand someone a build without
handing them a toolchain, that is the directory.

**Sync is not locking.** Branch `myedition` is the record of what this fork
changed, and two machines can still write the same file between commits. Commit
before switching machines and push to `mine`; a collision then costs a merge
rather than an afternoon.

## Getting to a running build on a new machine

Full detail in [build-windows-cli.md](build-windows-cli.md). The short version,
assuming Windows and nothing installed:

1. **Visual Studio Build Tools** — needed only for the CMake and Ninja that ship
   with it (verified with 2026, 18.5.2: CMake 4.2.3, Ninja 1.12.1). Any CMake
   ≥ 3.22 and any Ninja on `PATH` do just as well. The MSVC compiler itself is
   **not** usable here, see below.
2. **Qt and the toolchain**, via `aqtinstall` — official Qt binaries, no Qt
   account, fully scriptable:

   ```
   pip install aqtinstall
   python -m aqt install-qt windows desktop 6.8.3 win64_llvm_mingw -m qt5compat qtmultimedia qtpositioning qtserialport qtshadertools -O D:\Qt
   python -m aqt install-tool windows desktop tools_llvm_mingw1706 -O D:\Qt
   ```

3. **Build**:

   ```
   powershell -ExecutionPolicy Bypass -File scripts\win_build_release.ps1
   ```

   `-QtRoot` defaults to `D:\Qt`; pass it if you install elsewhere. Output lands
   in `build\Desktop_Qt_6_8_3_llvm_mingw_64_bit-Release\KoggerApp.exe`.

4. **Portable package** (~139 MB, runs with no Qt on `PATH`):

   ```
   scripts\win_deploy_windows_release.bat
   ```

### Why not MSVC

The Windows branch of the top-level `CMakeLists.txt` links the prebuilt
`third_party/freetype/lib/llvm-mingw-x64/libfreetype.a`. That is a MinGW archive;
MSVC's linker cannot consume it. Upstream's supported Windows kit is Qt 6.8.3
`llvm-mingw_64` with LLVM-MinGW 17.0.6, and this fork stays on it. Switching to
MSVC means building freetype yourself and patching `CMakeLists.txt` — a permanent
divergence from upstream for no gain.

### Why each Qt module is in that list

- `qtpositioning`, `qtserialport`, `qtmultimedia` — `find_package` components.
  `qtmultimedia` is this fork's addition.
- `qt5compat` — the QML imports `Qt5Compat.GraphicalEffects`. Not a
  `find_package` component, so omitting it still builds and then fails at runtime.
- `qtshadertools` — `qtgraphicaleffectsprivateplugin.dll` imports
  `Qt6ShaderTools.dll`. Omitting it produces a startup crash with no message.

## What differs from upstream

Six modified files, eight new ones. `git diff` against `origin/develop` is the
authority; this is the intent behind each.

### Modified

| File | Change | Why |
| --- | --- | --- |
| `CMakeLists.txt` | `+ Multimedia` in `find_package` and `target_link_libraries`, `+ add_subdirectory(src/audio)` | the sonifier needs `QAudioSink` |
| `src/scene2d/qPlot2D.h` | `soundEnabled` property, `setSoundFrequencyScale`, `onChartAdded` slot, sonifier members | QML-facing surface of the feature |
| `src/scene2d/qPlot2D.cpp` | owns an `EchogramSonifier`; hooks in `setPlot`, `plotMousePosition`, `onCursorMoved` | live and cursor column sources |
| `qml/scene2d/Plot2D.qml` | speaker button in `settingsRow`, "Sound: log/linear" combo in the echogram settings, `onExited` on the MouseArea | the control surface |
| `resources/icons.qrc` | `+ icons/ui/speakerphone.svg` | the file was on disk but not registered, so the icon would not load |
| `scripts/win_deploy_windows_release.bat` | `-no-virtualkeyboard` → `--skip-plugin-types virtualkeyboard`, added an `errorlevel` check, `llvm_mingw` path now overridable and defaults to `D:\Qt` | the upstream script is broken on Qt 6.8.3, see field notes |
| `CMakeLists.txt` | `+ add_subdirectory(src/processing)` | the deconvolution module |
| `src/scene2d/plot2D_echogram.{h,cpp}` | deconvolution parameters and `applyDeconvolution`, run at the end of `updateCash` | where the along-track pass gets its 2D field |
| `src/scene2d/plot2D.{h,cpp}` | `getEchogramDeconv*` / `setEchogramDeconv*` passthroughs | matches how compensation is plumbed |
| `src/scene2d/qPlot2D.h` | `Q_INVOKABLE` deconvolution accessors | QML surface |
| `qml/scene2d/Plot2D.qml` | "Deconvolution" row in the echogram settings: enable, Beam°, Iter | the control surface |
| `src/id_binnary.h` | `+ BoardBaikalSonar = 100` in `BoardVersion` | the Baikal single-chip sonar reports board id 100; 100 was free |
| `src/device/dev_driver.h` | `+ ver == BoardBaikalSonar` in `isSonar()` | that predicate gates the echogram, ranger, DSP, transducer, dataset and sound-speed controls, so without it the device connects and does nothing |
| `src/device/dev_driver.cpp` | `+ case BoardBaikalSonar` in the device-name switch | otherwise the default branch names it "Device ID: 100.0" |
| `CMakeLists.txt`, `src/main.cpp` | `+ src/screen_keyboard.{h,cpp}`, context property `screenKeyboard` | on-screen keyboard controller (see the feature section) |
| `qml/app/MainWindow.qml` | `+ KScreenKeyboard { }` next to the overlays | the keyboard panel |
| `qml/app/AppSettingsPage.qml` | Interface → "On-screen keyboard" selector | Auto / On / Off |
| `qml/kqml_types/ZOrder.qml`, `qml/kqml_types/CMakeLists.txt` | `+ screenKeyboard` layer, `+ KScreenKeyboard.qml` | z-order and module registration |
| `src/proto_binnary.h`, `src/id_binnary.{h,cpp}` | `+ ID_WIFI = 0x57`, `+ BoardWiFi = 87`, `+ IDBinWifi` | the Wi-Fi module (see the feature section) |
| `src/device/dev_driver.{h,cpp}`, `src/device/dev_q_property.h` | `idWifi`, `isWifi()`, `wifi*` readers, `Q_INVOKABLE wifi*` commands, `wifi*Changed` signals | QML surface of the module |
| `qml/app/DeviceSettingsPage.qml` | "Wi-Fi" island, `WifiBars` / `WifiField` components, baud lists to 4 000 000, `*.ufww` upgrade filter | the control surface |
| `src/link/link_manager.cpp`, `src/link/link_defs.h` | `KOGGER_SERIAL_PORTS` opt-in for non-USB ports on Linux; 1 M, 1.5 M, 3 M in `baudrates` | an on-board UART is otherwise never listed |

The `.bat` change is the only edit that is not about a new feature. It is a
genuine upstream bug and a candidate to report back.

### New

| Path | What |
| --- | --- |
| `src/audio/echogram_sonifier.{h,cpp}` | the synthesiser: oscillator bank + `QAudioSink` |
| `src/audio/CMakeLists.txt` | module glue, matches the `src/scene2d` pattern |
| `tools/sonifier_check/` | standalone program that proves the audio path works |
| `scripts/win_build_release.ps1` | scripted configure + build |
| `docs/build-windows-cli.md` | toolchain, install, build, how to debug a startup failure |
| `docs/echogram-sonification.md` | the feature in detail |
| `src/processing/echogram_deconvolution.{h,cpp}` | Richardson-Lucy along-track solver, no Qt in it |
| `src/processing/CMakeLists.txt` | module glue, matches the `src/audio` pattern |
| `tools/deconv_check/` | standalone program that measures what the deconvolution recovers |
| `docs/echogram-deconvolution.md` | the feature in detail, with the measured envelope |
| `docs/backlog.md` | accepted but unstarted ideas |
| `docs/HANDOFF.md` | this file |
| `src/screen_keyboard.{h,cpp}` | on-screen keyboard controller |
| `qml/kqml_types/KScreenKeyboard.qml` | on-screen keyboard panel |
| `docs/WIFI_MODULE.md` | the Wi-Fi module: protocol contract and what the app does with it |

`tools/sonifier_check` and `tools/deconv_check` are separate CMake projects, not
part of the application build. Nothing in the app references them. `deconv_check`
needs no Qt at all, because the solver it tests has none.

## Feature: on-screen keyboard

Added 2026-09-25 for the Orange Pi 5 Max head unit (touchscreen, no physical keyboard), but
generic: it works on any touch device and is invisible on a desktop unless switched on.

- `src/screen_keyboard.{h,cpp}` — `ScreenKeyboard`, exposed to QML as the context property
  `screenKeyboard`. Watches `QGuiApplication::focusObject()`; when an editable
  `TextInput`/`TextField`/`TextEdit`/`TextArea` has focus it reports `inputActive`,
  `passwordMode` (echoMode), `numericMode` (`inputMethodHints`) and `focusRect` (window
  coordinates). `sendText()` delivers a `QInputMethodEvent` commit, `sendKey()` a key
  press/release pair — both through the focus object's own `event()`, so application-wide
  event filters (`InputDeviceTracker`, hotkeys) never see synthetic keys. Visibility:
  `mode == On`, or `mode == Auto` and the last physical input was a touch
  (`InputDeviceTracker::touchMode`); `hide()` dismisses until focus moves. The mode lives in
  QSettings `main/ui/screenKeyboardMode` (0 auto, 1 on, 2 off).
- `qml/kqml_types/KScreenKeyboard.qml` — the panel: Latin, Cyrillic (RU/EN key), symbols
  (`?123`), numeric pad for number fields; shift with caps lock on a double tap, repeating
  backspace, Done (sends Return and hides), chevron to hide. It opens under the focused field
  when there is room, above it otherwise, and never takes focus itself. Sized from `Tokens` /
  `AppPalette.scale`, about 640×190 px at scale 1.
- `qml/app/MainWindow.qml` instantiates it once (`z: ZOrder.screenKeyboard`, 6600 — above
  sidebars and popups, below the power-off overlay). `AppSettingsPage.qml` → Interface →
  "On-screen keyboard: Auto / On / Off".

Verified by machine: clean Windows build (591/591), app starts with no QML warnings.
Confirmed by the user on the head unit touchscreen (2026-09-25): keys work over the whole
window. The first build closed the keyboard when a key above a workspace pane was tapped:
pane overlays call `forceActiveFocus()` on hover enter, and a touch synthesises hover for the
items behind the panel. Fixed by a hover-enabled MouseArea shield under the keys (commit
fd301826). Not verified: Cyrillic glyphs rely on the app font.
To try it on a desktop set the mode to On and tap any text field.

## Feature: Wi-Fi module (board 87)

Added 2026-09-25 for the head unit, which has no Wi-Fi of its own: a small ESP32-C3 module on the
board's UART speaks Kogger SBP and acts as the Wi-Fi client. Generic in the app: any serial link
that answers `ID_VERSION` with board 87 gets the island. Contract and field meaning:
[WIFI_MODULE.md](WIFI_MODULE.md).

- `IDBinWifi` (`src/id_binnary.{h,cpp}`) parses status v0, link report v1, scan frames v2
  (assembled by index, an 8 s timeout ends a scan whose frames stopped coming) and the saved list
  v4; sends the report period v1, join v3 and actions v4 through `hashBinFrameOut()`, so the
  module's `{code, checksum}` confirmations are checked by the existing `IDBin` machinery.
- `DevDriver` registers `idWifi` as a non-setup id and asks it once per connection for board 87
  only (`process()`); everything after that is pushed by the module. `receivedWifi()` fans the
  frames out to `wifiStatusChanged` / `wifiReportChanged` / `wifiNetworksChanged` /
  `wifiSavedChanged`, re-reads the saved list after a confirmed action and after a join succeeds,
  clears the scanning state on a refused scan, and reports refusals as `wifiCommandResult(version,
  resp)`. The connection reset path clears the mirror so a re-plugged device never shows stale data.
- `DevQProperty` exposes `isWifi` and the `wifi*` properties; the island in
  `DeviceSettingsPage.qml` is visible for `dev.isWifi` only and, like the basic sonars, gets the
  "Advanced settings" cut. The join rows use `WifiField` (a `TextInput` with `echoMode` toggled by
  an eye button), which is what the on-screen keyboard reads to switch into its password layout.
- The module joins with exactly the password it is sent and never reveals a stored one, so a saved
  network is rejoined either by its own auto-connect or by typing the password once more; the
  island says so in the caption.
- Linux lists only USB serial adapters. An on-board UART is opted in by name through
  `KOGGER_SERIAL_PORTS` (comma-separated, e.g. `ttyS6`); the head unit's kiosk service sets it.
- Boat link (module firmware 0.7+): `IDBinWifi` also carries the relay configuration v5 and the
  counters v6; `DevDriver::wifiSetRelay()` / `wifiRelayRefresh()` / `wifiRelayStatsRefresh()` and
  the `wifiRelay*` properties feed a second island, "Boat link". With the relay on, the module
  forwards frames not addressed to itself to the boat and writes the boat's frames into the port,
  so the boat's devices show up as further devices on the same link (one `DevDriver` per SBP
  address, as always) while the module answers at the configured address (87). A firmware that
  refuses v5 with ERR_VERSION is remembered as "no relay" and the island shows a hint instead.
- Topology: `DeviceTopologyModel` puts a Wi‑Fi module first in a link's members (node role 3,
  "bridge"), before the recorder and its port devices, because that is the order the data
  physically travels: the owner asked for Wi‑Fi → logger → sonar(s) after seeing
  logger → sonar → Wi‑Fi. The bridge → recorder connector is drawn dashed.

Verified by machine (2026-09-25): Windows build 74/74 after the C++ change and again after the
QML change; `qmllint` on the page reports nothing new beyond the pre-existing unresolved-import
noise. Board 87 was probed over the head unit's UART6 at 921600 with the module's own bench
script before this code was written (identity, v0-v4, report period, scan of 14 networks, saved
list, error codes: 16/16). Verified on the head unit the same evening: the Wi‑Fi device appears on `ttyS6` and the island
works (owner); with module firmware 0.7.0 and the relay set to the boat (10.0.0.10:14444 UDP,
module at address 87) the boat's logger and its sonar appear on the same link and the stream
flows (owner saw the three devices; counters and UART rates by machine). Not yet verified: the
`.ufww` upgrade path from the app (the module was updated with the module project's own updater
over the same UART), 3 and 4 Mbaud.

## Feature: boat mission (MAVLink)

Added 2026-09-26 for the head unit: waypoints placed on the map in the 3D pane go to the boat's
ArduPilot Rover autopilot as a mission, are read back and compared, and the mission is started,
held, sent home or stopped from the same panel. Details, the ArduPilot facts it rests on (measured
on SITL) and how to run the check: [BOAT_MISSION.md](BOAT_MISSION.md).

- `src/vehicle/mav_vehicle.{h,cpp}` (`MavVehicle`, context property `boat`) owns the MAVLink side and
  is transport-agnostic; `DeviceManager` feeds it every MAVLink frame of a live link and writes its
  bytes back into the autopilot's link. The official MAVLink C library is vendored in
  `third_party/mavlink` (MIT, listed on the About page).
- The 3D view got `geoToScreen`, `screenToGeo`, `cameraChanged`, `mapTapEnabled` and `mapTapped`; the
  route and markers are Qt Quick Shapes in `qml/scene3d/BoatMissionLayer.qml`, the panel is
  `qml/scene3d/BoatMissionPanel.qml`, opened from the boat button of the right 3D toolbar.
- `DeviceManager` now forgets `mavlinkLink_` when that link closes or is deleted (it used to keep the
  pointer).
- Verified: `tools/mission_check` against ArduRover 4.4.0 SITL, 17/17 (upload + read-back, edit
  invalidation, start, waypoint progress, HOLD, RTL home, disarm); Windows build and start clean.
  Not verified: the panel on the touchscreen, a mission on water.

## Wi-Fi module firmware 0.10 in the app (2026-09-26)

The module project added radio control (`ID_WIFI` v7), an access-point role and `ID_WIFI_NET` 0x58 (role,
access point, address/DHCP, two UART lines with counters, clients, own address). The app decodes and
edits all of it: `IDBinWifiNet` in `src/id_binnary.*`, properties `wifiRadio`, `wifiNet*` on
`DevQProperty`, four islands on the Wi-Fi module's settings page, the Wi-Fi island aware of the AP
state. Contract as implemented: WIFI_MODULE.md §2b. The module on the head unit was updated 0.7.0 →
0.10.0 over its own serial link the same day (module project's `sbp_update.py --sbp-only --route 87`);
the relay record carried over into line 0 and the boat link came back.

Boat panel fixes after the owner's first try (taps on the map did nothing visible): `Pane3DWindow.qml`
lacked `import kqml_types` (ReferenceError on Tokens/AppPalette in the panel's bindings), point
placement is now on as soon as the panel opens, the pane's double-tap fullscreen is off while points
are placed, and taps and added points go to the app log (`scene:` / `boat:` lines).

Second round (owner: "the menu at the top covers the icons"):
- The app's quick-actions bar (`HotActionsPanel` in `MainWindow.qml`, z above the panes) covered the
  panel's header. `WorkspaceView.hotActionsItem` now carries the bar; `Pane3DWindow.hotActionsInset`
  measures how far it reaches into the pane's top-left corner and the panel starts below it. The map
  banner from the first round is gone; the hint ("tap the map to add a point, drag a numbered point to
  move it, H is home") sits in the panel under "Add points".
- The waypoint markers never worked: the layer's root id was `layer`, and inside the Repeater delegate
  `layer` resolves to the delegate's own `Item.layer` (the scope object is searched before the outer
  component's ids). Markers had zero size at 0,0 and every plan change logged TypeErrors; the route line
  was fine. The id is `missionLayer` now. `qmllint` catches this class of bug (`Member "_pts" not found on
  type "QQuickItemLayer"`): run it with `-I <build>/qml` on changed QML before a deploy.
- `MavVehicle` logs the first mode and arm state it sees (`boat: Mode HOLD, disarmed`), so the log tells
  whether the boat is armed before the head unit is restarted.

## Boat start: navigation readiness, failed-start disarm, autopilot diagnostics (2026-09-26)

The owner's first start on the boat: the app armed it, DO_SET_MODE AUTO came back FAILED ("Flight
mode change failed"), and the boat stayed armed in HOLD. Rover 4.4 refuses a position mode once armed
without an EKF position; the app had only looked at the GPS fix. Now:
- the MAVLink library uses ArduPilot's dialect (`third_party/mavlink/ardupilotmega` and what it
  includes, same c_library_v2 commit) for EKF_STATUS_REPORT;
- `MavVehicle` judges navigation readiness from it like Rover does (`navState`, `navText`, panel line),
  refuses to start without it, and disarms again after a start it armed fails;
- the autopilot's own sensor health (SYS_STATUS) and its navigation settings go to the log and the
  panel, because the boat's ARMING_CHECK = 0 silences every "PreArm" reason.
On the boat this showed at once: GPS 3D with 27–30 satellites, EKF flags 0 and variances 0, AHRS/EKF
unhealthy, EK3_SRC1_YAW = 3 (GPS yaw with compass fallback). With a one-antenna GPS that yaw source
never aligns (BOAT_MISSION.md, "ArduPilot facts"); the fix is on the boat (EK3_SRC1_YAW = 1 and a
calibrated compass), the owner decides. `tools/mission_check` 36/36; details in BOAT_MISSION.md.

## Feature: echogram deconvolution

Sharpens the echogram along the track by undoing the beam's footprint, which
grows with range as `W(R) = 2*R*tan(theta/2)`. Richardson-Lucy, one dimension,
one kernel per row, sized in metres from navigation. The angular resolution is
typed in by the operator — no beam pattern lives in this repository.

Restores almost all of a one-column smear and about a third of a 2.4-column one;
past three columns the information is gone rather than attenuated, and no
iteration count recovers it. Full description and the measured envelope in
[echogram-deconvolution.md](echogram-deconvolution.md).

## Feature: echogram sonification

Plays the echogram like a waterfall spectrogram: near edge of the visible range
→ 7 kHz, far edge → 400 Hz, sample brightness → loudness. Speaker button on the
echogram itself; log/linear scale in the echogram settings.

Full description, including the signal chain and the exact brightness mapping, is
in [echogram-sonification.md](echogram-sonification.md).

## State of play

**Verified by machine.**

- Clean build, 580/580 targets from scratch, zero compiler warnings. Only
  `lrelease` complains, about duplicate messages in upstream's `.ts` files.
- App starts, loads QML with no errors, creates its window, exits 0 on close.
- `out_x64\KoggerApp.exe` runs with `PATH` reduced to `C:\Windows\System32;C:\Windows`,
  proving the portable package is self-contained.
- `tools\sonifier_check\` → 48000 Hz, `sink state=0 error=0`, PASS on both
  frequency scales: Qt really pulls the generated samples.

Reproduced end to end on the second machine on 2026-08-10, from a box with no Qt
at all: 581/581 targets, `sonifier_check` PASS on both scales, and `out_x64\KoggerApp.exe`
launched under `PATH=C:\Windows\System32;C:\Windows` to a window titled
`KoggerApp, KOGGER - 1.0.2`. The toolchain instructions in
[build-windows-cli.md](build-windows-cli.md) are known-good, not just remembered.

**Confirmed by ear.** Sound plays from a recorded echogram file (user, 2026-08-09).

**Not verified.**

- Live-device path (`Dataset::chartAdded` → column → sound). No sonar was
  connected; only the file/cursor path has been exercised.
- Pointer-follow responsiveness after the latency fix of 2026-08-10, below.
  Rebuilt and deployed, not yet tried by hand.

**Fixed 2026-08-10: the sound lagged the pointer by about half a second.**

Reported by the user as "very slow, needs to be realtime". It was not the event
rate — `Plot2D.qml` has `hoverEnabled: true` and calls `onCursorMoved` on every
mouse-move — and it was not the de-duplication either. Two independent buffers:

1. `QAudioSink` defaults to a **500 ms** buffer and `setActive` never overrode it,
   so the sound ran 419 ms ahead of the wall clock (measured).
2. `readData` copied the oscillator targets **once per call**, and a call covers up
   to 85 ms, so a column pushed mid-buffer was not picked up at all.

Now `setActive` calls `setBufferSize` before `start()` (22 ms granted on
Windows/FFmpeg, 35 ms end-to-end) and `readData` re-reads targets every 64 frames.
`sonifier_check` prints the sink buffer and fails above 100 ms. Detail and the
before/after table are in [echogram-sonification.md](echogram-sonification.md).

The lesson generalises past this feature: an audio path can be perfectly correct
and still feel broken, and the sink's default buffer is the first thing to measure.

**Fixed 2026-08-10: the sweep stepped between epochs instead of gliding.**

How many screen columns one epoch occupies depends on zoom, and the sound held
constant across all of them, then jumped. Amplitude smoothing could not fix this
and neither could dropping the de-duplication, because the targets genuinely were
constant across a run. The cursor path now blends the two neighbouring epochs,
interpolating linearly between the centres of their column runs — centre to
centre, so the weight is continuous across the seam. Detail in
[echogram-sonification.md](echogram-sonification.md), "Interpolation".

Two smaller changes went with it: sound is produced before `Plot2D::onCursorMoved`
rather than behind its `plotUpdate()`, and the amplitude glide went from 5 ms to
8 ms now that consecutive targets differ by little.

**Open.**

- Nothing outstanding on responsiveness. If a sweep still reads as stepped, the
  remaining granularity is one screen column, and the knobs are `kSmoothMs` in
  `echogram_sonifier.cpp` and the interpolation weight in
  `qPlot2D::sonifyAtCanvasPos`.
- `Core::UILoad` (`src/core.cpp:1886`) dereferences its `object` parameter with no
  null check, first at line 1899. Qt passes `nullptr` when a QML component fails
  to load, so every QML failure becomes an access violation instead of a readable
  error — this is how the missing `qtshadertools` presented. `src/main.cpp:376`
  guards the same signal correctly. Upstream bug, unfixed here.
- Cursor-to-epoch lookup uses `Plot2D::getEpochIndxByMousePos`, which reads
  `cursor.indexes` directly and ignores the echogram's wrap rotation. With wrap
  on, the sounded column can differ from the one drawn under the pointer.
  Pre-existing behaviour shared with the contact layer.

## Field notes

Traps hit while getting this working. Each cost real time; none is obvious.

**CMake paths need forward slashes.** CMake writes `-D` path values verbatim into
its own generated `.cmake` files, where `\Q` and `\T` are invalid escapes. Passing
`D:\Qt\Tools\...\windres.exe` aborts the configure step inside
`CMakeRCCompiler.cmake`. `scripts\win_build_release.ps1` converts them.

**PowerShell 5.1 turns native stderr into a terminating error** under
`$ErrorActionPreference = 'Stop'` — so a harmless CMake *warning* fails the
script, and the real message gets mangled into a PowerShell error record. Judge
native tools by exit code only; the build script has an `Invoke-Native` helper for
this. When you need clean output from a native tool, run it from Bash instead.

**`windeployqt` in Qt 6.8.3 has no `-no-virtualkeyboard`.** Upstream's
`win_deploy_windows_release.bat` passes it and does not check the exit code, so
on the Qt version the project itself specifies it silently produces an `out_x64`
containing the executable and three runtime DLLs and no Qt at all — 22 MB instead
of 139 MB. Fixed here; worth reporting upstream.

**A running instance blocks the deploy just as effectively.** If `out_x64\KoggerApp.exe`
is open, `windeployqt` gets `access denied` on every file it tries to replace and
the package silently keeps the previous build. Observed 2026-08-10, and easy to
mistake for the sync-client race below — the symptoms differ: a locked-by-sync
file reports "being used by another process" and clears on its own, while a
running application reports "access denied" and never clears. Check with
`Get-Process KoggerApp` before assuming anything else.

**The sync client fights the deploy.** `out_x64/` is inside the Yandex Disk sync
root, and rewriting 139 MB makes the client open every file it is uploading. The
deploy step then hits `The process cannot access the file because it is being used
by another process`: `rmdir /s /q "%outPath%"` fails silently (that is what `/q`
buys you), `copy` of `KoggerApp.exe` fails, `libunwind.dll` and `libc++.dll` report
`0 files copied` — and the script still exits 0, so nothing downstream notices.
Observed 2026-08-10; the directory kept a stale executable from the other machine
while looking complete at 60 files and 139 MB.

Re-running once the client has settled works, but that is a race, not a fix.
The durable answer is to keep build products out of the sync root — exclude
`build/` and `out_x64/` in the Yandex Disk client, or build outside
`D:\YandexDisk\`. Either also spares the sync a few hundred MB of churn per build.
Until then, judge a deploy by `KoggerApp.exe`'s timestamp in `out_x64/`, never by
its exit code.

**The deploy script cannot be run unattended.** `win_deploy_windows_release.bat`
asks `Do you want to archive the output directory? (y/n)` through `set /p` and
ends on `pause`, so with no console it stalls or falls through; and it expects
`windeployqt` already on `PATH` — the "add to path" block at the top is a comment
addressed to a human, not code. `scripts\win_build_release.ps1` puts the toolchain
on `PATH` itself, the `.bat` does not. To drive it from a script, prepend
`D:\Qt\6.8.3\llvm-mingw_64\bin` and `D:\Qt\Tools\llvm-mingw1706_64\bin` to `PATH`
and feed it an answer:

```
printf 'n\n\n' | cmd //c "scripts\win_deploy_windows_release.bat"
```

Making the prompt and the `pause` conditional on a parameter, and having the
script locate `windeployqt` the way the PowerShell one locates `cmake`, would
remove the whole class. Not done here.

**The executable is a GUI-subsystem binary**, so Qt sends warnings to the debugger
and a failed launch prints nothing whatsoever. To see anything:

```
set PATH=D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin;%PATH%
lldb -b -o "settings set target.env-vars QT_ASSUME_STDERR_HAS_CONSOLE=1" -o "process launch" -o "bt all" -- build\Desktop_Qt_6_8_3_llvm_mingw_64_bit-Release\KoggerApp.exe
```

`lldb` ships with the LLVM-MinGW toolchain. Add
`QT_LOGGING_RULES=qt.qml.import.debug=true` to trace QML module and plugin
resolution — that is what identified the missing `qtshadertools`.

**A missing Qt module can look like a compiler-level crash.** The chain was:
module absent → plugin DLL cannot resolve `Qt6ShaderTools.dll` → QML root object
never created → `Core::UILoad(nullptr)` → `0xC0000005` with no output. Check the
plugin's imports with `llvm-objdump -p` before suspecting anything else.
`api-ms-win-crt-*` entries in that output are OS API-set stubs, not files; they
are not missing.

**A control added to `Plot2D.qml` alone is invisible.** The settings surface the
user actually sees is `qml/app/EchogramSettingsPanel.qml`, and it does not touch
the plot: it reads and writes a view-state object built from `property alias`es
onto the controls inside `qml/scene2d/Plot2D.qml`. The inline controls are the
model, the panel is the view. A control added only inline works, persists, and
never appears — which is how the deconvolution shipped the first time. Three
edits, not one: the inline control, the alias (plus its `_changeProbe` entry),
and a row in the panel.

**Adding an icon means editing `resources/icons.qrc`.** `resources/icons/ui/` holds
208 SVGs; only 180 are registered. A file being on disk does not put it in the
resource system.

## Why a private repository and not a GitHub fork

The Fork button would give a shorter path to pull requests back to KoggerTech.
Two properties of forks rule it out here.

**A fork of a public repository is public, permanently.** GitHub offers no
visibility switch on it. The only route to a private copy is the one set up
here: a separate repository that happens to share history.

**Objects pushed into a fork network stay reachable across it.** A commit pushed
to one repository in the network can be fetched from another, and deleting the
branch does not take it back. A separate repository is not in that network.

Nothing is lost by giving the relationship up. It is a web-interface
convenience with no effect on git: pulling upstream works exactly as described
below, because it goes through `origin`, which still points at KoggerTech. A
pull request back to upstream remains possible — it needs a fork made at that
moment, which costs one click and one push.

## Staying in sync with upstream

```
git fetch origin
git log --oneline HEAD..origin/develop
git merge origin/develop
```

The v1.0.3 merge (2026-09-25, 116 upstream commits, 393 files) auto-merged 338 files and
conflicted in ten. Both sides were kept everywhere:

- pane modes: ours `2/3D` (relief) and upstream `Video` now coexist in `LayoutRules.js`,
  `LayoutTree.js`, `PaneContentLoader.qml`, `PaneEditorChrome.qml` (3D and 2/3D still share
  the single scene slot, Video is unconstrained);
- `DeviceSettingsPage.qml`: upstream rewrote device settings as islands; our echogram tab
  (Off / 8-bit / Compressed / IQ / RAW + USB-tap status) was re-expressed inside
  `KIslandSection`/`DevStackedRow`/`KIslandRow`, our dataset tab replaces upstream's
  `dataset.chart` row, our device-network groups sit before the "Settings file" island;
- `ConnectionViewer.qml`: Discover (ours) next to +RTSP (upstream);
- `dev_driver.cpp`: upstream reworked the firmware-upgrade retries into
  `checkUpgradeTimeouts()`; the FWTRACE lines were moved there;
- `CMakeLists.txt` (kec block kept), `main.cpp` (both includes), `scene3d_renderer.h` (relief
  and USBL render implementations).

Upstream 1.0.3 links `src/video` against the FFmpeg 7.1 libraries **from the Qt installation**
(`src/video/CMakeLists.txt`; headers vendored in `third_party/ffmpeg`). The Windows kit under
`D:\Qt` ships them. A Qt built from source (the head unit) does not: FFmpeg 7.1 is built there
and linked into `<Qt>/lib` (HeadUnit `deploy_myedition/22_build_ffmpeg.sh`, `23_install_ffmpeg.sh`).
Verified after the merge: Windows build 680/680 clean, app starts with no QML warnings,
`sonifier_check` PASS on Windows (22 ms) and on the board (100 ms); the kiosk runs it.
Not verified: Baikal device path (no board), live sonar device page.

The merge was done in a throwaway worktree (`git worktree add D:\Temp\kogger_merge_test`),
outside the sync root, and fast-forwarded into `myedition` only after the checks. Recommended
for the next one too: the sync client and a 300-file merge do not mix.

After any merge, rebuild and rerun `tools\sonifier_check` — it is the only
automated check this fork has. Then `git push mine myedition`, so the merge does
not sit on one machine only.

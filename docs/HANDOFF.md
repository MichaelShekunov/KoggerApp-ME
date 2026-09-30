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
| `qml/kqml_types/ZOrder.qml`, `qml/kqml_types/CMakeLists.txt` | `+ screenKeyboard` layer, `+ KScreenKeyboard.qml`, `+ WifiSignalIcon.qml` | z-order and module registration |
| `src/proto_binnary.h`, `src/id_binnary.{h,cpp}` | `+ ID_WIFI = 0x57`, `+ BoardWiFi = 87`, `+ IDBinWifi` | the Wi-Fi module (see the feature section) |
| `src/device/dev_driver.{h,cpp}`, `src/device/dev_q_property.h` | `idWifi`, `isWifi()`, `wifi*` readers, `Q_INVOKABLE wifi*` commands, `wifi*Changed` signals | QML surface of the module |
| `qml/app/DeviceSettingsPage.qml` | "Wi-Fi" island, `WifiBars` / `WifiField` components, baud lists to 4 000 000, `*.ufww` upgrade filter | the control surface |
| `qml/kqml_types/KTabBar.qml` | an option may carry `enabled: false` (dimmed, never selected by click, drag or keys); options without it behave as before | the Wi-Fi Radio island switches off 40 MHz where the protocols have no 802.11n |
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
| `qml/kqml_types/WifiSignalIcon.qml` | the Wi-Fi symbol lit by signal level (Devices chips, toolbar badges) |

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
  QSettings `main/ui/screenKeyboardMode` (0 auto, 1 on, 2 off). Since 2026-09-29 focus that arrives during a
  touch waits for that touch to end as a tap (`TouchTapGate`; see "Field test notes (2026-09-29)").
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
unhealthy, EK3_SRC1_YAW = 3 (GPS yaw with compass fallback: the first yaw alignment needs a GPS heading).
The boat has a two-antenna u-blox moving baseline (GPS_TYPE 17/18, antennas 0.52 m apart; a first guess of
a one-antenna GPS was wrong), and the app now reads the GPS heading itself (yaw fields of GPS_RAW_INT /
GPS2_RAW) and the GPS settings: the rover sits in RTK float, and ArduPilot takes the heading only from an
RTK fixed solution. So the fix is at the boat (sky view, antennas, interference), not in the app. The EKF
report alternates between two lanes (0x0000 / 0x0400): one reason for both, and a new reason within the
same state is logged at most once a minute. `tools/mission_check` 41/41; details in BOAT_MISSION.md.

## Boat panel after the first indoor tests (2026-09-26)

Owner's requests: heading source switch (`MavVehicle::setHeadingSource`, EK3_SRC1_YAW 3 or 1, refused
while armed), folding point list, one-tap Disarm, speeds 0.5/0.8/1/1.5/2, "Load from boat"
(`loadPlanFromVehicle`). mission_check 45/45 on SITL. Field diagnosis added at the same time: every
station change of the Wi-Fi module and its restarts are `wifi:` lines, and the boat link logs "restored
after N s" (it never logged restores before). That showed the module on the head unit restarting every
~36 s with its UART at the 921600 ceiling (HeadUnit docs/UART_ESP32.md) — a module firmware matter.

## Wi-Fi module port speed from the app (2026-09-26)

"Port speed" island on the Wi-Fi module's page (WIFI_MODULE.md §2c): set, follow with the link, verify an
answer at the new speed, save with ID_FLASH, or roll the link back. The Actions island's "Set baudrate" moves
the link as well (it used to change only the device); that was the old Actions group, removed 2026-09-29. On the head unit the owner went 2M → 3M → 4M → 2M: each
step answered at the new speed within 1.4 s and was saved (log, 2026-09-26 03:19-03:21).

## Wi-Fi Radio island: menu = what is in effect (2026-09-26)

Owner: what the menu shows must always be real. The Radio island now syncs to the values in effect (width from
the negotiated PHY), marks unapplied selections, and after Apply saves what is in effect when the module could
not do what was asked (WIFI_MODULE.md §2b'). The module on the head unit was set to 20 dBm / b/g/n / 20 MHz the
same day (`wifi_radio_set.py`): saved and in effect agree.

## Review fixes (2026-09-27)

A full review of d5f4c100..c83968ef found ten issues, all fixed in afd16b68:
- The port-speed change and the post-Apply radio check ran in timers of `DeviceSettingsPage`, which is
  unloaded as soon as it is left: leaving mid-change could leave the module and the link at different speeds,
  or an impossible radio setting saved. Both run in `DevDriver` now (`WifiPortChange`, `WifiRadioCheck` in
  `src/device/wifi_ops.*`; the transport comes through hooks, so mission_check drives them on made-up events).
- Module restarts: `WifiRestartWatch` compares the uptime with the time between reports (the 5 s drop margin
  missed boot loops). Line 1 speed is checked (9600–4 000 000) before sending.
- `MavVehicle`: a refused start or heading-source change no longer renames the running command; the settings
  are read again after a link loss while the heading source or EKF threshold is unknown (a mutant without
  this fails 4 SITL checks: the heading switch stays "settings not read yet"); EKF polling stops after 10
  unanswered requests; a lost final MISSION_ACK is settled by the read-back; GPS-heading and health changes are
  logged once they hold 3 s, with the number of flips.
- `tools/mission_check` 67/67 (was 45). The QML islands are only checked by qmllint (no new warnings; it cannot
  resolve `kqml_types`, so `dev.*` names were compared with the C++ declarations by script).
- Wi-Fi module firmware 0.11 (2026-09-27, 7691d851; WIFI_MODULE.md §2d): reset reason from v1, power up to 80,
  access-point channel 1–11, own address 1–255, LR only not offered and no PHY wait in the access-point role.
  mission_check 72/72. The head unit's module was updated to 0.11.0 the same day (HeadUnit docs); the kiosk branch
  with this support (`headunit` 23bcf614) is on the kiosk since 2026-09-27 (log: `wifi: module up 1297 s, last reset:
  software: update, role change or reboot (3)`, no QML errors). Not an app matter either: ~2 min after the module
  update the boat's stream mostly stopped; the head unit module received almost nothing over the air (the owner read
  the rate on its page), and restarting the boat alone restored the link, clean for 11+ minutes — the boat side hung
  (its module with old factory firmware, or its logger), not the head unit's 0.11. Whether the 2-minute relay gap
  during the update set it off is open (HeadUnit docs/STATE.md).
- On the head unit since 2026-09-27 (`headunit` 74b5afee), with the live boat (Verified, kogger.log): autopilot
  found 0.4 s after start, settings read 14 s after detection, the heading line held and logged once ("after 2
  changes"), no QML errors, no "module restarted" line in 3 minutes of once-a-second uptime reports. Not yet
  tried on the device: leaving the page during a port-speed change or right after a radio Apply.

## Wi-Fi module firmware 0.12 in the app: two equal ports (2026-09-27)

The module's 0.12 contract makes X1 and X2 equal (discovery on either, rates for both, `ID_WIFI_NET` v7 port pages).
The app side, WIFI_MODULE.md §2e:
- `WifiPorts`, `WifiByteRate`, `WifiLineChange` (+ `WifiLineEdit`) in `src/device/wifi_ops.*` (pure, driven by `tools/mission_check`);
  `IDBinWifiNet` decodes v7 through `WifiPorts` and sends the v7 requests; `DevDriver` probes v7 once per connection,
  treats ERR_VERSION or 4 s of silence as "firmware before 0.12", exposes `wifiNetPorts`, `wifiNetPortDevices`,
  `wifiNetPortPeers`, `wifiNetOwnPort`, `wifiNetPortSupport`, `wifiPortLine` and `wifiNetRefreshPorts()`.
- `wifiSetLineBaud(line, baud)` replaces `wifiSetPortBaud`: the app's own port (the module's flag b2; line 0 before 0.12)
  keeps the checked `WifiPortChange`, now rolling back to the link's real rate; the other port goes through its v3 record.
- The board-name switch of `receivedVersion` moved into `DevDriver::knownBoardName()` so the port device list names
  boards the same way (same strings, same fallback).
- Page: a "Ports" island (two cards, side by side or stacked) with v7; the Lines island and the address wording follow
  b2 instead of "line 0 is this port". Without v7 (the head unit's 0.11 today) the page is as before.
- `tools/mission_check --offline` runs only the checks on made-up events (no SITL): 67/67 with the new 28.
- Verified by machine: build clean, mission_check offline 67/67 (mutants caught), qmllint nothing new, `dev.*` names all
  declared; the page rendered with a mock device in a scratch harness. Not verified: real 0.12 firmware, the head unit.
- Second round, after the owner's first try with a real 0.12 module: each card carries its line's network settings
  (mode, destination, peer, own UDP port, X2 pins; header `X2 · UDP 14445 · senders`), applied with
  `wifiSetLineConfig(line, edits)` — only the edited fields over a freshly read record — with the refusal shown in the
  card (two UDP lines on one local port: ERR_PAYLOAD, and how to swap). The module address moved into the Ports island;
  with v7 the Lines island is gone. The page follows the running role: station hides the access point's settings and
  address island, access point hides the station controls; a pending role says "takes effect after a reboot". 0.12 was
  changed to ignore the v3 rate of the requester's own line; the app never relied on it. mission_check offline 70/70.
- Third round: 0.12 takes LR only (0x08) for an access point again (module-to-module links); the Radio island offers LR in
  both roles, warns that phones and laptops cannot join an LR-only access point, and explains an ERR_PAYLOAD to it as
  firmware 0.11 (WIFI_MODULE.md §2e).
- Fourth round (owner's bench): the width follows the protocols (no 40 MHz without 802.11n, enforced in
  `WifiRadioRules` and at the send point), and the "firmware 0.11" refusal text only when the module's version is below
  0.12. `KTabBar` options can be disabled (listed under "What differs from upstream"). mission_check offline 74/74.
- Fifth round: every Wi-Fi module's chip (Devices page) and toolbar badge show its signal with `WifiSignalIcon` (dot +
  three arcs, lit and coloured by `dev.wifiSignalLevel`; thresholds in `WifiSignal`, colours in the component).
  mission_check offline 76/76.

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

## Wi-Fi module ports up to 5 Mbaud (firmware 0.13, 2026-09-28)

Owner: the module will take UART rates up to 5 Mbit/s; the app must offer them. Two sessions did this in parallel:
92df3e8e (`WifiRates`, `dev.wifiMaxBaud`, 5M in the X1/X2 cards from firmware 0.13) and af439113 on top of it (a
refused `ID_UART` ends the port change before the link moves; the card says what 5M needs). WIFI_MODULE.md §2f.
The firmware side (0.13) is in progress in the ESP32_WiFi repo; 5M has not run on hardware yet, and the head unit's
UART6 was tried up to 4M only. On this PC both installs (`C:\Program Files\Kogger\KoggerApp`, `KoggerApp myEdition`)
run af439113 (`D:\Temp\kogger_install\update_kogger.ps1 -Tag af439113`, elevated; Verified: hashes match, a clean start).
Before starting a feature here, read `git log myedition`: another session may be doing the same one.

Follow-up the same day (ba8e1b9c): on the head unit the X1/X2 cards, and with them 5M, were hidden. The module, powered
on with the board 15 s before, left the one ID_WIFI_NET v7 request unanswered for 4 s, and the app took that for
"firmware before 0.12" for the whole session; only the hidden ports island polls v7, so nothing asked again. Now the
firmware version decides (`WifiNetRules::portsFor`): 0.12+ is asked again every 2 s until it answers. A 0.x version
is kept (major 0 was dropped, so the module's version never showed) and logged (`wifi: module firmware 0.13`). On
the kiosk (`headunit` 61004afb, Verified in kogger.log): firmware 0.13, both port summaries received.

The module side, found by the firmware session (bench, firmware 0.13, their measurement): a burst of requests
overflows the module's 24-message control queue and requests are dropped silently (20 GETs in a row -> 27 answers of
about 29, 60 -> 62 of about 87); after power-on Wi-Fi events share that queue, so the last request of the app's
start-up burst, v7, was the one lost. The app's re-ask stays as the defence; a firmware fix (a bigger queue, a short
wait instead of dropping, a loss counter) is proposed to the owner as 0.14.

Firmware 0.14 (b8c1a401): ID_WIFI v1 carries the requests the module could not queue (bytes 23-26, U4 LE, since its
start). The Wi-Fi island shows "requests lost: N"; the log gets the count with "module up" and a line when it grows
(WifiLossWatch, one per 10 s with the sum). On the kiosk (`headunit` 2173faf2, module 0.14): "requests it could not
queue: 0" (Verified, kogger.log). On this PC both installs run b8c1a401.

## Stall monitor (2026-09-28, 5a5bb10d)

The head unit hung fully several times at the trials; its logs showed the GUI thread going silent while other threads
of the app ran, and nothing from the kernel. `src/stall_monitor.*`: a 200 ms GUI heartbeat and a frame counter; a
monitor thread logs a stall at 2 s (every thread: state, kernel wait channel, CPU; load, memory, RSS, GPU memory) and
has the GUI and render threads write backtraces to stall.log (SIGUSR2, async-signal-safe; -rdynamic on Linux),
repeats every 5 s, logs the end; a `health:` summary every 60 s. Checked on the kiosk (`headunit` d9845cb4) with a
test stall (a `stall_test` file in the log directory): the lines and both backtraces were written (Verified). Wi-Fi
log lines carry the module's address; the X1/X2 card's syncNet TypeError is fixed. The head unit side (persistent
journal, a health log every 5 s, a collector for after a hang) is in HeadUnit docs/DIAGNOSTICS.md.

## Field test notes (2026-09-29, c0b96b10)

Owner's seven notes after the head unit's field test, all in one commit (`headunit` e32d91ea):

1. **Echogram scroll by touch** (`Plot2D.qml`, touch only, `inputState.touchMode`, no bottom-track tool): one finger
   dragged more than 15 px scrolls like the wheel, along time or depth by the first movement (the signs of the
   two-finger pan; a vertical echogram swaps the axes); holding still 350 ms shows the aim cursor, which then follows
   the finger as under a mouse press; a tap still selects the ping (press and release at its end). The mouse is
   unchanged; a second finger cancels (the pinch takes over).
2. **Boat panel**: opening or closing it leaves "Add points" off (`Pane3DWindow.qml`); before, opening turned it on.
3. **Devices without data** (`DevDriver::dataStale`, the rule in `src/device/device_liveness.h`): nothing from a device
   for 3 s on a live connection makes it stale; its toolbar badge and topology chip go dim with the no-data colour
   ("no data" in the tooltip and the chip), a Wi-Fi module's signal goes dark. The link's own state cannot tell: the
   head unit's module keeps the link busy while the boat is gone. A quiet device (a stopped sonar) is asked for its mark
   (`ID_MARK` GET v0) after 1 s and then once a second, so only silence to that counts. Log: `[addr] device <name>:
   nothing for 3.0 s, shown as without data` / `frames again`. *Assumption:* every device answers `ID_MARK` GET (the
   app parses its CONTENT; the Wi-Fi module's contract has it); a device that does not and also sends nothing on its
   own would show stale while there.
4. **Toasts** (`NotificationsOverlay.qml`): every one leaves after 4 s, warnings too (they stay in the console); a tap
   dismisses sooner. The "Hide important notifications" switch had nothing left to switch and is gone (the upstream
   setting `main/ui/hideImportantNotifications` is no longer read).
5. **Scrolling the settings is no tap for the keyboard** (`TouchTapGate`, `src/touch_tap_gate.h`, used by
   `ScreenKeyboard`): Qt focuses a text input on press, so a finger that started a scroll on a field focused it and
   the keyboard came up. Focus that arrives while a touch is down, with the keyboard not up already, holds the keyboard
   back: a tap brings it up at its end; after a drag (beyond `startDragDistance`), a second finger or a cancel it stays
   down until the field itself is tapped. The settings sidebar also stops scrolling to the focused field once the
   finger drags the page. Other controls act on release already (buttons, switches, spin buttons; a Flickable's drag
   cancels them).
6. **Save and close always in sight** (`SettingsSidebarBase.footer`, `MainWindow.deviceSettingsFooterComponent`): the
   Devices page has a bar pinned under it with "Close" and the save button. The ✕ in the sidebar header was pinned
   already. The first version (c0b96b10) only wrote the settings to the device; the owner changed the Wi-Fi power
   and it was not applied: "Apply and Save to device must be equivalent" (f5ad7c6d). Each island with an Apply has
   `pendingApply` (and `rolePending`, `apPending`, the cards' `netPending`/`ratePending`, `ownAddrPending`) and an
   `apply()` that its own Apply button calls too. `DeviceSettingsPage.applyAllAndSave()` runs what is pending, a
   step every 300 ms: settings (relay, lines, role saved for the next boot, access point, address/DHCP, USB network),
   the port changes (each waits for the running one, `wifiPortBusy`, at most 45 s), Flash settings, the radio (the
   module reconnects), the module's own address last (it answers as a new device). The bar reads the page through
   `store.devicePage`: "Apply and save (N)" in the accent colour while N changes wait, a second tap to confirm (as
   those Apply buttons ask), the progress, then one toast with what was applied and what was not; with nothing
   waiting it is "Save to device" as before. Joining a network stays with its own button (an action, not a
   setting). An unapplied radio or port-rate pick no longer goes back after 30 s (owner): it waits for Apply, the bar or
   Read back (a port card's Read back drops its rate pick too); the end of a line change no longer drops a pending
   rate pick of the same port, which "Apply and save" would have lost (line first, rate next).
   *Not verified:* on the head unit by touch; the wiring is checked by the build (qmlcachegen), qmllint and a clean
   start only (the page needs a live device; this PC has none).

**A second start after the end (2026-09-29).** Field test 2026-09-28: after "Mission Complete" (a plan without the
return home leaves the boat in AUTO) "Start mission" said done five times and the boat did not move; it ran only after
the RC switched modes. ArduPilot starts a mission only when AUTO is entered; the start now goes through HOLD unless the
boat is in HOLD, which also lets it arm when a plan with the return home ended in RTL (Rover does not arm there).
BOAT_MISSION.md; mission_check on ArduRover 4.4.0 SITL 142/142 (the new phase failed before the fix, as on the water).

**Channels (0b45230d).** Owner: scan the air and measure channel load to pick the freest channel on the boat. From a
scan the Wi-Fi island rates channels 1..13 (`WifiChannels`, the 2.4 GHz overlap, our own network left out) and names
the least busy one for the access point; measuring the traffic on a channel needs the module's firmware (a survey,
its own ID 0x59 ID_WIFI_SURVEY); done: firmware 0.16 by the module session, the app side 4bcfd365 (Wi-Fi
airtime bars, Measure, Live repeats the survey), WIFI_MODULE.md §2h. mission_check offline 104/104, four mutants caught.

**Sections fold (10079dfc).** Owner: the Wi-Fi page grew long, make its sections fold, folded by default, the last
state remembered. Every Wi-Fi island folds to its title (a tap on it; the triangle of the app's other fold headers);
the open ones are kept by key in QSettings (`deviceSettingsPage/openSections`), each on its own, across restarts.
Folded, Ports / Lines stop polling and Live turns off; the Boat link chip stays live. WIFI_MODULE.md §2i. Verified by
machine: `tools/qml_check/fold_check.ps1` (qmltestrunner, three starts, six mutants caught), qmllint adds nothing,
the Windows build, mission_check offline 116/116. Not verified: by touch on the head unit. On the kiosk as `headunit`
a9bb48f8 from 2026-09-29 by the owner's word (with 68848e06 and 878f0af7; no QML errors, module 0.17 in kogger.log);
this PC runs feb09790.

**One Actions, no baud rate on the Wi-Fi module (f6a40f61).** Owner: why two "Actions" in Advanced settings, and what is
"Set baudrate" doing there. The DeviceSettingsGroup "Actions" was upstream's old one: upstream replaced it with the
DevIsland "Actions" in 94993b06 (2026-09-01) and v1.0.3 has only that; the merge f95f9845 kept both (and a1e1b433
then taught the old one to move the link). Removed: the remaining one has the same buttons, and its baud row calls
the same `applyBaudrate()`. The catalog hides `action.baudrate` for the Wi-Fi module (`hideWhen: "isWifi"`): its
ports have their own sections. Verified by machine: the real catalog under qmltestrunner (fails on the old one),
fold_check, qmllint, the Windows build. Found on the way: `applyBaudrate()` retunes the link of the device's
topology group, and for a pinned link rewrites its record (`LinkManager::updateBaudrate` →
`exportPinnedLinksToXML`); for a device behind the relay or a logger (the boat's sonar or logger on the head unit)
that is the head unit's own ttyS6, while the rate change goes to the far device (*Inference* from the code, not
reproduced). The owner: hide it behind the module (1a384c3f): `behindModule` on the page (a bridge in the device's
topology group), `hideOnPage` in the catalog; checked under qmltestrunner with the page's code over mock topologies
(the head unit's, a logger on a PC, a lone sonar), four mutants caught. Behind a logger on a direct link the row
stays, as upstream has it (the same retune of that link, not changed). On the kiosk as `headunit` 131c957b from
2026-09-29 by the owner's word (no QML errors in kogger.log; the boat was away, so the hidden row on its devices
is not seen on the hardware yet); this PC runs a937fa06.

**The boat panel's Close, pinned at the bottom.** Owner 2026-09-30: the boat menu too should have a close button at
the bottom, always at hand. Its only close was the X in the header, inside the Flickable, which scrolls away once the
panel is taller than the 3D pane. A full-width "Close" (`PanelButton`, the panel's raised style) is now pinned at the
bottom and the Flickable ends above it; the X stays. Check: `tools/qml_check/boatpanel_check.ps1` (the real panel:
short content, content taller than maxHeight scrolled to its end with the X out of sight and the Close unmoved, a tap
closes; `-Picture` saves a look); fails on the panel before the change and on two mutants. On the kiosk as
`headunit` 2982b172 from 2026-09-30, the owner: the button is there; this PC runs 04325111.

**A layout change clears the echograms' synced cursors.** Owner 2026-09-30: switching from two panes to one (the
echogram only) left white lines in the echogram - a vertical one running with it and stopping in the middle, a
horizontal one at the very top. That is the aim's synced crosshair (`Plot2DAim::draw` with `cursor.selectEpochIndx`
set: x = width / 2 plus an offset that shrinks with each new ping, y at the synced depth or the bottom's, 0 without
one; white), set by another echogram's pointer (`Core::broadcastEpochCursor`) or a pick on the 3D track
(`EpochSelected3d`) and cleared only by its source - which, taken out of the layout, never does (the four echogram
slots are hidden, not destroyed). `Core::clearEchogramSyncCursors()` clears every echogram's synced cursor;
WorkspaceView calls it when the panes or their modes or contents change (`_paneSetKey`, not on a resize). Which of
the two sources drew the owner's lines is *Inference*; the fix is *Verified* by the owner on the kiosk (`headunit`
8d11c357 from 2026-09-30): from two panes to one, no lines. Check: `tools/qml_check/synccursor_check.ps1` (the first layout and a resize clear
nothing; two panes to one, a mode, a content clear), five mutants caught.

**Split screen: a border always shown, a small handle.** Owner 2026-09-30: between the split screen's panes a thin
border should always be seen, and the splitter handle made smaller. The split zone (WorkspaceView's Repeater over
`splitHandleModel`) draws the border in the gap PaneFrame leaves on the first pane's side of the split,
`[floor(split) - Tokens.paneGapPx, floor(split))`, in `AppPalette.borderFocus` (1 px in the edit mode, where the panes
touch), and the handle (`KDragBar`) always, at half its size (`AppPalette.splitBarLengthPx` 36 x
`splitBarThicknessPx` 10, the border's colour, a bead on it); the grab area stays finger-sized
(`AppPalette.dragBar*`) and grabs at the first touch (it used to take a tap to show the bar first). The zone sits at
whole pixels (at a fractional split the border's start came out a hair below the pixel edge and was drawn 3 px) and
moves with the panes' own animation (PaneFrame's Behaviors). Check: `tools/qml_check/split_check.ps1` (the real
delegate over three panes and a T junction: the border's place, colour and pixels, the handle's size and place, the
grab area, the edit mode, a moved split); eight mutants caught, the one at the fractional zone among them. On the
kiosk as `headunit` 8d11c357 from 2026-09-30, the owner: the border is seen; this PC runs b70fabbc.

**Boat link traffic: VERSION asked of every device ~3 times a second, forever (e04c3b23 + LinkAskAll).** Owner
2026-09-30: with the sonar off the boat sends 17-18 KB/s; what is it, can it be halved? The app's own count
(6770b6b2, "traffic [link]: ..." in kogger.log once a minute) on the kiosk: 11.2 KB/s of complete frames, of it
~5 KB/s frames with the VERSION ID from both Wi-Fi modules and the logger. Two sources (WIFI_MODULE.md 2n, the rates
match 5 + 3.33 + 3.33 per device): the driver at the search address 0 that every link gets never gets a frame and
asked VERSION v0-v2 every 200 ms (`DeviceLiveness::Unanswered`: each tick for 10 s, then every 5 s; it starts over after
2 s of marked ticks or on a frame after 1 s of silence, so a device whose frames carry the mark only now and then is
slowed down too), and the link asked every device every 300 ms while it received data (`LinkAskAll`: only after two
quiet ticks). What the app sends is logged too ("traffic [link] sent: ..."), frames without the mark as
"... unmarked". mission_check offline 159/159, the mutants of both rules caught, the Windows build. On the kiosk as `headunit` 38e72b7f (all three) from 2026-09-30 by
the owner's word: 8.9-9.3 KB/s against 14.3-15.1 KB/s before with the sonar pinging, the VERSION answers gone after
the first 10 s, the app sending 0.01 KB/s (WIFI_MODULE.md 2n, *Verified*). By the owner's word the boat's SR1_EXTRA1 and
SR1_EXTRA2 went from 10 to 4 Hz: 7.3 KB/s in all (HeadUnit docs/BOAT_MISSION.md).

**Link speed (firmware 0.18, 55f97d48).** The module session added a link rate report (ID_WIFI_SURVEY v1); the Wi-Fi
island shows it both ways (WIFI_MODULE.md 2m). mission_check 142/142, the text functions under qmltestrunner. On the
kiosk as `headunit` da4a3de7 from 2026-09-30 (with 0da0534d, the heading in use; no QML errors, both modules 0.18); this
PC runs 8b6e4ee3.

**Heading source: what is in use now (0da0534d).** Owner: the Heading choice is not clear, what is the source now. The panel
showed only EK3_SRC1_YAW; it now shows the selection with a tick, "In use now" (MavVehicle::headingInUse, from the GPS
heading and the EKF's fallback message, BOAT_MISSION.md) and why the choice is closed. mission_check 134/134.

**Data widget "From launch point" (80520c86).** Owner: the widgets should show the current distance from the boat's launch point to
the boat. The autopilot's home to its position (BOAT_MISSION.md); home asked for again on arming. mission_check offline
126/126, the catalog field under qmltestrunner, the Windows build. On the kiosk as `headunit` d64994aa from 2026-09-30 (with
70071164; the board rebuilt mav_vehicle.cpp and the two QML files, no QML errors in kogger.log); this PC runs 5d1cc72f.

**"Apply and save" sent one Wi-Fi module's fields to the other (70071164).** Owner 2026-09-30: "something is wrong
with LR: data lags, the devices do not answer". The kiosk log (2026-09-26 by the board's clock): 21:59:15 on the head
unit module's page "Apply and save" applied Role, Radio, Module address - `[87] wifi-net: role saved 1 running 0`,
its module saved as an access point for its next boot; 21:59:20 the boat's module got the head unit's 15 dBm;
22:00:08 on the boat module's page Radio, Module address - from then on the boat module's reports came as [87] (its
uptime alternating with the head unit module's: a "the module restarted" line a second, each re-asking the fast
report), [88] silent. One page serves every device and only `dev` changes on a switch: the own-address fields and
the role pick were read from a device only when it sent new net settings, the islands' edit flags outlived the
switch, and "Apply and save" took the other device's values for edits. Fixed: the role and the own address are
pending only once edited here; `onDevChanged` → `_forgetEdits` drops every island's edits, clears the own-address
fields and the port speed pick, turns Live off, stops a running "Apply and save", then `_readNewDevice` reads the
new device's values; the bar's first tap names what the second will apply and to which device. Guard:
`tools/qml_check/devswitch_check.ps1` (every pending condition gated by an edit, every edit flag reset on the switch;
15 findings on the code before, none now). The modules' settings (the boat's address back to 88, the head unit's
saved role back to station, the radio) are the module session's task; LR itself (0.5 or 0.25 Mbit/s against the
boat's ~0.74) explains the lag (*Inference*).

**Wi-Fi diagnostics (822ca18e; on the kiosk as `headunit` 110d6fa2 from 2026-09-30, this PC 57c9d875).** Owner: keep both charts, extend the diagnostics. A tap on a channel
shows its details, "Both ends" puts how each end hears the other side by side, "Link events" lists the link's
recent events in words (WIFI_MODULE.md §2k). Verified by machine: mission_check offline 122/122 (+6 checks of
WifiLinkLog, four mutants caught), the page's new functions and its peer binding under qmltestrunner (today's
sequence from kogger.log in words, an access point's clients, a restart, the other end over a mock topology; five
mutants caught), fold_check, qmllint, the Windows build. Switched on the owner's word 2026-09-30 (no QML errors in kogger.log; the boat was off, so both ends
and the survey as an access point, now possible with the boat's new module on 0.17, are not seen on
the hardware yet).
7. **RSSI digits on the Wi-Fi badge** (`WifiSignalIcon.showRssi`): the RSSI in dBm under smaller arcs on the toolbar
   badge, its pop-out and the topology chip, while there is a link. The module on this app's own port (ID_WIFI_NET v7
   flag b2) is asked for a link report every 200 ms (`WifiSignal::kFastReportMs`, ID_WIFI v1 SETTING, 100..60000 by
   its contract) once per connection and module start (log `wifi: link report every 200 ms asked`); a module reached
   through the relay keeps its second (its reports cross the radio). The module keeps the period until it restarts;
   `ID_FLASH` v0 ("Save to device", a port rate change on 0.12-0.14) saves it with the rates.

Verified by machine: Windows build clean (no compiler warnings in the changed files); qmllint adds nothing but the
kinds every file already has (AppPalette not seen as a singleton, context properties); `mission_check --offline`
97/97 (+10: liveness, keyboard gate, report period) and six mutants of the two rules caught; the app starts on this
PC with no QML errors in kogger.log (start-up screens only: the Devices page and its bar were not opened, no device
here); `headunit` e32d91ea builds on the board (dev_driver.cpp and screen_keyboard.cpp rebuilt, no warnings).
Not verified: anything by touch on the head unit, the report period on the module, a stale device on a real link
loss. This PC: package c0b96b10 made from `D:\Temp\kogger_merge_test` (the myedition checkout has an untracked
`out_x64.zip`, which would mark the build dirty); `update_kogger.ps1` takes `-Dist` for that.

Deployed 2026-09-29 on the owner's word: the kiosk runs `headunit` e32d91ea (no QML errors; `[0] wifi: link report
every 200 ms asked (the module on this app's port X1)`; the boat's module, flags 0x03 on its X1, not asked; no device
marked without data in the first 100 s with the boat's sonar pinging - all Verified in kogger.log); both installs on
this PC run c0b96b10 (hashes match the stage, UPDATE_DONE). The log showed both Wi-Fi modules as `[0]`: the tag read
`m_devAddress`, which nothing sets. 886d9384 (`headunit` 8755c0d6, built on the board, not switched) tags with the
route address (`m_busAddress`, the device's key in DeviceManager). The same log: the boat's module (0.15) last reset
by its task watchdog (reason 6), about an hour before; the head unit's by power-on.

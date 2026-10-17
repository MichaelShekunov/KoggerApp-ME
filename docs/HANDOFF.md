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
| `src/system_monitor.{h,cpp}`, `src/system_stats.h` | the computer the app runs on, for the "System" widget |
| `qml/app/SystemOverview.qml` | the "System" widget's cell |
| `src/device/ring_upgrade.{h,cpp}` | firmware update of two sonars sharing one line as a ring (and of a lone sonar with a bus address) |
| `tools/ringupdate_check/` | standalone program: the ring update against a simulated ring, and live on a serial port |
| `docs/RING_UPDATE.md` | two sonars on one line: how they show up, how they are updated, what was verified |
| `src/device/bus_address.{h,cpp}` | setting and saving a sonar's bus address, with the proof that it was saved |
| `tools/busaddr_check/` | standalone program: the bus address setting against simulated boards, and live on a serial port |
| `docs/fork-overview.ru.md` | the fork for upstream's GUI developer (Russian): upstream behaviour it changes, new ids and keys, and in its appendix every touched file, listed by git |

This table is not complete (the boat mission's files, among others, are missing); the appendix of
`docs/fork-overview.ru.md` lists the rest as of `388f0865`, except `src/kec`: the overview goes to upstream's
developer without the sonar-side codec (owner, 2026-10-01), so it neither lists nor describes it.

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
On Android the panel never opens and the mode choice is hidden: there every input is touch and Android raises its
own keyboard, so a tap on a field opened both (Xiaomi tablet, 2026-10-02; `docs/android.md`).

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
  an eye button). `ScreenKeyboard::passwordMode` reports that `echoMode`, but `KScreenKeyboard.qml`
  does not read it: there is no password layout (corrected 2026-10-01; this line used to say there was).
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

**The main menu's device chain scrolls.** Owner 2026-10-01: the chain of devices no longer fits the main menu's
width and is cut (two Wi-Fi modules, the logger and the ring's two sonars); make that area scroll. The chain
(HotActionsPanel `qaConnectionsComp`) is now in `KScrollStrip` (kqml_types): as wide as the chain up to the room
of its place - the expanded menu's width, or collapsed up to the window's right edge - and past it it scrolls
by finger and by the wheel; in the menu the cut edge fades into the menu's colour (over the view, collapsed,
none). A badge's menu is a Popup, so the clip does not cut it. Check: `tools/qml_check/scrollstrip_check.ps1`
(the real strip over a row of cells: fits, overflows, the wheel to its end and no further, a drag, a tap through,
back in view when the chain shrinks or the room grows, no fade colour; and the menu's use of it), nine mutants -
seven caught, the two surviving showed that Flickable keeps its content in bounds by itself, so those handlers
went; qmllint adds only the kind the component's other lines have; the Windows build. On the kiosk as `headunit`
cc29162b from 2026-10-01 by the owner's word (no QML errors); this PC runs ea8eb8a5.
The owner on the kiosk: the chain does not scroll. That strip was the wrong place: the chain he saw cut is the
main menu's device topology (DeviceTopologyView, the Connections page and the device settings tab), where a
line's whole chain is one row of cells on a grid of at most four columns - and the head unit's line now has five
(two Wi-Fi modules, the logger, the ring's two sonars), so the fifth was cut at any width; the hot actions' row of
badges fitted. Each topology row is now a KScrollStrip (maxWidth the view's content width, fading into
`fadeColor`, AppPalette.bg by default - both pages draw it on that); the hot actions' strip stays, scrolling only
if it ever outgrows its room. scrollstrip_check now also runs the real view over the head unit's line (five
scroll, the fifth reached and tapped; three fit) and inside a vertical Flickable as on the page: a sideways swipe
scrolls the row and not the page, an up-down swipe on the row the page and not the row - by touch, as QtTest's
mouseDrag does not start an upward drag of a Flickable at all (an experiment: not even over a plain MouseArea).
13/13; the row as wide as its chain (what he saw) fails it. On the kiosk as `headunit` 54be84e4 from 2026-10-01
by the owner's word (no QML errors); the owner on its screen: the scrolling works. This PC still runs ea8eb8a5
(the owner cancelled the UAC prompt for 981d367a).

**The "System" widget: the computer the app runs on.** Owner 2026-10-01: a compact, good-looking widget for the
system's state - the cores' frequencies, memory, the CPU temperature, the drives, and what else. `SystemMonitor`
(`src/system_monitor.*`, context property `systemMonitor`) reads once a second while a widget cell shows one of its
fields (`subscribe` / `unsubscribe` from WidgetCellContent; the volumes every tenth time), and for a moment 3 s after
the start, so kogger.log has its line once with no widget shown (`system monitor: N cores (max MHz ...), throttled ..., CPU .. C, GPU .. MHz .. C, memory ../.. GB, volumes ..,
uptime .. s`). Linux: each core's cpufreq (now, `cpuinfo_max_freq`, and `scaling_max_freq` below it = "held below
its maximum"), `/proc/stat` (loads), `/proc/meminfo` (MemAvailable), the thermal zones (the hottest CPU zone, the
GPU's), devfreq (the GPU's frequency), QStorageInfo (local volumes, not /boot, three at most) with `/sys/block` (the
disk's kind eMMC / SD / NVMe / USB, an eMMC's `life_time` as its wear, on the disk's first volume), `/proc/uptime`.
Windows: loads (ntdll), frequencies (powrprof), memory, volumes, uptime; no temperatures. The parsers are pure, in
`src/system_stats.h`. Fields (group "system"): "System" - all of it in one cell (`qml/app/SystemOverview.qml`) -
and "CPU", "CPU temperature", "Memory", "Storage", "Uptime". The overview: the mean load and a temperature chip (and
a red mark when held back), a bar per core (height its frequency of its maximum, colour its load, a gap between the
clusters), memory and the volumes as lines over bars (the disk's kind as a chip coloured by its wear), the GPU
(the next paragraph) and the uptime; a wide cell (2x1, 4x2) in two columns. What does not fit is left out from the bottom up; a line too long
drops its least needed part (the frequency, the wear's number, the GPU's frequency) or shows a percentage instead of
the amount - measured in the font it is drawn in. The head unit as read on 2026-10-01 (*Verified*): 8 cores, 4 to
1800 MHz and 4 to 2400 MHz; zones package, bigcore0, bigcore2, littlecore, center, gpu, npu; devfreq fb000000.gpu;
one eMMC (mmcblk0, type MMC, life_time 0x01 0x00: up to 10 % of its life) with "/" (12 GB, 74 % used) and "/data"
(17 GB); its font DejaVu Sans (`fc-match sans-serif`). Checks: mission_check offline 220/220 (+16: the parsers on the
head unit's names, the live monitor on this PC), four C++ mutants caught; `tools/qml_check/sysgui_check.ps1` (the
real overview, cell and catalog over mock monitors: the bars, the fallbacks, every cell size at two scales, every
height, the subscription; `-FontDir` with DejaVu Sans measures as the head unit does) 10/10 in both fonts, 18
mutants caught; qmllint adds only the kinds those files already have; the Windows build. QtTest takes a test
function whose name ends in `_data` for the data of another and silently does not run it. Not done (asked "what
else"): the NPU's load (debugfs, root), the app's own memory, the network per interface. On the kiosk as `headunit`
9c824fee from 2026-10-01 by the owner's word (no QML errors; its line: `8 cores (max MHz 1800 x4, 2400 x4),
throttled no, CPU 45.3 C, GPU 1000 MHz 45.3 C, memory 0.9/15.6 GB, volumes / (eMMC, wear <=10 %) 8.8/11.7 GB, /data
1.0/16.5 GB`); this PC runs b1f77ce6. The owner, on the kiosk's screen 2026-10-01: the widget works.

**The GPU in the "System" widget.** Owner 2026-10-01: add the GPU to the main widget. It is now a line like the
memory's, right under the cores (wide: under them in the left column): its temperature as a chip, "12 % · 300 MHz",
the bar its frequency of the top of its devfreq table, coloured by its load - the GPU drawn like one more core. The
load: this process's DRM fdinfo (`drm-engine-<e>: N ns`, each client once, the busiest engine class over the wall
time; `system_stats::parseDrmFdinfo` / `drmBusy`), which panthor reports only with its job profiling on - on the
head unit a udev rule turns that on at boot (HeadUnit docs/STATE.md); the kiosk is the GPU's only user. Without it
the line shows the frequency and a plain bar. The stall monitor's GPU memory reads the same parser. A field "GPU"
too; the footer keeps the uptime only. Measured on the kiosk with profiling on: 10.3 ms of GPU time in 2.09 s
(0.5 %) at idle. Checks: mission_check offline 225/225 (+5 on the kiosk's own fdinfo lines), four C++ mutants
caught; sysgui_check 11/11 in both fonts, 25 mutants caught; qmllint adds nothing; the Windows build. The same day
the head unit got its clock (chrony, the RTC set; HeadUnit docs/STATE.md, "Часы"). On the kiosk as `headunit`
94b7e725 from 2026-10-01 by the owner's word (no QML errors; at start `GPU 500 of 1000 MHz, load 50.4 %, 48.1 C`, so
the load is read there); this PC runs 8ee2cbb1. The owner: the GPU works.

**The "System" widget in a 1 x 1 cell: CPU, GPU, RAM, eMMC.** Owner 2026-10-01: CPU and GPU on one widget, RAM and
eMMC filled with numbers, try to fit all of it in one widget. His panel (read from KoggerApp.conf on the head unit):
one column of four 1 x 1 cells - From launch point, Voltage, On the link, System - where System had the header, the
cores and two lines (GPU, RAM in percent); the eMMC no longer fitted. A cell that cannot show the GPU, memory and the
system volume under the header and the cores now goes compact: four lines over bars, `CPU (52°) 40 %`,
`GPU (48°) 12 %`, `RAM 0.9/15.6`, `eMMC 8.9/11.7`, spread over the height, with less margin in a small cell. Every line
has its texts from the fullest down and shows the first that fits (measured with FontMetrics, rounded up as a Text
is, with room for a synthetic bold): the frequency goes first, then the wear and the unit; a volume keeps its numbers
before its disk's name ("eMMC" for "/", the disk's first volume); the CPU's chip gives up its held-back mark, then its
frame (bare coloured text), before its temperature. The GPU's bar now follows its load like every other line (its
frequency only without a load reported). Checks: `sysgui_check.ps1` 12/12 in DejaVu Sans and Arial - an acceptance
test at 1 x 1, both scales, four mocks (the head unit, hot, everything at its top, a Windows PC): the CPU's and the
GPU's temperature and load, memory and the system volume in numbers, the head unit's disk named eMMC, nothing cut;
a sweep over every width of a 1- and a 2-unit-high cell: no text cut but a name with nothing else to give (it is
what showed that a Text lays a bold label out wider than FontMetrics measures it, in DejaVu Sans Bold too). 31
mutants caught in either font; qmllint adds only the kind the file has; the Windows build. On the kiosk as
`headunit` e1f821b8 from 2026-10-01 by the owner's word (no QML errors); this PC runs 2783f4bf.

**Which eMMC partition: the one with the data.** Owner 2026-10-01: the eMMC has two partitions - which is shown? It
should be the one where the data is saved, maps and logs. It was "/" (mmcblk0p1, the system). On the head unit
(Verified) the app's data dir ~/.local/share/KOGGER (the map tiles, kogger.log, the mosaic) and ~/.config/KOGGER are
bind mounts from /data (mmcblk0p2, fstab). SystemMonitor marks the volume of AppDataLocation `data` (by its device,
so a bind mount counts) and lists a device once, by its shortest mount point; the compact cell puts that volume
right after memory (`/data 0.1/16.5`), and the Storage field shows it. Found on the way (Verified): the sonar
recordings (KLF) go by default to Documents/KoggerApp/logs (`Logger`, empty `logging/recordFolder`) - on the head
unit ~/Documents on "/", where 5 files lie (1.5 MB); exports too. Checks: sysgui_check 12/12 in both fonts (the
acceptance test asks for the data volume in numbers, `/data` on the head unit; the catalog's Storage), two more
mutants caught; on this PC the log marks `C: (app data)`. On the owner's word the head unit's ~/Documents is now a
bind mount from /data too (HeadUnit docs/STATE.md, `move_documents_to_data.sh`), so the recordings and the
exports land on /data; the app's paths are unchanged. On the kiosk as `headunit` 302cba27 from 2026-10-01 (no QML
errors; `/data (app data) 1.0/16.5 GB` in its line); this PC runs a60bde45.

**A working sonar behind a recorder that is not recording is not red.** Owner 2026-09-30: the logger has no flash drive (red, rightly), and the sonar behind it turned red too while it worked. `RecorderStatus.pillState` took a device's colour from the recorder's per-port flags; a recorder that is not recording (severity "crit": storage lost, a failed recording, recording off) calls its ports lost. Such a recorder's port flags are now ignored and the device is shown by its own frames (dataStale, then the link). Check: `tools/qml_check/recorderpill_check.ps1` (fails on the rule before: "the working sonar behind it green"). On the kiosk as `headunit` 54ae4e46 from 2026-09-30, the owner: the sonar is green. This PC still runs 60073967 (the owner cancelled the UAC prompt for 0eb37775).

**Boat GUI: mission progress, the track by link quality, the ghost boat, "On the link".** Owner 2026-09-30, from
my list: 2-5. (2) While a mission runs, the boat panel and the widgets "Mission left", "Mission time left",
"Battery at finish" show the way left, its time at the boat's speed and the battery at its end
(`src/vehicle/mission_progress.h`, MavVehicle.mission*). (3) The map draws the boat's track in the colour of the
head unit module's Wi-Fi signal on each stretch, a loss red and dashed (`src/vehicle/link_track.h`,
MavVehicle.linkTrack*, a switch and a legend in the panel). (4) While the link is lost, a faint boat where it should
be now, along the mission at its last speed (MavVehicle::updateGhost). (5) The widget "On the link": what the link
carries now by kind as a bar, and "Wi-Fi signal" (`src/device/air_meter.h`, DeviceManager::airChanged,
DeviceManagerWrapper.air*). Details: BOAT_MISSION.md, WIFI_MODULE.md 2n. Checked: mission_check offline 204/204 (+45:
the geometry, ETA, battery trend, ghost path, track thinning and qualities, the air meter's kinds and window; the
vehicle end to end - RTL progress, the ghost walking home at 1 m/s after kConnectedMs, the stretch over a loss),
five C++ mutants caught; `tools/qml_check/boatgui_check.ps1` (the real layer, panel, cell and catalog over mocks)
8/8, eight mutants caught; the Windows build. Not yet seen on the water: the battery trend needs a calibrated
monitor, and the ghost's path assumes the autopilot keeps its mission without the link.

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

## Two sonars on one line: the ring update (2026-10-01)

Two Basic2D sonars with the ring firmware share one host line (host TX -> board 1 -> board 2 -> host RX) and
show up as two devices, addresses 1 and 2 (one device per route, as for the two Wi-Fi modules). The stock
upgrade cannot reach a bootloader in such a ring; a sonar on a bus address 1..15 (`DevDriver::isRingBoard()`)
is updated by `RingUpgrade` instead: UPGRADE = this sonar, UPGRADE ALL ON THIS LINE = every sonar of the
line, the last one first. The link's rate is pinned for the run (`Link::setRatePinned`: no baud search, no
ask-all) and route-0 frames go to the engine only. Verified: `tools/ringupdate_check` offline (26 checks,
with a control for the reset race), live on the two boards (C++ engine, 10.5 s for both), and inside the app
through `KOGGER_AUTOTEST=COM13` + `KOGGER_AUTOUPGRADE=<file>|all|2` (kogger.log 2026-10-01 11:56:06-11:56:16:
both boards 1.12, 469 chunks each). Details, the measured bootloader behaviour and the limits:
`docs/RING_UPDATE.md`. Not done: the head unit, where the rate is the Wi-Fi module's port, not the PC's.

## Ring line speed (2026-10-03, a89ffc8b + b72b9a7b)

The owner wants other wire lengths in a ring. A board's RX and TX are one UART with one rate register and each
board passes the other's bytes on, so the whole line runs at one rate, picked for the longest wire. A ring sonar's
page has a "Line speed" island (921600 / 460800 / 230400 / 115200); the per-device baud control is hidden for ring
boards (`baudrateHidden`). `RingRate` (`src/device/ring_rate.*`): pings off, ID_UART v0 to the last board, then the
first, then the host; both must answer at the new rate; pings back, ID_FLASH v0, proof by ID_FLASH v1 (the firmware
reloads the rate, applies it at boot). Verified: `tools/ringupdate_check` 40/40 (14 new), three mutants caught; live
on the bench ring 921600 -> 460800 (3.2 s, independent probe), a ring update of both boards at 460800 (the
bootloader takes it), and inside the packaged app 460800 -> 921600 (`KOGGER_AUTORATE=921600|2`). Not tried on
hardware then: 230400, 115200. Wire lengths in the UI are an estimate (220 ohm, 100 pF/m), not measured.
The owner's own run found the weak spot: at 115200 with 50 ms pings the last board could not relay the first
board's frames (6.9-13.5/s of 20) and a change from there refused at its first step. b72b9a7b: pings off on the
last board first, ID_DATASET asked up to three times, each board restored, saved and proven alone (pings checked
too); ringupdate_check 43/43, five mutants caught; live 115200 -> 921600 on the congested ring (4.8 s), in the app
921600 -> 230400, and the owner's clicks 230400 -> 460800 -> 921600 plus a power cycle (both back at 921600).
A lower rate carries less: raise Period if an echogram drops out (the island says so).
`docs/RING_UPDATE.md` "Скорость линии кольца".

## Bus address from the app (2026-10-02, f18a7077 + 0b7ab76e)

The owner, before a trip: everything from the GUI, no Python. A Basic2D sonar with the ring firmware (1.12+), not
behind a recorder (`DevDriver::isBusAddressSettable()`), has a "Bus address" island: 0 / 1 / 2 and "Set and save
address N". `BusAddress` (`src/device/bus_address.*`) runs the sequence of the firmware tree's chain_addr.py: one
answer to a broadcast GET of ID_UART v1, SET, read back, ID_FLASH v0, proof by a restore from flash. While it runs
its line belongs to it: frames go to it only, the devices' own frames are dropped (`Link::setDevicesMuted`), the
rate is pinned; on success `DeviceManager::rekeyDevice` keys the same device object by the new route.
Verified: `tools/busaddr_check` 14/14 offline, three mutants caught; live on ring board 2427959761 - the class
alone 2 -> 0 -> 2, inside the packaged app 0b7ab76e 2 -> 1 right after the device showed up
(`KOGGER_AUTOTEST=COM13`, `KOGGER_AUTOADDRESS=1`), and by the owner's click 1 -> 2 (kogger.log 19:40:25); no QML
warnings. Found on the way: f18a7077 refused a lone board inside the app ("2 answers to one request (2, 2)": the
board's answer to the app's own poll was counted), fixed in 0b7ab76e. Not done: the `headunit` branch (handed to
its session), Android. `docs/RING_UPDATE.md` "Адрес шины из приложения".

## Android (2026-10-02)

Android is an upstream platform (platform/android: Java USB serial, GLES shaders, manifest); the fork now builds
and runs there too. Building: `docs/build-android-cli.md` (`scripts/win_build_android_release.ps1`, the toolchain
in D:\Android, `-AppIdSuffix .myedition` for a package beside the official app, `tools/android_check/apk_deps_check.ps1`).
On a device: `docs/android.md`. Tested on a Xiaomi tablet, Android 16, over the boat's Wi-Fi: the link, the Wi-Fi
island, the autopilot and its mission work as on the head unit. Found there and fixed, each checked again on the
device: two keyboards for one field (`ScreenKeyboard` off on Android), the link cut in the background (Android 16
blocks a background app's network; `KoggerLinkService`, a foreground service while a link is open), the "System"
widget's cores, uptime and disks, and the relief cloud, which was on by default on every platform (the store
restores before UILoad finds the 3D view) and covered the 3D pane with a blob. Also checked there: the file picker
and recent files, sonification (0 underruns, 37 ms), deconvolution on the CPU (53-64 ms a pass, no OpenCL for apps).
Later: a 70-minute soak with the screen off and 16.5 hours on the link in one process, no leak; the black
echogram was the boat without its sonars (with the ring aboard, 2026-10-03: both sonars behind the recorder,
CHART_Z at 20/s). Swiped out of Recents, Qt's terminateQt() once hung and left a windowless process; KoggerActivity
now ends it 3 s after a final onDestroy. Open: a 3.5 s GUI stall opening a 43 MB file (read on the GUI thread,
upstream's default), USB-OTG.

## A sonar behind a recorder: commands and update (2026-10-05)

A lone Basic2D (firmware 1.14, no bus address) behind the RECORDER Mini: the echogram came, every setup group
stayed "Not applied". Upward the recorder passes a ring board's own address as its route, but stamps a board with
no address with its port number -- the device plaque follows the connector, "Port 1" / "Port 2". The
`ID_NODE_CMD` wrapper (ced45807) named that port as the sonar's address, and the board ignored every command.
Plain commands work (the owner, through `KOGGER_NODE_WRAP=0`).

`NodeProbe` (`src/device/node_probe.*`, transport-free like `BusAddress`): once a ring sonar behind a recorder
shows up, `GET ID_UART` v1 wrapped, then plain, with a drain after each wait; the answer picks the mode. The setup
is held meanwhile and asked again afterwards; the probe runs again after a silence. `KOGGER_NODE_WRAP=0|1` forces
a mode, `KOGGER_NODE_ADDR` the address named. Verified: `tools/nodeprobe_check` 30/30 against a model of the
recorder and boards, two mutants caught (no CONTENT filter, no drain); on hardware "the board (address 0) answers
plain" in 1.4 s on route 2, by itself again after the update below, and on route 1.

Update through the recorder, for a sonar whose commands go plain (`isRecorderUpgradeSupported`): `SET ID_UART`
v0 115200 so the recorder follows the line (it detects its ports' rates), the sonar must be heard again, then the
stock update with the IDBins on route 0 -- the bootloader takes route 0, not 1 -- and the route back at the end.
The host port's rate is pinned meanwhile (`linkRatePinRequested` -> `Link::setRatePinned`). A ring board with an
address behind a recorder is refused: its bootloader's route-0 answers would come under the port. Verified by the
owner, 2026-10-05 15:33, SN 993653903 on SONAR 2: heard at 115200 after 1.3 s, 7.5 s to "successfully flashed",
the bootloader's `ID_UPDATE` answers as `KP2 [2]`. The first build waited for the sonar to report 115200 in
`ID_UART` v0 and failed twice -- this firmware hardly answers that through the recorder; after those two tries
the echogram stopped until a power cycle, cause unknown.

Open: the recorder answers route 0 itself -- `KP1 [250] id 0x25` for every chunk, and it got `ID_BOOT` v1 too; it
kept running. What its firmware does with those is unknown, so update while it is not recording and with one sonar
behind it (route 0 reaches every port). No offline check for the update path. Not tried: the update via SONAR 1,
a retry of a board left in its bootloader. `docs/RING_UPDATE.md` "Одиночная плата за логгером".

## Chinese UI and the rest of Russian (2026-10-06)

Simplified Chinese is language index 3 (`zh`, appended so saved indexes keep their meaning): all 2383 messages
of `translations/translation_zh.ts`, machine-translated by Claude and **not read by a native speaker yet**.
`translation_ru.ts` lacked 1122 messages; all are filled now -- 49 of them were lupdate's same-text guesses,
which lrelease ships as they are, and 6 of those were wrong for their context (minutes shown as metres in the
USBL panel, the Wi-Fi security "Open" as the verb, ...). `tools/ts_check/ts_check.ps1` fails on unfinished
messages and on placeholder, tag, line-break or edge-whitespace drift; four mutants of the zh file caught.

Han glyphs: on a Russian Windows Qt drew Chinese with MS UI Gothic plus SimSun for what it lacks;
`preferSimplifiedChineseFont()` in `src/main.cpp` puts Microsoft YaHei UI / Noto Sans CJK SC first (measured
by glyph runs: 45 + 17 glyphs in two fonts before, 62 in one after). Not checked: the APK on a phone in
Chinese, the HeadUnit kiosk (likely needs a CJK font package). Details, the update procedure and a glossary
for a reviewer: `docs/TRANSLATIONS.md`.

`scripts/win_deploy_windows_release.bat` needs `windeployqt` on `PATH` (its header says so) but empties
`out_x64` before it finds out: run without `D:\Qt\6.8.3\llvm-mingw_64\bin` on `PATH`, it left the folder
empty (rebuilt at once). Proposed, not done: look the tool up before the `rmdir`.

## Android APKs had no TLS (2026-10-07)

The owner: maps do not load on the phone, though the app shows internet. The APKs of 2026-10-05/06 carried
Qt's OpenSSL backend plugin but not OpenSSL itself: `CMakeLists.txt` takes it from
`<ANDROID_SDK_ROOT>/android_openssl` and skipped it silently, and this PC's SDK had no such folder. Without
it every https request fails on Android (OpenStreetMap and Baidu tiles, the tile manifest); the internet
indicator is Android's network status (`QNetworkInformation`), not a request. Google Satellite goes over
plain http. Which source the phone used, and whether the default "Limit downloads on metered networks"
(on by default, `deferTilesOnMetered_`) was holding tiles back as well, was not established.

Fixed in the build: KDAB `android_openssl` cloned into the SDK (b71f147, OpenSSL 3), `CMakeLists.txt` warns
when it is missing and prints the two libraries when found, and `tools/android_check/apk_deps_check.ps1` now
fails an APK whose OpenSSL backend has no `libssl_3.so`/`libcrypto_3.so` beside it (the 2026-10-06 APK fails
on exactly those, both ABIs; the new one passes with 97 libraries per ABI).
`out_android/KoggerApp-1.0.3-myedition-zh-ssl-universal-testsigned.apk` (same test key) went onto the owner's
phone at 11:20 over the old one, and the owner reported Baidu tiles loading -- an https source, so TLS works
on the phone (verified 2026-10-07). Whether the metered limit was switched off as well was not reported.
`docs/build-android-cli.md` has the details.

Asked for a "normal signed APK", the owner chose to keep the test key:
`out_android/KoggerApp-1.0.3-myedition-20261007-111802.apk` is the same file as
`...-zh-ssl-universal-testsigned.apk` (SHA-256 `a6083aa7...`) under a plain name. It is for the owner's own
devices: a device that has it cannot move on to a release-signed build without uninstalling. The owner's rule
since then: a build's file name ends with the build's date and time, `YYYYMMDD-HHMMSS` (when the build wrote
the file). The scripts do it themselves: `win_build_android_release.ps1` (now also checks the libraries and,
given `-Keystore`/`-KeyAlias`/`-KeyPassFile`, signs; `-SkipBuild` re-signs the last build),
`win_package_release.ps1` (`...-portable-<stamp>.zip`, `...-setup-<stamp>.exe`, `build_info.txt` now has
`built` and `packaged` apart), and both `win_deploy_*.bat`. Details and checks in `docs/build-android-cli.md`,
"Signing".

## Pings between GNSS fixes: a windowed fit (2026-10-08)

Asked for by the session "GODIO автопилот интеграция KoggerApp" on the owner's behalf: the GODIO boat
sends float32 coordinates (a 0.42 x 0.63 m grid), consecutive fixes are often equal, and pairwise
interpolation put every ping between two equal fixes on one spot. `TrackSmoother` (`src/track_smoother.*`,
transport-free; `DataInterpolator` adapts it to `Dataset`) places an epoch without a fix of its own on the
least-squares line through the raw fixes within a window of its own -- clamp(min((0.48/T)^2 / fixes per
second, 5 m / speed), 1 s, 6 s), rate and speed from the fixes of the 5 s before it, T 0.08 m by default;
only with 12 fixes in it (else pairwise at once: 1 Hz recordings draw exactly as before), two fixes or more
on each side, the value held to their range -- and refines an interpolated heading on a +-0.5 s line,
unwrapped (one raw heading on each side at least). An epoch waits half its window (2.7 s at the boat's
6.58 Hz and 0.7 m/s). The mosaic settings carry a "Track smoothing" switch (on), a responsive / normal /
smooth choice (T 0.12 / 0.08 / 0.06 m) and a line with the window, the delay or why it is inactive; a
change reads an open recording again with the view kept (`Core::setTrackSmoothing`). An epoch that carries a raw fix keeps it (`kRaw`,
`getPositionGNSS()`, used by export and log); the same line at its time goes beside it
(`Epoch::setTrackPosition`/`getTrackPosition`), and the sonar position -- mosaic, bottom track, tiles,
echogram cursor -- the relief cloud and the 3D boat track line are built from that. Raw values are never
written; an epoch is placed once, when a fix more than 1 s later exists, so `positionAdded` -- and the 3D
track line and the mosaic with it -- runs about a second behind the boat; the 3D boat arrow, the grid circle
and the camera follow take the newest raw fix instead (`Dataset::lastPositionChanged` ->
`GraphicsScene3dView::onLastPositionChanged`, `Dataset::getLastRawPosition`) -- they were on
`positionAdded` and lagged a second in the builds of 12:24/13:05 and APKs `-123142`/`-131126`, fixed from
13:21; waiting epochs carry the pairwise position meanwhile;
`Dataset::flushInterpolation` (from `Core::onFileStopsOpening`) places a file's last second. Recordings with
1 Hz positions (before the GODIO fix) never have 2+2 fixes in +-1 s and draw exactly as before. Also fixed:
`interpolateAtt`/`interpolateArtificalAtt` overwrote the time of every epoch between two headings, a raw fix's
included, when those had none. `tools/interp_check`: 40/40, fifteen mutants caught; ping position error
2.0-2.5x lower than pairwise in the simulation with the 0.30 m GNSS noise the GODIO session calibrated on
the lake recording. Not tried on the water. Details: `docs/track-interpolation.md`.

## Two-channel compressed echogram (2026-10-17, 588cb0f4)

The nanoSSS firmware 11.9 (`l412_adc`, two channels) sends the compressed echogram as
`ID_CHART_Z` **v1**: the stream holds both sides end to end, first half port, second half
starboard. v0 stays what GODIO and the Baikal sonar send, one channel. The version carries the
channel count exactly as it does on the plain `ID_CHART`.

Why end to end and not interleaved like `ID_CHART` v1: the codec's transform runs along range,
and interleaving puts a different channel in every other sample. Measured on ten real logs
through the reference codec, 2000 cells a side: interleaved 469 B at rms 3.38, end to end
430 B at rms 3.05. The firmware side is in `l412_adc/docs/COMPRESSION.md`.

What changed here: `IDBinChartZ::parsePayload` accepts v1 and restarts reassembly when the
channel count changes; `IDBinChart::acceptDecoded` takes `channels` and puts the second half in
`m_completeChart2`. Nothing else was needed: `DevDriver::receivedChart` already splits a chart in
two on `ver == v1`, and `ID_CHART_Z` uses that same callback.

How it was found: the owner flashed 11.9, switched compression on and saw no echogram while the
picture kept scrolling (with Pos zeroing on, every attitude frame opens an epoch). 6c38f881 refused
every v1 frame with `respErrorVersion`.

**Check:** `tools/chartz_check`, offline, on `nanosss_v119_both.bin` — 74 KB straight off the
serial port of a real board with both DATASET bits set, so every ping is there plain AND
compressed. The application's own classes decode the compressed one and it must match its plain
twin side for side. The twin is found by content and accepted only when clearly closer than the
neighbouring pings (a neighbour of the same noise is rms ~4-5 away and passed for the twin in the
first version of the check). Build it like `busaddr_check` (Qt 6 Core + Gui, the llvm-mingw kit),
run `chartz_check nanosss_v119_both.bin`. **Control:** built with `-DCHARTZ_SRC=<dir>` holding
6c38f881's `id_binnary.*` it has to fail.

Verified by machine: 588cb0f4 — 0 of 32 compressed frames refused, 16 pings decoded, 2000 cells a
side, every ping's twin found (rms 1.24-1.46, next best 4.0-5.0, one index offset), sides not
exchanged; the same figures, ping for ping, as the reference decoder `KoggerEchoCodec/ref/ping.py`.
Control on 6c38f881: 32 of 32 refused, 0 pings, FAIL. `id_binnary.cpp` and `dev_driver.cpp`
compile with no warnings.

Also in 588cb0f4: `ID_VOLTAGE` registered at last (the class existed, the frames were dropped),
with names for the channels the nanoSSS v4 firmware uses for diagnostics; `IDBinVoltage::_v` grows
to 256 entries, since a `uint8_t` channel id overran 255; `IDBinAttitude` v1 read all four
quaternion components into `m_w0`.

Built and packaged from `D:\Temp\kogger_merge_test`: the build tree under `D:\YandexDisk\` cannot
build at the moment. The sync client removes
`build\...\qml\kqml_types\CMakeFiles\kogger_kqml_types_autogen.dir\AutogenInfo.json` (its conflict
copies `AutogenInfo (2).json` and `(3).json` lie beside it); `cmake .` regenerates it and it is gone
again within a minute. Same cure as "The sync client fights the deploy": exclude `build/` and
`out_x64/` from sync.

Not verified: the echogram on screen and the circular mosaic in the running app.

## Circular mosaic lag with a zeroed bottom track (2026-10-17, 76c1e271)

The owner, nanoSSS on the bench with Pos zeroing and Bottom track zeroing on: the heading pointer
followed the board at once, the circular mosaic trailed it. Settings at the time (registry,
`HKCU\Software\KOGGER\KoggerApp`): bottom track window on, 3; last N 60; Clear old data off.

Where the time went, read from the code (not measured before the fix):

- **Not the track smoother.** Pos zeroing stamps its fake fixes one *second* apart
  (`DateTime(testTime_, 100)`, `testTime_` + 1 per attitude frame) while they arrive at the ping
  rate, so `chooseWindow` sees 1 Hz, under 12 fixes in any window, and places every epoch at once.
- **Not the processor's coalescing.** A live link sets `kConnection`, which gives
  `pendingWorkTimer_` 10 ms without the restart-on-every-event of file mode (333 ms).
- **The bottom track, twice.** The mosaic horizon is `min(bottomTrack, chart, attitude, sonarPos)`
  (`DataHorizon::tryCalcAndEmitMosaicIndx`), and the bottom-track part trails the last processed
  epoch by `windowSize` (`Dataset::onDistCompletedBatch`, "guard interval", 52db3dfc). Separately,
  the 3D bottom track -- whose `updatedPoints` put epochs into `pendingMosaicIndxs_` -- stops
  `max(3, windowSize)` short of it (`Dataset::onLastBottomTrackEpochChanged`, "minMagicRenderGap",
  upstream 1ea62fc7, no rationale given). A ping was painted only once the third after it had
  arrived: ~170 ms at 18 pings/s, 30-40 degrees behind the pointer at a brisk hand turn.

Both holds exist because the next pass re-processes the last `windowSize` epochs and a real depth
can move. A zeroed depth (`BottomTrackProcessor::zeroDepth_`, the constant 0.01 m) cannot. The
processor now says so on what it publishes -- `BottomTrackUpdate::settled` per update,
`BottomTrackParam::depthFixed` on the parameters it posts back -- and both holds are zero for it.
Per update rather than a dataset flag, because a pass's batches arrive before its parameters: a
stored flag would let the first real pass after zeroing is switched off through unguarded.

**A real bottom track is unchanged.** For the record: the algorithm assigns a window's result to
its centre epoch (`btIndex = iepoch - epoch_min_index - windowSize/2`), so an epoch's depth depends
on (windowSize - 1) / 2 epochs after it -- 1 at the owner's 3, against the 3 the two holds keep.
Shortening them for a real bottom is possible but untested on water and touches upstream's
"magic" minimum; left as it is.

**Measured from now on:** while a live mosaic paints, `DataProcessor` logs every 10 s
`mosaic latency: N pings in 10 s, chart -> painted median .. ms (p90 .., max ..); waiting for the
bottom track .. ms, for the horizon .. ms, in the queue .. ms, painting .. ms; bottom track window
W`. Stamps per epoch at `onChartsAdded`, `onBottomTrack3DAdded`, `onMosaicCanCalc`, the dispatch
in `runCoalescedWork` and a non-cancelled `onWorkerFinished`; live link and mosaic on only, the
table bounded.

Verified by machine: builds with no warnings (`KoggerApp` target, llvm-mingw). Not verified: the
lag on screen, and the latency line itself -- it needs the owner's live session.

## Circular mosaic: what the latency line measured, the last ping, repeats (2026-10-17)

The owner's log of 10:37-10:39 (76c1e271) gave "painting" 99-106 ms at bottom track window 3,
0-1 ms at window 1 and 140-152 ms at 11. Read from the code, that number was not the time to the
first paint, and the real lag was elsewhere:

- **The line counted the wrong epochs.** `MosaicProcessor` reports an epoch painted
  (`onMosaicEpochsProcessed`) before its job ends, and that report takes the epoch out of
  `mosaicInFlightIndxs_`; `latencyPainted` then counted only what was still in flight -- the
  epochs a job left unpainted, and those late.
- **Every ping was painted about W + 1 times.** Each pass of a zeroed bottom track re-processes
  the last W epochs, `BottomTrack::updateRenderData` reported all of them as updated, and
  `onBottomTrack3DAdded` took such epochs out of the processed set and queued them again.
- **The painted edge stood one ping behind the newest.** An epoch's footprint is the stretch to
  the next epoch (`Dataset::calcDimensionRects`), so epoch i is indexed -- and queued, by
  `onSendTilesByZoom` -- only once ping i+1 is in, and a job paints the pairs up to its last
  epoch: 36-50 ms at 20-28 pings/s, before any processing.

Changes:

- `DataProcessor::onSendTilesByZoom`: live, the epoch after the indexed one is queued with it.
  It is dispatched once the mosaic horizon has reached it (`runCoalescedWork`), and its job paints
  the pair up to the newest ping.
- `BottomTrack::updateRenderData`: with `BottomTrackParam::depthFixed` (and not a manual edit) an
  existing vertex is reported only when its depth changed -- once, when zeroing is switched on;
  new vertices always are. A new epoch still enters the mosaic queue through `onSendTilesByZoom`.
  A real bottom track is unchanged.
- The latency line counts an epoch at its first "painted" report (`latencyFirstPaint`) and says
  how many reports came per ping ("painted N times a ping"); the per-job line adds how long the
  surface part waited for the 3D bottom track, a blocking call into the GUI thread in every job
  that carries surface work.

Verified by machine: builds with no warnings (`KoggerApp`, llvm-mingw). Not verified: the edge on
screen and the new line -- both need the owner's live session.

## nanoSSS: the distance is per side (2026-10-17)

The owner set 20 mm and expected 20 m a side; the board held count 998, 10 m a side. The firmware
session checked the board: count 2000 at 20 mm gives 1000 cells a side, and the firmware only caps
count at 5000 and drops an odd one (999 -> 998). A two-sided device (ID_CHART / ID_CHART_Z v1, both
sides interleaved) counts both sides in ID_CHART_SETUP. The device page's "Distance, m" took
resolution x count for the distance and wrote count back from it, and "Resolution, cm" kept that
distance by recomputing count: "20 m" at 20 mm sent 1000, 10 m a side; 1996 samples at 10 mm
changed to 20 mm sent 998. The owner's screenshot showed the same: Distance 30 m over 30 mm and
1000 samples, 15 m a side.

Now `DevDriver::chartSides()` (QML `chartSides`: 2 after a v1 frame, 1 after v0, 0 before the first;
the page falls back to 2 for NanoSSS) makes the page's distance per side, count = sides x distance
/ resolution, rounded to even for two sides. `kogger.log` says what went out ("chart setup sent:
count, resolution, offset; per side") on every change, and what the device holds ("chart setup on
the device: ...") when its answer changes.

Verified by machine: builds. Not verified: on the device.

## The rest of the lag was the black-stripe filter (2026-10-17)

The owner found it: with FBS ("Fix black stripes", forward / backward 5 / 5, Settings -> Dataset)
switched off, the circular mosaic is real-time. `Dataset::addChart` fills a black stripe in a ping
from the pings after it (backward steps), so it releases charts only up to `endIndx -
backwardSteps` (`chartAdded(lastIndx)`): the mosaic horizon trails the newest ping by the backward
steps -- 5 pings, about 250 ms at 20 pings/s. Forward steps fill from the pings before and hold
nothing back, so FBS with backward 0 should keep the filling without the lag (read from the code,
not tried).

The latency line could not see it, because its clock started at that very release; my 5 ms of the
11:11 session was true for the part it measured and said nothing about the hold. It now starts
when the epoch appears (`onEpochAdded`; with Pos zeroing, the attitude frame that also moves the
heading pointer), says how long the chart's release took, and how many pings the filter holds
(`Dataset::chartHoldEpochs`).

Verified by machine: builds. Not verified: the new line in a live session.

## Circular mosaic: arcs between pings, and the last ping of lag (2026-10-17)

The owner's screenshot: range rings drawn as polygons. `MosaicProcessor::updateData` joined the two
pings' samples at each range with a straight 4-connected Bresenham chord; turning on the spot those
samples lie on a circle, and the chord cuts inside it by r (1 - cos(turn / 2)).

Now (`src/data_processor/mosaic_arc.h`) the sonar's pose between two pings is interpolated -- origin
on the straight line, heading the short way, range linearly -- and the sample at range r goes along
origin(u) + r(u) dir(heading(u)). Moving straight that is the old chord exactly; turning on the spot,
the arc. Chord or arc is chosen once a pair, by the far ends (`pathFor`): the chord stays where the
arc would not leave it by half a pixel even there, and for a turn over 60 deg between two pings (a
heading glitch). Chosen per range, the last chords and the first arcs left a seam of holes.

`tools/arc_check`: 20/20 -- 20000 chord cases identical to the old Bresenham pixel, u and step; the
arc on its circle (0.96 px), moving and turning with differing ranges on the interpolated pose
(1.11 px), 4-connected, the short way across 180 deg, nothing outside the mesh; range rings round
within 0.95 px against 2.18 / 4.69 / 10.45 px for the old chord at 6 / 12 / 20 deg a ping; no hole in
a whole turn walked as `updateData` walks it. A mutation run (control passes first) caught 10 of 10.
On a synthetic recording (`ring_bench.ps1 -Klf`, 12 deg a ping) the old build drew 30-gons and the new
one circles, in the app.

**Live latency**, on the bench below (30 pings/s, 6 deg a ping), ping -> first painted: 82 ms at
a43a11bb with the arc. A per-epoch trace found two waits of a ping each:

1. **An 8-bit echogram (ID_CHART, sent in parts) was one epoch off its heading.** `IDBinChart` compared
   a part's resolution and offset with the last *completed* chart's. On connecting (zero) the first
   part completed an empty chart and the second part completed the first part alone; that went into
   an epoch of its own, and for the rest of the session every epoch held one ping's echogram with the
   next ping's heading, its horizon closing a ping late. The same after every change of resolution or
   offset. Now compared with the chart being filled, and only when there is one; `DevDriver` also
   drops an empty chart. 48 ms after. The compressed echogram (ID_CHART_Z, the owner's) is decoded
   through `acceptDecoded`, not this branch (read from the code, not checked on the board); the
   8-bit fix is not checked on the board either.
2. **The look-ahead of a43a11bb was undone.** The newest epoch's 3D bottom-track vertex arrives before
   its indexing, and `onBottomTrack3DAdded` took it out of the queue, putting back only indexed
   epochs. A queued epoch now stays queued. 16 ms after: the 10 ms coalescing timer and 6 ms of
   painting. The owner's live session of 11:26 (compressed, a43a11bb) showed exactly that queue wait,
   37-59 ms, with no wait for the bottom track or the horizon.

The latency line now starts at the epoch's position (`DataHorizon::positionAdded` connected to
`DataProcessor::onPositionAdded`; with Pos zeroing, the attitude frame that moves the heading
pointer), so a hold before the chart's release -- the black-stripe filter's -- is in it.

**Test bench.** `KOGGER_AUTOTEST=UDP:<port>` opens a hidden UDP link listening on <port>, never written
to `pinned_links.xml`. `tools/mosaic_bench/ring_bench.ps1` writes the synthetic sonar as a KLF, or
plays it to the app live in real time, nudges the window by a pixel and prints the latency lines; the
app grabs its window (`KOGGER_UI_PROBE_PNG`) and quits. It runs on the user's own settings (Pos
zeroing on, the mosaic shown): `reg export HKCU\Software\KOGGER\KoggerApp` before, `reg import` after.
The nudge was there because a live mosaic did not start until the camera moved -- not the view's
doing but the clear before the stream (see "The mosaic after connecting" below, fixed the same day);
`-NoNudge` runs as a user who does not touch the view.

**Careful with a live serial link in a test run.** A run with `KOGGER_AUTOTEST=COM13` hung when
UiProbe quit the app (the GUI thread stopped right after the window grab), and the process could not
be killed: one thread stayed in the CP2105 driver and kept COM13 until the cable is replugged.
Quitting the app from its window with the board connected worked all day; stop a stream before the
probe quits.

Not verified: on the owner's board turning; the 8-bit fix on hardware.

## "Clear old data" without the stutter (2026-10-17)

The owner: with "Clear old data" on, the mosaic stutters, and the more epochs (N), the worse. In
FAKE_COORDS+N with that box every mosaic job was a full restart: the hot cache cleared, the worker's
`clearMosaic` zeroing every tile, the last N epochs painted again, every tile of the mesh posted. On
the bench (6 deg a ping, 27 pings/s, N = 80): 233 ms a job, at 37 ms between pings.

1. **The arc, cheaper** (`mosaic_arc.h`): the heading turns by a constant rotation and origin and
   range by constant increments, at most a pixel a step, instead of a cos and a sin every half pixel:
   103 -> 39 us a 6 deg wedge of 300 px (the chord 11 us). `tools/arc_check` 20/20, its pose test now
   by the distance to the curve (1.25 px worst); 11 of 11 mutants caught, the control passing.
2. **The rebuild's list** of the last N is made by index, not by walking `epIndxsFromBottomTrack_`,
   which grew with the session. 89 ms a job after 1 and 2 -- still the whole window every job.
3. **Incremental** (`DataProcessor::runCoalescedWork`, `MosaicProcessor::eraseOwnersBelow`). The
   rebuild runs once: when the mode comes on, N changes, the mosaic restarts or the bottom track is
   recalculated in full (`clearOldTracking_`, `clearOldN_`). From it on, the worker notes for every
   pixel the pair that painted it last, by the pair's older epoch (`owners_`: 4 bytes a pixel, apart
   from the tiles so a tile sent to the renderer carries none of it, only tiles painted in the mode).
   The jobs after paint new epochs as without the mode and then erase the pixels whose pair is below
   the window: last N plus the three pairs `updateDataWrapper` walks for context before them, which
   is what the rebuild shows. A tile is scanned only when its `oldest` (a lower bound of its owners)
   is below that, so the cost follows what left the window, not N; a tile the mesh let go of (LRU)
   comes back from the hot cache first. By the owners, not by walking the old pairs again: a walk
   depends on the bottom track, which can change after the paint. Painted first, erased after, so a
   sector the sonar has come round to is never shown empty in between. An epoch queued ahead of the
   horizon now survives the rebuild (before, the next rebuild painted it anyway).

After, on the bench: **3 ms a job at N = 80, 160 and 320 alike** (max 4), ping -> first painted 13 ms
median. The bench now measures the grab: the angle the mosaic covers around the sonar. N = 15 gives
205 deg (two sides of 17 pairs of 6 deg: 204), N >= 80 the full 360; a build without the erase gives
360 at N = 15, so the measure catches it. Inside a full disc no pixel of the empty background.

`KOGGER_PROFILE=<name>` runs the app as KoggerApp-<name>: settings in
`HKCU\Software\KOGGER\KoggerApp-<name>` and a log of its own, so `ring_bench.ps1 -Profile bench` runs
beside the user's open app without touching their settings (the bench profile is a copy of the
owner's with "Clear old data" on; N is `main\dataset\fakeCoordsLastNSlider` there).

Not verified: switching the box or moving the N slider in a running app (the rebuild path, read
and not driven by the bench); a mosaic of more than 512 tiles (the hot-cache return in the erase); on
the owner's board turning.

## The mosaic after connecting (2026-10-17)

The owner: after starting the app and connecting to the sonar the mosaic stays empty, everything on;
a touch of the zoom and it draws. The mosaic paints only visible tiles, and `DataProcessor` learns
them from the view's rect, which the view sends when its camera moves. The clear before a stream
(`clearAllProcessings`: on connecting `Core::resetRealtimeSessionState`, on the bench something at
start) dropped the visible tiles with everything else; the camera had not moved, so no rect came, and
every epoch was indexed with no tile visible (`onSendTilesByZoom` returned). The same after the
toolbar's "reset processing": the worker's tiles emptied, `lastVisTileKeys_` left as it was, so an
unchanged view was taken as already sent.

Now `requeueViewRect` keeps the last rect the view sent and applies it again after either clear.
kogger.log says when the view starts or stops showing tiles ("mosaic view: N visible tiles at zoom Z,
M epochs indexed without any") and when an epoch comes with none ("mosaic view: no visible tiles").
Bench, no nudge (`ring_bench.ps1 -NoNudge`), N = 80: 0 of 360 deg before (df2f85e6), 360 after, ping
-> painted 11 ms from the first seconds (the nudge had cost the first five); N = 15 still 205 deg; with
the nudge 360. The diagnosis by a temporary log in a test build: 16 tiles at start, dropped by the
clear, then 200 epochs without one.

Seen on the owner's board the same day (0bfdd1fd): the mosaic draws at once after connecting, the
zoom untouched. Not verified: the toolbar's reset in the app.

## Ctrl + drag does not tilt the 3D view (2026-10-17, a question, nothing changed)

The owner: Ctrl + left drag should tilt the map, and does not always. `GraphicsScene3dView::mouseMoveTrigger`
tilts (and turns) only when the camera is in perspective and North mode is off
(`m_camera->getIsPerspective() && !isNorth_`); with Ctrl held and either false the drag does nothing at
all, not even a pan. North mode ("North mode" in the 3D settings, `scene3d/view/isNorthViewButton`) was
on in the owner's settings; switching it on also resets the view to straight down. Perspective ends
above ~5 km from the ground (`perspectiveEdge_` 5000 m, 100 m of hysteresis): a flat map, no tilt. The
tilt is at most 30 deg from straight down (`Camera::rotate`): further the same way does nothing. Follow
the boat with the navigator view pulls the tilt back to 30 deg on every position (off for the owner).
Ctrl is read from each mouse event (`PaneInputBridge.modifierKey`), so keyboard focus does not matter.

Bench: `ring_bench.ps1 -CtrlDrag` posts a Ctrl + left drag 150 px up into the 3D view 8 s in and prints
where the disc meets the dark each way. Qt 6 takes Ctrl from the thread's key state, not from
MK_CONTROL (posted alone, the drag panned the view 150 px), so the bench sets Ctrl in the key state it
shares with the app's GUI thread through AttachThreadInput: no key reaches the keyboard or another
window. N = 80: North mode on -- the disc stays round and in place (up 308, down 319, left 323, right
304 px); off -- tilted (up 227, down 336).

The owner's decision, 2026-10-17 evening: leave it as it is (North mode keeps forbidding the tilt).

## Performance: opening a file, zooming the mosaic (2026-10-17)

The owner: open files much faster; paint the mosaic faster (in parallel, per side or per channel);
compute the other zoom levels in the background. Measured on a 348 MB DSSS recording (57 079 pings,
2 083 621 frames; a copy outside the synced folder) with `tools/perf/open_bench.ps1`: the app opens
the file as a double click does, in a profile of its own (`-Profile perf`, a copy of the owner's
settings with Pos zeroing off), grabs its window and quits; `-Threads` prints CPU per thread each
second, `-WheelAt/-Wheel` zoom the 3D view by posted wheel notches. Where the time went came from a
sampling profiler outside the tree (every 2 ms each running thread suspended, its stack walked with
dbghelp on the process's unwind data, the exe's symbols from llvm-nm).

**Where it was.** The file is read on the GUI thread (`DeviceManager::openFile`, no
`SEPARATE_READING`: that variant reads on a thread of its own but sleeps a millisecond every 11
frames). Of the GUI thread while opening: the frame parser walking every byte through its state
machine, `ReliefCloud::onDatasetGrew` re-arming a QTimer for every epoch (a tenth), QML bindings
re-run for every attitude message and every piece of data (`attitudeUpdated`,
`timelinePositionChanged`, a tenth). After it, half of the GUI thread was the epoch index
(`Dataset::calcDimensionRects`), most of that a `QPainterPath::intersects` per 2.56 m tile of a
swath's bounding box -- the box itself, so every tile passed. After a zoom: every level passed on the
way recomputed the visible mosaic, and with shadows on every delivery of tiles rebuilt the shading
normals of every tile on screen (12 603 deliveries in 25 s).

**Done** (each with its check):
- Frame parser: a frame's body copied as a block, `read()` one memmove (13579c4b). `tools/parse_check`
  parses a file and fingerprints every frame; `compare.ps1` against a git revision: same 2 083 621
  frames, 1.29 -> 0.42 s; a mutant copying a byte too many caught.
- No timer re-arm and no QML update per epoch (a7a57f05): the cloud's tail timer re-arms itself for
  the rest of the quiet second; `attitudeUpdated` and `timelinePositionChanged` at most every 33 ms,
  the last always. `dataUpdate` itself untouched (the epoch rate and CSV logging count it).
- Tile cover row by row (2456a865): `tools/tiles_check`, 120 000 quads against the old test --
  boxes and convex quads the same, crossed ones a superset, none lost, 5-8 times faster.
- Zoom level sent once the camera keeps it 200 ms, 10 % hysteresis (265f954a); the data processor
  then takes the view's last rect again (an unchanged rect is otherwise skipped).
- Shading normals about the changed tiles only (60266e4f); `KOGGER_CHECK_NORMALS=1` compares with a
  full rebuild after every delivery: worst 1.2e-7 over 12 603.
- `KOGGER_PROFILE` everywhere (2a4320b7): fifteen `QSettings("KOGGER", "KoggerApp")` wrote the owner's
  settings from test profiles -- the owner's saved 3D camera was a test's. `koggerAppName()`
  (`src/app_profile.h`) works before `main()` too. The owner's camera restored from the 15:28 copy.

**Result** on that file: "file opened!" 4.5 s from the app's start -> 1.93 s (the app starts in
0.73 s; the stall monitor no longer fires); CPU to open and settle 8.3 -> 5.4 s; after a 6-notch zoom
3.7-5.5 -> 3.7-3.9 s, the GUI thread 1.56 -> 1.0 s. The live ring bench unchanged (205 / 360 deg).

**Then, the mosaic itself:**
- `KOGGER_MOSAIC_DIGEST=1` (956616b5): 1.5 s after the last delivery of tiles the view logs a hash
  of the tiles it holds (keys, pixels, heights; and of keys and pixels alone). `KOGGER_MOSAIC_DUMP=
  <folder>` (f69a90c8) also writes the tiles (`digest_<n>.bin`); `tools/perf/mosaic_dump_compare.ps1`
  compares two dumps tile by tile (`-Picture` draws where they differ). Window grabs are no check:
  two runs alike come out ~8 700 pixels apart there (the camera after the wheel).
- Pairs painted on several threads (8c17d03d): `MosaicProcessor::updateData` lists a chunk's pairs,
  contiguous runs go to `KOGGER_MOSAIC_THREADS` threads (else the cores less two, at most 8), each
  into buffers of its own, merged point by point by the highest pair number -- what painting in
  order leaves. A big job in chunks of 64, not 10. The digest the same with 1 thread and 8; after a
  zoom the work ends at 12.2 s against 14.3 s.
- The zoom levels next to the view's painted in the background (811cc6f2): `MosaicPrecomputer`, a
  thread at low priority with two paint threads and a `MosaicProcessor` of its own (`setBackground`),
  paints the next finer level over the view and the next coarser over twice the view (levels 2..6)
  into the hot cache and the per-zoom processed sets. `DataProcessor::maybeStartPrecompute` starts it
  once the view's work is done and the bottom track has been quiet 1.5 s; a generation number
  cancels it (zoom change, settings, reset) and drops what an older one still sends; a bottom-track
  change reaching back to an epoch sent to it drops the precomputed levels (`dropPrecomputed`: a
  painted tile is not repainted under its head index), a new ping's does not. One log line a run;
  `KOGGER_MOSAIC_PRECOMPUTE=0` off, `KOGGER_PRECOMPUTE_TRACE=1` what it waits for,
  `KOGGER_PRECOMPUTE_QUIET_MS` the quiet time. `tools/perf/precompute_check.ps1` opens a file with
  it and without, zooms and compares the tiles: on the 348 MB file the last tile of the new level
  0.01-0.02 s after the zoom against 1.0-1.3 s (five runs each way), the same 93 tiles, heights and
  marks, 7 pixels of 6.1 million 1-3 apart in three tiles at the survey's edge -- PASS within its
  tolerance then (64 pixels, 8 apart; now shares, "Comparing two mosaics" in the next section).

**Not yet / open** (as left on the first PC; the next section takes them up):
- The 7 pixels: not the bottom track (the precompute 9 s later: the same), not the job the view's
  worker does after it (with its epochs marked done: the same). A guess, unchecked: a tile's head
  index is set per chunk from the tiles the chunk touched, and the precompute's chunks see more.
  -> Larger on another file and the same on the build before: "Comparing two mosaics" below.
- While data streams the bottom track is never quiet for 1.5 s, so the precompute waits until the
  stream stops; nothing is precomputed during a live survey. -> Done below.
- The precomputed levels add to the hot cache (on the test file ~1 000 tiles over three levels,
  ~64 KB each); past its 2 048 the oldest go to the DB, the view's own among them when they were
  not touched lately. Not measured on a big survey. -> Measured below: +140 MB on 1.1 GB.
- The old level kept on screen until the new one covers it (the renderer drops it on the first tile
  of the new level). -> Done below. The DB of tiles lives only for the session (deleted on open and
  close).

**To continue on another PC** (the owner moves to another machine after 2026-10-17 17:40; the
owner's summary in Russian: `docs/perf-2026-10-17.ru.md`). On the first PC 273964bc is installed.
- The code and docs are this repo. Let the sync client finish, then `git fsck` (on 2026-10-16 a sync
  left two objects out). `D:\YandexDisk\DelphiProjects\KoggerApp_myEdition_transfer\` holds a bundle
  of every commit since a044aa93, the last state known on GitHub `mine` (`git bundle verify`, then
  `git fetch <bundle> myedition`), the test profile's settings (`KoggerApp-perf.reg`) and a copy of
  the assistant's notes from the first PC (`claude_memory_2026-10-17\`, context, not instructions).
- Build per `build-windows-cli.md`, never inside the synced folder (the sync client deletes autogen
  files there): a worktree outside it, `git worktree add --detach D:\Temp\kogger_build <commit>`.
  `.git/worktrees` lists the first PC's (`D:\Temp\kogger_merge_test`, `D:\Temp\ka_headunit_wt`),
  missing elsewhere: no `git worktree prune` on another PC -- the metadata syncs back and breaks them.
- Package in that worktree: `scripts\win_build_release.ps1`, then `scripts\win_package_release.ps1
  -SkipBuild` (`dist\...-setup-*.exe`, `dist\stage`; build_info.txt says "dirty: no" only from a
  clean tree). Install for the user, the app closed: `scripts\win_install_user.ps1 -Tag <commit8>
  -Dist <worktree>\dist` (was `D:\Temp\kogger_install\update_kogger_user.ps1`). What is installed:
  `build_info.txt` in `%LOCALAPPDATA%\Programs\KoggerApp myEdition` -- read it before installing.
- Test file: `D:\YandexDisk\ООО Коггер\LOGS\03.05.2025_Ереванское_DSSS\00014_03.05.2025.KLF`, copied
  out of the synced folder first (`D:\Temp\perf\`; the app may write beside it).
- The test profile (`-Profile perf`) keeps its settings in `HKCU\Software\KOGGER\KoggerApp-perf`:
  `reg import` the exported one (the owner's settings with Pos zeroing off), or runs start from
  defaults -- another camera, other digests; a comparison within one PC still holds.
- Checks: `tools\perf\precompute_check.ps1 -Exe <KoggerApp.exe> -File <the copy>` (a build tree's exe
  also needs `-PathPrefix 'D:\Qt\6.8.3\llvm-mingw_64\bin;D:\Qt\Tools\llvm-mingw1706_64\bin'`, a
  dist\stage one does not): PASS, ~0.01 s against ~1 s, 7 pixels. Opening times
  `tools\perf\open_bench.ps1`; where the time goes `tools\perf\sampler\profile_open.ps1` (builds the
  sampler into %TEMP%, lists the exe's symbols with llvm-nm).
- Met on the way: a source file put back with Copy-Item keeps its old time and ninja does not rebuild
  it (set LastWriteTime); a test window under the mouse takes its wheel (one run was zoomed by
  hand); window grabs differ between runs alike -- compare tiles, not grabs.
- Next, by the owner's word: the "Not yet / open" list above; the owner's open questions -- tilt in
  the North mode ("Ctrl + drag does not tilt the 3D view" above) and the LR label (`backlog.md` 12).
  Answered on the second PC the same evening: tilt as it is, the label not now.

## Performance, continued: live, every level, the old level kept, memory (2026-10-17, evening)

On the second PC, from ff8a2816 (the same build installed there per user); the commits 29f39dcb
(precompute), 84f3b03a (old level kept), 4a015bd2 (memory), 7e0c6445 (tools) and the one with
this section, which is the build installed there after it. The owner: the
precompute during a live survey, memory on a big survey, the old level kept on screen; then "every
zoom level has to be worked in the background, else zooming in further repaints it all"; runs kept
short, about a minute. Test files, copies in `D:\Temp\perf\`: 00014 (348 MB) and 00008 (325 MB,
`LOGS\01.05.2025_Ереванское_DSSS`), 00017_03.05.2025 (1.1 GB, 186 085 pings). [verified] below is
a measurement or a check that ran; [inference] is not.

**Precompute while data streams.** `DataProcessor::bottomTrackSettledBefore()`: once the bottom
track has been quiet `KOGGER_PRECOMPUTE_QUIET_MS` (1.5 s) every epoch; while it streams those before
the latest streaming run's `indexFrom` less max(its window, 32) -- a streaming run reworks only its
window and the next one starts no earlier. Such a run logs ", streaming: epochs before N"; a
bottom-track result reaching back to an epoch already sent drops the precomputed levels and says so.
The job-finished handler armed the precompute's timer only when no next run was pending, never while
data streams: now after every job unless it is armed (60 s of 00014 at 45 pings a second: no run
while streaming before, 101 after [verified]). The levels are taken in turn from the one after the
last run: the nearest always had new epochs (100 runs of 101 on it), now 17-18 on each of levels
5..2 [verified]. Live latency the same with it and without: median 197-198 ms, p90 243-246
[verified].

**Every level.** Targets: levels 2..6 but the view's, nearest first, at equal distance the finer. A
coarser one over 2^k the view (what zooming out shows), the next finer over the view, a deeper one
over twice what zooming in to it shows, about the ground point under the cursor
(`GraphicsScene3dView::sendZoomFocus`: after the mouse rests 300 ms, and at every wheel turn ->
`DataProcessor::onZoomFocus`). `MosaicIndexProvider::tilesInQuadNed` returns nothing past 2 000
tiles, so from an overview the deeper levels got no tiles at all [verified by
`KOGGER_PRECOMPUTE_TRACE`, ff8a2816 too]. `dataTilesNear` takes the tiles with data from the
tile-epoch index (walking the area's grid or the index, the smaller), at most 320 a level, the
nearest to the focus: four levels and the view's fit the hot cache's 2 048. An epoch reaching past
them is painted inside them only and stays unmarked (MosaicProcessor's outOfFov), and the view
paints it again when it comes there: from an overview of 00008 that is 4 413 of 5 252 epochs at
level 2 -- work spent twice, not a wrong picture. A focus moved more than an eighth of the view's
diagonal re-opens the levels chosen about it. On 00008 from the overview (level 6): 6 -> 5, the last
tile 0.04 s after the zoom against 1.38 s without; 6 -> 5 -> 3 -> 2, at level 2 0.01 against 0.84 s
[verified, `precompute_check.ps1 -WheelAt "35,42,49" -Wheel 18`]. 00014 as on the first PC: 0.02
against 1.71 s, the same 7 pixels [verified].

**The old level kept** (`SurfaceView::keepAsFallback`): on a zoom the textured tiles of the old level
move to `fallbackTiles_` and are drawn first, pushed back by a polygon offset, so the new level wins
where both lie. A kept tile goes once the new level's visible tiles over it (its children, or its
parent) are there and the new level has not changed for 400 ms -- a tile comes in pieces while its
job paints, and going at the first piece left holes (seen in a grab); all of them 6 s after the zoom
at the latest, and at once on zooming back. One log line: "mosaic zoom fallback: N tiles of zoom A
kept under zoom B, covered after T ms", or "..., K left after T ms (why)". Window grabs 0.7 s after a
6 -> 5 zoom without the precompute: ff8a2816 shows the screen mostly empty, this build the old level
under the new one, and 1.65 s after it the new level whole [verified by looking at the grabs]. At a
survey's edge a kept tile whose new tiles hold no data stays the 6 s: drawn under, harmless.

**Memory** (`open_bench.ps1 -Memory` on 00017, 1.1 GB): private bytes 4.56 GB, 4.70 with the
precompute (+140 MB) [verified]. 1.7 GB of it were two copies of the tile-epoch index no code read
since 47fb15e5: `Epoch::setTraceTileIndxs` (a QMap of QSets a ping) and
`Dataset::appendTileEpochIndex`; only the data processor's own copy is read. Without them 2.81 GB
settled, 3.20 GB at the peak [verified], the mosaic the same (00008, zoom 6 -> 5: digest
f159bd6c17acc561 before and after [verified]). Most of what is left should be the echograms, 5 000
one-byte samples a side a ping, ~1.9 GB here [inference: from the chart setup line and the ping
count, not measured apart].

**Comparing two mosaics.** The precompute and the view paint the same pings into tiles that differ
a little. On 00008 0.1-0.42 % of the pixels, up to 26 apart, single pixels along the pairs' edges,
and 1-3.4 % of the height points; on 00014 the 7 pixels. ff8a2816 gives the same on 00008 whenever
its precompute gets to run (6 -> 5 -> 3 -> 2: 0.24 %, heights 3.4 %) [verified], so not this
evening's change. The view alone painted twice, on 6 threads in chunks of 64 and on 1 in chunks of
10: the same pixels, 6.7 % of the height points another [verified]. The pixels most likely come from
the float32 pixel positions, measured from the mesh's origin, which is another one for another set
of tiles [inference, no change tried]. So `mosaic_dump_compare.ps1 -Judge` judges shares: PASS with
no tile of B missing in A, at most 0.01 % painted in one only, 1 % another value, 10 % of the height
points another. The live check at 0.2 % and 3 %: two live runs without the precompute differed by
0.044-0.067 % and 0.02-1.7 % [verified]; a live run with it gave the very digest of one without.

**Tools.** `tools/perf/live_zoom_check.ps1`: plays a recording to the app as a live device over UDP
(`KOGGER_AUTOTEST=UDP:<port>`), zooms out 5 s in and back in as it stops, prints the precompute's
runs while streaming, the latency lines, the zoom and its last tile, and compares two modes' tiles.
`open_bench.ps1 -Memory`; `-WheelAt "35,42,49"` turns the wheel several times (a few notches move
the camera about a level). `precompute_check.ps1` passes both on.

**Seen, not changed:**
- Opening the 1.1 GB file stops the GUI thread for 5.2 s (the stall monitor's two lines): the file
  is read on it. The CMake option `KOGGER_SEPARATE_READING` (off) reads on a thread of its own;
  not tried.
- ~2.8 GB for a 1.1 GB file after the fix above, the echograms one byte a sample.
- From an overview the deepest levels repaint most of their epochs when the view comes (above).
- Over a finished level the green bottom-track line shows in dashes, the mosaic's heights covering
  it in places; ff8a2816 the same [verified in grabs of both after a 6 -> 5 zoom on 00008].

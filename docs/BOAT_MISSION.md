# Boat mission: waypoints on the map, mission to the autopilot (MAVLink)

A boat with an ArduPilot Rover autopilot (built and tested against Rover 4.4.0) is driven from the
3D pane: points are placed on the map, uploaded as a mission, read back and compared, and the
mission is started, held, sent home or stopped. The autopilot is reached over whatever link carries
its MAVLink frames; on the head unit that is the serial link of the Wi-Fi module, which relays the
boat's stream (see WIFI_MODULE.md).

Status 2026-09-27: implemented on branch `myedition`. Verified by machine with `tools/mission_check`:
67/67 — 34 on made-up events (readiness, heading and health rules, command names, log settling, and the
Wi-Fi module's port-speed change, radio check and restart watch from `src/device/wifi_ops.*`) and 33
against ArduPilot SITL, ArduRover 4.4.0, the same commit as the boat's autopilot (the mission steps, plus
a link dropped right after the autopilot is found and a lost final MISSION_ACK). On the head unit the owner placed and dragged points
and uploaded a mission to the boat. The first start on the boat was refused by the autopilot (its
EKF had no position: its two-antenna GPS never gave a heading, see "ArduPilot facts"); a mission on
water is not yet verified.

## What the app does

- `src/vehicle/mav_vehicle.{h,cpp}` — `MavVehicle`, context property `boat`. Transport-agnostic:
  `feed()` takes MAVLink bytes, `sendBytes()` gives them. In the app `DeviceManager` emits every
  MAVLink frame of a live link (`mavlinkFrameReceived`) and writes the outgoing bytes into the link
  the autopilot was last heard on (`sendMavlinkBytes` → `Link::write`).
  - Talks as ground station 255/190 and sends a 1 Hz heartbeat while `controlEnabled` is on and the
    autopilot was heard within 10 s. That heartbeat arms the autopilot's GCS failsafe.
  - Locks on the first heartbeat of an autopilot (not a GCS), asks for AUTOPILOT_VERSION and
    HOME_POSITION, reads the stored mission back and compares it with the plan. Streams are only
    requested when the vehicle sends no position of its own within 5 s (SITL): a real autopilot
    streams at its SRx_ rates and a request would overwrite them.
  - Telemetry: HEARTBEAT (armed, mode), GLOBAL_POSITION_INT, GPS_RAW_INT, SYS_STATUS (battery and
    sensor health), HOME_POSITION, MISSION_CURRENT, MISSION_ITEM_REACHED, NAV_CONTROLLER_OUTPUT,
    STATUSTEXT, AUTOPILOT_VERSION, EKF_STATUS_REPORT (asked for with REQUEST_MESSAGE every 2 s when
    the autopilot does not stream it; after 10 requests without a report it stops asking until the
    link comes back), PARAM_VALUE for a fixed list of settings read after the autopilot is found, and
    again after a link loss while EK3_SRC1_YAW or FS_EKF_THRESH is still unknown (the read windows then
    count from the reconnection; without that a drop right after detection left the heading switch at
    "settings not read yet" for the whole session) (AHRS_EKF_TYPE, EK3_ENABLE, EK3_SRC1_POSXY/VELXY/YAW, EK3_GPS_CHECK, COMPASS_ENABLE/USE,
    ARMING_CHECK, FS_EKF_ACTION, FS_EKF_THRESH; GPS_TYPE/TYPE2, GPS_AUTO_SWITCH, GPS_PRIMARY,
    GPS_AUTO_CONFIG, GPS_DRV_OPTIONS, GPS_POS1/2_*, GPS_MB1/2_TYPE and _OFS_*), six requests a second,
    logged one line per group; GPS2_RAW.
  - GPS heading (`gpsHeadingState`, `gpsHeadingText`, panel line "GPS heading: ..."): from the yaw
    field of GPS_RAW_INT and GPS2_RAW — not configured / configured but none now / degrees — with the
    second receiver's fix (RTK float or fixed). With EK3_SRC1_YAW 2 or 3 the EKF has no yaw without it.
    Heading and sensor-health changes go to the log once they have held 3 s, with the number of flips
    ("after N changes"), so a flapping RTK heading or RC link does not push everything else out.
  - Navigation readiness (`navState`, `navText`, panel line "Navigation: ..."): from
    EKF_STATUS_REPORT, the way Rover 4.4 judges it once armed — a horizontal position (absolute or
    relative) outside constant-position mode, and no EKF failsafe (velocity variance, or position and
    compass variances together, at FS_EKF_THRESH). Changes go to the log with the flags, variances and
    GPS. `healthText` (panel line "Autopilot: not healthy: ...") lists the sensors the autopilot has
    and uses but reports unhealthy in SYS_STATUS.
  - Plan: list of points, speed (0 = the boat's WP_SPEED), return home at the end; stored in
    QSettings `boatMission/*`.
  - Mission built as: item 0 home slot, optional DO_CHANGE_SPEED, one NAV_WAYPOINT per point
    (frame GLOBAL_RELATIVE_ALT, z = 0), optional NAV_RETURN_TO_LAUNCH.
  - Upload: MISSION_COUNT → answer MISSION_REQUEST / MISSION_REQUEST_INT with MISSION_ITEM_INT →
    MISSION_ACK, then an automatic read-back (MISSION_REQUEST_LIST → MISSION_REQUEST_INT →
    items → MISSION_ACK) and comparison. `planOnVehicle` is true only after a matching read-back;
    any edit of the plan clears it. When the last item went out and no MISSION_ACK comes (lost over the
    relay: the autopilot has already stored the mission and left the upload), the read-back decides
    instead of a failure.
  - Commands as sequences with retries (4 × 1.5 s) and COMMAND_ACK checking: start (MISSION_SET_CURRENT
    1, HOLD unless the boat is in HOLD, arm if needed, DO_SET_MODE AUTO), HOLD, RTL, arm, disarm. The HOLD step
    (2026-09-29): ArduPilot starts or resumes a mission only when AUTO is entered, so a boat left in AUTO by a plan
    without the return home ("Mission Complete") took "Start" (MISSION_SET_CURRENT and AUTO both acknowledged) and
    did nothing (field test 2026-09-28: five starts, the mission ran only after the RC switched to LOITER and back);
    and Rover does not arm in RTL ("Mode not armable"), where a plan with the return home ends. Reproduced on SITL
    (the check's last phase, `MISSION_CHECK_REPRO_HOLD=1` starts it from HOLD as on the water: FAIL before the fix),
    142/142 after it. HOLD, RTL and disarm interrupt a
    running sequence. Start is refused without link, points, a verified upload, a 3D fix, a ready
    navigation or a home. A start that armed the boat and then fails (AUTO refused, no answer) disarms
    it again ("Disarm after the failed start"); HOLD, RTL or disarm pressed meanwhile cancel that.
    A refused command (start, heading source) never renames the command still running.
- `src/scene3d/scene3d_view.*` — `geoToScreen(lat, lon)`, `screenToGeo(x, y)`, signal
  `cameraChanged()` (emitted at the top of `onCameraMoved()`), property `mapTapEnabled` and signal
  `mapTapped(lat, lon)`: while enabled, a left tap without a drag is a map position and nothing else.
- `qml/scene3d/BoatMissionLayer.qml` — route, numbered markers (drag to move), home, the leg from
  the boat to its target and the way home, drawn with Qt Quick Shapes over the scene.
- `qml/scene3d/BoatMissionPanel.qml` — status, "Add points", point list (folds away under
  "Points: N", state kept), speed (0.5 / 0.8 / 1 / 1.5 / 2 m/s), return home, upload, clear, "Load
  from boat" (the autopilot's mission becomes the plan: waypoints, the first DO_CHANGE_SPEED, a closing
  RTL; anything else is named and left out), start, HOLD, return home, arm/disarm, heading source
  ("GPS, 2 antennas" = EK3_SRC1_YAW 3, "Compass" = 1; PARAM_SET confirmed by PARAM_VALUE, refused
  while armed), heartbeat switch, last messages. Moving commands, Arm, a heading-source change and a
  plan-replacing load ask for a second tap within 3 s; HOLD and Disarm never do.
- `qml/scene3d/Scene3DRightToolbar.qml` — the boat button (shown once an autopilot is heard or a
  route exists); `qml/app/Pane3DWindow.qml` hosts the layer and the panel and binds
  `mapTapEnabled`; `qml/app/MainWindow.qml` turns command results and autopilot warnings into
  notifications.
- `third_party/mavlink` — the official C library, MIT (see its README).
- Data widget "From launch point" (`DataFieldCatalog`, key `apHomeDist`, group autopilot; owner 2026-09-30): the
  distance from the autopilot's home to the boat now, `MavVehicle::distanceM` of `homeLatitude/homeLongitude` and
  `latitude/longitude` (GLOBAL_POSITION_INT); `DataWidgetPopup` passes the context property `boat`. Metres under
  1 km, km over; a dash without the link, home or a position. Home is where ArduPilot put it: the first position,
  then where the boat is armed; the app asks for HOME_POSITION again on arming (80520c86).

## ArduPilot facts this rests on (Rover 4.4.0, SITL, 2026-09-26)

- Items are requested with MISSION_REQUEST even when MISSION_ITEM_INT is sent; MISSION_ITEM_INT is
  accepted. Item 0 reads back as the home (frame 0).
- A MISSION_COUNT sent while the autopilot is still starting gets MAV_MISSION_NO_SPACE.
- GCS failsafe: the condition starts when the last heartbeat from SYSID_MYGCS is FS_GCS_TIMEOUT old
  (float, no upper clamp in the code; documented range 2–120) and acts FS_TIMEOUT later. Measured:
  RTL 181.2 s after the last heartbeat with 175 + 5 outside AUTO.
- In AUTO with FS_THR_ENABLE = 2 the GCS failsafe continues the mission: `failsafe_trigger()` takes
  the "continue in AUTO" decision from the type of the check that crossed the threshold, and the
  radio check runs more often (source of Rover 4.4 and of master, 2026-09-26; SITL run). Once an
  RC failsafe has triggered, a later GCS loss does not act at all. A mission that must come back on
  its own therefore ends with NAV_RETURN_TO_LAUNCH (the panel's default).
- Mode changes while armed (Rover `Mode::enter`): a mode that needs a position (AUTO, RTL, GUIDED,
  ...) is refused ("Flight mode change failed") unless `ekf_position_ok()` — armed: horizontal
  position absolute or relative, not constant-position mode — and no EKF failsafe. Disarmed, every
  mode is accepted. `ekf_check` (armed only, 10 Hz) raises the EKF failsafe ("EKF variance") after
  1 s of `ekf_over_threshold()`: velocity variance ≥ FS_EKF_THRESH counts twice, position and compass
  once each, two or more trips it, and so does the lack of a position.
- EKF_STATUS_REPORT comes from the configured EKF (AHRS_EKF_TYPE); nothing at all is sent for DCM.
  EKF3 sets every flag only when the filter is healthy and aligned (`filterHealthy` = healthy, tilt
  aligned, yaw aligned when a compass is in use), so flags 0 without UNINITIALIZED means an EKF that
  was once healthy and now is not, or is not aligned. The variances are the square roots of the
  innovation test ratios: all 0 means nothing is being fused.
- EK3_SRC1_YAW = 3 ("GPS with compass fallback", `AP_NavEKF3_MagFusion.cpp`): the first yaw
  alignment comes from GPS yaw only (a moving-baseline, two-antenna GPS); the compass fallback needs
  GPS yaw to have worked before (`learnMagBiasFromGPS`) and the vehicle to be moving. With a
  one-antenna GPS the yaw never aligns, the EKF never uses GPS, and it reports flags 0.
  Exactly (SelectMagFusion, Rover-4.4.0): without GPS yaw it falls back only after 10 s without it, with
  `gps_yaw_mag_fallback_ok` and `inFlight`, then says "EKF3 IMU%u yaw fallback active"; until then no yaw is
  fused; when GPS yaw returns it goes back to it. The EKF's yaw messages: "MAG%u initial / in-flight yaw
  alignment complete", "MAG%u ground mag anomaly, yaw re-aligned" (compass), "yaw aligned using GPS" (two
  antennas), "yaw aligned to GPS velocity" and "emergency yaw reset" (GSF), "yaw fallback active", "yaw
  external". The boat panel shows what is in use from these (`MavVehicle::headingInUse`, 2026-09-30).
- ARMING_CHECK = 0 skips every pre-arm check (`AP_Arming_Rover::pre_arm_checks` returns at once), so
  the autopilot never reports "PreArm: ..." reasons; SYS_STATUS health still comes.
- GPS heading over MAVLink (`AP_GPS::gps_yaw_cdeg`): the yaw field of GPS_RAW_INT / GPS2_RAW is 0 when
  that GPS is not configured for yaw, 65535 when it is but has none, else centidegrees (36000 = north).
- u-blox moving baseline (GPS_TYPE 17 base / 18 rover, `AP_GPS_UBLOX.cpp`): the heading comes from the
  rover's RELPOSNED only with an RTK fixed carrier solution (carrSolnFixed; float is rejected on
  purpose), then `calculate_moving_base_yaw` checks the antenna offsets: at least 5 cm, and the measured
  baseline not more than 20 % shorter than the configured one (GPS_POS1/2_* with GPS_MBn_TYPE 0).
- The boat (2026-09-26): GPS_TYPE 17/18, GPS_POS1_X −0.52, rover in RTK float with 28 satellites and
  yaw 65535 for minutes on end — so no heading, no EKF yaw, no navigation. Not an app matter: sky view,
  antennas, cables or interference at the boat.

## How to run the SITL check

```
# WSL (Ubuntu): the official SITL binary of the release
mkdir -p ~/sitl && cd ~/sitl
wget https://firmware.ardupilot.org/Rover/stable-4.4.0/SITL_x86_64_linux_gnu/ardurover
wget https://raw.githubusercontent.com/ArduPilot/ardupilot/Rover-4.4/Tools/autotest/default_params/rover.parm
wget https://raw.githubusercontent.com/ArduPilot/ardupilot/Rover-4.4/Tools/autotest/default_params/motorboat.parm
chmod +x ardurover
./ardurover --model motorboat --home 40.35,45.0,1900,0 --defaults rover.parm,motorboat.parm -I0 --speedup 5 --wipe &

# Windows: build and run the check (Qt 6.8.3 llvm-mingw kit)
cmake -G Ninja -S tools/mission_check -B build/mission_check -DCMAKE_PREFIX_PATH=D:/Qt/6.8.3/llvm-mingw_64
cmake --build build/mission_check
build/mission_check/mission_check.exe --host 127.0.0.1 --port 5760
# without SITL: only the checks on made-up events (navigation rules, Wi-Fi module sequences and port pages)
build/mission_check/mission_check.exe --offline
```

Start SITL as a child of a shell that stays alive (as above). Started with `exec` or `setsid nohup`
from a background `wsl` call it died right after the first connection (2026-09-26, cause not
investigated).

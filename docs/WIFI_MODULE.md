# Wi‑Fi module (board 87) in KoggerApp

A small Wi‑Fi client module (ESP32‑C3) speaks the Kogger SBP protocol over a serial port. It
scans, joins the boat's access point, reports link state and traffic, and takes firmware updates
the same way sonars do. KoggerApp treats it as a device with **board id 87** and one extra command,
**`ID_WIFI` = 0x57**. This file is the protocol contract as the app implements it, plus what the app
does with it. Nothing here depends on how the module is built.

Status: implemented 2026‑09‑25 on branch `myedition`; verified against the module on a head unit
with the module on an on‑board UART (`/dev/ttyS6`, 921600 baud). Not verified: firmware upgrade of
the module from the app, 3 and 4 Mbaud.

## 1. How the app sees the module

- An ordinary serial link, default 921600 baud (in the app's baud search list). `route` = 0.
- `ID_VERSION` answers with board 87; the device list shows it as **"Wi‑Fi"**.
- `ID_UART` (0x18) as on sonars: SET v0 `{KEY 0xC96B5D4A, U1 1, U4 baud}`, 9600 … 4 000 000. The
  confirmation comes at the old rate, then the module switches. Persisted only by `ID_FLASH` v0
  ("Flash settings"); otherwise a power cycle returns to 921600.
- `ID_MARK`, `ID_FLASH` (v0 save / v1 restore / v2 erase), `ID_BOOT`, `ID_UPDATE` as on sonars. After
  `ID_BOOT` v0 ("Reboot") the module waits 5 s for an update before rebooting. Update files are
  `KoggerWiFi_<version>.ufww`; the file dialog filter includes `*.ufww`.
- The module's port understands a second, text protocol for bench use. The first valid frame after
  power‑up locks the port to one protocol until reboot, so KoggerApp must be the only program
  opening that port.

## 2. `ID_WIFI` = 0x57

All multi‑byte fields little‑endian. GETTING → CONTENT of the same version. Every SETTING is
confirmed with CONTENT + `resp` flag and payload `{code, ck1, ck2}` (`ck` = checksum of the frame
being confirmed), which `IDBin::checkResponse()` verifies. Codes: 1 OK, 3 ERR_PAYLOAD,
5 ERR_VERSION, 6 ERR_TYPE, 8 ERR_RUNTIME.

| Version | GETTING | SETTING |
|---|---|---|
| v0 | status | — |
| v1 | link report | `{U2 period ms}`: 0 off, 100…60000 |
| v2 | start a scan; one CONTENT v2 per network | same, with a confirmation |
| v3 | status | join: `{U1 flags (bit0 save), U1 len, SSID, U1 len, password}` |
| v4 | saved networks, one CONTENT v4 each | action `{0}` leave + auto‑connect off, `{1}` auto‑connect on, `{2, U1 len, SSID}` forget, `{3}` forget all |

**CONTENT v0 — status**, 39 bytes + SSID. Also pushed by the module on every state change.

| Byte | Field |
|---|---|
| 0 | state: 0 IDLE, 1 SEARCHING (looking for saved networks), 2 CONNECTING, 3 CONNECTED |
| 1 | flags: bit0 auto‑connect, bit1 scan in progress, bit2 current network is saved |
| 2 | S1 RSSI, dBm (0 when not connected) |
| 3 | channel |
| 4 | security: 0 OPEN, 1 WEP, 2 WPA, 3 WPA2, 4 WPA/WPA2, 5 WPA3, 6 WPA2/WPA3, 7 ENTERPRISE, 8 OWE, 255 other |
| 5 | last disconnect code (ESP‑IDF) |
| 6 | reason category: 0 none, 1 network not found, 2 wrong password, 3 association, 4 link lost, 5 other |
| 7 | reserved |
| 8–13 | BSSID |
| 14–29 | IP, mask, gateway, DNS — 4 bytes each, a.b.c.d |
| 30–33 / 34–37 | U4 receive / transmit, bytes per second |
| 38 | SSID length, then the SSID (up to 32 bytes) |

**CONTENT v1 — link report**, 22 bytes: `U1 state, S1 RSSI, U4 rx B/s, U4 tx B/s, U4 rx total,
U4 tx total, U4 uptime s`. Sent unsolicited once a second; the period is changed by SETTING v1.
Traffic is the module's Wi‑Fi interface over the last full second.

**CONTENT v2 — one network**, 13 bytes + SSID: `U1 index, U1 total, S1 RSSI, U1 channel,
U1 security, U1 flags (bit0 saved, bit1 current), BSSID[6], U1 len, SSID`. Strongest first; no
networks → a single `{0, 0}` frame. A scan takes 2–4 s and is refused with ERR_RUNTIME while the
module is joining a network.

**CONTENT v4 — one saved network**: `U1 index, U1 total, U1 len, SSID`; none → `{0, 0}`. Up to 8.
Passwords stay in the module and are never sent out.

**Behaviour.** After power‑up and after a loss the module joins the strongest saved network on
its own. A network from v3 is saved only once an address was obtained. On a wrong password the
module gives up at once and pushes v0 with reason 2. Hidden networks are joined only by an explicit
v3. The module joins with exactly the password it is sent: for a saved network the app asks for
the password once more (or leaves the join to auto‑connect).

## 3. Where it lives in the app

- `src/proto_binnary.h` — `ID_WIFI`; `src/id_binnary.h/.cpp` — `BoardWiFi = 87` and `IDBinWifi`
  (parsing of v0/v1/v2/v4, the v1/v3/v4 setters through `hashBinFrameOut()`, scan bookkeeping with
  an 8 s timeout for lost frames).
- `src/device/dev_driver.h/.cpp` — `idWifi`, asked once per connection for boards 87 only
  (`process()`), `receivedWifi()`, the `wifi*` readers and `Q_INVOKABLE` commands,
  `wifiCommandResult(version, resp)` for refusals. `src/device/dev_q_property.h` exposes them as
  `dev.wifi*` properties.
- `qml/app/DeviceSettingsPage.qml` — the "Wi‑Fi" island (state chip, current network with RSSI,
  channel, security and address, traffic, auto‑connect switch, Scan / Disconnect / Other…, scan
  result, join rows with a password field, saved networks with Forget). Baud lists extended to
  4 000 000; upgrade filter takes `*.ufww`.
- `src/link/link_manager.cpp` — on Linux only USB adapters are listed as serial ports; an on‑board
  UART is opted in by name with the environment variable `KOGGER_SERIAL_PORTS` (comma‑separated
  port names or device paths, e.g. `ttyS6`). The head unit's kiosk service sets it.

## 4. Acceptance

1. Module on a serial port, KoggerApp opens it → a "Wi‑Fi" device appears.
2. The island shows CONNECTED; RSSI and rates change once a second.
3. Scan → list of networks; joining the boat's access point with a typed password → CONNECTED, the
   network appears under "Saved networks".
4. Wrong password → a clear notice, nothing hangs.
5. Power cycle → the module rejoins on its own (about 8 s); the app shows it without any action.
6. "Set baudrate" 2 000 000 → the link survives; a power cycle returns it to 921600 unless saved.
7. "Upgrade" with a `.ufww` of a higher minor version → new version after the reboot (not yet
   verified from the app).

# Wi‑Fi module (board 87) in KoggerApp

A small Wi‑Fi client module (ESP32‑C3) speaks the Kogger SBP protocol over a serial port. It
scans, joins the boat's access point, reports link state and traffic, and takes firmware updates
the same way sonars do. KoggerApp treats it as a device with **board id 87** and one extra command,
**`ID_WIFI` = 0x57**. This file is the protocol contract as the app implements it, plus what the app
does with it. Nothing here depends on how the module is built.

Status: implemented 2026‑09‑25 on branch `myedition`; verified against the module on a head unit
with the module on an on‑board UART (`/dev/ttyS6`, 921600 baud): the Wi‑Fi island. The boat link
(section 2a, module firmware 0.7+) is implemented in the app but not yet exercised against a
relaying module. Not verified either: firmware upgrade of the module from the app, 3 and 4 Mbaud.

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
| v5 | relay configuration, 10 bytes | the same 10 bytes; stored by the module and applied at once (firmware 0.7+) |
| v6 | relay counters, 47 bytes | — |

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

## 2a. Boat link: the module relays the boat's SBP stream (firmware 0.7+)

The boat's devices (a logger, a sonar) sit behind the boat's access point. With the relay on, the
module parses the port's byte stream into whole SBP frames: frames not addressed to the module go to
the boat over UDP or TCP (whole frames, packets of at most 512 bytes, longer frames cut into 512‑byte
pieces), and whatever comes from the boat is written into the port unchanged. KoggerApp keeps one
device per SBP address on a link, so the boat's devices simply appear as further devices on the
module's serial link, while the module itself answers only at the address set in v5.

**v5 configuration** (SETTING and GETTING, 10 bytes): `U1 mode` (0 off, 1 UDP, 2 TCP), `U1 addr`
(the module's own SBP address while relaying; must differ from the boat device's address, usually
0; 87 is the convention), `ip[4]` (the boat; `0.0.0.0` with UDP = answer whoever sent the last
datagram), `U2 rport` (the boat's port), `U2 lport` (own UDP port, 0 = same as rport). The
confirmation comes from the module's **old** address; the CONTENT v5 that follows already carries the
new one, which is how the module reappears as a new device. Refused with ERR_PAYLOAD when the mode
is on and the boat port is 0.

**v6 counters** (GETTING, 47 bytes): `U1 state` (0 off, 1 no Wi‑Fi, 2 no peer, 3 ready), `ip[4]` and
`U2 port` of the peer, then ten `U4`: frames up, bytes up, packets up, drops up, packets down, bytes
down, frames down, drops down, frames for the module, TCP connects.

Firmware before 0.7 answers v5 and v6 with ERR_VERSION; the app remembers that and shows the
"no relay in firmware" hint instead of the fields.

## 2b. Firmware 0.10: radio, role, access point, address, two lines

The module project calls this contract version 2. Everything below is in its docs/SBP_WIFI.md
(§3, §3a) and docs/NETWORK.md; the app follows it field by field.

- **`ID_WIFI` v7 radio** (0.8+). SETTING `{U1 power in 0.25 dBm 8..84, U1 protocols 0x01 b / 0x03 b/g /
  0x07 b/g/n / 0x08 LR / 0x0F b/g/n+LR, U1 width 1 = 20 MHz / 2 = 40 MHz, U1 power save 0/1/2}`; CONTENT
  adds what is in effect: power in use, negotiated mode (0 LR, 1 11b, 2 11g, 4 HT20, 5 HT40, 0xFF none),
  protocols and width in use. Protocol and width changes make the module reconnect. Firmware before 0.8
  answers ERR_VERSION; the app then hides the Radio island.
- **`ID_WIFI` v0**: state 4 = the module runs as an access point; byte 7 = number of its clients.
- **`ID_WIFI_NET` = 0x58** (0.10). Every SETTING starts with the key 0xC96B5D4A. v0 role `{saved, running}`
  (0 station, 1 access point; SETTING `{role, flags bit0 reboot now, bit1 forget lines}`); v1 access point
  `{channel, hidden, max clients, auth 0 open / 1 WPA2 / 2 WPA2+WPA3, len, SSID, len, password}` (the
  password never reads back; empty keeps it); v2 address and DHCP (20 bytes); v3 one line per frame
  `{line, mode 0 off / 1 UDP / 2 TCP, dest 0 fixed / 1 senders / 2 broadcast, ip, rport, lport, baud,
  TX pin, RX pin, uart}`; v4 line counters (57 bytes); v5 access-point clients; v6 the module's own address
  while a line relays. The ID_WIFI v5/v6 relay record is line 0's view. The app asks v0, v1, v2, v3, v6
  once per connection; four seconds of silence (or ERR_ID) mean firmware before 0.10, and the old Boat
  link island stays.
- Where: `IDBinWifi` (v7, AP state), `IDBinWifiNet` (`src/id_binnary.*`), `DevDriver::receivedWifiNet`,
  the `wifiRadio`, `wifiNet*` properties (maps and lists) and `wifiSetRadio`, `wifiNetSet*` invokables;
  islands Radio, Role and access point, Access point address and DHCP, Lines in `DeviceSettingsPage.qml`.
  The app log gets one `wifi-net:` / `wifi-radio:` line whenever a reported setting changes.

## 2c. Port speed of the module (line 0)

The module's line 0 runs at any speed from 9600 to 4 000 000 (`ID_UART` v0 `{KEY, 1, U4 baud}`, answered at the
old speed, then it switches; kept only after `ID_FLASH` v0). The Wi-Fi module's page has a "Port speed" island
(460k … 4M): the app sets the module, moves its own link (`linkManagerWrapper.updateBaudrateFor`, which also
rewrites the pinned record), asks for the status until the module answers at the new speed (each check forgets
frames from before it), and only then saves in the module; no answer within about 6 s puts the link back.
Steps are `wifi-port:` lines in the log. The head unit runs at 2 000 000 since 2026-09-26 (the boat stream is
~92 KB/s, a full 921600 port). The Actions island's "Set baudrate" now moves the app's link with the device too.

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
  result, join rows with a password field, saved networks with Forget) and the "Boat link" island
  (mode Off / UDP / TCP, module address, boat IP and port, own UDP port, Apply / Read back, live
  counters polled every 2 s while the relay is on). Baud lists extended to 4 000 000; upgrade filter
  takes `*.ufww`.
- `src/device/device_topology_model.cpp` — a Wi‑Fi module is a *bridge* (node role 3): it is the
  hop the link physically enters, so it is listed first, then the recorder, then the devices on
  the recorder's ports by address. `DeviceTopologyView.qml` draws the bridge → recorder hop
  dashed (radio) and gives the module the Wi‑Fi icon; `HotActionsPanel.qml` uses the same icon.
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
8. Boat link: mode UDP, address 87, the boat's port → the module reappears as a device at address
   87 with counters, and the boat's logger appears as a device at address 0 on the same link.
   Verified 2026‑09‑25 with the boat at 10.0.0.10:14444 (UDP): about 35 datagrams and 110 frames
   per second come down with no drops.
9. The device chain reads Wi‑Fi → logger → sonar(s), the module first.

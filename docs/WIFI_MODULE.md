# Wi‑Fi module (board 87) in KoggerApp

A small Wi‑Fi client module (ESP32‑C3) speaks the Kogger SBP protocol over a serial port. It
scans, joins the boat's access point, reports link state and traffic, and takes firmware updates
the same way sonars do. KoggerApp treats it as a device with **board id 87** and one extra command,
**`ID_WIFI` = 0x57**. This file is the protocol contract as the app implements it, plus what the app
does with it. Nothing here depends on how the module is built.

Status: implemented 2026‑09‑25 on branch `myedition`; verified against the module on a head unit
with the module on an on‑board UART (`/dev/ttyS6`, 921600 baud then; 2 000 000 since 2026-09-26, §2c): the Wi‑Fi island. The boat link
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
  TX pin, RX pin, uart}` (line 1 needs a speed 9600–4 000 000 or the module refuses the whole line with
  ERR_PAYLOAD; the app checks it before sending); v4 line counters (57 bytes); v5 access-point clients; v6 the module's own address
  while a line relays. The ID_WIFI v5/v6 relay record is line 0's view. The app asks v0, v1, v2, v3, v6
  once per connection; four seconds of silence (or ERR_ID) mean firmware before 0.10, and the old Boat
  link island stays.
- Where: `IDBinWifi` (v7, AP state), `IDBinWifiNet` (`src/id_binnary.*`), `DevDriver::receivedWifiNet`,
  the `wifiRadio`, `wifiNet*` properties (maps and lists) and `wifiSetRadio`, `wifiNetSet*` invokables;
  islands Radio, Role and access point, Access point address and DHCP, Lines in `DeviceSettingsPage.qml`.
  The app log gets one `wifi-net:` / `wifi-radio:` line whenever a reported setting changes.

## 2b'. The Radio island shows what is in effect

The selections show the values in effect while the module has a link: power in use (the driver's step: asking
21 dBm gives 20, so 20 is the top option), protocols in use, and the width taken from the negotiated PHY (HT40
= 40 MHz, anything else 20). The module's own "width in use" field is the driver's setting and read 40 on an
HT20 link (2026-09-26), so it is not used for that. Captions add the saved value when it differs. After Apply
`DevDriver` waits for the link, compares, and when the module could not do what was asked (a power between
steps, 40 MHz from an access point that gives 20) it saves what is in effect and says so (`wifi-radio:` line);
no link within 30 s gives a warning and changes nothing. The module does not push its radio state after a
reconnect, so `DevDriver` asks for v7 at every station state change and the check asks every 2 s until the
link is back. The check is `WifiRadioCheck` (`src/device/wifi_ops.*`) and finishes whether or not the page is
open: the settings page is unloaded as soon as it is left, and the first version's page timers died with it
(review, 2026-09-27). An unapplied selection returns to what is in effect after 30 s.

A module restart is a `wifi: the module restarted (uptime …, was …, … expected)` line: an uptime behind the
last report, or behind the last report plus the time since by more than 5 s (`WifiRestartWatch`), so a boot
loop of a few seconds and a restart between rare reports both show (the first version needed a 5 s drop).

## 2d. Firmware 0.11 (module contract 3)

- `ID_WIFI` v1 is 23 bytes: the last one is the reason of the module's last reset (`esp_reset_reason_t`: 1
  power-on, 3 software — update, role change, reboot window —, 4 panic, 5 interrupt watchdog, 6 task watchdog,
  7 other watchdog, 9 brownout, 14 power glitch, 15 CPU lockup, 0 unknown). The Wi-Fi island shows it as "last
  restart" next to the uptime, a restart line carries it, and the first report of a session is logged as
  `wifi: module up N s, last reset: …`. 22-byte reports (0.10 and older) still parse; every CONTENT is read by
  its minimum length, as the contract asks (new fields only at the end).
- v7 power is sent up to 80 quarter-dB (20 dBm, the driver's top; 0.11 stores 81…84 as 80). The module pushes
  v7 after each connection; the app still asks at every station change (0.10 needs it).
- `ID_WIFI_NET` v1: access-point channel 1–11 (0.11 refuses 12–13); v6: own address 1–255 (0 is where devices
  sit). The fields and the driver's checks follow.
- Access-point role: LR only was not offered (0.11 refuses it: no phone or laptop could join; offered again for 0.12,
  §2e), and the post-Apply
  radio check does not wait for a negotiated PHY there — an access point has none (0xFF), so the width in effect is
  the driver's (v7 byte 7). A refused radio setting cancels the check.

## 2e. Firmware 0.12: two equal ports X1 / X2 (`ID_WIFI_NET` v7)

The module project's ports contract for 0.12. What changes for the app and how it is implemented:

- **Addressing (module side).** Own address = `ID_WIFI_NET` v6 if set, else 87 (station) / 88 (access point), whether
  or not a line bridges; 0 and 255 are refused (the app offers 1–254 once v7 is known). The module answers `ID_VERSION`
  sent to route 0 or 255 from any channel, from its own address: the app's ordinary search (`ID_VERSION` to route 0
  on link open) therefore finds it on X1, X2 or the network with no change in the app. Unsolicited frames (v0, v1
  report, v7 radio) go to every channel that asked the module anything within 60 s; the app asks all the time.
- **Rates.** `ID_UART` v0 SET changes the UART the request came through; `ID_WIFI_NET` v3 SET takes the `baud` of
  **both** lines (the current value keeps it; pins still only for line 1). A new rate on the requester's own port is
  provisional: kept once a request for the module comes through that port, or on `ID_FLASH` v0, otherwise back
  after 10 s. A rate for the other port, or from the network, is saved at once. `ID_FLASH` v0 saves both ports.
- **`ID_WIFI_NET` v7, GETTING only.** No payload: page 0 of both ports (two CONTENT frames); `{U1 port, U1 page}`:
  that page. 0.11 answers ERR_VERSION. The app sends the no-payload request once per connection together with the
  other `ID_WIFI_NET` reads; ERR_VERSION (or 4 s without an answer) means "no port information": the islands stay as
  they were for 0.11 and no notice is shown. Decoded by `WifiPorts` (`src/device/wifi_ops.*`) by minimum length:
  page 0 = 116 bytes, page 1 = 3 + 12·n, page 2 = 5 + 8·n; bytes past those are ignored; a short payload, a port
  above 1 or an unknown page changes nothing. Ages arrive in 0.1 s (0xFFFF = never).
- **QML surface** (`DevQProperty`): `wifiNetPortSupport` (-1 unknown, 0 none, 1 yes); `wifiNetPorts[0..1]` — `port,
  valid, flags, uartRunning (b0), bridging (b1), here (b2: this request came through this port), reports (b3),
  slip (b4), provisional (b5), usb (b6), baud, baudSaved, connected (0 nothing … 8 other framed traffic), rxBytes,
  txBytes, rxAge, txAge, rxBps, txBps, protocols[6] {name, fromPort, toPort, age}, sbpRequestsFrom, sbpContentFrom,
  sbpRequestsTo, sbpContentTo, moduleRequests, moduleFrames, rxOverflows, txDrops`; `wifiNetPortDevices[port]` — SBP
  `{kind 1, addr, board, name, fw, serial, age}` (name from the device list's own board names,
  `DevDriver::knownBoardName`), MAVLink `{kind 2, sysid, compid, heartbeat, mavType, autopilot, frames, age}`;
  `wifiNetPortPeers[port]` — `{valid, mode, state, peers[{ip, port, age}]}`; `wifiNetOwnPort` (the port with b2, -1 =
  the network). Ages in seconds, -1 = never. RX = from the port, TX = to the port. `rxBps`/`txBps` come from two
  successive summaries at least 1 s apart (`WifiByteRate`); a counter that went back or a gap over 10 s starts over
  (-1 until the next summary).
- **Ports island** (`DeviceSettingsPage.qml`, visible with v7): "connected through X1 / X2 / over the network", then
  two equal cards, side by side from 660 px (× scale), stacked below. Each card: X1/X2 with badges (this device,
  provisional, USB, SLIP, off), rate and saved rate, what is connected and whether it gets the module's reports, RX/TX
  per second with totals and the last RX age, frames per protocol, SBP counters, losses when there are any, the
  devices heard (page 1), the line's mode, state and peers (page 2), and a rate selector (9600 … 4 000 000) with a
  two-tap Apply. While the island is on screen `dev.wifiNetRefreshPorts()` asks every 2 s for page 0 of both ports
  and pages 1 and 2 of each. The old "Port speed" island shows only without v7.
- **Setting a rate**: `dev.wifiSetLineBaud(line, baud)` (replaces `wifiSetPortBaud`). The port this app is connected
  through — b2 of the latest summary; before 0.12 line 0 when the link is serial, as before — goes through
  `WifiPortChange` (§2c); 0.12 takes the status polls at the new rate as the confirmation, and `ID_FLASH` v0 saves it.
  The old rate for the roll-back is now the app link's own rate (link list); it used to be the `ID_UART` mirror, whose
  115200 default made the 921600 fallback dead code and could roll the link back to a rate it never had. The other
  port goes through `WifiLineChange`: v3 is read, the line's record is sent back with only the rate changed, and
  after the module's answer v3 and v7 are read back (before 0.12: line 1 only; the module ignores line 0's rate in v3).
  `wifiPortBusy`, `wifiPortTarget` (0 for a settings change) and `wifiPortLine` cover both sequences.
- **Rate in v3 for the requester's own line** (0.12, decided after review): the module ignores the `baud` field of a v3
  SET for the line of the port the request came through — a host changes its own port with `ID_UART` v0, and a
  read-back record would otherwise carry a stale rate. For the other line, or any line from the network, a new rate is
  applied and saved at once. So the app never uses v3 for its own port's rate, and a settings change of its own line
  simply sends the read-back record.
- **Line settings in the cards** (owner, 2026-09-27: "where do I set the UDP port of a connector"): X1 = line 0, X2 =
  line 1. The card's header reads `X2 · UDP 14445 · senders` (the local UDP port, 0 = the peer port, shown as the port
  in effect; `TCP → ip:port`; `line off`). Its "Network side" shows the line's state and peers (page 2), the counters
  to and from the network (v4) and the settings: mode, destination (UDP), peer IP (fixed or TCP), peer port, own UDP
  port, and on X2 the TX/RX pins. Apply (two taps) calls `dev.wifiSetLineConfig(line, edits)` with only the fields
  the operator changed; `WifiLineChange` reads v3, applies those fields over the fresh record (`WifiLineEdit`), sends
  it, and reads back v3 and v7. The end arrives as `wifiLineChangeFinished(line, outcome, resp, rate)`; a refusal is
  shown inside the card and keeps the edits. Two UDP lines cannot share a local port (the module answers ERR_PAYLOAD):
  the card says so, with the hint that swapping the ports of X1 and X2 needs one line moved to a free port first, and
  it refuses locally before sending when the other line already listens on that UDP port (compared as the port in
  effect, lport or else rport — Inference about the module's rule). The poll of the island also reads v3 and v4.
- **Module address** (v6) is a row of the Ports island (1–254). With v7 the Lines island is not shown at all (the
  cards replace it; its hidden instance still reports the answers to the page's other ID_WIFI_NET SETTINGs); on 0.10
  and 0.11 it is exactly as before.
- **Role-driven page** (owner, 2026-09-27): the RUNNING role (`ID_WIFI_NET` v0) decides what is shown. Station: the
  "Role" island keeps only the role selector (Apply and reboot / Save for next boot), the access point's name,
  password, security, channel, clients and the address/DHCP island are hidden. Access point: the Wi-Fi island shows
  only the access point's state (auto-connect, scan, networks, join and saved networks are hidden). A saved role that
  differs from the running one shows "Saved role: … — takes effect after a reboot". While the role is not known, and
  on firmware without `ID_WIFI_NET`, nothing is hidden by role (the ID_WIFI v0 access-point state still hides the
  station controls, as before). Consequence: the access point's settings can no longer be prepared in the station
  role; they appear after the reboot into it.
- **LR only for an access point** (0.12; owner: "I need to test LR across the whole chain"): the module takes protocols
  0x08 in the access-point role again (module-to-module links; 0.11 answers ERR_PAYLOAD). The Radio island offers LR in
  both roles; with LR selected in the access-point role it warns that phones and laptops cannot join, only Espressif
  stations with LR (another Kogger module set to LR or b/g/n+LR). An ERR_PAYLOAD to that request is shown as "This
  firmware refuses LR only for an access point (0.11); update the module to 0.12" (inline and as the notice). The
  station hint says the access point must be LR-capable (another Kogger module). The post-Apply radio check is unchanged.
- **Width follows the protocols** (owner's bench, 2026-09-27: LR picked with 40 MHz still selected went out as 0x08 +
  40 MHz, the module refused it, and the page blamed the firmware). 40 MHz needs 802.11n: with b, b/g or LR only the
  width is 20 MHz, the 40 MHz button is off (with the hint), and the operator's width comes back with b/g/n or
  b/g/n+LR; a width in effect without 802.11n does not overwrite that choice. `DevDriver::wifiSetRadio` (and the
  radio check's correction) sends 20 MHz for them whatever it is given (`WifiRadioRules::widthFor`, checked by
  mission_check). A refused radio setting reads "Firmware 0.11 refuses LR only for an access point; update the module
  to 0.12" only when LR only was asked of an access point and the module's firmware (ID_VERSION v2, `wifiLrApSupport`)
  is below 0.12; otherwise "The module refused these radio settings (ERR_PAYLOAD)". The module's version is 0.x, so the
  app's `fwVersion` text (set only for a major above 0) is empty for it: the rule reads the version fields directly.
- **Signal icon** (owner, 2026-09-27: "the Wi-Fi icon should show the signal level"): `WifiSignalIcon`
  (`qml/kqml_types/`, the dot and three arcs of `icons/ui/wifi.svg`) replaces the plain symbol on every Wi-Fi module's
  chip on the Devices page (`DeviceTopologyView`) and on its round badge in the main window's toolbar
  (`HotActionsPanel`, and its pop-out). `dev.wifiSignalLevel` comes from the ID_WIFI v1 report (the v0 status until
  one came): a station reports its access point's RSSI, an access point its weakest client's (0 = no clients).
  Levels (`WifiSignal::level`, the thresholds live only there, checked by mission_check): -1 no link — not connected,
  or an access point without clients — everything dim; 0 below -80 dBm only the dot, red; 1 from -80 dBm one arc,
  orange; 2 from -70 dBm two arcs, yellow-green; 3 from -60 dBm three arcs, green; unlit arcs dim. The colours are
  chosen in the component only (a darker set for the light theme). Every module has its own chip and its own badge
  (the topology lists each Wi-Fi module on a link as a bridge, the head unit's station and the boat's access point
  alike), so each shows its own module; the badge's tooltip adds the RSSI. Since 2026-09-29 (c0b96b10, owner: "the
  current signal level in digits, updated often") the RSSI in dBm stands under smaller arcs (`showRssi`); the module
  on this app's own port (v7 flag b2) is asked for a v1 report every 200 ms (`WifiSignal::kFastReportMs`, SETTING v1)
  once per connection and module start, a module reached through the relay keeps its second; a module that has sent
  nothing for 3 s shows no link (`DevDriver::dataStale`). `ID_FLASH` v0 saves the period with the port rates.
- **Log**: `wifi-net: port X1: flags 0x0f rate 3000000 saved 3000000 connected 4`, `wifi-net: port X2 devices: …`,
  `wifi-net: port X1 line: mode … state … peers …` when they change (not the counters); `wifi-port:` lines for both
  rate sequences.
- **Read this way where the contract leaves room** (Inference): page 2 `ip[4]` is a.b.c.d in byte order, like v3/v4;
  the per-protocol counters are shown as bare numbers ("units": frames, and whatever the module counts for raw); a
  page 1 with more than 8 entries is still read if its bytes are there.
- Verified by machine (2026-09-27): Windows build clean; `tools/mission_check --offline` checks the decoder byte by
  byte against the contract's tables, the byte rates, `WifiLineEdit` and `WifiLineChange` on made-up frames (70/70;
  a decoder with a wrong offset, the wrong flag bit or an edit applied to the wrong field fails them); qmllint shows
  nothing new; every `dev.*` name used by the page is declared in C++ (script). The page was rendered with a mock
  device in a scratch harness (X1 / X2 / network, wide and narrow, a change running, a refused line change, station /
  access point / pending role, 0.11 and pre-0.10 fallbacks). The owner ran the first round against a real 0.12 module
  (the cards showed the sonar on X2 as SBP device 0 at 15.6 KB/s). **Not verified by machine**: the line-settings
  Apply and the role-driven page against the real module, and the head unit.

## 2f. Firmware 0.13: port rates up to 5 000 000

- Contract (ESP32_WiFi `docs/SBP_WIFI.md`, `ID_UART`): either port takes any rate from 9600 to 5 000 000 from 0.13
  (up to 4 000 000 before), through `ID_UART` for the port a request comes through and through the port's line
  record (`ID_WIFI_NET` v3) for the other one. 5 Mbaud is the ESP32-C3 UART's top (exactly, from its 80 MHz
  clock); the contract marks it as not yet run on hardware.
- App: `WifiRates` (`src/device/wifi_ops.h`): `kMin` 9600, `kMax` 5 000 000, `maxFor(major, minor)` = 5 000 000
  from 0.13, 4 000 000 before and while the version is not known; `dev.wifiMaxBaud`. The X1/X2 cards offer 5M only
  when the module's firmware takes it (92df3e8e). The driver and the port classes take up to `kMax` and leave the
  limit to the module: a module that refuses `ID_UART` (its confirmation is an error) ends the port change at once —
  the confirmation comes at the old speed before the module switches, so this app's link has not moved (af439113;
  before, the link followed after 400 ms and came back only after about 6 s of "no answer").
- With 5M selected the card says both ends must do it: the head unit's UART6 was tried up to 4 000 000 only (it
  needs an 80 MHz clock for 5M, HeadUnit docs/UART_ESP32.md), and a USB adapter needs 5M in its data sheet. For the
  other port the module keeps the rate, so it has to be set back there if the device on it stops answering.
- `tools/mission_check` 112/112 (the version rule, 5M accepted, 6 000 000 refused, the refusal path).

## 2g. Firmware 0.15: a rate for this app's port is saved at once

Owner's decision in the module firmware (0.15): `ID_UART` through the same port saves the rate at once; there is no
return after 10 s any more, and only holding the module's BOOT button 5 s puts both ports back to 921600. The
v7 `provisional` flag is never set (so the card's "provisional… back in 10 s" note does not show).
- `WifiNetRules::rateSavedAtOnce` (0.15+), `dev.wifiRateSavedAtOnce`. WifiPortChange for such a module does not move
  the link back when the module gives no answer at the new rate: the module is there. The warning says the module
  keeps the rate, and how to recover (BOOT 5 s, then this link to 921600). 0.14 and older: unchanged.
- The X1/X2 card for this device's port says that the rate is kept at once and what a rate this device cannot do
  costs (the link until BOOT is held); the 5M note no longer promises that the link comes back by itself.
- `tools/mission_check`: the 0.15 path (no answer: the link stays at the new rate, nothing sent back) and the rule.

## 2h. Channels: the least busy one for the boat (2026-09-29)

Owner: scan the air and measure channel load, so that the freest channel can be picked on the boat.

**What the app does now (0b45230d), from a scan (ID_WIFI v2).** A scan hears access points by their beacons: SSID,
channel, RSSI. `WifiChannels` (`src/device/wifi_ops.h`) sums their power per channel 1..13, weighted by how much of
the rated channel's band each covers: a 20 MHz channel is flat over +-9 MHz (802.11 OFDM mask) and channels are 5 MHz
apart, so a network 1, 2, 3 channels off covers 14/18, 9/18, 4/18 of it, one 4 off only its skirt (0.01), 5 and more
nothing. Our own network (the one the module is on, flag bit1 of CONTENT v2) is left out. The least busy channel for
the access point is the lowest level among 1..11 (the module's AP takes 1..11); within 1 dB of it 1, 6 or 11 go first,
then the lower channel. The Wi-Fi island shows it under "Networks nearby" (a bar per channel with the count of
networks on it, the least busy channel, the channel the boat link is on and how to change it: the boat module's
"Role and access point" → Channel). Only a station scans: the module refuses v2 as an access point, so on the kiosk
it is the head unit's module that rates the air, at the head unit.

**Live (775143c1).** Owner: "not a history of channel use: the air as it is now". A switch in the view: while it is
on and the page is shown the module scans again 3 s after each scan ends (each scan 2-4 s, the boat link may stall
meanwhile, so it is off whenever the page opens); the view says how old its picture is ("updated N s ago", "from an
earlier scan"). A new scan no longer blanks the results: the last scan's networks stay until the new scan's first
result replaces them (`IDBinWifi::scanFresh_`). With the module's survey (0x59) Live will repeat the survey instead.

What a scan cannot tell: how much a channel is used (one busy network is worse than three idle ones), stations and
traffic of networks whose beacons are too weak, and anything that is not Wi-Fi except through what it does to
reception. mission_check (offline) checks the weights, our own network left out, the pick and bad entries.

**Asked of the module's firmware: a channel survey (the contract is the module project's).** Proposed 2026-09-29;
the module session answered the same day and asks the owner before it starts:

- Its own command, **0x59 `ID_WIFI_SURVEY`** (proposed): the version field of an SBP frame has 3 bits and ID_WIFI and
  ID_WIFI_NET use all eight versions. 0x59 is free in this app (src/proto_binnary.h: after ID_WIFI_NET 0x58 the next
  code in use is ID_NAV 0x64; checked by search).
- How: the module listens (promiscuous) while it dwells on each channel of a passive scan that returns to its own
  channel between channels (`home_chan_dwell_time`); an access point runs as APSTA for the survey, since IDF 5.5.5
  refuses `esp_wifi_set_channel` on an access point with clients. Whether its clients stay and whether promiscuous
  frames arrive during the dwells is to be tried on hardware (their Inference).
- What: per channel the airtime of the Wi-Fi frames heard (rate or MCS, length, preamble; frames failing the FCS
  count too), frames, other BSSIDs, the strongest RSSI and the noise floor (rx_ctrl.noise_floor of the C3, signed
  dBm) with a flag when the noise figure is not reliable. Not seen: interference that is not Wi-Fi (IDF has no public
  channel-busy (CCA) counter), so the app labels it "Wi-Fi airtime", not "load".
- Defaults: 100..120 ms a channel, channels 1..13 measured, 1..11 to pick from (country "01"), 2..3 s in all.

**The survey in the app (4bcfd365, firmware 0.16; the module's contract: its docs/SBP_WIFI.md 3b).** `IDBinWifiSurvey`
sends SETTING v0 {key, dwell 0 = the module's 120 ms, mask 0 = every channel} and collects one CONTENT v0 per channel
1..11 (14 bytes: channel, dwell, Wi-Fi airtime per mille, frames, transmitters, strongest RSSI (-128 nothing), noise,
flags bit0 own / bit1 the access point may use it / bit2 noise not to be believed). A survey late by
`WifiSurvey::timeoutMs` (11 x (dwell + 30 ms) + 6 s) is asked for once more (GETTING v0), then given up; a refusal is
named (ERR_RUNTIME: a survey, a scan or a join runs). Support by the firmware version (0.16+, `WifiSurvey::supportFor`).
The Channels view shows "Wi-Fi airtime" bars (the share above each, our channel marked), Measure, and Live repeats the
survey instead of the scan; it works in both roles, so the boat module's page gets it once that module has 0.16 (it
cannot be updated over the air). The channel picked (`WifiSurvey::best`): the least airtime among the channels the
access point may use; within 0.5 % our own channel first (no move for nothing), then 1/6/11; our own channel is
compared less 2 % for our own link (an Assumption: that share is not measured). The noise floor is shown only when the
module trusts it: on the ESP32-C3 it never does (-93 dBm on every channel, the module session's measurement). Log:
`[87] wifi-survey: Wi-Fi airtime 1:0.7% ... 6*:4.2% ...; best N by the mean of K`. mission_check checks the rules
offline.

**Longer and by the mean (owner, 2026-09-29).** The kiosk's first surveys at the module's default 120 ms a channel
jumped (one channel 16.7, 9.7, 0.0 % in a row; the pick 2, 7, 5). The app now asks 300 ms a channel
(`WifiSurvey::kDwellMs`, about 3.6 s a survey) and picks by the mean of the last surveys (`WifiSurveyWindow`: up to 5
not older than a minute, started over when the module's own channel changes); the bars show the last survey with a
tick at the mean. With the kiosk's three real surveys the mean picks 7 (checked offline).

**Our own network's airtime (firmware 0.17).** CONTENT v0 ends with U2 "own network airtime" per mille (16 bytes,
14 before): the part of the airtime in frames of the module's own BSS (its MAC as an access point, the access point's
BSSID as a station; its own transmissions are not heard, an access point hears its clients' uplink). The pick compares
the other networks' airtime only, on every channel (a neighbour hears our link too: channel 10 ours 6.0 %, channel 9
1.4 %, the module session's measurement); without the field (0.16) the old guess stays. The bars show the others'
share with ours on top in a lighter tone. From 0.17 the module also takes a firmware update from the network: the
app's Upgrade reaches the boat's module through the relay once that module has 0.17 (it has 0.15, one wired update
first); only the Wi-Fi password protects it then (the module project's SECURITY.md, UPDATE.md).

## 2i. The page's sections fold (2026-09-29)

Owner: the Wi-Fi page grew long; its sections fold, folded by default, the last state remembered. Each Wi-Fi island
of the device page (Wi-Fi, Boat link, Radio, Port speed, Ports, Role and access point, Access point address and DHCP,
Lines) folds to its title row; a tap on the title opens or closes it, with the triangle left of it as on "Advanced
settings". The open ones are kept in the app's settings (`deviceSettingsPage/openSections`, keys `wifi.net`,
`wifi.relay`, `wifi.radio`, `wifi.port`, `wifi.ports`, `wifi.role`, `wifi.ip`, `wifi.lines`), each on its own
(several may be open, not an accordion), the same for every module, across restarts.

Folded:
- the chips in the Wi-Fi and Boat link titles stay live (the Boat link stats keep being polled for its chip);
- Ports and Lines stop their 2 s polling, opening polls at once;
- Live turns off (it is for looking at the air);
- an edit left in a folded section still counts for "Apply and save (N)".

"Advanced settings" below keeps its own fold (per device, not across restarts).

Mechanism: `KIsland.folded` (the first visible row only, the rest clipped, so they take no clicks; the footer
hides), `DevIsland.foldKey`, `DevIslandTitle.foldIsland`. Check: `tools/qml_check/fold_check.ps1`, three
qmltestrunner starts over one settings file with the real components (folded by default, a folded row takes no
click, the title opens, each section on its own, remembered across starts); six mutants of the mechanism caught.

## 2j. One Actions, no baud rate for the module (2026-09-29)

Owner: why two "Actions" in Advanced settings, and what is "Set baudrate" doing there. The second one was upstream's
old Actions group left by the merge (HANDOFF.md); it is gone. The remaining Actions has no baud row on the module's
page (`DeviceParamCatalog`, `hideWhen: "isWifi"`): the ports' own sections (§2c, §2e) change a rate with the link
following, the answer read back and the rate saved, while that row sets the device and retunes the link its page's
device belongs to - for the boat's module behind the relay, the head unit's own port.

Nor behind the module (owner, 1a384c3f): on the head unit the boat's logger and sonar share the topology group of the head
unit's port with the module, so their row would send the rate to the far device and retune that port, saving it in
pinned_links.xml (the link is pinned). The page tells it from the topology (`behindModule`: a bridge, role 3, in the
device's group, the device not one); the catalog hides the row by it (`hideOnPage`), in Actions and in favourites
alike. A logger on a PC's USB and the sonars behind it keep the row as upstream has it (the owner's choice).

## 2c. Port speed of the module (line 0)

The module's line 0 runs at any speed from 9600 to 4 000 000 (`ID_UART` v0 `{KEY, 1, U4 baud}`, answered at the
old speed, then it switches; kept only after `ID_FLASH` v0). The Wi-Fi module's page has a "Port speed" island
(460k … 4M, firmware before 0.12; with 0.12 the Ports island of §2e) that calls `dev.wifiSetLineBaud(0, …)`; the sequence runs in `DevDriver` (`WifiPortChange`,
`src/device/wifi_ops.*`), so it finishes when the page is left mid-change: the app sets the module, moves its
own link (`LinkManagerWrapper::updateBaudrate`, which also rewrites the pinned record), asks for the status
until the module answers at the new speed (each check forgets frames from before it), and only then saves in
the module; no answer within about 6 s puts the link back. The island shows "Changing to …" meanwhile.
Steps are `wifi-port:` lines in the log. The head unit runs at 2 000 000 since 2026-09-26 (the boat stream is
~92 KB/s, a full 921600 port). The module's page has no "Set baudrate" in Actions any more (§2j).

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
6. Port speed (with 0.12+ the X1 card) 2 000 000 → the link follows, the module answers at the new rate, and the rate
   is saved (the owner went 2M → 3M → 4M → 2M on the head unit, 2026-09-26).
7. "Upgrade" with a `.ufww` of a higher minor version → new version after the reboot (not yet
   verified from the app).
8. Boat link: mode UDP, address 87, the boat's port → the module reappears as a device at address
   87 with counters, and the boat's logger appears as a device at address 0 on the same link.
   Verified 2026‑09‑25 with the boat at 10.0.0.10:14444 (UDP): about 35 datagrams and 110 frames
   per second come down with no drops.
9. The device chain reads Wi‑Fi → logger → sonar(s), the module first.

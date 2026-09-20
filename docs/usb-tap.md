# USB tap: IQ and RAW capture from the Baikal-sonar

The Baikal-sonar has a second CPU core that owns a USB CDC port of its own.
When the operator selects **IQ** or **RAW** in the echogram switch of the
device settings page, the sonar keeps ranging as before, sends no echogram
over the control link, and the second core streams diagnostic data over USB.
The app receives that stream with `UsbTap` (`src/device/usb_tap.*`), writes it
to a file in the log directory and shows a status line under the switch.

| Switch | Dataset mask bit | What the device publishes | Where it arrives |
| --- | --- | --- | --- |
| Off | — | rangefinder only | control link |
| 8-bit | 1 (`MASK_CHART_V0`) | echogram | control link |
| Compressed | 21 (`MASK_CHART_Z_V0`) | compressed echogram | control link |
| IQ | 22 (`MASK_TAP_IQ_V0`) | complex baseband of the two-pass receiver, one block per ping | USB port of the second core |
| RAW | 23 (`MASK_TAP_RAW_V0`) | raw ADC samples (firmware step 2, not emitted yet) | USB port of the second core |

The five positions are mutually exclusive in the switch; the bits themselves
are independent in the protocol.

## Finding the port

The second core enumerates as a CDC device with `VID 0x600D`, `PID 0x1705`
(the TinyUSB demonstration identifiers; the product string names the board).
`UsbTap` scans `QSerialPortInfo::availablePorts()` once a second while a tap
mode is selected, opens the first matching port and asserts DTR — the
firmware streams only while the port is open. Unplugging or a device restart
closes the port; the scan resumes.

## Wire format

Frames are KLF, the same envelope the control link uses:

```
BB 55 route(00) mode(01) id len payload c1 c2
```

`c1`/`c2` is Fletcher-8 over `route .. payload`. All integers are
little-endian. Three identifiers are used:

**ID_TAP_META (0x78), 44 bytes**, one per ping, before its data:

| offset | type | field |
| --- | --- | --- |
| 0 | u16 | seq — sequence number of the published ping |
| 2 | u16 | ver — layout version, 1 |
| 4 | u32 | ping_ms — start of the ping, ms since the sonar started |
| 8 | u32 | rate_hz — sample rate of one ADC; three ADCs interleave |
| 12 | u16 | decf — decimation factor of the baseband |
| 14 | u16 | freq_khz — carrier |
| 16 | u16 | resol_mm — echogram cell |
| 18 | u16 | nsamp — echogram cells |
| 20 | u16 | offset — first echogram cell |
| 22 | u16 | bb_len — complex samples in this ping |
| 24 | u32 | sound — sound speed, mm/s |
| 28 | u32 | dist_mm — rangefinder distance |
| 32 | u32 | pings — the sonar's ping counter |
| 36 | u32 | stack_free — untouched stack of the main core, bytes |
| 40 | u16 | pulse — periods in the transmitted pulse; 0 = passive |
| 42 | u16 | flags — bit 0 IQ requested, bit 1 RAW requested |

**ID_TAP_IQ (0x79), 8 + 8·n bytes**: `u16 seq`, `u16 first` (index of the
first sample), `u8 n` (samples in this frame, at most 30), `u8 fmt` (1 =
float32 pairs), `u16 total` (= bb_len), then `n` pairs of float32 `I, Q`.

**ID_TAP_STAT (0x7B), 29 bytes**, once a second: `u32 tick`, `u32 millis`,
`u32 frames`, `u32 torn` (pings abandoned because the sonar started to
overwrite the buffer), `u32 skipped` (pings not sent: port closed or host not
reading), `u32 last_seq`, `u32 stack_free`, `u8 state`.

The baseband rate is `3 · rate_hz / decf`; one sample spans
`sound / 2 / (3 · rate_hz / decf)` of range.

## Recorded file

`tap_YYYYMMDD_HHMMSS.tapiq` in the log directory, little-endian:

```
"TAPIQ1\0\0"  u32 version=1  u32 reserved
repeat: "PING"  u32 length  META[44]  float32 I,Q × bb_len
```

Only complete pings are written. A ping with missing frames — torn on the
device or lost in transit — is dropped and counted as *incomplete* in the
status line.

## Status line

`COM5: 123 pings, 41.4 KB/s, 0 incomplete; device: torn 0, skipped 15; core 0
stack free 7716 B` followed by the file path. *Skipped* counts pings the device
had ready while nobody was reading the port, so it grows before the app opens
it and stays put afterwards.

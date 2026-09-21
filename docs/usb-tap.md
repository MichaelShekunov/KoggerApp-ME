# USB tap: IQ and RAW capture from the Baikal-sonar

The Baikal-sonar has a second CPU core that owns a USB device of its own: a
CDC serial port and, since firmware 2.6, a CDC-NCM network adapter next to it.
When the operator selects **IQ** or **RAW** in the echogram switch of the
device settings page, the sonar keeps ranging as before, sends no echogram
over the control link, and the second core streams diagnostic data over USB —
as bytes on the serial port or as UDP datagrams over the network adapter,
whichever the host is listening on.
The app receives that stream with `UsbTap` (`src/device/usb_tap.*`), writes it
to a file in the log directory and shows a status line under the switch.

| Switch | Dataset mask bit | What the device publishes | Where it arrives |
| --- | --- | --- | --- |
| Off | — | rangefinder only | control link |
| 8-bit | 1 (`MASK_CHART_V0`) | echogram | control link |
| Compressed | 21 (`MASK_CHART_Z_V0`) | compressed echogram | control link |
| IQ | 22 (`MASK_TAP_IQ_V0`) | complex baseband of the streaming receiver, streamed while the listening window runs | USB port of the second core |
| RAW | 23 (`MASK_TAP_RAW_V0`) | raw ADC samples of the listening window, three ADCs interleaved, 12-bit, streamed while the window runs | USB port of the second core |

The five positions are mutually exclusive in the switch; the bits themselves
are independent in the protocol.

## Finding the port

The second core enumerates as a CDC device with `VID 0x600D`, `PID 0x1705`
(the TinyUSB demonstration identifiers; the product string names the board).
`UsbTap` scans `QSerialPortInfo::availablePorts()` once a second while a tap
mode is selected, opens the first matching port and asserts DTR — the
firmware streams only while the port is open. Unplugging or a device restart
closes the port; the scan resumes.

## Network path (CDC-NCM)

Since firmware 2.6 the second core is a composite device: the CDC port above
plus a CDC-NCM network adapter, which Windows 11 (inbox `UsbNcm`) and Linux
(`cdc_ncm`) bring up without drivers. The device is 192.168.7.1/24 and leases
192.168.7.2 to the host by DHCP. It streams to whoever last sent it a UDP
datagram on port 18020 and goes back to the serial port three seconds after
the last one.

Next to the serial scan, `UsbTap` looks once a second for a local IPv4 address
in 192.168.7.0/24. When it finds one it binds UDP port 18020 on that address
and sends `hello` to 192.168.7.1:18020 every second; the frames then arrive as
datagrams, each holding up to five whole 256-byte frames, and go through the
same parser as the serial bytes. Without the adapter nothing is sent, so no
probe leaves through the default route. With the adapter up the stream comes
over UDP even while the serial port is open as well, and the status line then
starts with `UDP 192.168.7.1`. The device sends its datagrams with a zero UDP
checksum (allowed for IPv4); Windows 11 accepts them. One listener at a time:
`tapdump.py --udp` from the firmware tools and the app compete for port 18020,
whoever bound it first gets the stream and the other retries every second.

Checked on Windows 11 only; Linux has not been tried. The app's UDP path was
built against the same frames the firmware tool `tapdump.py --udp` received
complete on the board; the GUI itself has not been run against the board.

## Setting the addresses

The device page has a **Network (USB)** group for the three addresses of that
link: the device's own, the subnet mask and the one it hands the host by DHCP,
plus a switch for whether to hand one out. **Apply** sends all three together,
because one without the others is meaningless, and the device validates them
again and keeps the old ones if the set cannot work. The change takes effect at
once, without a reboot; it lives only until the next one unless written with
**Flash settings** in Actions.

There is no port to set. The device streams to whatever port the host asked
from, and 18020 is fixed in the firmware because it is the way back: send the
same frame there, broadcast if need be, and the device answers broadcast with
its MAC and current addresses even when the subnets do not match. The firmware
tool `Sonar/tools/netcfg.py` does exactly that and can also put an address back
temporarily, without the serial link.

## Wire format

Frames are KLF, the same envelope the control link uses:

```
BB 55 route(00) mode(01) id len payload c1 c2
```

`c1`/`c2` is Fletcher-8 over `route .. payload`. All integers are
little-endian. Three identifiers are used:

**ID_TAP_META (0x78), 44 bytes**, one per ping, sent *after* the window's
IQ and RAW frames and closing them:

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
| 22 | u16 | bb_len — complex baseband samples in the listening window (the same number the IQ frames carry as `total`) |
| 24 | u32 | sound — sound speed, mm/s |
| 28 | u32 | dist_mm — rangefinder distance |
| 32 | u32 | pings — the sonar's ping counter |
| 36 | u32 | stack_free — untouched stack of the main core, bytes |
| 40 | u16 | pulse — periods in the transmitted pulse; 0 = passive |
| 42 | u16 | flags — bit 0 IQ requested, bit 1 RAW requested |

**ID_TAP_IQ (0x79), payload always 248 bytes**, sent *during* the listening
window: `u16 seq`, `u16 first` (index of the first sample in the window),
`u8 n` (samples in this frame, at most 30), `u8 fmt` (1 = float32 pairs),
`u16 total` (samples in the window; the sonar refines it when the window
ends, so the last frame is authoritative), then `n` pairs of float32 `I, Q`
and zero padding up to the fixed length. Size the data by `n`, never by the
payload length. The samples are the live baseband of the streaming receiver
taken from its ring while the window runs; nothing is buffered on the device
beyond that ring, so a window of any length can be tapped.

**ID_TAP_RAW (0x7A), payload always 248 bytes**, sent *during* the listening
window of a ping, before its META: `u16 seq`, `u32 first` (index of the first
sample in the window's interleaved stream), `u32 total` (samples in the window;
the sonar refines it when the window ends, so the last frame is authoritative),
`u8 n` (samples in this frame, at most 156), `u8 fmt`, then the samples in
ADC order `s0 s1 s2 s0 s1 s2 …`, then zero padding up to the fixed payload
length. Size the data by `n` and `fmt`, never by the payload length: a full
frame carries two padding bytes, the last frame of a window many more. The
fixed 256-byte frame keeps every USB transfer word-aligned, which the device's
DMA requires. `fmt` 1 is `uint16` per sample; `fmt` 2 packs two 12-bit samples
into three bytes: `b0 = a[7:0]`, `b1 = a[11:8] | b[3:0] << 4`, `b2 = b[11:4]`,
an odd tail being one sample in two bytes. The device sends RAW frames
straight to the bulk endpoint in batches of up to 31 frames; the channel
carries about 13 MB/s, the packed stream needs 4.2 MB/s (5.6 MB/s unpacked).
Missing indices are gaps: samples the device dropped because its buffer
filled, or frames lost in transit; the device counts its own drops in STAT.

**ID_TAP_STAT (0x7B), 37 bytes** (33 or 29 in older firmware), once a
second: `u32 tick`, `u32 millis`, `u32 frames`, `u32 torn` (unused since the
streaming receiver; kept for the layout), `u32 skipped` (pings not sent: port
closed or host not reading), `u32 last_seq`, `u32 stack_free`, `u8 state`,
`u32 raw_dropped` (raw samples the device dropped because the second core fell
behind the ADC rings), `u32 iq_dropped` (the same for baseband samples).

The baseband rate is `3 · rate_hz / decf`; one sample spans
`sound / 2 / (3 · rate_hz / decf)` of range. The raw stream runs at
`3 · rate_hz`.

## Recorded files

Both live in the log directory, little-endian.

`tap_YYYYMMDD_HHMMSS.tapiq`:

```
"TAPIQ1\0\0"  u32 version=1  u32 reserved
repeat: "PING"  u32 length  META[44]  float32 I,Q × bb_len
```

Only complete pings are written. A window closes on its META if every
sample has arrived, a quarter of a second after META otherwise, or when the
next window's first frame arrives; a window with missing samples — dropped on
the device or lost in transit — or without META is discarded and counted as
*incomplete* in the status line.

`tap_YYYYMMDD_HHMMSS.tapraw`:

```
"TAPRAW1\0"  u32 version=1  u32 reserved
repeat: "PING"  u32 length  META[44]  u32 total  u32 got  u32 ngaps
        ngaps × (u32 first, u32 count)  uint16 × total
```

Every window is written as received: `total` samples with the missing ones
zero and listed in the gap table; META is all zeros if it never arrived. A
window closes on its META (which the device sends after the last chunk), on
the next window's first frame, or a quarter of a second after META if chunks
were still trickling in.

## Status line

IQ: `COM5: 123 pings, 239.0 KB/s, 0 incomplete; device: torn 0, skipped 15,
dropped 0 samples; core 0 stack free 43888 B` followed by the file path. Over
the network the line starts with `UDP 192.168.7.1` instead of the port name.
When neither the port nor a 192.168.7.x adapter is found the line says so and
asks whether the second core is running.
*Skipped* counts pings the device had ready while nobody was reading the port,
so it grows before the app opens it and stays put afterwards; *dropped* counts
baseband samples the second core could not take from the ring in time. A
1500-cell window at 10 mm is 6297 baseband samples in 210 frames.

RAW: `COM5: 28 RAW windows, 2 with gaps, 265.0 KB/s; device dropped 31221
samples; core 0 stack free 7716 B` followed by the file path. A window of
1500 cells at 10 mm is 20 ms and 56 673 samples; 3000 cells is 40 ms and
112 230 samples, and both arrive complete. The first window after the mode is
switched, or after the window length changes, is usually partial: the device
picks the ADC ring up mid-window and counts what it skipped.

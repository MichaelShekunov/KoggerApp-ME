# KoggerApp on an Android tablet

What the fork does on a real Android device, measured rather than assumed. The
build itself is `build-android-cli.md`; this is what happens after install.

Tested on 2026-10-02 on a Xiaomi tablet, model 2410CRP4CG, Android 16, arm64-v8a,
Adreno GPU, screen 3200 x 2136. The package was the side-by-side build
(`org.kogger.koggerapp.myedition`) next to the official KoggerApp 1.0.3. The
tablet was on the boat's Wi-Fi (`KoggerBoat`, the access point of the boat's
Wi-Fi module); the boat was on land, autopilot in HOLD, disarmed. Every
check below watched the app and the boat only: nothing was armed, started,
applied, saved, recorded or flashed.

## What works

All of it seen on the tablet, most of it in the app's own log (logcat, tag
`default`):

- **Start and drawing.** The QML loads, the split 3D/echogram view draws on GLES,
  70-80 frames/s on the stall monitor's minute line. Turned to portrait, the
  layout restacks itself (Interface → "Rotate layout with device").
- **The link to the boat over Wi-Fi.** UDP discovery found the module and opened
  `UDP 10.0.0.10 · 14444 → 14444` with no setup; about 3.5 KB/s in,
  0.08 KB/s out.
- **The Wi-Fi island.** Module `[88]`, firmware 0.19, access point `KoggerBoat`
  channel 10, -67 dBm, radio power, DHCP pool, line and port rates (X1 5 Mbit/s,
  connected) — the same lines the head unit logs.
- **The boat.** ArduPilot found, mode, EKF and GPS state (3D, 19 satellites,
  HDOP 0.7), the mission read back ("The boat holds this plan (3 points)") and
  drawn in 3D with the home mark.
- **The device chain** in the top bar and in Settings: Wi-Fi → Recorder, with the
  module's signal on its chip.
- **The fork's echogram settings**, deconvolution among them.
- **The stall monitor**, now including in the background (see the fix below).
- **The top-left menu button.** It sits inside the 90 px band of the hidden status
  bar; a tap there opens the app's menu, not the system shade.
- **Opening a recording.** Settings → Files → open goes to Android's own file
  picker; a 43 MB side-scan KLF from Downloads opened, the echogram and the track
  drew, and "Recently opened" reopened it after the app had been reinstalled and
  restarted. Opening a file closes the live link, as on the desktop.
- **Deconvolution, on the CPU.** OpenCL is not there for apps ("OpenCL runtime not
  present"), so the threads take over. `tools/deconv_check` cross-compiled with the
  NDK and run on the tablet (2000 x 800, 8 iterations, the shipped default):
  1 thread 144 ms, 2 100 ms, 4 74 ms, 8 53 ms, auto 64 ms; its checks PASS. The PC
  does the same in 9 ms on threads and 6 ms on its GPU, so a full echogram pass is
  six to ten times dearer here: fine for a recording, heavy on live data, where
  every ping repaints the whole image.
- **Sonification.** On touch it follows the aim cursor, which comes after holding
  a finger still for 350 ms (a finger that moves at once scrolls the echogram -
  the head unit's field-test design); a plain tap and swipe gives silence, which is
  correct. Held and dragged over the recording: Android's AudioFlinger shows the
  app's track at 48 kHz stereo, a 768-frame buffer (16 ms), **0 underruns and
  37 ms latency** (the PC measured 35 ms); MIUI's own counter logged 5 s of signal,
  peak amplitude 15762 of 32767.

## What was broken, and is fixed

Each one found on the device, fixed, and checked again on the device.

### Two keyboards for one field

`ScreenKeyboard` in Auto opens the head unit's on-screen keyboard when the last
input was touch. On Android every input is touch, and Android raises its own
keyboard on focus, so a tap on a number field opened both: the fork's panel next
to the field and the system keyboard at the bottom. Now the panel never opens on
Android, and the "On-screen keyboard" choice is hidden there. Same field, same
tap afterwards: only the system keyboard.

### The link died with the screen

Pressed Home, the app kept running — the stall monitor's line came every minute —
but not one frame arrived from the boat, and on return the app logged
**"Link to the boat restored after 209.8 s"**, the whole time it had been away.
Android's own network policy says why:

```
UID=10379 ... blocked_state={blocked=APP_BACKGROUND,allowed=FOREGROUND|TOP|NOT_IN_BACKGROUND|...}
```

Android 16 cuts the network of an app in the background. A recording would have
lost everything from the moment the operator switched apps.

`KoggerLinkService` (platform/android/src/org/kogger/koggerapp/) is a foreground
service of type `connectedDevice`, started when the first link opens and stopped
when the last one closes (`Core`, `openLinkOrder_`). While it runs the app is in
the FGS state, the block is lifted (`effective=NONE` in the same dump), and the
service also holds a Wi-Fi lock and a partial wake lock. Same test afterwards:
3.40, 3.45 and 3.41 KB/s logged in the three background minutes, no "restored"
line on return.

On Android 13 and later the service's notification shows only if the app has the
notification permission, which it does not ask for; the service runs either way
and is listed in the system's active apps.

### "System" widget: no cores, no uptime, the wrong disks

Android keeps `/proc/stat` and `/proc/uptime` from apps, and mounts its read-only
system images first. The widget's log line read

```
system monitor: 0 cores (max MHz ), ... volumes / 0.7/0.7 GB, /system_ext 0.6/0.6 GB, /product 3.4/3.4 GB, uptime -1 s
```

and after the fix

```
system monitor: 8 cores (max MHz 1900 x3, 2611 x4, 2803 x1), ... volumes /data (app data) 44.7/225.0 GB, /storage/emulated 44.7/225.0 GB, uptime 2717835 s
```

The cores and their clocks come from sysfs when `/proc/stat` gives nothing, the
uptime from `CLOCK_BOOTTIME`, and on Android read-only volumes and FUSE views are
skipped (`/storage/emulated` was `/data` once more, `fuse` in `/proc/mounts`):
the line now reads `volumes /data (app data) 44.8/225.0 GB` alone. The
CPU load stays unknown — no public source left — and the widget already shows a
dash for that. Temperatures and memory were right from the start.

### The relief cloud was on by default, and covered the 3D view

The layer's store says off, but it restores before `Core::UILoad` finds the 3D
view, so `Core::setReliefCloudVisible(false)` returned without doing anything,
and a scene object starts visible. The cloud was therefore built on every file
load - about 40 rebuilds of 60 ms each for that recording, one every 500 pings - and drawn in the
ordinary 3D pane as a light-blue blob over the track. Now the cloud starts
hidden, `Core` keeps the requested state and applies it once the view exists,
and a rebuild, when the layer is on, waits until the last one is a second old
(a timer builds the rest when the growth pauses); `clearData()` also forgets the
old count, which kept a second file's cloud from building until it outgrew the
first. Same file afterwards: no rebuild, and the 3D pane shows the track. Switched on
in Settings (toolset Bottom Track or Maximum), the layer builds once, and a
reload of the same file rebuilds it **4 times instead of about 40** - at 500,
7142 and 13745 pings a second apart each, and once more after the load over all
19691 - with the same GUI stall as without the cloud.
This is the same code on the desktop (an inference; only the tablet was checked).

Seen on the way: from above, a cloud of one colour reads as a flat blob, with
nothing of the relief it carries. Colouring it by depth would show it; left as
it is, as a decision about the picture rather than a fix.

## Observed, not explained

- **Opening a 43 MB recording stops the GUI for 3.4-3.7 s.** The stall monitor
  catches it every time, with the relief cloud off as well as on: the file is
  read on the GUI thread (`KOGGER_SEPARATE_READING`, upstream's option for a
  reading thread, is off by default). Only measured on the tablet.

- **The echogram stayed black on the boat link.** `CHART` frames do arrive
  (about 8 a second through the module, `[88]`, and the recorder, `[250]`), but
  they are about 60 bytes each and no sonar became a device. The same link was
  not looked at from the PC or the head unit that night, so whether this is the
  boat's state or the Android build is open.
- **`throttled yes`.** On Android the vendor's power service lowers
  `scaling_max_freq` as a matter of course, so the "held below its maximum" mark
  is on most of the time. It is true, but it is not the warning it is on the
  head unit.
- **SELinux denials** for `getattr` on `/dev/pmsg0` from `qtMainLoopThread` in
  logcat. Harmless; where in the code they come from was not traced. (The other
  one, `read` on `/proc/loadavg` from the stall monitor, is gone: on Android that
  line now says `load -`, and the whole log holds no such denial.)

## Not tested on the device yet

The relief cloud in a 2/3D pane as such (it was checked as a layer in the 3D
pane), USB-OTG serial, and the screen actually turning off with the service
holding the locks (not tried: if the tablet has a PIN, a locked screen would have
ended the night's testing).

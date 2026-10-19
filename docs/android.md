# KoggerApp on an Android tablet

What the fork does on a real Android device, measured rather than assumed. The
build itself is `build-android-cli.md`; this is what happens after install.

Tested on 2026-10-02 on a Xiaomi tablet, model 2410CRP4CG, Android 16, arm64-v8a,
Adreno GPU, screen 3200 x 2136. The package was the side-by-side build
(`org.kogger.koggerapp.myedition`; `.me` since the rename of 2026-10-18) next to the official KoggerApp 1.0.3. The
tablet was on the boat's Wi-Fi (`KoggerBoat`, the access point of the boat's
Wi-Fi module); the boat was on land, autopilot in HOLD, disarmed. Every
check below watched the app and the boat only: nothing was armed, started,
applied, saved, recorded or flashed.

A second device on 2026-10-05: a Xiaomi phone, model 2201116TG, Android 13 (API 33),
arm64-v8a, beside the official KoggerApp. Installed with `adb install`; on the first start the app opens the
system "All files access" screen, and MIUI kills the cached process meanwhile, so the app starts afresh on the
way back — not a crash (no FATAL, no tombstone). Then `App is created`, the map and the echogram pane drawn.
That package is signed with the test key of the 2026-10-05 build PC, not the tablet's (`build-android-cli.md`).

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
  NDK and run on the tablet once (2000 x 800, 8 iterations, the shipped default):
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

Then a soak, read only: the app on the live link sent to the background at
22:16, the screen left to turn itself off (it did at 22:26), the app's log
streamed off the tablet for 70 minutes. The same process all along, a traffic
line every minute - 71 of them, no gap over 90 s, 3.18 to 3.64 KB/s - no "lost"
or "restored" line, no crash; RSS fell from 556 to 319 MB as Android trimmed the
backgrounded app. The screen-off case is therefore held by the service's Wi-Fi
and wake locks as well.

Left on the link for the night and sampled every 15 minutes up to 01:13: the same
process, 3.30-3.50 KB/s, still no "lost" or "restored" - about three hours on the
link, nearly all of it with the screen off. RSS rose from 340 to 408 MB in the
small hours and then fell back: at 14:46 the next day, 16.5 hours on the link in
the same process, it was 270 MB, so the night showed no leak. The only drop in
what the log still held was one of 3.1 s at 14:44 that morning ("Link to the boat
restored after 3.1 s"), with the boat's autopilot busy at the time; the hours
between 02:12 and the morning had already left the log's ring buffer.

One limit, by design of Android rather than of the code: from Android 12 on, a
foreground service cannot be started from the background. The service stops when
the last link closes, so a link that closed and opened again while the app was
off screen would run without it, and be cut. A UDP link here closes only when
told to (Close, opening a file), which the operator cannot do from the background;
discovery picking a new address for the module there is the one way it could
happen. Not seen, not tested.

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

### Swiped out of Recents, the process stayed without a window

The morning after the soak, the task was removed from Recents
(`removetask notifyTaskRemoved`), `KoggerActivity.onDestroy` ran (the service
stopped, `Multicast lock released`), and then Qt's own `onDestroy` never
returned: Android logged `Activity destroy timeout` 10 s later and did not kill the
process. Qt 6.8's `QtActivityBase.onDestroy` (read from `Qt6Android.jar`) calls
`QtNative.terminateQt()` and only then `System.exit(0)`; that wait is where it
stuck, after the GUI thread had spent 15 s in state D. The next launch got a new
activity in the old process and showed nothing but the splash window; only a
force stop brought the app back. The service kept the process alive long enough
for this - without it MIUI would most likely have killed it overnight.

Now `KoggerActivity`, when it is finishing for good, starts a watchdog before
Qt's `onDestroy`: if the process is still alive 3 s later it ends it, and the
next launch is a clean start. Checked on the tablet: the app runs with it, and a
plain swipe-up is ended by MIUI itself (`Killing ... SwipeUpClean`, 0.6 s). The
hang itself could not be produced on demand, so the watchdog has not been seen
firing.

After the cold start the ring was on the boat: Wi-Fi -> Recorder -> two sonars
behind it, `CHART_Z` from both at 20 frames/s (10.4 KB/s on the link), and the
echogram drawing - a thin band at zero range, the sonars being out of water on
land (an inference). So the black echogram of the first night was the boat
without its sonars, not Android.

## Observed, not explained

- **Opening a 43 MB recording stops the GUI for 3.4-3.7 s.** The stall monitor
  catches it every time, with the relief cloud off as well as on: the file is
  read on the GUI thread (`KOGGER_SEPARATE_READING`, upstream's option for a
  reading thread, is off by default). Only measured on the tablet.

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
pane), USB-OTG serial, recording a KLF on the tablet, and the boat's sonars, which
were not on the link that night.

"Share the map" (built 2026-10-18). Opening a KOGGER map from Telegram and WhatsApp
works on the 2026-10-05 phone (2026-10-18): files from other apps come in through
`KoggerOpenActivity`, because a messenger's open made a second `KoggerActivity` in the
messenger's task and Qt restarted the app; `KoggerActivity` is now `singleTask`. All in
`map-export.md`, "Android: from a messenger and back".

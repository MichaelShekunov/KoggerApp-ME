# Echogram sonification

Plays the echogram as sound, the way a waterfall spectrogram is played back: one
echogram column is one spectral frame, distance maps to pitch and sample
brightness maps to loudness.

Added in this fork; not present upstream.

## Using it

The speaker button sits in the control column on the echogram itself, between
the level control and the settings gear (`qml/scene2d/Plot2D.qml`, `settingsRow`).
It lights up in the accent colour while sound is on. The state is deliberately
not persisted — the application never starts making noise on its own.

The frequency scale is chosen in the echogram settings panel, next to the theme
and compensation combos ("Sound: log" / "Sound: linear"). That choice *is*
persisted, per plot, under `scene2d/plot2d/<indx>`.

If the button refuses to light up, the audio device could not be opened. Nothing
else in the application is affected.

## Mapping

| Echogram | Audio |
| --- | --- |
| near edge of the visible range | 7000 Hz |
| far edge of the visible range | 400 Hz |
| sample brightness 0..255 | oscillator amplitude 0..1 |

Near is high, far is low — closer echoes sound higher, which matches the way
depth is normally spoken about and the way a falling pitch reads as "going
down". The mapping ran the other way until 2026-08-10.

**That description holds for a single-channel echogram only.** On a dual-channel
one the visible window is signed and the column is laid out
`[far ch1 … nadir][nadir … far ch2]` — `Epoch::chartTo` writes the first half
with `reverse`, so bin 0 is the *far* edge of channel 1 and the nadir sits at the
seam. The ramp is therefore monotone across the whole port-to-starboard sweep:
far on one side is 7 kHz, the nadir is somewhere in the middle, far on the other
side is 400 Hz. Pitch does not distinguish the two sides, and "near is high" is
not what a side-scan operator hears. Backlog item 3 is where that gets resolved
properly; nothing has been changed on the assumption of what is wanted.

The endpoints are the *visible* distance window (`cursor.distance.from/to`), not
the device's full range, so zooming the echogram retunes the sound with it.

Two scales are available, both in terms of `t`, which is **1 at the near edge and
0 at the far edge**:

- logarithmic (default) — `f = 400 · (7000/400)^t`, about 4.1 octaves spread
  evenly across the range; hearing is logarithmic, so depth resolution stays
  audible near the surface.
- linear — `f = 400 + 6600·t`, frequency inversely proportional to distance.

Brightness is taken through the same window that `Plot2DEchogram::updateColors()`
applies to the displayed image, so moving the level slider changes what you see
and what you hear together.

## Which column is sounded

One button, two sources, picked automatically:

- **Live.** Every `Dataset::chartAdded` sounds the epoch that just arrived.
- **Cursor.** With no live column in the last 1.5 seconds, moving the mouse over
  the echogram sounds the column under the pointer, on every mouse-move event.
  Leaving the plot fades out.

The cursor path hangs off `qPlot2D::onCursorMoved`, which is what plain hovering
reaches. `plotMousePosition` is gated on a held mouse button in `Plot2D.qml`, so
hooking only that gives sound that updates just while dragging. `onCursorMoved`
does not touch `cursor_.mouseX/mouseY`, so the rotation into the horizontal frame
for a vertical plot is done at the call site.

Live wins while data is streaming; a recorded file falls through to the cursor
because nothing emits `chartAdded`.

The lookup reads `cursor.indexes` directly and does not apply the echogram's wrap
rotation, so with wrap enabled the sounded column can differ from the one drawn
under the pointer. That is pre-existing behaviour shared with the contact layer,
not something this feature introduces.

Sound is produced *before* `Plot2D::onCursorMoved` runs, not after. That call
ends in `plotUpdate()`, and the audio has nothing to gain by queueing behind a
repaint; the lookup depends on nothing the repaint changes.

## Interpolation

How many screen columns one epoch occupies depends on zoom. Sounding that epoch
unchanged across all of them is what made a sweep step instead of glide, and no
amount of amplitude smoothing fixes it — the targets really were constant, and
`sonifyEpoch` is a pure function of the epoch, so recomputing it changes nothing.

The cursor path therefore blends the two neighbouring epochs. For the column
under the pointer it finds the run of columns sharing that epoch, takes the run's
centre, and interpolates linearly toward the centre of the run on whichever side
the pointer leans:

```
u = (column - centre) / (neighbourCentre - centre)     clamped to 0..1
target = (1 - u) * column(epoch) + u * column(neighbourEpoch)
```

Interpolating centre to centre rather than edge to edge is what makes it
continuous: the weight moves by exactly `1 / (neighbourCentre - centre)` per
column and matches on both sides of the seam, so crossing between two epochs
produces no jump. At `u = 0` — pointer on a run's centre, or no usable neighbour —
the epoch is sounded unblended, so the live path is unaffected.

The cost is a second `Epoch::chartTo` per mouse-move. De-duplication is now on
the *column* rather than the epoch, since the sound changes within a run.

## Signal chain

`src/audio/echogram_sonifier.{h,cpp}`.

A column is resampled to 256 bins by `Epoch::chartTo` — the same call that fills
the echogram image, so the audio resolution does not depend on window height. A
dual-channel echogram is split near/far exactly as `Plot2DEchogram::updateCash`
splits it.

Each bin drives one sine oscillator from a 4096-entry lookup table. Amplitudes
approach their targets through a one-pole filter with an 8 ms time constant:
long enough that a new column does not click, short enough that a fast pointer
sweep is not smeared. It only has to take the edge off, because the cursor path
already hands it targets that change gradually — see "Interpolation" below. Bins that are silent
and staying silent are skipped entirely, so a mostly dark echogram costs almost
nothing. The sum is scaled by `1/sqrt(256)` and passed through `tanh` as a soft
limiter.

Oscillator phase and amplitude belong to the audio thread and are touched only
inside `readData`. Everything a caller can change — targets, frequency
increments, gain — crosses a mutex, copied once per 64-frame block (1.3 ms at
48 kHz) rather than per sample. Per *block*, not per call: see below.

The sink asks for 48 kHz mono float and falls back to the device's preferred
format, handling `Float` and `Int16` output and duplicating mono across channels.
Frequencies are clamped to 45 % of the sample rate so a low-rate device cannot
alias.

## Latency

Pointer-follow is only usable if the sound changes while the pointer is still
moving, and two separate things stand between `pushColumn` and the speaker.

**The sink's own queue.** `QAudioSink` defaults to a 500 ms buffer — measured
96000 bytes at 48 kHz mono float, with the sound running 419 ms ahead of the
wall clock. Every column was heard nearly half a second late, which is what made
the first version feel broken rather than merely coarse. `setActive` now calls
`setBufferSize` for `kTargetLatencyMs` (20 ms) before `start()`; the call is
ignored afterwards. Backends clamp upward, so the constant is a request for
"your minimum", not a figure that has to be exact — Windows/FFmpeg grants 22 ms.

**The block size inside `readData`.** Qt asks for up to 16384 bytes (85 ms) at a
time. Reading the targets once per call would put that whole span between a new
column and the sound even with a small sink buffer, so they are re-read every
`kControlBlockFrames` frames instead. That is why `bytesAvailable()` also
reports one `kTargetLatencyMs` chunk rather than a flat 65536: it caps how much
Qt asks for in one go, and 65536 invited 341 ms chunks.

Measured on Windows/FFmpeg, 48 kHz mono float:

| | sink buffer | sound running ahead |
| --- | --- | --- |
| before | 500.0 ms | 419 ms |
| after | 22.0 ms | 35 ms |

`sonifier_check` prints the sink buffer and fails above 100 ms, so a dropped
`setBufferSize` cannot regress this silently.

## Verifying it

`tools/sonifier_check` is a standalone Qt console program that opens the default
audio device, sweeps a single bright band from the near edge to the far edge over
three seconds, and asserts three things: that the frequency ramp descends monotonically from
`kFreqMax` at the near edge to `kFreqMin` at the far edge; that the sink stayed
in `QAudio::ActiveState` with `QAudio::NoError` — that is, that Qt really pulled
the generated samples; and that the sink buffer is at most 100 ms, which is what
keeps pointer-follow responsive.

The ramp check runs before the audio device is opened, so a reversed ramp is
reported even when the device then fails to open (that failure is still its own
non-zero exit). It exists because the direction is exactly the kind of detail an
upstream merge can reverse without anything else noticing.

```
cmake -S tools/sonifier_check -B build/sonifier_check -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=D:/Qt/6.8.3/llvm-mingw_64 -DCMAKE_CXX_COMPILER=D:/Qt/Tools/llvm-mingw1706_64/bin/clang++.exe
cmake --build build/sonifier_check
build/sonifier_check/sonifier_check.exe          # --linear for the other scale
```

Exit code 0 is a pass. It is also audible: the pitch must fall smoothly from
7 kHz to 400 Hz as the band travels away. Set `QT_ASSUME_STDERR_HAS_CONSOLE=1` to
see the sample rate, sink buffer and sink state it prints.

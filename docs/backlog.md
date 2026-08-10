# Backlog

Ideas accepted but not started. Each entry records what was asked for, what the
code already provides, and what the first real decision will be — so picking one
up does not begin with re-reading the whole tree.

Nothing here is committed to. An entry is a starting point, not a design.

## 1. Radar-style persistence on a polar view

Asked for 2026-08-10.

The newest echogram data draws at full brightness and older data decays toward
the background, the way a phosphor PPI display on an old radar fades between
sweeps. Intended for a **polar** rendering of the mosaic, not for the existing
rectangular echogram.

### What exists today

The mosaic itself is real and has a processing chain behind it:
`src/data_processor/mosaic_processor.cpp`, `mosaic_db.cpp`,
`src/mosaic_index_provider.cpp`, with its UI in
`src/scene3d/controllers/mosaic_view_control_menu_controller.cpp` and
`qml/scene3d/Scene3DLayerControl.qml`. It is drawn as part of the 3D scene.

**There is no polar or PPI view.** A repo-wide search for `polar` finds only
`UniPolar` in `src/xtf_conf.h` (XTF signal polarity) and freetype's
trigonometry helpers. Nothing renders a sweep in polar coordinates.

So this is two pieces of work of very different size, and they should not be
conflated:

1. A polar rendering mode for the mosaic — the large piece, and the one that
   decides everything else.
2. A per-cell age term feeding brightness — small once (1) exists.

### The decision that comes first

Where age lives. Two shapes, and they are not equivalent:

- **Decay in the renderer.** Each cell keeps a last-updated timestamp; brightness
  is a function of `now - updated` evaluated at draw time. Fade is continuous and
  free to retune, costs a timestamp per cell, and the display keeps changing
  while no data arrives — which means repainting on a clock rather than on data.
- **Decay baked into the stored value.** Cells are multiplied down as sweeps
  pass. Cheap to draw and needs no timestamps, but it is lossy: the decay rate is
  fixed at write time, cannot be changed after the fact, and destroys the
  underlying intensity, which matters if the same mosaic data is also used for
  anything quantitative.

The first is almost certainly right for a display feature, but it forces the
question of what drives repaints when nothing is arriving — worth settling before
any code.

### Open questions

- Does the fade follow wall-clock time or sweep count? They differ whenever the
  vessel slows, stops, or the ping rate changes.
- Does a re-observed cell reset to full brightness, or accumulate?
- Is the polar view a new pane, or a mode of the existing 3D mosaic layer?

## 2. Deconvolution against the beam pattern, using navigation and attitude

Asked for 2026-08-10.

Sharpen the imagery by deconvolving the sonar's directivity out of it, in the
spirit of a synthetic aperture: consecutive pings observe the same patch of
bottom from known, slightly different positions and orientations, so the beam's
smearing can in principle be undone.

### What exists today

The per-ping inputs are already there, which is the hard part of the plumbing:

- **Attitude.** `Epoch::_attitude` carries `yaw/pitch/roll` with `isAttAvail()`,
  plus an `artificalAttitude_` fallback and a `DataType` recording provenance
  (`src/epoch.h`).
- **Position.** GNSS `lat/lon`, an external position, and a separate
  `sonarPosition_`, each with its own `DataType`.
- **Complex signal.** `setComplexF()`, `complexSignals()` and
  `isComplexSignalAvail()` exist, so I/Q is representable per channel and group.

What does not exist is any model of the sonar's directivity. A search over `src/`
for `directivity`, `beamPattern`, `beamwidth` and friends finds exactly one hit —
`float BeamWidth` in `src/xtf_conf.h` — and that is a field of the XTF file
format, not a description of a Kogger device.

### Before any of this: where the beam pattern is allowed to live

**KoggerApp is open source. Anything committed here is published.** A sonar's
directivity pattern is device characterisation — the sort of thing that comes out
of firmware, a calibration rig, or an internal acoustic model — and firmware
source and anything derived from it, algorithms and calibration data included,
must not be reproduced outside the firmware repositories.

So the coefficients cannot be typed into this repository, and they cannot be
copied in from the simulation projects either. The pattern has to *arrive at
runtime*: queried from the device, or read from a data file that ships separately
and is not part of this tree. That constraint shapes the design rather than
following from it, which is why it is written here first — a parametric model
with the numbers baked in is the obvious implementation and the wrong one.

Worth settling with the firmware side before design: whether the pattern is
something a device can report at all, and in what form.

### The fork that decides what is actually achievable

Whether the processing is **coherent** or not, and the two are not variations of
one feature:

- **Coherent**, using `complexSignals()`. This is the only path on which the word
  "synthetic aperture" means what it usually means, and the only one that can
  genuinely beat the physical aperture. It needs phase to survive between pings,
  which puts hard demands on navigation accuracy — sub-wavelength, in practice —
  and therefore on what the attitude and position sources can actually deliver.
- **Incoherent**, on the 8-bit `Echogram::amplitude` envelope. Deconvolving the
  beam footprint out of a magnitude image. Far more forgiving of navigation error
  and applicable to recorded data that never carried I/Q, but it is
  resolution *enhancement*, not aperture synthesis, and it will not do what a
  reader of the word "SAS" expects.

Which one is meant should be decided before anything is designed, because the
answer determines whether ordinary GNSS is adequate or hopeless.

### Open questions

- Do the devices in use actually populate the complex path, or is
  `isComplexSignalAvail()` false in practice for the hardware this targets?
- What is the real positioning accuracy available per ping, and does it come from
  GNSS, from `sonarPosition_`, or from a dead-reckoning source?
- Is the target a live view or a post-processing pass over a recording? Coherent
  processing over a stored file is a very different engineering problem from
  doing it at ping rate.

## 3. A 3D audio scene instead of an echogram

Asked for 2026-08-10.

Stop encoding range as pitch and place the reflections around the listener
instead: a near target sounds near, a far one far, and port and starboard
side-scan arrive from the left and the right. In headphones, listening to the
water rather than reading it.

This is the natural successor to the current sonifier, not a variation of it.

### What exists today

- **The sonifier**, `src/audio/echogram_sonifier.{h,cpp}`: 256 sine oscillators,
  mono, straight into a `QAudioSink` at ~22 ms latency. Measured cost with every
  bin sounding is 1.7 % of one core.
- **The geometry is already in the column.** For a dual-channel echogram
  `fillEpochColumn` lays the bins out `[far ch1 … nadir][nadir … far ch2]`, with
  the seam at `part1`. Left and right are not something that has to be
  reconstructed — the split is there, and it is currently being flattened into
  one pitch ramp.
- **Qt ships the renderer.** `QtSpatialAudio` is present in the 6.8.3
  `llvm-mingw_64` kit — `QAudioEngine`, `QSpatialSound`, `QAudioListener`,
  `QAudioRoom`, headers, cmake config and `Qt6SpatialAudio.dll`. It comes with
  `qtmultimedia`, which this fork already links, so it adds no new dependency
  and `windeployqt` will carry it.

### What this fixes on the way

The pitch ramp is monotone across the *whole* signed window, so on a dual-channel
echogram far-port is 7 kHz, the nadir lands in the middle, and far-starboard is
400 Hz. Pitch cannot tell the sides apart, and "near is high" is not what an
operator hears. Putting the sides in space is the principled answer to that; see
`echogram-sonification.md`, "Mapping".

### The two things that will decide the design

**Source count.** A spatial engine positions tens of sources, not 256 — each one
carries an HRTF convolution rather than a table lookup, so the cost per source is
nothing like the 6.6 µs/s/bin measured for a bare oscillator. Some reduction is
unavoidable: cluster bins into a handful of angular sectors, or position a small
number of band-limited sources and keep the fine structure spectral. Measure one
`QSpatialSound` before designing around any particular number — the existing
`tools/sonifier_check` is the pattern for that.

**What the freed dimensions carry.** Once space carries side and range, pitch and
loudness are no longer spoken for, and there are now more display dimensions than
there is data to fill them. Loudness in particular is contended: it is the
current brightness cue *and* the strongest distance cue there is. Decide what
brightness maps to before anything else, because everything downstream follows
from it.

Worth knowing going in: distance in headphones is a weak percept, carried mostly
by loudness, direct-to-reverb ratio and high-frequency rolloff, and a generic
HRTF gives good left/right but poor elevation and unreliable front/back. Left
versus right — the side-scan case — is the part binaural audio does well. Height
and distance will need help from `QAudioRoom` or from deliberate cues, and are
worth prototyping before being promised.

## 4. "Remove the water"

Asked for 2026-08-10.

A display mode that shows stable surfaces and objects and suppresses the ordinary
water background. Explicitly **not required to be physically correct** — an
alternative way of looking, not a measurement.

That relaxation is the most useful thing in the request and should be kept:
it licenses a heuristic, and it says the result is a view, not a new truth.

### What exists today

Half of this is already in the tree, on the wrong axis.

`Epoch::Echogram::compensateInPlace` and `updateCompesated` (`src/epoch.h`) run a
one-pole mean *along range within a single ping*, subtract 55 % of it, and apply
a range-dependent gain. That is background subtraction already. It is selected as
a display mode through `imageType` in `chartTo` and reaches the plot as
`Plot2D::getEchogramCompensation()`, so the precedent for "another way of drawing
the same data" is established and has a place to plug into.

Also present: per-echogram `tgc` with `gTgcGainNear` / `gTgcGainFar`, and bottom
detection in `src/data_processor/bottom_track_processor.cpp`.

### The axis that is missing is time

"Stable" is a statement about consecutive pings, and nothing in the compensation
path looks across them. Water-column scatter decorrelates from ping to ping;
bottom and real objects persist. That difference — not brightness, not range — is
the signal this feature is actually built on, and it is exactly the axis the
existing compensation does not have.

Which raises the dependency that decides the scope:

- **Stationary or slow**, ping-to-ping in raw range bins. Cheap, needs no
  navigation, and is wrong the moment the vessel moves at any speed.
- **Moving**, which requires knowing that this ping's cell is the same patch of
  bottom as the last one's — i.e. the navigation and attitude plumbing of item 2,
  or the existing mosaic grid (`src/data_processor/mosaic_processor.cpp`,
  `mosaic_db.cpp`) as the place where accumulation happens.

Deciding between these two is deciding whether this is a small feature or a large
one.

### Shared machinery with item 1

Items 1 and 4 are duals and should not each grow their own history buffer. Item 1
wants per-cell age in order to *fade* what is old; item 4 wants per-cell temporal
consistency in order to *select* what is persistent. Both need the same thing:
somewhere that remembers what a cell looked like over the last N observations. If
both are built, build that once.

The same discipline as item 1 applies too — accumulate alongside the data, never
overwrite it. A mode that destroys amplitude cannot be turned off.

### Open questions

- What counts as stable: presence above threshold across N pings, low variance,
  correlation between consecutive pings? Each has a different failure mode, and
  the cheapest to compute is the easiest to fool by a bright transient.
- Does the mode suppress the water or merely dim it? Full suppression is more
  striking and hides the fish, the wake and the thermocline, which are sometimes
  the point.
- Is it a third `imageType` alongside raw and compensated, or a layer over the
  top of them?

## 5. Echo cloud

Asked for 2026-08-10.

Every sample becomes a particle in 3D: its coordinate from platform position plus
range, its size or brightness from amplitude. The camera can fly inside the cloud
of reflections. Aimed at side-scan and scanning sonar, where it should look
unlike anything the rectangular echogram shows.

### What exists today

The primitive is already there. `BottomTrack::setData` takes a
`QVector<QVector3D>` with `primitiveType` defaulting to `GL_POINTS` and draws it
through `glDrawArrays(m_primitiveType, 0, m_data.size())`
(`src/scene3d/domain/bottom_track.{h,cpp}`), and `point_group` / `point_object`
sit alongside it. A cloud is not a new kind of object for this renderer.

What is new is the count. The bottom track carries roughly one point per ping;
this carries one per *sample*, so it is larger by the number of range bins — and
then multiplied again by every ping in a recording. Nothing in `src/scene3d` is
built for that: there is no instancing, no point-sprite shader among the files in
`shaders/`, and no level-of-detail or spatial culling anywhere. Sizing this
honestly, against a real recording, is the first task and probably decides the
design.

### Side-scan and scanning sonar are not the same problem here

They were named together in the request, but only one of them yields a point.

- **Scanning sonar** gives a bearing per ping. Bearing plus range plus attitude
  is an unambiguous position, give or take the beam's vertical width. The cloud
  is a faithful picture of where the energy came from.
- **Side-scan** does not. A sample at range *R* lies somewhere on an arc — every
  direction within the fan that is *R* away. Across-track position is recoverable
  only by assuming a bottom, normally a flat one.

Rendering side-scan samples as points therefore bakes in the flat-bottom
assumption, and does it invisibly. In a fly-through that matters more than on a
2D waterfall: the cloud will look like measured geometry while partly being an
assumption, and it will be wrong exactly where the bottom is interesting.

Worth considering that drawing the ambiguity honestly — each sample as its arc,
letting overlapping pings intersect into structure — may be both more truthful
*and* the more unusual image the idea is reaching for.

### Depends on the others

This is the display end of a chain rather than a standalone feature:

- **Item 2** supplies the geometry. Position and attitude per ping are what turn
  a range into a coordinate at all; without them the cloud is in sensor
  coordinates and smears as the vessel moves.
- **Item 4** supplies the selection, and is close to a precondition. Most samples
  are water background, so an unfiltered cloud is both unaffordable and unreadable
  — a fog with the interesting 1 % buried inside it. "Remove the water" is what
  makes a cloud worth flying through, and it also cuts the particle count by
  whatever the threshold rejects.

Doing 5 before 4 means building a renderer for data that mostly should not be
drawn.

### Open questions

- Live, or post-processing over a recording? A live cloud has a bounded window;
  a whole-survey cloud is an out-of-core problem.
- Is amplitude mapped to size, to brightness, to opacity, or to several at once?
  Additive blending of many faint particles reads very differently from opaque
  sprites, and the choice changes what the eye picks out of the volume.
- Does the cloud persist for the whole session or decay behind the vessel? If it
  decays, that is item 1's machinery again.

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

Asked for 2026-08-10. **The incoherent half is built** — see
[echogram-deconvolution.md](echogram-deconvolution.md). What remains open here is
the coherent path, attitude compensation, and per-column kernel sizing; the rest
of this entry is kept because it still frames those.

The confidentiality question resolved itself the way the entry hoped: the beam
width is typed in by the operator, so no pattern is stored in this repository.

Worth carrying forward: the measured limit. Deconvolution restores almost all of
a one-column smear and roughly a third of a 2.4-column one, and past about three
columns the information is gone rather than attenuated. Anything in this list
that expects sharpening to rescue the far edge of a wide beam should be costed
against that table, not against hope.

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

## 6. Interface lock

Asked for 2026-08-10 (raised as item 7 in the request sequence).

A padlock icon that has to be held for two seconds to lock the interface, and
held again to unlock it. Guards against stray input — a deck, gloves, spray and a
moving boat.

Much the smallest item here, and the only one that is purely UI.

### What exists today

- **Both icons are already drawn.** `resources/icons/ui/lock.svg` and
  `lock-open.svg` are on disk. **Neither is in `resources/icons.qrc`**, so
  registering them is step one — a file on disk is not in the resource system,
  and this fork has already lost time to exactly that (see HANDOFF, "Adding an
  icon means editing `resources/icons.qrc`").
- **The control pattern.** `KCircleIconButton` with `iconTintColor` and
  `fillColor` driven from a state bool is how the sonification speaker button is
  built (`qml/scene2d/Plot2D.qml`, `settingsRow`).
- **Hold-to-act.** `longPressTimer` in `Plot2D.qml` already does press-and-hold,
  at 500 ms, but it is gated on `Qt.platform.os === "android"`.
- **Input gating**, of a sort. `Plot2D.externalInputRouting` disables a plot's
  `MouseArea` and is set from `WorkspaceView.qml`. It is per-plot and exists for
  routing, not for locking — a precedent for the mechanism, not a foundation.

### The part that will be got wrong

**A lock that disables `MouseArea`s is not locked.** Keyboard input arrives by a
different road — `src/hotkeys_manager.cpp`, plus the `StandardKey` shortcuts in
`MainWindow.qml` and the page-up/page-down bindings — and none of it passes
through a `MouseArea`. Whatever gate is chosen has to cover pointer, wheel, touch
*and* key paths, or the lock will look convincing and stop nothing. Deciding
where that single gate lives is the whole design; the icon is an afternoon.

Two more that follow from it: the lock control itself must stay live, or there is
no way back; and if any action must remain reachable while locked — recording
start/stop is the obvious candidate — that has to be settled before the gate is
written, not carved out afterwards.

### Smaller decisions

- **Show the hold.** Two seconds with no feedback reads as a dead button; users
  will let go at one second and conclude it is broken. A ring filling around the
  icon, both directions.
- **Do not start locked.** The sonifier deliberately does not persist its on
  state so the application never starts making noise by itself; the same argument
  is stronger here, because a user who does not know the gesture cannot recover
  without being told. If it must persist, the unlock affordance has to be
  self-explanatory on a cold start.

### Open questions

- Two seconds each way? The two directions are not worth the same: an accidental
  *lock* is a nuisance, an accidental *unlock* defeats the feature. A slow unlock
  with a quicker lock may fit the purpose better.
- Does the lock cover the whole application, or one plot / one pane? The existing
  input gate is per-plot, and the answer decides whether this is a `Plot2D`
  property or something owned by `Core`.
- Should locking also suppress the settings panels and dialogs, or only the
  chart interactions?

## 7. Voice commands

Asked for 2026-08-10 (raised as item 8 in the request sequence).

The operator says "частота 500" and the application sets the sonar's transmit
frequency to 500 kHz, then confirms out loud.

### The two halves are not equally hard

**Speaking is solved.** `qtspeech` is available for the `win64_llvm_mingw` kit —
confirmed against `aqt list-qt --modules 6.8.3 win64_llvm_mingw` — so
`QTextToSpeech` costs one more module in the install line, one `find_package`
component, and nothing else. It is not in the current kit; it would have to be
added.

**Listening is not.** Qt has no speech recognition, in this kit or in any module
Qt ships. That half needs an external engine, and it must be **offline**: a boat
has no connectivity to depend on, and streaming deck audio to a cloud service is
a question nobody wants to answer. Offline means a model file, in Russian, and
the portable package is 139 MB today. This is the first dependency in this fork
that would not come from Qt, and it should be chosen deliberately rather than by
whichever demo works first.

### What exists to command

The path is already there and already public: `DevDriver::setTransFreq(int freq)`
and `sendTranscSetup(const ChannelId&, uint16_t freq, uint8_t pulse, uint8_t boost)`
in `src/device/dev_driver.h`. Nothing confidential has to be added to drive a
sonar from this application — the protocol is in the open-source tree already.

Note the signature: the command is *per channel*. "Частота 500" does not say which
one. On a multi-channel device the utterance is incomplete, and the resolution —
active channel, all channels, or an obligatory channel word — has to be decided.

### The thing that makes this risky rather than fun

**A misrecognition writes to the hardware.** "Частота 500" heard as "частота 100"
changes the sonar's operating point, on a deck, with engine noise and wind, from
a speaker who may be shouting.

Confirming *afterwards* — which is how the request is phrased — narrates the
mistake rather than preventing it. The aviation pattern is read-back before
execution: the application says "частота 500, подтвердите", and acts on "да".
That costs a second per command and removes almost the whole class. Whether every
command deserves it or only the transmit parameters is a real decision; changing
a display range is not changing what the transducer does.

Two levers matter more than engine choice:

- **A closed grammar, not dictation.** A fixed vocabulary of command words plus
  numbers is dramatically more robust in noise than open transcription, and it can
  be checked against what the device will actually accept before anything is sent.
  `freq` is a `uint16_t`; range-check before transmit, not after.
- **Push-to-talk, not always listening.** An open microphone on a working deck
  will fire on crew conversation. A held key or button also gives the operator an
  unambiguous way to not be heard.

### Interactions with the rest of this list

- **Audio contention.** The sonifier already owns an output stream. Speech output
  has to mix with it or duck it, and if item 3 ever happens the operator is
  wearing headphones full of water noise — the confirmation has to cut through
  that deliberately, not compete with it. Microphone input alongside speaker
  output on the same device also invites the application hearing itself.
- **Item 6.** If the interface is locked, is the voice channel still live?
  Answering "yes" makes voice the way to work with a locked screen, which is
  arguably the best reason for either feature to exist. Answering "no" is also
  defensible. It should be answered on purpose.

### Open questions

- Which engine, and what does its Russian model cost in package size and in
  startup time?
- Is the confirmation a spoken read-back, a spoken acknowledgement, or both?
- What is the vocabulary? Writing the full command list down first will show
  whether this is ten utterances or a hundred.

## 8. OptiX for the GPU work

Raised 2026-08-10, explicitly for later.

The OpenCL backend added for the deconvolution is compute-shaped: buffers in,
arithmetic, buffers out. OptiX is ray-tracing-shaped, and the interesting
question is which backlog items are actually ray problems rather than array
problems.

Item 5, the echo cloud, is the obvious candidate — placing samples in space and
asking what a camera sees is literally ray traversal, and the side-scan arc
ambiguity described there is a geometry query. Item 2's coherent path is not:
it is dense arithmetic over regular arrays, which is what OpenCL already does.

Two things worth settling before any of it:

- **OptiX is NVIDIA-only and closed.** KoggerApp is GPL-3.0 and open; a hard
  dependency on a proprietary SDK would exclude every other GPU, and the linking
  question deserves an answer rather than an assumption. OpenCL was chosen for
  the deconvolution partly because Apache-2.0 headers and a runtime-loaded ICD
  raise no such question — see `third_party/opencl/README.md`.
- **What it would be for.** The deconvolution does not need it: the RTX 5080
  beats 32 CPU threads by under 2x there, because the pass is transfer-bound
  rather than arithmetic-bound. More hardware does not fix a bandwidth limit.

## 9. Relief from the side-scan image

Asked for 2026-08-10.

Infer bottom topography from a side-scan picture: darker reads as a slope facing
away, lighter as ground facing the sonar, and the shadows behind obstacles are
matched against their brightness to get height.

### What exists today

Bathymetry already has a home. `SurfaceProcessor` takes a `BottomTrack*` and
feeds `surface_mesh`, with `isobaths_processor` and `surface_view` /
`isobaths_view` drawing the result. So there is already a surface built from
soundings — one depth per ping, at nadir — plus the mosaic beside it.

That matters more than it looks, see "Anchoring" below.

### The difficulty that decides whether this works

**Intensity conflates geometry with material, and the image does not say which.**
Dark can be a slope facing away, a shadow, soft mud, or the nadir gap. Bright can
be a slope facing the sonar, or gravel, or rock, or a hard target on flat ground.
A sand-to-mud boundary on a perfectly flat bottom produces exactly the brightness
step that "darker = slope" reads as topography.

So the naive mapping will produce a sediment map wearing the costume of a terrain
map, and it will look plausible, which is the dangerous part. Anything built here
needs a way to say how much of a given brightness change it attributes to slope
rather than to bottom type — even if the answer is only a confidence band.

**Before any of that: the image already has a systematic across-track brightness
gradient** that has nothing to do with relief — beam pattern, grazing angle and
TVG all vary with range. Reading it as slope would tilt the whole swath. That
gradient has to be flattened first; the existing "Source data" compensation modes
are the beginning of an answer rather than the whole of one.

### Shadows are the stronger cue, and should come first

The request names both mechanisms, and they are not equally reliable.

**Shadow length is geometry**, nearly parameter-free. For an obstacle at slant
range `R` casting a shadow of length `L`, with the sonar at altitude `H`:

```
h ≈ L * H / (R + L)
```

No reflectance model, no assumption about sediment. Altitude comes from the
bottom track that is already computed. This gives quantitative heights for
discrete objects — wrecks, boulders, pipelines — which is most of what an
operator wants a height for.

**Shading is ill-posed.** Recovering a surface from brightness needs a backscatter
model, and the unknowns (sediment type, roughness) affect intensity more strongly
than a few degrees of slope do.

Recommendation: build the shadow measurement first. It is smaller, it is
defensible, and it produces a number rather than an impression. Shape-from-shading
then fills the continuous relief between the objects, with the shadows as
calibration.

### Anchoring

Shape-from-shading recovers *gradients*, and integrating gradients drifts — error
accumulates across the swath, so the far edge ends up metres out with nothing to
catch it.

The fix is already in the tree: the bottom track supplies a measured depth per
ping. Use the existing surface as the low-frequency constraint and let the image
supply only the high-frequency detail between soundings. That turns an
unconstrained integration into an interpolation with known endpoints, which is a
far better-behaved problem.

### It would improve item 5

Item 5 records that a side-scan sample maps to an arc, not a point, and that
placing it as a point silently assumes a flat bottom. Real relief removes that
assumption. If both are wanted, 9 before 5.

### Open questions

- Port and starboard see the same slope with opposite sign. Two channels
  disagreeing is information, not a problem — but which is trusted where, and
  what happens on overlapping passes, is a design decision.
- Is the output a separate layer, or does it merge into the existing surface? The
  second is more useful and much more dangerous, because inferred relief would
  then be indistinguishable from measured soundings.
- What is done where there is no shadow and no texture — flat, featureless mud
  returns no shape information at all, and the honest output there is "unknown"
  rather than "flat".

## 10. Heading source: device or track

Owner, 2026-10-07: with the new boat the mosaic looked as if turns were ignored; the
choice "heading from the device or from the track" is to be made explicit. To be discussed.

### What exists today

- `Epoch::tryRetValidYaw()` (`src/epoch.h`) returns the device yaw if there is one, else
  the course from the track (`Dataset::addArtificalYaw`: atan2 of the last two GNSS fixes,
  kept while standing still).
- Device yaw arrives through `attitudeComplete` from the boat's autopilot (MAVLink ATTITUDE,
  its EKF yaw), NMEA HDT, the sonar's `ID_ATTITUDE`, SimpleNavV2 and the USBL solution.
  Whichever writes into an epoch last wins; there is no priority between them.
- That one yaw is read by the mosaic strips (`MosaicProcessor`) and their bounds
  (`Dataset::calcDimensionRects`), the sonar lever arm (`fruOffsetToNed` in `Dataset`), the
  relief cloud, the echogram motion compensation and the 3D boat.
- Not established for the 2026-10-07 case: which source the new boat delivered. No
  recording or log of that session reached this PC.

### Proposal

- One setting, Heading: Auto / Device / Track. Auto is today's rule, so existing data
  looks the same.
- Track course over a baseline of a few metres instead of two consecutive fixes, kept
  below a minimum speed, so that it holds at survey speed and in GNSS jitter.
- A check that needs no choice: while moving, compare device yaw with track course; if the
  device yaw stands still while the track turns, or disagrees by more than a crab-angle
  allowance for long, say so.
- Switching recomputes the mosaic from the dataset, as the angle offsets do, so a recorded
  file can be compared both ways.

### Open questions

- Scope: the mosaic only, or every reader above? The mosaic and the sonar lever arm must
  agree, or strips and sonar position drift apart.
- Several devices sending yaw: a priority, or a source picker?
- Device chosen, a ping without device yaw: skip the strip, or fall back to the track?
- Side-scan beams are square to the hull, not to the track. A boat crabbing in current or
  wind puts a track-oriented image off by the crab angle: right as a fallback, wrong as a
  default where the device heading is good.

## 11. Controls as widgets

Owner, 2026-10-08, marked as an idea for the future and unrelated to the work of that
day: "выносить контролы в виде виджетов" — take the controls out as widgets. Recorded as
said; nothing about it has been discussed yet.

### What exists today

Not surveyed yet.

### Open questions

- What "widget" means here: a control taken out of the settings panel and put on the
  2D/3D view as a floating, movable panel; a dockable pane; or something outside the
  app.
- Which controls first, and whether the user chooses which ones go out.
- Whether the arrangement is saved per user and per platform (desktop, phone, Head Unit).

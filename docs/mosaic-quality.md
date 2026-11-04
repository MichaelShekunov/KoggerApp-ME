# Mosaic quality: brightness, crop, overlaps

Asked for 2026-10-18: "the mosaic's quality: crop, stitching; the sonar's directivity taken into
account, analysed from the bright areas of the echogram". The owner's summary is
[mosaic-quality.ru.md](mosaic-quality.ru.md).

## What was wrong

On every test recording the mosaic of a survey of parallel lines looked like corduroy: a bright band
along each track, a dark outer edge, and every line a different brightness from its neighbour. Where
lines overlapped, the newest line covered the older one completely -- with its dark, noisy outer edge
and its glaring strip under the sonar -- and its first ping cut a straight edge across what was there.

## Where the brightness comes from

A side-scan sample's level is, on average, a product: a part that depends on the angle the sample
left the sonar at (the vertical directivity of the transducer, and how the bottom scatters at that
angle) and a part that depends on its slant range (spreading, absorption, the device's own gain).
Over a flat bottom a sample at slant range `r` over altitude `h` left at `acos(h / r)` from straight
down. A survey sees the same angle at many ranges as the altitude changes, so the two parts can be
separated:

```
log(mean level) = A(angle) + B(range)
```

fitted by alternating least squares over a table of mean levels, 1 degree by 25 cm, one table a side
(`src/data_processor/mosaic_beam.h`). The correction multiplies each sample by `exp(C - A - B)`, `C`
chosen so the survey keeps its mean level (the colour levels set for it still fit), and never goes
further than 12 dB either way (`kMaxCorrDb`).

Why both parts and not the usual "mean level by angle": with the altitude changing over a survey, a
mean by angle carries the range part into the angles. `tools/beam_check` makes up a sonar with a known
`A` and `B`, speckles it over altitudes of 2 to 15 m, and asks for both; a correction by angle alone
must fail the same test, and does (2.5 dB rms off, 8 dB left between near and far), so the check
would notice a regression to it.

Measured on the three test recordings (2026-10-18; the numbers of the device's own pattern are kept
out of this repository, see "Confidentiality" below): the range part is the larger one -- it is what
makes the band along the track -- and the angle part falls off towards the edge of the beam. The two
parts leave 1.3 to 1.6 dB rms of 3.4 to 3.5 dB per cell; what is left is mostly the bottom itself
(different bottoms at different depths), which must stay. The level is an amplitude, not a dB value:
its spread grows with its mean (std / mean nearly constant across angles), so the correction is a
gain.

The model is measured at run time, from the user's own data, on the levels the mosaic paints (the
selected source: raw, side-scan or TGC). Nothing about any device is stored.

### Confidentiality

A sonar's directivity is device characterisation (see backlog item 2). The repository holds the method
only; the pattern of a device exists at run time, in the application's memory, its log line and the
settings page. Do not paste measured curves or edge angles into this repository or its documents.

## When it is measured (DataProcessor)

From the epochs with a bottom track, at most 4000 of them evenly spread, in some 30 to 60 ms
(`beamEstimate`). Pings under 1 m of altitude are left out: their angles are all but unknown, and in
every 2025 test recording the first 2700 to 2900 pings are the boat out of the water with the altitude
stuck at 0.41 m. A side's model needs 300 pings and data out to 30 degrees from straight down
(`kMinPings`, `kMinAngleCover`) -- a start in 20 m of water on a 25 m range sees 37 to 40 degrees only.

- **A file**: once its bottom track has come (700 ms after the last batch, at most 2.5 s after the
  first), the mosaic waiting for it (`beamHolding`, a file only: the state is read when deciding --
  a reset passes through the undefined state). Too few epochs for a model: painted as they are.
- **Live**: no waiting. At 300 epochs, again every 300 more while there is no model or its sides have
  fewer than 2000 pings, then as the epochs double up to 20 000 (`beamMaybeSchedule`). A new model is
  taken -- and the mosaic repainted -- only when it differs from the one in use by more than 1 dB
  (`maxDifferenceDb`, over the angles and ranges where a sample can exist).
- A new source, other channels, a full bottom-track rework, another recording: measured afresh.
- **After an echogram edit** (`beamRemeasureAfterEdit`, at the start of `DataProcessor::runEditRebuild`, once
  the bottom track is quiet; with the correction off too, as turning it on uses the model kept):
  measured again from the epochs as the edit left them (a ping left out has no bottom in
  `fromIndexMosaicCopy`) and taken as it comes -- no 1 dB threshold, as the rebuild repaints everything
  anyway -- so a recording edited in the session paints as it does reopened with its edits. Before the first
  measurement nothing is done there: that one sees the edits. The log says
  `mosaic beam: measured after an echogram edit on ...`. **[verified: 00014 with pings 20000..44999 (25 000 of its 57 079) marked bad, the background precompute off: the session measured the model again from 1647 pings a side instead of 3314, the same model as the recording reopened with its edits, and the two mosaics agree to the pixel (0 of 567 644 painted pixels apart); the build before measured nothing after the edit and painted 81.6 % of those pixels another value, up to 67 levels apart (`tools/perf/beam_edit_check.ps1`, 2026-10-19)]**

A change of the settings repaints the mosaic (`restartMosaic`, as a new source always did).

## Crop

"Crop near the track" and "Crop the far edge" (degrees from straight down; 0 and 90 are off) leave
the samples outside the window unpainted -- a hole where no other line covers the place. On the test
recordings neighbouring lines do not reach each other's nadir, so a 10-degree crop leaves black strips
along every track; it is off by default. The softer way is the overlap rule below.

The angle comes from the depth under the sonar. Under `kNoAngleAltitudeM` (5 cm) there is none to take it
from -- "Bottom track zeroing" gives every ping 1 cm, and a lost bottom 0 -- so neither crop applies there,
and the correction takes the range part B and C alone, as at the beam's best angle (`sampleGain`). Before
2026-11-04 such a sample sat at the horizon: the owner's far crop of 82 deg left everything past 7 cm
unpainted, and a sonar turned on the spot on the bench drew no mosaic at all.

## Overlaps

Per pixel, which ping's sample stays where pings overlap (`mosaic_beam::Overlap`):

- **Newest on top** -- as before.
- **Newest, edges under** (default) -- the newest, but a sample from a ping's edge -- nearer straight
  down than 12 degrees, or past the angle where that side's measured beam is 6 dB down (75 degrees
  when not measured, or not fallen that far within the data) -- only where no other ping's middle is.
  Where there is no other ping, the edges are painted as before: no holes. This is the
  "shine-through" of survey software.
- **Nearest the track** -- the sample seen nearest straight down ("nadir priority"). On the test
  recordings it shows the angle part's residual of a dB or two as stripes repeating with every line,
  where the other two rules break the repetition; offered, not the default.

The two rules with a choice keep, per pixel, the quality of the sample painted there
(`SurfaceTile::getMosaicQualityRef`, one byte a pixel, allocated only under those rules): a pixel
keeps the best, the newest of equals. It travels with the tile through the worker's mesh, the hot cache
and the session DB (`quality_blob`, added to an older file's table on open), and is copied wherever a
tile's image is restored (`SurfaceProcessor`, `putTilesIntoMesh`). The renderer gets none of it
(`DataProcessor::emitDelta`). The parallel painter's merge takes the best quality, the latest of
equals, and weighs it against the tile's own -- the result of painting in pair order, so one thread
and eight give the same picture. "Clear old data" clears a pixel's quality with its colour.

## What it costs a sample

The mosaic asks for a colour per step of each ping's line, for both pings of a pair. Taking the angle
with `std::acos` and the gain with `std::pow` there was half of the painting threads' work (a profile of
opening 00017 with `tools/perf/sampler`, 2026-10-18). Now `MosaicProcessor::sampleColor` works with the
cosine of the angle, altitude / slant range, as it is:

- the crop and the edges of "Newest, edges under" are compared as cosines (`PaintRules::prepare`; off is
  2 and -1, not `cos 0` and `cos 90`, which is 6e-17 and would cut a 0 altitude);
- the gain comes from tables (`SideModel::prepare`): `10^(-A/20)` over 4096 steps of the cosine,
  `10^(-B/20)` per range bin, `10^(C/20)`, and the 12 dB stop as factors -- the stop of a sum in dB is the
  stop of the product. Within 0.045 dB of the direct formula over a survey's angles and ranges;
- "Nearest the track" takes its angle from a table of acos (`angleFromCos`): within 0.03 degree, 0.32 in
  the last step to straight down;
- the parallel painter keeps a pixel's quality in the top byte of the stamp it already had (quality,
  then pair number): the best sample, the last of equals, is the largest stamp -- no buffer of qualities.

Opening 00017 (1.1 GB, 186 000 epochs) to the first whole mosaic on screen, three runs each: the old
build 10.12 / 10.46 / 14.51 s, the new one with the defaults 10.91 / 11.00 / 11.54 s -- medians 10.46
against 11.00. Before the tables it was some 13 s. Single runs of each part apart -- the correction
alone, the overlap rule alone -- put each at a few tenths of a second, but runs of one configuration
spread by half a second, so the split is a guess [inference]. A file whose bottom track covers (nearly)
every epoch is measured at once, not after the 700 ms meant for batches still coming.

## On the way: the swath's edge

Between two pings the colour is blended along the path from one ping's sample to the other's. Where
one of them had no sample (past its range) it was blended with that 0 -- a dark fringe -- and at the
end painted a 0, which is "nothing", over what was there. One ping without a sample now gives the
other's as it is. This changes old pictures slightly at the far edge, in every mode.

## Settings (Settings, Mosaic)

"Even out brightness" (on), "Crop near the track, deg" (0), "Crop the far edge, deg" (90), "Overlaps"
(newest, edges under). Below them, when measured: the measured angle part for the left and right sides,
dB by degree, labelled, with the crops marked, and a line -- brightest at, down 6 dB at (or "no 6 dB fall
up to"), the near-to-far range part taken out, the pings it was measured on. The block is
`qml/app/MosaicQualitySettings.qml`, a part of the Mosaic group of `AppSettingsPage.qml`; it sends the
four values to `MosaicViewControlMenuController::setQuality`, which queues
`DataProcessor::setMosaicQuality`; the state comes back as `mosaicBeamState` (`beamState` in QML). The
numbers of the "not measured yet" line come with the state (`kMinPings`, `kMinAngleCover`), not from QML.
Each of the four rows carries a KPinTag (`app:mosaic.beamCorrection`, `nadirCrop`, `farCrop`, `overlap`; since
ae023ac9), so it can be pinned as a settings widget (docs/HANDOFF.md, "Settings pinned as widgets").
`tools/qml_check/pin_coverage_check.ps1` fails when a value row has no tag and is not on its list of rows left
out (these four are not on it); `beam_edit_check.ps1` turns the correction off and on through such a widget.

## Checks

- `tools/beam_check` (CMake, no Qt): 18/18. Binning against `acos`; recovery of a made-up response
  (0.02 dB rms where not at the 12 dB stop, 4 % of the samples at it); flat by angle and by range
  after the correction (15 dB -> 0.1 dB); the mean level kept (x0.993); the control by angle alone
  fails; refusals (too few pings, data short of 30 degrees, no altitude); the 6 dB edge (63.5 against
  63.9 made up) and none where the data ends first; `maxDifferenceDb`; the per-sample paths (the acos
  table, `gainFast` against `gain`, the crop and the qualities by the cosine). Five mutants of the
  header -- bin edges one off, the sign of `A`, `C` inverted, no stop, no angle cover -- each fail it
  (run before the per-sample paths were added; their own checks have no mutants yet). Since 2026-11-04
  23/23 with section 6, a zeroed bottom: every sample of a 15 m ping painted under a far crop of 82 deg by
  the range part alone, the old rule as the control (2 of 1000 painted), from 5 cm down as before; eight
  mutants of `angleKnown`, `gainRangeFast` and `sampleGain` each fail it.
- `tools/perf/beam_edit_check.ps1 -Exe <KoggerApp.exe> -File <recording> [-PathPrefix ...]` (three windows
  of the app, 20 to 60 s each, the background precompute off): a long stretch marked bad in the session; the
  recording reopened with its edits; the same edit with the correction turned off before it and on after it
  (through a settings widget). The model measured after the edit, its line the reopen's, the mosaics with no
  tile in one only and `mosaic_dump_compare.ps1 -Judge` on the rest. On 00014: PASS, 7 of 7; the build that
  skipped the re-measure with the correction off (6a2684e1) fails the three "off" checks, ad52a0c0 all six.
  **[verified]**
- `tools/qml_check/mosaicquality_check.ps1` (qmltestrunner, a mock controller): 5/5 -- sent once at
  start as the controls hold it, each control sends its value, the status line in each state, the chart
  shown only with a model and each side's curve in its colour at its dB, the correction off hides both.
  It found the chart's `top` property overriding a FINAL one, which would have kept the block from
  loading in the application.

  ```
  cmake -S tools/beam_check -B build/beam_check -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=D:/Qt/Tools/llvm-mingw1706_64/bin/clang++.exe
  cmake --build build/beam_check
  build/beam_check/beam_check.exe
  ```

- `tools/perf/precompute_check.ps1` on 00014 with the defaults: PASS (the precompute's zoom level
  against the view's: 0.004 % painted in one only, 0.01 % another value) -- the precompute paints
  with the same model and rules.
- Two runs of a file with the same settings give the same mosaic hash (00014: "Nearest the track"
  `a57de2c90f5af3ad` twice, "Newest, edges under" `3a35b09cb52625c1` twice, also across two builds).
- On the last build (the per-sample tables, the stamps with the quality, the immediate estimate of a
  whole file), with the defaults: `precompute_check` PASS on 00014 (93 tiles in both, none painted in
  one only, 8 of 3.86 million pixels another value, at most 10 apart; 4 of 7 533 height points), and two
  runs of 00014 one hash, `67cff3d944892266` (another than before: the tables' gain is within 0.045 dB
  of the formula's). "Nearest the track" has had no precompute check.
- Live, `tools/perf/live_zoom_check.ps1 -FromOffset 20000000 -Wheel 0` (from where the boat is in
  the water, the camera as a file run left it), the old build against the new one, 60 s at 45 pings a
  second: the same pings painted per 10 s (431 / 432, 366 / 366, 424 / 422, 447 / 447, 445 / 445),
  the median latency 197 to 210 ms against 197 to 199. The model came at 1232 epochs, was kept once
  (0.97 dB), repainted once (1.76 dB).

Live runs from the start of a 2025 recording stream its first minute of the boat out of the water;
the boat's position jumps as its navigation becomes ready, and only tiles in view are painted live,
so the pings stop being painted once the boat leaves the view -- in either build, depending on the
camera (an old-build run from a saved profile painted no ping at all; HANDOFF has the camera saved
from the 1.1 GB file, past `tilesInQuadNed`'s 2 000 tiles). Hence `-FromOffset`, and one saved profile
(`reg export`, `reg import`) restored before each of two compared runs, with `-Wheel 0`.

## Diagnostics

- The log, at each estimate: `mosaic beam: measured on N of M epochs in T ms; left: ...; right: ...`
  (pings, data reach, peak, the -3/-6/-10 dB edges, what the fit left of what there was), and whether a
  new model was kept or repainted for.
- `KOGGER_MOSAIC_PING_DUMP=<file>`: at each estimate, up to 16 000 of the epochs with a bottom track,
  evenly, in a pass of their own (denser than the estimate's, for straight runs of a line), little-endian
  -- "KPD2", then per ping and side: epoch (int32), side (uint8), altitude, resolution, offset, north,
  east, yaw, pitch, roll (float32 each; pitch and roll NaN without attitude), count (uint32) and the raw
  levels. For an analysis outside the application. The log's "in T ms" leaves the dump's pass out: since
  6a2684e1 the clock starts in `DataProcessor::beamMeasure`, after the dump **[verified by reading]**.

## Open

- Fine stripes along every line, some 4 m apart on screen at zoom 6 (investigated 2026-10-18, offline,
  on the dumps): not a fault of their own but the harmonics of a pattern each line repeats. On 00017 the
  mosaic's spectrum peaks at 12.06 m -- half the line spacing, 24.7 m by the pings' positions -- and at
  6.3 and 4.2 m; inside the cross line's swath the stripes run along the cross line, so they come with
  each line's pings, not from the bottom [verified]. The correction weakened them about threefold in
  amplitude (the 4 m band from some 0.8 to 0.3 dB), they only show more on the flattened picture; the
  overlap rule hardly changes them [verified]. Within one line the corrected level by ground range has
  0.5 to 1.5 dB of structure, the same in both halves of a straight run, but unlike between lines: it
  lines up on none of slant range, the distance behind the bottom, ground range or angle (correlations
  near 0), is not the heel (roll 2.1 to 2.2 degrees on every line of 00014) nor a range setting (one for
  all runs), and is not the beam's fine structure (the angle part at 0.25 degree differs from the app's
  by 0.05 dB rms) [verified]. What is left is either bottom that runs along the lines (lines along the
  contours) or something per line not found [inference]. Options: blending the seams (softer lines, at a
  risk of ghosts where the lines do not register), or a normalisation along each line over a moving
  window of pings (it takes out whatever runs along the track, the real bottom included).
- The angle part varies with the altitude by a dB or two (A + B cannot hold that); a third,
  altitude-dependent part would also take out differences of the bottom that go with depth.
- Live, the first epochs are painted without a model and repainted once it comes; a model is not kept
  between sessions.
- After an echogram edit the session measures the model again over the whole bottom track. A reopen measures
  by the file rule above (700 ms after the last batch, at most 2.5 s after the first), then again only as
  `beamMaybeSchedule` allows, and takes a new model only past 1 dB. A file whose bottom track is still coming
  then can keep a model measured on part of it, and the reopen paints otherwise than the session did
  **[inference, from the code; found in the review of 6a2684e1, not run]**. On 00014 the reopen measures over
  the whole track and the two models are the same **[verified, `tools/perf/beam_edit_check.ps1`, 2026-10-19]**.
- With data streaming, the re-measure waits, as the rebuild does, for 500 ms of a quiet bottom track, which
  comes only between batches **[inference, from the code]**. Live editing was not run.
- A map export takes the brightness model (none with the correction off) and the crop / overlap rules once, at
  its start (`DataProcessor::mapExportConfig`, `MapExportConfig::beamModel`, `paintRules`) **[verified by
  reading]**. An export started between an echogram edit and the rebuild after it, or still running when that
  rebuild measures again, paints with the model from before the edit **[inference, from the code; not run]**.
- Not checked on the water, on another device, or with a TGC source of 0 % near gain (the owner's): there
  the near range is 0 before the correction, and no gain brings it back -- "Raw" or "Side-Scan" suit the
  correction better.
- Polish has no strings for this (shown in English, as for every newer string); Chinese is a machine
  translation, not read by a speaker.

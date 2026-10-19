# Echogram editing

Asked for 2026-10-18 ("make echogram editing"). Asked which of the candidates to do, the owner chose all
four: edits saved next to the recording, with undo and redo; pings marked bad; the recording trimmed;
the pencil's own points shown, and a magnet. The same night the owner asked for more ideas for editing
the echogram and the mosaic from what other programs do, implemented: the filters below. The owner's
summary is [echogram-editing.ru.md](echogram-editing.ru.md).

Markers: **[verified]** = run and seen, **[inference]** = from reading the code, not run,
**[assumption]** = believed, not checked.

## What was there

Upstream KoggerApp already has a bottom-line palette (`qml/app/BottomTrackEditPopup.qml`, opened from the
quick-actions panel): navigate, pencil, raise, lower, eraser; in the 3D view, bottom-track points picked
and deleted (Delete) or limited. **[inference]**

- The pencil sets the bottom of every ping it crosses and marks it `DistanceSourceDirectHand`; the bottom
  track's runs skip such a ping, also a full recompute (`BottomTrackProcessor::bottomTrackProcessing`,
  `publishBatch`). The eraser does the same with no bottom (NaN). "Raise" and "lower" set a ping's search
  limit (`DistProcessing::max` / `min`) and run the bottom track again over the stroke.
- After an edit the 2D line, the 3D bottom track and through it the surface and the isobaths are
  updated; the mosaic is asked to repaint the changed pings.

What it lacked or did wrong, all in this work:

| | before | now |
| --- | --- | --- |
| kept | in memory only: a file opened again, or read again for the track smoothing (`Core::reopenFilesKeepingView`), lost them | a file next to the recording, read back on opening |
| undo | none | every stroke a step, Ctrl+Z / Ctrl+Y and buttons |
| mosaic after an edit | not repainted where already painted: a tile refuses a pair older than its head index (`MosaicProcessor::pixel`, `headIndx <= t->getHeadIndx()`) **[inference]** | painted again once the edits are quiet (`DataProcessor::rebuildAfterEdit`) **[verified]** |
| surface after the eraser | the erased point stays: the triangulation cannot drop a point (`SurfaceProcessor`, a NaN is skipped, the old height stays) **[inference]** | built again **[verified on the mosaic and the dumps; the 3D surface itself not looked at]** |
| raise/lower during a run | the stroke's run was dropped while the automatic one was busy (`DataProcessor::bottomTrackProcessing`, `if (btBusy_) return`) **[inference]** | waits and is joined with others of the same settings (`pendingManualBt_`) |
| raise/lower, first ping | with an odd window every ping was searched with its neighbour's limits, the first kept its old bottom **[verified, 00014]** | the ping's own limits (`limits_index`) **[verified]** |
| 3D "max/min" | sent the range of before the loop (`btP` copied before `indexFrom`/`indexTo` were set) **[inference]** | the range just set |
| marks, trim | none | ping marks and a trim, left out of everything downstream |
| hand points | drawn like the bottom track's | an orange line over it; search limits dashed |
| magnet | none | the pencil snaps to the bottom the bottom track finds near it |

## The model and the editor

- `src/echogram_edit/echo_edit_model.{h,cpp}` -- no dataset, Qt Core only, so `tools/echoedit_check` tests
  it alone: a ping's edit state (`PingState`: the bad mark and each channel's distance, limits, source),
  a step (`Command`: kind, the pings' states before and after, the trim before and after), the history
  (`History`: at most 100 steps and 2 000 000 ping changes, the newest step always kept), what a step
  changes downstream (`removesPoints`, `epochsToTrackAgain`), the file of edits (`EditFile`, JSON), the
  magnet's search (`trackResponse`, `trackPeakWithin`).
- `src/echogram_edit/echogram_editor.{h,cpp}` -- `EchogramEditor`, owned by `Core`, QML `echoEdit`. A
  stroke is `beginStroke(kind)` .. `touch(epoch)` before each change .. `endStroke()`: the states before
  were snapshotted at the first touch, the states after are read at the end, and a ping that did not
  change is not in the step. Plot2D (`setMousePosition`: a press opens the stroke, the release at
  `x == -1` closes it), the 3D bottom track (`BottomTrack::actionEvent`, `keyPressEvent`) and the script
  runner (below) use it the same way.
- Undo applies the states before, redo the states after, through `Dataset::setPingEditState` (under the
  pool's write lock).

### When the bottom track runs again

- A "raise" or "lower" stroke runs it as it goes (as before); its end does not run it again.
- **Undo restores the depths kept from before the step, exactly**, and runs nothing -- except for pings
  whose restored state carries a search limit that differs from the current one (`epochsToTrackAgain(c,
  true)`). A run over a stretch would not give the depths back: its first pings are found with a shorter
  window. With this rule "undo all" gives back every ping's bottom bit for bit **[verified, 00014]**.
- Redo and "reset all" run it over the pings whose limits change or whose hand point goes: the depths kept
  with a raise step may predate the run its tool started.

### Ping marks and the trim

`Epoch::isMarkedBad` (one byte) and the trim (`Dataset::editTrim`: two atomics, the pings kept are
`[start, end)`; `end` open means "and every ping to come"). `Dataset::isEpochExcluded` is either. A ping
left out:

| where | how |
| --- | --- |
| echogram | shaded: trimmed dark, marked red (`Plot2D::drawEditShading`, right after the echogram) |
| 3D bottom track, surface, isobaths | no point: `BottomTrack::updateRenderData` gives it NaN, the surface skips a NaN |
| mosaic | `Dataset::fromIndexMosaicCopy` hands the mosaic (and the precompute, and mosaic-quality's brightness model, which sample through it) a copy with no bottom: no pair with it is painted |
| CSV export, XTF export | skipped (`Core::exportPlotAsCVS`, `ConverterXtf::toXTF`) |
| map export: the KMZ track, the survey's times | skipped (`MapExporter::start`; a .kmap carries no track) **[verified: 00014 with 4065 pings left out, HANDOFF "Brought together"]**; its mosaic and depths leave the ping out through the mosaic row and the bottom track's NaN above **[verified by reading]** |
| echogram relief cloud | skipped (`ReliefCloud`) |
| bottom track | still run over it (its depth is kept; only the products leave it out) |

### After an edit: the products built again

`EchogramEditor::afterChange` asks `DataProcessor::rebuildAfterEdit(withSurface)`. The processor waits
until the bottom track is quiet (no run, none waiting, 500 ms since its last 3D echo or the end of a run)
and then:

- **without the surface** (a step that takes no ping out of the products: pencil, raise, lower, unmark,
  undo of those): `restartMosaic()`, as for new mosaic settings;
- **with the surface** (a step that takes a ping out: eraser, mark, trim, reset, and the undo of steps that
  put pings back): the worker's surface, mosaic and isobaths start from nothing
  (`ComputeWorker::clearForEditRebuild`), every bottom-track point and epoch is queued again, the precompute
  and the hot cache are dropped and the session DB's rows not read back (`resetMosaicProcessed`). The mesh
  keeps its tiles blank; those that showed something are posted again, so a tile nothing is painted into
  now reaches the view blank (a tile that never had anything is not: 14 blank tiles appeared on 00014
  before that).

Strokes in a row make one rebuild (the timer restarts). On 00014 (57 079 pings, 350 MB) the rebuild of the
visible zoom takes about a second **[verified: the digest 1-3 s after the step]**; on a 1.1 GB survey it
is the time of painting the view after opening, some seconds **[inference]**.

### The file of edits

Next to the recording: `<recording>.kedit` (`00014_03.05.2025.KLF.kedit`), JSON, written atomically
(`QSaveFile`) 1.5 s after the last change, at a file's close, at another file's open, at the app's end and
when a phone suspends the app. A recording with no edits left has its file removed. The recording itself is
never written.

```json
{
  "kind": "KoggerApp echogram edits", "version": 1,
  "record": "00014_03.05.2025.KLF", "recordBytes": 348309504, "epochs": 57079,
  "trim": { "start": 2924, "end": null },
  "bad": [[22832, 23973]],
  "bottom":   [{ "address": 1, "from": 28540, "values": [6.685359954833984, null] }],
  "limitMin": [{ "address": 1, "from": 45663, "values": [13.227899551391602] }],
  "limitMax": [{ "address": 1, "from": 39955, "values": [13.105899810791016] }]
}
```

- Indices are the recording's epochs as KoggerApp reads it. `recordBytes` and `epochs` say whether it
  still is that recording: when either differs, nothing is applied, the file is left as it is, edits are
  not saved this session, and the user is told (`EchogramEditor::fileRead`). "Reset all" then moves it to
  `.kedit.bak` and saving resumes. A newer `version` is refused, not misread.
- Floats are written as they are (a double of the float); rounded to 0.1 mm a drawn stretch came back
  36 um off and the reload was not bit for bit **[verified, 00014]**.
- `bottom` holds the points set by hand; null is a point erased by hand. `limitMin` / `limitMax` the search
  limits ("lower" / "raise"). The channel is its `ChannelId::address` (a file's channels share its uuid).

**When it is read.** In `Core::onFileStopsOpening`, after the reading and before the data horizon hands
the epochs on: the bottom track's run, the 3D bottom track, the surface and the mosaic of the opening are
built with the edits from the start -- hand points skipped, limits honoured, marked and trimmed pings out.
Nothing is built twice. With `KOGGER_SEPARATE_READING` (off in every build) the processing runs while the
file is read, so the limits are run again and the products rebuilt after (`fileRead(true)`) **[inference:
that build is not made]**.

**Not saved:** live data (no recording open); several files opened together (append) -- the first file's
edits are saved when the second is appended, then nothing until a single file is opened, and the user is
told. A folder that cannot be written: the user is told once, the edits stay in memory.

`Dataset::onDistCompleted(Batch)` now skips a hand point too: a run that read an epoch before the hand set
it no longer overwrites it when its results arrive.

### The magnet

`EchogramEditor::magnetDistance`: within 12 pixels of the pencil (`Plot2D::magnetDistance`), the bottom
the bottom track would find there -- its filter (`trackResponse`, the per-ping step of
`BottomTrackProcessor::bottomTrackProcessing` copied word for word: the moving average that widens with
range, the five taps of the preset, the gain with range), summed over the pings of its window around the
ping (`windowSize`, both channels merged as it merges them), its peak search and threshold
(`trackPeakWithin`), the bottom-track settings in force. No peak over the threshold: the pencil stays where
it is. The magnet's switch is in the palette and kept in the settings (`echogram/editMagnet`).

A first version looked for "the strongest rise within the radius" of the raw column. On 00014 started 0.3 m
off the bottom track's bottom it landed within 10 cm of it for 27 % of the pings from below and 7 % from
above **[verified]**: on a side-scan the nadir return is weak and the rise farther out stronger. Copying the
filter keeps the two definitions of "the bottom" one. It is a copy: if the bottom track's filter changes,
the magnet must follow -- `tools/perf/echoedit_e2e.ps1`'s magnet check would show it drift.

### The filters: what other programs offer

Asked for the same night ("look on the internet for more ideas for editing the echogram and the mosaic,
and implement them"). What hydrographic programs offer for single-beam and side-scan data, from their
documentation **[read, none of them tried]**:

| idea | seen in | here |
| --- | --- | --- |
| a median filter run over the bottom, as often as wanted | Hydromagic (median and mean filters, a range filter, an eraser, "delete above / below", Ctrl+Z) | the smoothing brush |
| a gap interpolated instead of left empty | HYPACK SBMAX64 (soundings rejected over a range can be interpolated between the good ones) | the gap filler |
| pings rejected in the waterfall per side: port, starboard or both, flagged and restorable | CARIS | one side out of the mosaic |
| sea spikes found with the median and the median absolute deviation over a window | a FIG Working Week 2025 paper on single-beam data | "remove the spikes" |

Not taken: the mean filter (it smears a spike into its neighbours; the median does not); a range filter
and "delete above / below a line" (the bottom track's own minimum and maximum distance, and "raise" and
"lower", do that in its terms). The turns are this editor's own idea, from the mosaic's geometry (below).
The numbers are this editor's choices, set in `echogram_editor.cpp`; how they did on the test recordings
is under Results.

**The smoothing brush** (tool 9, `EchogramEditor::smoothRange`). Each move of the brush gives every ping
it sweeps that has a bottom the median of the bottoms of the ping and its 4 neighbours each way
(`kSmoothHalf`), all from the depths before that move; sweeping again smooths more. The new depths are
points set by hand, on every channel, as the pencil's. A ping with no bottom is left alone. One stroke,
one step.

**The gap filler** (tool 10, `fillGapsRange`). A ping with no bottom between two that have one gets the
straight line between them, when the gap is at most 1000 pings (`kMaxFillGap`); the gap's ends are looked
for beyond the brush too, so a brush over part of a gap fills that part on the gap's own line. An open
end (no bottom before the first ping, or after the last) is not filled. Hand points.

**One side out of the mosaic** (tool 11, `setMosaicOut`). The side under the cursor -- in a side-scan pair
the first channel is drawn above the middle line, the second below -- is left out of the mosaic over the
pings the brush sweeps; its depths stay in the bottom track, the surface and the exports. "Unmark pings"
puts both sides back. A side is a sub-channel: the two sides of 00014's DSSS pair are sub-channels 0 and
1 of the one channel at address 1 **[verified, the end-to-end dump]** -- keyed by the address alone, as
first written, one side took both. `Epoch::Echogram::mosaicOut` per echogram (channel and sub-channel); a
ping's `ChannelBottom::mosaicOut` holds a bit per sub-channel for the history (`subs`, the count of them,
is the data's and not compared); the file has `"mosaicOut": [{ "address": 1, "sub": 1, "ranges":
[[from, to]] }]` (absent in a file written before it, read as none). The mosaic copy
(`Dataset::fromIndexMosaicCopy`) drops the bottom of a side that is out, and the mosaic paints no pair
with a side that has no bottom (`MosaicProcessor`, `paintPair`); the other side is painted as before. The
echogram tints that half blue (`Plot2D::drawEditShading`).

A side going out gets the full rebuild, as points going do (`removesFromMosaic`): the mosaic's own restart
(`ComputeWorker::clearMosaic`) clears the pixels but keeps the height points the mosaic wrote
(`HeightType::kMosaic`), and the surface's extrapolation reads them. On 00014 the turns step rebuilt that
way left 382 of 2 025 height points other than a rebuild from nothing gave **[verified]**. The same holds
for a settings change that shrinks what the mosaic paints (a channel, an angle) -- not touched here.

**Remove the spikes** (a button, the whole recording, `removeSpikes`). For every ping in use (not left
out, not set by hand -- the hand's decisions are neither judged nor taken as neighbours) with 4 or more
such neighbours within 10 pings each way (`kSpikeHalf`): a spike when its depth is farther from the
neighbours' median than 4 robust deviations (`kSpikeK` x 1.4826 x the median absolute deviation) and than
0.2 m + 5 % of the median (`kSpikeMinAbsM`, `kSpikeMinRel`). The deviation keeps a cliff from being a spike
(on one side of a 3 m step half the neighbours are 3 m off, and so is the deviation); the floor keeps a
smooth bottom's centimetres of noise from being spikes. Every spike becomes a hand point at that median;
one step; the user is told how many.

**The turns out of the mosaic** (a button, the whole recording, `turnsOutOfMosaic`). The mosaic lays a
ping's swath across its heading. Where the heading turns by dpsi while the boat moves ds, the swath's far
end moves range x dpsi; when that is more than ds the inner side's far end moves backwards and paints over
pings already painted. For each ping with a position, a heading (`Epoch::tryRetValidYaw`, the one the
mosaic uses) and a range (`getMaxRange`): the heading change between the pings 10 m of straight-line
distance behind and ahead of it (`kTurnBaseM` = 20 m between them; a boat turning on the spot is a turn),
unwrapped across 0/360, over the distance between them; the ping turns when range x |dpsi| / ds > 1/1.5
(`kTurnRadiusFactor`: a turn radius under 1.5 ranges -- for a steady turn the base does not matter, it
only averages the heading's wobble). A run of turning pings shorter than 2 m of track that turns less than
30 degrees is a wobble and dropped (`kTurnMinRunM`, `kTurnMinDeg`); the rest is widened by 5 m of track
each way (`kTurnMarginM`) and both its sides go out of the mosaic, as tool 11 would; one step.

The first settings (a 10 m base, no wobble rule) took 46 % of 00014's pings: the yaw of a small boat
swings several degrees along a line (median 0.23 degrees ping to ping, 3.3 at the 99th percentile), and a
10 m base read a swing of 8 degrees either way as a turn; each such blip then took 5 m of track each side
**[verified: the track dumped, the finder replayed offline]**. With 20 m and the wobble rule the turns at
the line ends, the start's manoeuvring and a stop where the boat stood still while its heading drifted
110 degrees remain (26 % of 00014's pings in use, 23.5 % of 00008's **[verified]**; 00014's eight lines
of ~300 m at 6.4 cm a ping would be some 37 000 of its 54 000 pings **[inference, from the track plot]**).

### The palette and the keys

`qml/app/BottomTrackEditPopup.qml`, three columns: the bottom line's tools; the history, the ping tools and
the trim; the brushes and the whole-recording filters. A tool is a number in `core.bottomTrackEditTool`,
turned into a `MouseTool` (`src/scene2d/plot2D_defs.h`) by `Plot2D.qml`'s `_applyBtTool`:

| tool | button | MouseTool |
| --- | --- | --- |
| 0 | navigate | `MouseToolNothing` |
| 1 | pencil | `MouseToolDistance` |
| 2 | raise | `MouseToolDistanceMax` |
| 3 | lower | `MouseToolDistanceMin` |
| 4 | eraser | `MouseToolDistanceErase` |
| 5 | mark bad pings | `MouseToolMarkBad` |
| 6 | unmark | `MouseToolUnmarkBad` |
| 7 | the recording starts here | `MouseToolTrimStart` |
| 8 | the recording ends here | `MouseToolTrimEnd` |
| 9 | the smoothing brush | `MouseToolSmooth` |
| 10 | the gap filler | `MouseToolFillGaps` |
| 11 | one side out of the mosaic | `MouseToolMosaicOut` |

Also: the magnet (a toggle), undo and redo (their tooltips name the step, `kindName`), "the whole recording
again" (the trim off), "reset every edit" (pressed twice within 4 s; undone like any step), "remove the
spikes" and "the turns out of the mosaic" (each one step over the whole recording). A trim tool
moves the boundary with the cursor and makes it one step at the release; the ping under the cursor is kept
(the end is the ping after it). Ctrl+Z / Ctrl+Y (and Ctrl+Shift+Z) in `MainWindow.qml`, not while a text
field has the keys. The tools 5-11 do nothing in the 3D view. Icons: Tabler outline (MIT), as the others.

## Checks

- `tools/echoedit_check` (CMake, Qt Core; `run.ps1 -Mutants`): the model alone. 62 checks: NaN equality,
  ranges and runs, what each kind of step takes out and what it tracks again, the history's limits, 40
  random sessions of 150 operations with undo and redo checked against a timeline of states, the file
  written and read back and its errors refused, the magnet's search on synthetic columns (a return from
  10 m, a fish above the bottom, the threshold, noisy windows of 11 pings, one beam and side-scan); the
  filters: the median skipping pings with no bottom, the smoothing brush, gaps filled on their line (a
  gap of 10 with a limit of 10 filled, 11 not; an open end not), 30 spikes of 1-3 pings in a noisy bottom
  with a 3 m cliff (all found, given the bottom within 5 cm, the cliff not), a hand point not judged, a
  lawnmower survey with compass headings (0-360) whose three half circles are found whole, the lines left
  alone and the margin taken, a 200 m curve no turn, turning on the spot a turn.
  **20 mutants**, each a fault in a copy of the model, **all caught** **[verified, 2026-10-18]**.
- `tools/qml_check/echoedit_tools_check.ps1` (qmltestrunner, offscreen): the tool numbers 0-11 of the
  palette, Plot2D.qml's table and the MouseTool enum agree (read from the three files); the real
  `_applyBtTool` and `kindName` (16 kinds of step), cut out of the QML, run against mocks. 7/7.
- `tools/perf/echoedit_e2e.ps1` (three windows of the app, closing themselves): a script of edits
  (`KOGGER_ECHOEDIT_SCRIPT`, `src/echogram_edit/echo_edit_script.h`) on a recording, judged from its dumps
  (the edits, every ping's bottom as float32, the CSV export, the mosaic dumps of `KOGGER_MOSAIC_DUMP`).
  What it checks is in its header; the results are below.

### Results on the test recordings

`tools\perf\echoedit_e2e.ps1`, 2026-10-18, one build (22:35) for all of it, the profile "echoedit"; the first pings of both recordings
are the boat out of the water and are trimmed (`-TrimStart`). **[verified]** for every number here.

| | 00014 (348 MB, 57 079 pings, trim 2924) | 00008 (325 MB, 87 433 pings, trim 2647) |
| --- | --- | --- |
| magnet from 0.3 m below / above, within 10 cm | 100 % / 99 % of 50 801 pings, median 0 | 100 % / 100 % of 79 914, median 0 |
| trim: the mosaic | 39 741 pixels only before, none gained | 1 019 only before, none gained |
| marked, drawn, erased, raised, lowered | each where made; the CSV 53 014 rows | each where made; the CSV 83 037 rows |
| undo all; redo all | every bottom bit for bit, the mosaic the same | the same |
| the file; reload; reset; its undo; cleanup | as written, read back, bit for bit, removed | the same |
| smoothing, 3 sweeps over 1 % of the recording | 17 pings changed, all by hand, each within the depths around it; roughness 1.07 -> 0.64 m | 63 pings changed (4 more set by hand at their old depth), each within the depths around it; roughness 66.1 -> 28.4 m |
| gap filler over an erased stretch (0.2 % of the recording) | 114 pings on the straight line (worst 0.5 um off), nothing else | 175 erased pings on the straight line (37 of them where they were), nothing else |
| one side out of the mosaic | sub-channel 0 of 1 only; 3 848 pixels lost, 4 gained; no depth changed | sub-channel 0 of 1 only; 4 519 pixels lost, none gained; no depth changed |
| remove the spikes (6 put in, 1-3 pings, 1.6x deeper) | the 6 back on the bottom (exactly); 272 more found, 0.50 % of the pings in use | the 6 back on the bottom (exactly); 14 more found, 0.02 % of the pings in use |
| the turns out of the mosaic | 14 turns, 14 282 pings in use (26 %); no depth changed | 22 turns, 19 930 pings in use (23.5 %); no depth changed |
| the filters undone; redone | bit for bit, the mosaic the same, its heights too | bit for bit, the mosaic the same, its heights too |

The big recording, 00017 (1.1 GB, 186 458 pings, trim 2792), the filters run only, the same build: 11 of
11 **[verified]**; 51 turns, 29 298 pings in use (16 %) out of the mosaic -- every line end of its
28-line survey, the lines left alone (the track plotted and looked at); 43 spikes found (0.02 %), runs of
4-6 pings 1.2-1.6 m deeper than their neighbours.

The two whole-recording buttons run on the GUI thread: "remove the spikes" took about 35 ms on 00014 and
00008 and 0.1 s on 00017, "the turns" about 0.2, 0.25 and 0.37 s **[verified, from the log's timestamps,
less the script's 50 ms between steps]**; the rebuild after them runs on the worker.

"Raise" in this script puts the limit at 0.85 of the stretch's median bottom, above the real bottom, so the
bottom track finds the strongest return above the limit: near the transducer (15.4 -> 0.6 m on 00014,
3.1 -> 0.6 m on 00008). That is what the limit says; the check is that nothing lies below it.

What the end-to-end runs found and what was changed for it:

- The side out of the mosaic first took both sides: on 00014 they are sub-channels 0 and 1 of one
  channel, and the side was keyed by the channel's address. Now per sub-channel.
- The turns first took 46 % of 00014's pings: the yaw's wobble along the lines read as turns. Now a 20 m
  base and the wobble rule (above); also only pings in use are marked (the boat's track before the trim
  had been marked and counted).
- After the filters were redone the tile heights differed from those of the forward run in 382 of 2 025
  points: the mosaic-only restart keeps the heights the mosaic wrote. Now a side going out of the mosaic
  gets the full rebuild.
- Spikes "found in the recording itself" on 00014 (272 pings, 0.5 % of those in use): the first dozen
  are at the start, right after the trim, where the bottom track jumps between 0.4 m and 20 m (the end of
  the range); the median there is 0.4 m. Whether that start is worth keeping at all is the user's call
  (trim further); the button only makes the line consistent.

## Merging with the other branches

`mosaic-quality` and `map-export` were branched from `me` c4e1d3f4 like this work, apart from it. They came
into `me` on 2026-10-19: merge 30b1138f (`install-maps-mosaic`, the two together as installed on the laptop
since 2026-10-18; the mosaic quality there is aff9949c, the same work as `mosaic-quality` 879503ed put on top of
map-export) and 344919cd (the map session's results from the phone). What their meeting needed:

- **mosaic-quality.** Its per-pixel quality (`SurfaceTile::getMosaicQualityRef`) is dropped in
  `ComputeWorker::clearForEditRebuild` as in its `clearMosaic`: `SurfaceTile::init` keeps it, and the old quality
  would refuse the repaint. Done in the merge **[verified by reading]**. Its brightness model samples epochs through `Dataset::fromIndexMosaicCopy`, so a ping left out,
  or one side of it, reaches the model with no bottom, and `mosaic_beam::Table::add` refuses a non-finite altitude
  **[verified by reading]**. The model was measured once the survey's bottom track was in and again on "a full
  bottom-track rework", and an edit kept it, so a recording edited in the session painted with a model the
  reopened one did not have. Since the owner's yes the same evening, `runEditRebuild` measures it again first
  (`beamRemeasureAfterEdit`, docs/mosaic-quality.md, "When it is measured") **[verified: 00014 with pings 20000..45000 (some 25 000 of its 57 079) marked bad: the session measured the model again from 1647 pings a side instead of 3314, the same model as the recording reopened with its edits, and the two mosaics agree to the pixel (0 of 567 644 painted pixels apart); the build before measured nothing after the edit and painted 81.6 % of those pixels another value, up to 67 levels apart (`tools/perf/beam_edit_check.ps1`, 2026-10-19)]**.
  `restartMosaic` itself keeps the model, because its settings changes use it.
- **map-export.** The export paints its mosaic again from the epochs, with a `MosaicProcessor` of its own and
  through `fromIndexMosaicCopy`, so pings left out are out of its mosaic. Its depth grid comes from the 3D bottom
  track, where they are NaN, and `buildDepthGrid` skips non-finite points (`map_export_geo.cpp`) **[verified by
  reading]**. Since the merge the track and the survey's times skip them too (`MapExporter::start`, the table
  above). A side out of the mosaic keeps its depth, so it stays in the grid. An edit made while an export runs
  reaches the slices painted after it: the export reads the live dataset, as it already did with a survey still
  streaming **[inference]**.
- **third-party-icons** (2026-10-19, the icons' licences): independent of this work; merged into `me`
  together with it on 2026-10-19 (merge 5b4dfa5f; both branches' sections at the end of
  `docs/HANDOFF.md` kept). Its `tools/qml_check/about_check.ps1` asks every icon to be Tabler or named in
  `third_party/icons/README.md`: the ten this branch adds are Tabler.

## Not done, not verified

- Not on the water, not with a device streaming: the edits work on live data in memory (not saved); live
  editing was not run.
- The 3D surface after a rebuild was not looked at on screen; the checks look at the mosaic dumps (which
  carry the tiles' heights) and the bottom. The 2D shading and the hand line were looked at on the
  screenshots of the end-to-end run.
- Android and the head unit (touch): the tools take a finger as before (a tool on keeps the finger for
  editing); the three-column palette on a small screen not looked at.
- `KOGGER_SEPARATE_READING` builds: the reading path with processing running is written, not run.
- The Chinese strings are machine translations, not read by a speaker.
- The filters' numbers (the spikes' window and thresholds, the turns' base, radius and wobble rule) are
  this editor's, checked on two recordings of one device (00014, 00008); other devices, ranges and sea
  states not tried **[assumption that they carry over]**.
- Whether a stretch "remove the spikes" changes is a real spike or junk to trim (the start of 00014) is
  the user's judgement; the button makes the line consistent with its neighbours, nothing more.
- The brushes and the side tool work in the 2D echogram only, as the mark and trim tools.
- The ten icons added are Tabler outline icons (MIT), as the palette's others; `THIRD_PARTY_NOTICES.md`
  names no icon set at all (pre-existing, not changed here).

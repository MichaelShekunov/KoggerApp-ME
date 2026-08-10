# Echogram deconvolution

Sharpens the echogram along the track by undoing the smear the sonar's beam
introduces. Added in this fork; not present upstream.

This is the incoherent path of backlog item 2: it works on the amplitude image
and a parametric beam width, not on I/Q. It is **not** synthetic aperture, and
the difference is not cosmetic — see "What this cannot do".

## Using it

Echogram settings panel, under "Source data":

| Control | Meaning |
| --- | --- |
| Deconvolution, beam ° | toggle, plus the sonar's along-track angular resolution in degrees, 0.1 … 30.0 |
| Deconvolution iterations | Richardson-Lucy iterations, 1 … 20, default 8; shown only while the toggle is on |

Off by default. Both values persist per plot under `scene2d/plot2d/<indx>`, like
the theme and compensation settings beside them.

**Live and recorded data go through the same path.** The pass sits in
`Plot2DEchogram::updateCash`, which builds the echogram image out of whatever the
`Dataset` holds; nothing about it distinguishes a streaming device from an
imported file. The one difference a recording can make is navigation: if the file
carries no usable positions the kernel falls back to one metre per ping, and the
angle then reads as a footprint in pings rather than in metres.

### Where the controls live

The visible panel is `qml/app/EchogramSettingsPanel.qml`. It does not talk to the
plot directly — it reads and writes a view-state object whose properties are
`property alias`es onto the controls inside `qml/scene2d/Plot2D.qml`. Those inline
controls hold the value and make the call; the panel is the face.

So a control added only to `Plot2D.qml` works and persists but is invisible, which
is exactly what happened first time round here. Adding one means three edits:
the inline control, an alias in the view-state block (and its `_changeProbe`
entry), and a row in `EchogramSettingsPanel.qml`.

**The beam width is yours to supply.** Nothing in this application knows any
device's directivity, and nothing here should: KoggerApp is open source, and a
measured beam pattern is device characterisation. The operator types the number,
so the pattern arrives at runtime and stays out of this repository.

## What it does

At slant range *R* the beam's along-track footprint is

```
W(R) = 2 * R * tan(theta / 2)
```

so a point target is smeared over `W(R)` metres of track, and the smear **grows
with range** — the far edge of the image is blurrier than the near edge, from
geometry alone. Each row of the echogram is therefore the true reflectivity
convolved with a kernel of its own width, and the deconvolution is one
dimensional, along the track, with a different kernel per row.

Along-track only, on purpose. Across-track resolution comes from pulse length,
not from the beam angle, so applying an angular kernel there would sharpen
nothing and invent detail.

The kernel is a Gaussian with FWHM = `W(R)`, and the solver is Richardson-Lucy:
non-negative and flux-preserving by construction, which is what makes it safe on
intensity data. A linear inverse filter would ring and would clip negative lobes
into black bands around every bright return.

### Navigation

The kernel is in metres; the image is in columns. Navigation supplies the
conversion: the along-track distance between the epochs behind consecutive
columns, from `Position::ned`, preferring the sonar position and falling back to
GNSS. The median across the visible window sizes the kernel, so the same angle
means the same footprint whether the vessel is fast or slow, and whether or not
the display is decimating pings.

If fewer than half the columns carry usable positions the median is not trusted
and one metre per ping is assumed. The control then reads as a footprint in
pings rather than in metres — still useful, no longer physical.

## What this cannot do

Deconvolution recovers detail; it does not create it. A Gaussian of width sigma
suppresses spatial frequency *f* by `exp(-2 pi^2 sigma^2 f^2)`, and past a
certain smear the information is not attenuated but **gone** — no iteration
count brings it back.

Measured by `tools/deconv_check` on a synthetic point target, 20 iterations,
recovery of the true peak:

| smear (sigma, columns) | peak recovered |
| --- | --- |
| 0.7 | 93 % |
| 1.2 | 68 % |
| 1.6 | 50 % |
| 2.4 | 31 % |
| 3.3 | 23 % |
| 5.8 | 13 % |
| 7.4 | 10 % |

So: near-complete restoration up to about one column of smear, useful gain to
about two, and past three it is contrast enhancement rather than resolution
recovery. This is a property of deconvolution, not of this implementation —
Richardson-Lucy was run to 400 iterations against the sigma 7.4 case and moved
the peak from 15.6 to 33 out of 220.

The practical consequence: this helps most where the footprint is already small
— near range, narrow beams, and decimated displays where one column already
spans several pings. It will not rescue the far edge of a wide-beam side-scan.

## Cost

Measured on a 2000 x 800 window with every row populated, 3 iterations: **44 ms**
on one core. Scales linearly in iterations, so the default 8 is about 120 ms,
and the maximum 20 about 290 ms.

It runs on the GUI thread inside `updateCash`, and only when the cache is
invalidated — a new ping, a scroll, a zoom, or a parameter change — not on every
frame. Real echograms are cheaper than the benchmark, because rows that are
entirely blank are skipped.

That is still enough to be felt while scrolling. Two known ways down, neither
done: a recursive (IIR) Gaussian, which makes a blur cost the same regardless of
sigma and would remove the kernel-radius term entirely, or moving the pass to a
worker thread.

## Implementation

`src/processing/echogram_deconvolution.{h,cpp}` holds the solver. It is a free
function over raw pointers with no Qt in it, so `tools/deconv_check` builds it
without a Qt kit and tests it without a GUI.

`Plot2DEchogram::applyDeconvolution` (`src/scene2d/plot2D_echogram.cpp`) adapts
the echogram to it, and two details there matter:

- **It works on a copy.** `_cash` is reused between frames and updated per
  column; deconvolving it in place would sharpen already-sharpened data on every
  repaint, compounding without limit.
- **It undoes the display wrap first.** Columns are stored rotated by
  `wrap_start_pos`, so screen order is not along-track order. Filtering in screen
  order would splice the newest ping against the oldest.

When the pass runs it rewrites the whole image rather than the changed columns,
because a deconvolved column depends on its neighbours: one new ping invalidates
everything within a kernel radius of it.

## Side-scan

Dual-channel side-scan is the case this helps most — along-track smear is
exactly what limits a side-scan image — and it needs no separate mode. One pass
serves both channels, because both are sampled along the same track and the
kernel depends only on the range at a given row.

The layout is `[far ch1 … nadir][nadir … far ch2]`: `Plot2DEchogram::updateCash`
writes the first half through `chartTo`'s `reverse`, so row 0 is the *far* edge of
channel 1 and the nadir sits at the seam. Range falls to the seam and rises
again, and both halves are absolute slant ranges — the sign only says which side.
Kernel width follows that curve, so the sharpening is strongest at the nadir and
weakest at both outer edges, which is what the geometry demands.

Range is *slant* range throughout, which is what the footprint formula wants: the
beam's angular spread projects at the distance the sound actually travelled, not
at its horizontal ground projection.

That mapping lives in `echo_deconv::rowRanges` rather than in the echogram, so
`deconv_check` can test it without Qt. It is there because getting it wrong is
invisible: the picture still looks like an echogram, it is merely sharpened with
the wrong kernel. One case was in fact wrong on the first pass — with the window
scrolled entirely onto one side (`to < 0`) channel 1 covers `[-to, -from]`, not
`[0, -from]`, so the ranges ran down to zero through a nadir that was not on
screen. The regression test covers all three cases: single channel, both sides,
and one side only.

## Verifying it

`tools/deconv_check` is a standalone program with no Qt dependency. It asserts
three things: that the row-to-range mapping is right for all three window layouts
(single channel, side-scan with both sides, side-scan on one side only); that
every row of a deconvolved test scene moved closer to the truth; and that a
one-column smear recovers at least 80 % of a point target's peak. It prints the
recovery table above and the cost measurement.

```
cmake -S tools/deconv_check -B build/deconv_check -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=D:/Qt/Tools/llvm-mingw1706_64/bin/clang++.exe
cmake --build build/deconv_check
build/deconv_check/deconv_check.exe
```

Exit code 0 is a pass.

The guard deliberately asserts only in the regime where the method is supposed to
work. Asserting a number at sigma 7 would enshrine the fact that the information
is gone, and would break the moment anyone improved the solver.

## Known simplifications

- **One spacing for the window.** The kernel is sized from the median along-track
  spacing, not per column. Sizing it per column would track acceleration, but it
  breaks the symmetry Richardson-Lucy needs between its blur and its correction
  step, so it is a separate job rather than a tweak.
- **Attitude is not used yet.** Position gives the along-track spacing; yaw,
  pitch and roll change where the footprint actually lands and are not accounted
  for. Item 2 in the backlog still owns that.
- **The beam is modelled as a Gaussian**, parameterised only by width. A real
  two-way pattern has sidelobes, and they smear differently.

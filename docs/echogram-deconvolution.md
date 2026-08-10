# Echogram deconvolution

Sharpens the echogram along the track by undoing the smear the sonar's beam
introduces. Added in this fork; not present upstream.

This is the incoherent path of backlog item 2: it works on the amplitude image
and a parametric beam width, not on I/Q. It is **not** synthetic aperture, and
the difference is not cosmetic — see "What this cannot do".

## Using it

Echogram settings panel, the "Deconvolution" row:

| Control | Meaning |
| --- | --- |
| On | enable, off by default |
| Beam° | the sonar's along-track angular resolution, degrees, 0.1 … 30.0 |
| Iter | Richardson-Lucy iterations, 1 … 20, default 8 |

Both values persist per plot under `scene2d/plot2d/<indx>`, like the theme and
compensation settings beside them.

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

Row-to-range mapping accounts for the dual-channel layout
`[far ch1 … nadir][nadir … far ch2]`, whose halves run in opposite directions. A
single linear ramp would be wrong for one of them, and wrong in the way that is
hardest to notice: plausible, and mirrored.

## Verifying it

`tools/deconv_check` is a standalone program with no Qt dependency. It builds a
synthetic scene of point targets, smears it with the forward model, deconvolves,
and asserts that every row moved closer to the truth and that a one-column smear
recovers at least 80 % of the peak. It prints the recovery table above and the
cost measurement.

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

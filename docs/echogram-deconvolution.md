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
| Compensate motion (position + pitch) | resample into the measured geometry rather than assuming even pings; **off** by default, see below |

Deconvolution itself is off by default. All three settings persist per plot
under `scene2d/plot2d/<indx>`, like the theme and compensation settings beside
them.

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
entry), and a row in `EchogramSettingsPanel.qml`. All three deconvolution
controls went in that way.

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

### The beam is not uniform across its cone, and that matters twice

The kernel is a **Gaussian**, not a rectangle. That is the point: sensitivity is
highest on the beam axis and falls away towards the edge of the cone, so a target
entering the beam fades in rather than switching on. A rectangular kernel would
assert uniform sensitivity across the cone and a hard edge, and deconvolving with
it would carve those edges into the image as ringing.

The falloff bites a second time, and this one is easy to miss. The pattern applies
on **transmit and again on receive**, so a target's echo follows the product of
the pattern with itself. Squaring a Gaussian narrows it by √2. Since a datasheet
quotes the one-way −3 dB beamwidth — the number an operator will type — the smear
is

```
W(R) = 2 * R * tan(theta / 2) / sqrt(2)
```

Taking the typed angle as the smear directly makes the kernel **41 % too wide**,
and an over-wide Richardson-Lucy kernel does not merely under-sharpen: it gathers
from neighbours that never contributed, and invents structure. That factor was
missing until 2026-08-10.

The solver is Richardson-Lucy: non-negative and flux-preserving by construction,
which is what makes it safe on intensity data. A linear inverse filter would ring
and would clip negative lobes into black bands around every bright return.

### Navigation and attitude

The kernel is in metres; the image is in columns. Navigation supplies the
conversion: the distance between the epochs behind consecutive columns, from
`Position::ned`, preferring the sonar position and falling back to GNSS.

That displacement is **projected onto the heading**, not taken as its own length:

```
step = dn*cos(yaw) + de*sin(yaw)
```

A crabbing vessel covers ground sideways, and sideways motion does not advance
the beam along its own fore-aft axis — which is the axis the smear is along.
Taking `sqrt(dn² + de²)` counts drift as progress and sizes the kernel too
narrow. Yaw comes from `Epoch::tryRetValidYaw`, which falls back to the
artificial attitude; the azimuth convention (from north, clockwise, `dN = cos`)
matches `Plot2D`'s contact projection.

Heave is deliberately **not** added. Vertical motion does not move the footprint
along the track — it changes the range to it. That is range-bin migration, a
different correction, and not done.

If any column lacks a position the pass falls back to one metre per ping. The
control then reads as a footprint in pings rather than in metres — still useful,
no longer physical. The requirement is every column, not most: a hole in the
middle would splice two stretches of track together as though they were adjacent.

### Pitch, and what the resampling is worth

Pitch swings the beam fore and aft, moving the footprint by `R·tan(pitch)`
*whether or not the vessel moves*. At 40 m, one degree is 0.7 m — easily more
than a ping's advance. So the samples along a row are not evenly spaced even at
constant speed, and a convolution in column space is not the operation the water
performed.

`Params::compensateGeometry` resamples each row onto an even along-track grid
built from the measured footprint positions, deconvolves there, and resamples
back. It is **off by default**, and exposed in the panel as "Compensate motion
(position + pitch)".

It was shipped on, tried against a real recording, and turned back off. Both
trials say the same thing.

**Real data first, because it is the more interesting failure.** The far half of
a recorded echogram tore into horizontal smear while the near half looked
correct — the effect appeared to switch on at a particular depth. It did. The
pitch term is `R·tan(pitch)`, proportional to range, so at long range it crowds
neighbouring footprint positions together; the interpolation was running through
nodes that nearly coincided, where a polynomial fit swings wildly. The guard at
the time only demanded that positions be *ordered*, which a separation of a
millimetre satisfies.

Fixed two ways: `kMinStepFraction` now demands each gap be at least a quarter of
the average before a row is resampled at all, and the interpolation onto the grid
is linear rather than cubic — linear cannot overshoot, and echogram geometry
decides node conditioning without telling us. Coming back off the grid is still
Catmull-Rom, where spacing is constant and a cubic is safe.

**Synthetic second.** `deconv_check` blurs a scene in true pitch-perturbed
geometry (±2° at 40 m, a ±2.8 column wander) and asks both paths to undo it,
scoring how much of a target's energy lands within one sigma of its true
position:

| path | energy within one sigma |
| --- | --- |
| assume even pings | **90.7 %** |
| resample into measured geometry | 78.9 % |

The stable interpolation is blunter than the unstable one, so the gap widened
once the tearing was fixed: the sharpness that made these numbers look close was
the same overshoot that wrecked the real image.

Two independent trials against, none in favour. Hence the default.

The honest fix remains a **spatially varying kernel applied in place**, with the
correct adjoint for Richardson-Lucy, so no resampling happens at all. That is
real work rather than a tweak, and the switch stays in the UI so it has something
to be judged against.

Worth keeping in view separately: the larger prize from attitude is probably not
a better kernel at all. It is **un-wobbling the image itself** — pitch walks
targets along the screen by ±2.8 columns in the case above, and nothing here
corrects that, because the result is mapped back onto the columns it came from.
That is a display-geometry feature, not a deconvolution one.

## What this cannot do

Deconvolution recovers detail; it does not create it. A Gaussian of width sigma
suppresses spatial frequency *f* by `exp(-2 pi^2 sigma^2 f^2)`, and past a
certain smear the information is not attenuated but **gone** — no iteration
count brings it back.

Measured by `tools/deconv_check` on a synthetic point target, 20 iterations,
recovery of the true peak:

| smear (sigma, columns) | peak recovered |
| --- | --- |
| 0.5 | 98 % |
| 0.8 | 88 % |
| 1.1 | 71 % |
| 1.7 | 45 % |
| 2.3 | 33 % |
| 4.1 | 18 % |
| 5.2 | 13 % |

So: near-complete restoration up to about one column of smear, useful gain to
about two, and past three it is contrast enhancement rather than resolution
recovery. This is a property of deconvolution, not of this implementation —
Richardson-Lucy was run to 400 iterations against a sigma 7.4 case and moved the
peak from 15.6 to 33 out of 220.

Sigma here is the *smear*, after the two-way narrowing. The same typed beamwidth
therefore lands a row further up this table than it did before that factor was
applied, which is why the numbers improved without the solver changing.

The practical consequence: this helps most where the footprint is already small
— near range, narrow beams, and decimated displays where one column already
spans several pings. It will not rescue the far edge of a wide-beam side-scan.

## Cost

Rows are independent — each is its own 1D problem writing only to its own row —
so the pass is split across worker threads. `Params::threads` selects how many;
0, the default, asks for one per hardware core.

Measured on a 2000 x 800 window with every row populated, 8 iterations:

| threads | ms | speedup |
| --- | --- | --- |
| 1 | 103 | 1.0x |
| 2 | 52 | 2.0x |
| 4 | 42 | 2.5x |
| 8 | 14 | 7.1x |
| 16 | 13 | 7.7x |
| auto (32 cores) | **9** | **12x** |

Scaling is not linear — memory bandwidth, and only 800 rows to divide — but the
serial figure was what made scrolling drag, and 9 ms is comfortably inside a
frame.

Rows are handed out **interleaved**, not in contiguous slices. Kernel radius
grows with range, so a slice-per-thread split would give the far-range worker
several times the work of the near-range one and the whole pass would wait on it.

The pass still runs on the GUI thread inside `updateCash`, and only when the
cache is invalidated — a new ping, a scroll, a zoom, or a parameter change — not
on every frame. Real echograms are cheaper than the benchmark, because blank
rows are skipped.

### OpenCL

There is a GPU path, and it produces a **bit-identical** image to the CPU one —
`deconv_check` compares the two over 100421 samples and finds zero differing.
That is the property that matters: the backend is a speed choice, not a second
algorithm that would make the picture depend on the machine.

Measured on the same 2000 x 800 window:

| backend | ms | vs one core |
| --- | --- | --- |
| cpu, 1 thread | 84 | 1.0x |
| cpu, 8 threads | 13 | 6.5x |
| cpu, all cores (32) | 9 | 9.7x |
| **opencl, RTX 5080** | **6** | **15.0x** |

Note the shape of that: the GPU beats 32 CPU cores by well under 2x. The
arithmetic is trivial for a modern GPU and the pass is dominated by moving a
6.4 MB field across PCIe and back. It earns its place as headroom for heavier
work — a spatially varying kernel, far more iterations, the backlog items that
are GPU-shaped — rather than as a fix for this pass, which the thread pool had
already brought inside a frame.

**Nothing links OpenCL.** `OpenCL.dll` is opened with `LoadLibrary` when the pass
first runs and its entry points are resolved by name, so the build needs no SDK,
`windeployqt` carries nothing extra, and a machine without a runtime keeps the
CPU path instead of failing to start. `Params::useOpenCL` is a preference: the
backend declines what it cannot do — no runtime, no GPU device, or
`compensateGeometry`, whose per-row resampling is sequential in a way the kernel
is not — and the CPU path then runs unchanged.

**Device choice is not GPU-only.** OpenCL is not a GPU API: a CPU device runs the
same kernel across cores with the runtime's own vectorisation, which the scalar
CPU path here does not have. Devices of every type are enumerated and ranked:

1. **discrete GPU** — `CL_DEVICE_HOST_UNIFIED_MEMORY` false;
2. **CPU device** — vectorised, and independent of any display adapter;
3. **integrated GPU** — last, because it shares memory bandwidth with the host
   and can be very small.

That last rank is not snobbery. The integrated part on the development machine
reports **one** compute unit against 32 CPU cores, so choosing it over the thread
pool would be a pessimisation. The ranking picks it only when nothing else is
offered.

What is actually enumerable here, for the record: two platforms, NVIDIA CUDA with
the RTX 5080 (84 compute units, discrete) and AMD Accelerated Parallel Processing
with gfx1036 (1 compute unit, integrated). **No CPU device on either** — current
AMD runtimes no longer expose one and no Intel CPU runtime is installed. The code
path for it exists and is untested for want of a device.

Headers are vendored from Khronos under Apache-2.0, one-way compatible with this
project's GPL-3.0. See `third_party/opencl/README.md`, including why they are not
taken from the CUDA toolkit that also ships them.

If the pass ever needs to be faster still, the next lever is algorithmic rather
than hardware: a recursive (IIR) Gaussian makes a blur cost the same regardless
of sigma, removing the kernel-radius term that dominates at long range.

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

- **Yaw is used; pitch is measured but not applied by default; roll is not used
  at all.** Roll rotates a side-scan fan across track, changing which ground
  patch each range bin holds. That is range-bin migration again, and belongs with
  the mosaic rather than with a display filter.
- **One spacing for the window.** Without `compensateGeometry` the kernel is
  sized from the median projected step, so a row is treated as evenly sampled.
- **The beam is modelled as a Gaussian**, parameterised only by width. A real
  pattern is closer to a sinc in its main lobe and has sidelobes besides;
  sidelobe energy arrives as faint replicas that a sidelobe-free kernel cannot
  remove and Richardson-Lucy may sharpen into artefacts.
- **Only the along-track cut of the beam is used.** Reduced sensitivity towards
  the edges of the fan *across* track — the grazing-angle falloff that makes the
  outer swath dimmer — is an amplitude effect, not a resolution one. TVG and the
  "Source data" compensation modes address it; this pass does not touch it.

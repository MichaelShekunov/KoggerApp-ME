# Relief from the side-scan image

Backlog item 9. Height of objects on the bottom, measured from the shadows they
cast.

This is the geometric half of the idea. The other half — reading slope from
brightness — is not built, deliberately, and the reason is at the end.

## Why shadows and not brightness

The request phrased it as "darker is a slope, lighter is high ground". That works
as an impression and fails as a measurement, because **intensity conflates
geometry with material**. Dark can be a slope facing away, a shadow, soft mud, or
the nadir gap. Bright can be a slope facing the sonar, or gravel, or rock. A
sand-to-mud boundary on a perfectly flat bottom produces exactly the brightness
step that "darker is a slope" reads as topography — plausibly, which is the
dangerous part.

A shadow's length does not have that problem. It is similar triangles:

```
h = L * H / (x + L)
```

for an object at ground range `x` casting a shadow `L` long, with the sonar at
altitude `H`. No backscatter model, no assumption about sediment. The altitude
comes from the bottom track the application already computes.

So this yields a number where shape-from-shading would yield an impression.

## The step that is easy to skip

Side-scan measures **slant** range; the formula wants **ground** range.

```
x = sqrt(slant^2 - H^2)
```

At an altitude of 12 m, a slant range of 13 m is 5 m out along the bottom, not
13. Using slant range directly overstates every height, worst near nadir where
the two diverge most. `groundRange()` does the conversion and `relief_check`
asserts that single case, because it is the one a reader is most likely to assume
away.

## Detection

`echo_relief::findShadows` walks one across-track profile from near to far.

The reference level is the **median** of the usable part of the profile, not the
mean: shadows and bright targets both drag a mean toward whatever it is meant to
be distinguishing. A run of samples below `shadowFraction` of that reference,
lasting at least `minShadowMetres` of ground, is a shadow. The object stands at
its **near** edge — the last place the beam still reached the bottom.

Three refusals, each deliberate:

- **Inside `minGroundRangeM` of nadir, nothing is measured.** The grazing angle
  approaches vertical there, shadows become vanishingly short, and the height
  they imply is mostly noise.
- **A shadow reaching the end of the profile is dropped, not reported.** Its far
  edge is not in view, so its length is a lower bound and its height would be
  understated — quietly, which is worse than absent. `relief_check` asserts this
  rather than tolerating either outcome.
- **Heights above `maxHeightFraction` of the altitude are rejected.** An object
  approaching the sonar's own height casts a shadow that never closes, and the
  formula runs away.

## Verifying it

`tools/relief_check` builds a flat bottom at a known altitude, stands an object
of known height on it, casts the shadow the geometry demands, and asks the
detector for the height back.

```
cmake -S tools/relief_check -B build/relief_check -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=D:/Qt/Tools/llvm-mingw1706_64/bin/clang++.exe
cmake --build build/relief_check
build/relief_check/relief_check.exe
```

Sixteen combinations of range and height, all recovered within 15 % — in practice
within 2 cm on a 12 m altitude. Plus the two hand-worked geometry cases, and the
truncated-shadow refusal. No Qt: the core has none, so this runs anywhere a
compiler does.

That accuracy is a statement about the arithmetic, not about the sea. It says the
formula and the detector agree with the geometry that produced the test. Real
shadows have soft edges, the bottom is not flat, and the altitude carries its own
error.

## What is not built, and what it would need first

Shape-from-shading — the continuous relief between objects — is not here.

Beyond being ill-posed, it has a prerequisite this does not: **the image already
carries a systematic across-track brightness gradient** that has nothing to do
with relief, from beam pattern, grazing angle and TVG together. Read as slope, it
would tilt the entire swath. That gradient has to be flattened before brightness
says anything about geometry, and the existing "Source data" compensation modes
are the beginning of an answer rather than the whole of one.

When it is attempted, it should be **anchored** rather than integrated free.
Shape-from-shading recovers gradients, and integrating gradients drifts, so the
far edge of a swath ends up metres out with nothing to catch it. `SurfaceProcessor`
already builds a surface from bottom-track soundings; using it as the
low-frequency constraint and letting brightness supply only the detail between
soundings turns an unconstrained integration into an interpolation with known
endpoints.

Backlog item 5 is also waiting on this: it records that a side-scan sample maps
to an arc rather than a point unless a flat bottom is assumed. Measured relief
removes that assumption.

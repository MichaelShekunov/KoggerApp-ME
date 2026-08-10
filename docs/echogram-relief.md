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

## Filling between survey lines — attempted, not working

The intended use is specific: the bottom track gives depth only under the boat,
so a surface built from it interpolates blindly between survey lines, while the
swath covers that gap. `acrossTrackRelief` turns swath brightness into relief for
that gap.

The model, for small slopes. With the sonar at altitude `H` and grazing angle
`phi` at ground range `x`, brightness goes as `K(x) * (sin(phi) + s*cos(phi))`
for an across-track slope `s = dz/dx`. Dividing out `sin(phi)`, which is known
exactly from the altitude, leaves

```
u = I / sin(phi) = K(x) * (1 + s*x/H)
```

so a sample's ratio to a fitted `K` gives `s = (r - 1) * H / x`, and `z` follows
by integrating outward from the nadir.

Note what `H/x` does: at the outer swath, where the beam grazes, the same
brightness change means a far **smaller** slope, because grazing incidence is
violently sensitive to slope. Omitting that factor exaggerates the outer swath,
which is exactly where the fill matters most.

### It does not yet work, and here is the number

`relief_check` renders a bottom of known shape through that reflectance model
with a constant material term — the most favourable case there is, no sediment
change, no noise, nothing clipped — and asks for the shape back:

| | recovered rms | true rms | residual |
| --- | --- | --- | --- |
| free integration | 0.37 m | 0.50 m | **75 %** |
| tied to both survey lines | 0.71 m | 0.69 m | **102 %** |

A residual near 100 % means the output carries about as much error as there is
relief to find. It is not fit to drape a surface on, and it is not enabled
anywhere in the application.

The check **reports these rather than asserting them**. Gating on the figure
would either block the shadow work that does pass, or invite tuning the threshold
until it went green — which is the same as deleting the measurement.

### Why, specifically

The fit that removes the material term also removes relief. A quadratic in log
range cannot tell a slowly varying sediment change from a slowly varying hill,
because within one profile **they are the same signal**. What survives the fit
integrates into a drift, and anchoring to the survey lines then tilts that drift
rather than curing it — which is why the anchored figure is worse than the free
one, not better.

That the anchoring makes it worse is the informative part. It says the error is
not a constant offset the endpoints could absorb, but a shape error distributed
along the profile.

### What would have to change

Not tuning. Three candidates, in the order I would try them:

- **Use both sides of the boat.** Port and starboard see the same seabed with
  opposite slope sign, so a real slope flips sign between them while a sediment
  change does not. That is an actual discriminator between the two things the
  single-profile method cannot separate, and it needs no new data.
- **Use the overlap between adjacent lines.** Where two swaths cover the same
  ground from opposite directions, the same argument applies over a much longer
  baseline, and the survey already produces the overlap.
- **Solve the whole grid at once**, as a boundary-value problem over the mosaic
  with the bottom track as hard constraints, instead of integrating each profile
  independently and hoping the ends line up.

The first two are the reason to keep this rather than delete it: the physics is
right and the estimator is under-determined, which is a different problem from
the physics being wrong.

## What is not built at all

Anything that writes into the surface. `SurfaceProcessor` builds the mesh from
bottom-track soundings and nothing here touches it — deliberately, since feeding
it a 100 %-residual estimate would make inferred relief indistinguishable from
measured soundings inside the same surface.

Backlog item 5 is also waiting on this: it records that a side-scan sample maps
to an arc rather than a point unless a flat bottom is assumed. Measured relief
would remove that assumption; this estimate is not yet good enough to.

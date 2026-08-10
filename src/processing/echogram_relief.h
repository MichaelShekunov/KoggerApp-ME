#pragma once

#include <cstdint>

// Object height from acoustic shadow, on a side-scan profile.
//
// This is the geometric half of "relief from the side-scan image". It is first
// because it is the half that yields a number: a shadow's length and the sonar's
// altitude give a height by similar triangles, with no model of how the bottom
// scatters sound. Recovering slope from brightness needs such a model and is
// ill-posed - sediment changes intensity more than a few degrees of slope do.
namespace echo_relief {

struct Params {
    // A sample counts as shadow below this fraction of the profile's reference
    // level. Shadows are near-zero returns, so this is not delicate; too high
    // starts swallowing merely-dark sediment.
    float shadowFraction = 0.30f;

    // Shorter runs are noise or speckle, not shadows. Ground metres.
    float minShadowMetres = 0.5f;

    // Ignore everything inside this ground range. Near nadir the geometry is
    // degenerate - the grazing angle approaches vertical, so shadows are
    // vanishingly short and the height they imply is dominated by noise.
    float minGroundRangeM = 2.0f;

    // Heights above this are rejected as implausible for a shadow measurement:
    // an object taller than the sonar's altitude cannot cast a shadow that
    // closes, and the formula runs away as the object approaches the towfish.
    float maxHeightFraction = 0.8f; // of altitude
};

struct Shadow {
    int   firstSample = 0;    // index of the shadow's near edge
    int   lastSample  = 0;
    float groundRangeM = 0.0f; // where the object stands
    float lengthM      = 0.0f; // shadow length, ground metres
    float heightM      = 0.0f; // implied object height
};

// Ground range on a flat bottom, given the slant range and the sonar's altitude
// above that bottom. Returns 0 inside the water column, where a slant range
// shorter than the altitude cannot correspond to a bottom return at all.
float groundRange(float slantRangeM, float altitudeM);

// Height of an object whose shadow begins at ground range x and runs L metres
// further, with the sonar at altitude H:
//
//     h = L * H / (x + L)
//
// From similar triangles: the ray leaving the sonar at height H and grazing the
// object's top at height h reaches the bottom L further on. Nothing about
// backscatter enters it, which is the whole appeal.
float heightFromShadow(float groundRangeM, float shadowLengthM, float altitudeM);

// ---------------------------------------------------------------------------
// Across-track relief, for filling the gap between survey lines.
//
// The bottom track gives depth only under the boat, so a surface built from it
// alone interpolates blindly between lines. The swath covers that gap; this
// turns its brightness into the relief there.
//
// The slope model, for small slopes: with the sonar at altitude H and a grazing
// angle phi at ground range x, brightness goes as sin(phi) + s*cos(phi) for a
// across-track slope s = dz/dx. So a sample's ratio r to the local trend gives
//
//     s = (r - 1) * H / x
//
// and z comes from integrating s outward from the nadir, where the bottom track
// already knows the depth.
//
// Note what H/x does: at the far edge, where the beam grazes, the same
// brightness change means a far *smaller* slope, because grazing incidence is
// violently sensitive to slope. Reading brightness as slope without that factor
// exaggerates the outer swath, which is exactly where the fill matters most.

struct ReliefParams {
    // Width of the trend estimate, as a fraction of the profile. The trend
    // carries beam pattern, TVG, mean grazing-angle falloff and mean sediment -
    // everything that is not relief. Wider keeps more real relief in the
    // residual and more artefact with it.
    float trendFraction = 0.25f;

    // Slopes beyond this are refused. Brightness that implies a cliff is far
    // more likely to be a sediment boundary, and letting it through would put a
    // step in the surface that looks like measurement.
    float maxSlope = 0.6f;   // ~31 degrees

    // As findShadows: nothing is inferred inside this ground range.
    float minGroundRangeM = 2.0f;
};

// Writes, for each usable sample, its ground range and the bottom elevation
// there relative to the depth under the boat - positive is shallower.
//
// Returns how many samples were written. The caller places these across track,
// perpendicular to the heading, at the depth the bottom track gives for the ping.
int acrossTrackRelief(const int16_t* samples, const float* slantRangeAt, int count,
                      float altitudeM, const ReliefParams& params,
                      float* groundRangeOut, float* elevationOut, int maxOut);

// Finds shadows in one across-track profile running near to far.
//
// `samples` are 0..255 amplitudes; `slantRangeAt` gives each sample's slant
// range in metres and must increase. `altitudeM` is the sonar's height above the
// bottom, which the bottom track already computes per ping.
//
// Returns how many shadows were written to `out`, up to `maxOut`.
int findShadows(const int16_t* samples, const float* slantRangeAt, int count,
                float altitudeM, const Params& params, Shadow* out, int maxOut);

} // namespace echo_relief

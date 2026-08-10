#pragma once

#include <cstdint>

// Along-track deconvolution of an echogram.
//
// At slant range R the beam's along-track footprint is W(R) = 2*R*tan(theta/2),
// so every row of the echogram is the true reflectivity smeared by a kernel
// whose width grows with range. Undoing that smear is what this does.
//
// Along-track only. Across-track resolution is set by pulse length, not by the
// beam angle, so applying an angular kernel there would sharpen nothing and
// invent detail.
namespace echo_deconv {

struct Params {
    // The sonar's along-track beamwidth, degrees, as a datasheet states it:
    // one-way, -3 dB. Operator-supplied - no beam pattern is built into this
    // application. The two-way narrowing is applied internally, see
    // footprintMetres.
    float angularResolutionDeg = 1.0f;

    // Richardson-Lucy iterations. Each one sharpens and amplifies noise a
    // little more; past ~10 the gain is mostly speckle.
    int iterations = 8;

    // Rows whose kernel is narrower than this many columns are passed through
    // untouched - there is nothing to undo, and a sub-pixel kernel only
    // amplifies noise.
    float minSigmaColumns = 0.4f;

    // Hard ceiling on kernel half-width in columns. Bounds the cost at long
    // range, where the footprint can exceed the visible window.
    int maxKernelRadius = 64;

    // Worker threads. 0 asks for one per hardware core. Rows are independent -
    // each is its own 1D problem writing only to its own row - so this scales
    // until the rows run out.
    int threads = 0;

    // Resample each row onto an even along-track grid before deconvolving, using
    // the measured footprint positions, and back afterwards.
    //
    // Off, on the evidence of two independent trials.
    //
    // Synthetic: `deconv_check` blurs a scene in true pitch-perturbed geometry
    // and asks both paths to undo it. Assuming even pings gathers 90.7 % of a
    // target's energy within one sigma of the truth; resampling gathers 78.9 %.
    // An echogram carries barely one sample per sigma, and a round trip through
    // interpolation costs more resolution than correcting the kernel width
    // recovers.
    //
    // Real data: it tore the far half of a recorded echogram. The pitch term
    // scales with range, so at long range it crowded the footprint positions
    // together and the interpolation went unstable - visible as a smear that
    // began at a particular depth and worsened outward. That instability is
    // fixed (see kMinStepFraction, and the interpolants), but fixing it removed
    // the sharpness that made the synthetic numbers look close in the first
    // place.
    //
    // Still exposed in the UI, because the honest fix - a spatially varying
    // kernel applied in place, with the correct Richardson-Lucy adjoint and no
    // resampling at all - would be judged against this switch.
    //
    // Positions still improve the pass when this is off: they set the kernel
    // width in real metres. This flag governs only the resampling.
    bool compensateGeometry = false;
};

// Result of one pass, for reporting and for tests.
struct Stats {
    int rowsProcessed = 0;
    int rowsSkipped   = 0;
    int maxRadius     = 0;

    // Rows that wanted the spatial path but fell back to even sampling because
    // the footprint positions were not monotonic - pitch swinging faster than
    // the vessel advances really can walk the footprint backwards.
    int rowsNonMonotonic = 0;

    // Whether the pass used measured geometry at all, or assumed even pings.
    bool spatial = false;
};

// `columns` holds `width` pointers to `height` samples each, in *along-track
// order* - consecutive entries must be consecutive pings, so the caller undoes
// any display wrap first. Values are 0..255 amplitude and are modified in place.
//
// `rangeAt` is `height` slant ranges in metres, one per row, already absolute.
// A non-positive range marks a row as unusable and skips it.
//
// `alongTrackM` is `width` cumulative along-track positions in metres, one per
// column - the ping's displacement projected onto the vessel's heading, so a
// crab angle does not read as forward progress. nullptr means no navigation:
// the pass then assumes even pings at `fallbackSpacingM`.
//
// `pitchRad` is `width` pitch angles. Pitch swings the beam fore and aft, moving
// the footprint by R*tan(pitch) *independently of the vessel*, and at 50 m one
// degree is 0.87 m - easily more than a ping's advance. Supplying it turns the
// column positions from "where the boat was" into "where the beam looked", which
// is what the convolution is actually over. nullptr skips that correction.
//
// With positions in hand the row is resampled onto an even along-track grid,
// deconvolved there, and resampled back, so uneven speed and pitch oscillation
// stop being modelled as a single average spacing.
Stats alongTrack(int16_t* const* columns, int width, int height,
                 const float* rangeAt, const float* alongTrackM,
                 const float* pitchRad, float fallbackSpacingM,
                 const Params& params);

// Exposed for tests: the along-track *smear* in metres at a slant range - the
// geometric arc the beam subtends, narrowed by the two-way factor because the
// pattern applies on transmit and again on receive.
float footprintMetres(float rangeM, float angularResolutionDeg);

// Slant range per row, `height` entries, for the echogram's column layout.
//
// Single channel: rows run linearly from `from` to `to`.
//
// Dual channel (side-scan): the visible window is signed and the column is
// [far ch1 .. nadir][nadir .. far ch2] - Plot2DEchogram writes the first half
// with chartTo's `reverse`, so row 0 is the *far* edge of channel 1 and the
// nadir sits at the seam. Range therefore falls to the seam and rises again.
// Both halves are absolute slant ranges; the sign only says which side.
//
// Mirrors the split in Plot2DEchogram::updateCash. If that changes, this must.
void rowRanges(float from, float to, int height, bool dualChannel, float* out);

} // namespace echo_deconv

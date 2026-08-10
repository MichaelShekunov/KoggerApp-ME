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
    // Along-track angular resolution of the sonar, degrees. Operator-supplied:
    // no beam pattern is built into this application.
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
};

// Result of one pass, for reporting and for tests.
struct Stats {
    int rowsProcessed = 0;
    int rowsSkipped   = 0;
    int maxRadius     = 0;
};

// `columns` holds `width` pointers to `height` samples each, in *along-track
// order* - consecutive entries must be consecutive pings, so the caller undoes
// any display wrap first. Values are 0..255 amplitude and are modified in place.
//
// `rangeAt` is `height` slant ranges in metres, one per row, already absolute.
// A non-positive range marks a row as unusable and skips it.
//
// `spacingM` is the along-track distance in metres between consecutive pings,
// from navigation. The kernel is sized from the median of these, so a column
// count means the same thing whatever the vessel's speed. Pass nullptr when no
// navigation is available and the caller supplies `fallbackSpacingM` instead.
Stats alongTrack(int16_t* const* columns, int width, int height,
                 const float* rangeAt, const float* spacingM,
                 float fallbackSpacingM, const Params& params);

// Exposed for tests: the along-track footprint in metres at a slant range.
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

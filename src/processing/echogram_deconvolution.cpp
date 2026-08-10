#include "echogram_deconvolution.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

namespace echo_deconv {

namespace {

constexpr float kDegToRad   = 3.14159265358979323846f / 180.0f;
constexpr float kFwhmToSigma = 1.0f / 2.35482004503f;
constexpr float kEps        = 1.0e-3f;

// Two-way narrowing: 1/sqrt(2). See footprintMetres.
constexpr float kTwoWayNarrowing = 0.70710678f;

// A row is only resampled if its closest pair of footprint positions is still
// this fraction of the average apart. Interpolating through nodes that nearly
// coincide produces wild excursions, and the pitch term crowds them together at
// long range - so without this the far half of an echogram tears while the near
// half looks correct.
constexpr float kMinStepFraction = 0.25f;
constexpr float kMaxValue   = 255.0f;

// Replicate the edge rather than zero-padding: a dark border would be read as
// real, and Richardson-Lucy would then sharpen it into a bright rim.
inline float sampleClamped(const std::vector<float>& v, int i)
{
    if (i < 0) { return v.front(); }
    if (i >= int(v.size())) { return v.back(); }
    return v[size_t(i)];
}

// The kernel is symmetric, so each weight serves both sides and the inner loop
// runs to radius rather than 2*radius. Interior samples skip the edge clamp
// entirely, which is where all but a kernel's width of the work happens.
void convolve(const std::vector<float>& src, std::vector<float>& dst,
              const std::vector<float>& kernel, int radius)
{
    const int n = int(src.size());
    const float* s = src.data();
    const float* w = kernel.data() + radius; // w[0] is the centre tap

    for (int i = 0; i < n; ++i) {
        float acc = s[i] * w[0];
        if (i >= radius && i < n - radius) {
            for (int k = 1; k <= radius; ++k) {
                acc += (s[i - k] + s[i + k]) * w[k];
            }
        }
        else {
            for (int k = 1; k <= radius; ++k) {
                acc += (sampleClamped(src, i - k) + sampleClamped(src, i + k)) * w[k];
            }
        }
        dst[size_t(i)] = acc;
    }
}

// Resampling: linear going onto the even grid, where node spacing is whatever
// the vessel and the swell decided, and Catmull-Rom coming back off it, where
// spacing is constant and a cubic is safe.

// Catmull-Rom, for going back onto the even grid where spacing is constant.
inline float catmullRom(float ym1, float y0, float y1, float y2, float t)
{
    const float a0 = 2.0f * y0;
    const float a1 = -ym1 + y1;
    const float a2 = 2.0f * ym1 - 5.0f * y0 + 4.0f * y1 - y2;
    const float a3 = -ym1 + 3.0f * y0 - 3.0f * y1 + y2;
    return 0.5f * (a0 + a1 * t + a2 * t * t + a3 * t * t * t);
}

float medianOf(std::vector<float> v)
{
    if (v.empty()) { return 0.0f; }
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    return v[mid];
}

} // namespace


float footprintMetres(float rangeM, float angularResolutionDeg)
{
    if (!(rangeM > 0.0f) || !(angularResolutionDeg > 0.0f)) {
        return 0.0f;
    }
    // The geometric arc the beam subtends...
    const float arc = 2.0f * rangeM * std::tan(0.5f * angularResolutionDeg * kDegToRad);

    // ...but the smear is narrower than the beam, because the beam is used
    // twice. Sensitivity falls off towards the edge of the cone on transmit and
    // again on receive, so a target's echo follows the *product* of the pattern
    // with itself. Squaring a Gaussian narrows it by sqrt(2), and a datasheet
    // quotes the one-way -3 dB width, which is what an operator will type.
    //
    // Treating the typed angle as the smear directly makes the kernel 41 % too
    // wide, and an over-wide Richardson-Lucy kernel does not merely under-sharpen
    // - it pulls in neighbours that never contributed and invents structure.
    return arc * kTwoWayNarrowing;
}


void rowRanges(float from, float to, int height, bool dualChannel, float* out)
{
    if (!out || height <= 0) {
        return;
    }

    if (!dualChannel) {
        for (int row = 0; row < height; ++row) {
            const float d = from + (to - from) * (float(row) + 0.5f) / float(height);
            out[row] = std::fabs(d);
        }
        return;
    }

    const float fullrange = to - from;
    if (!(fullrange > 0.0f)) {
        for (int row = 0; row < height; ++row) { out[row] = 0.0f; }
        return;
    }

    // Channel 1 spans [from1, to1] and is written reversed, so row 0 holds to1.
    // With the whole window on one side (to < 0) it starts at -to, not at the
    // nadir - there is no nadir in view to start from.
    const float range1 = (to >= 0.0f) ? (0.0f - from) : (to - from);
    const float near1  = (to >= 0.0f) ? 0.0f : -to;
    const float far1   = -from;

    int part1 = int(float(height) * (range1 / fullrange));
    if (part1 < 0)      { part1 = 0; }
    if (part1 > height) { part1 = height; }

    for (int row = 0; row < part1; ++row) {
        const float t = (float(row) + 0.5f) / float(part1);
        out[row] = std::fabs(far1 + (near1 - far1) * t);
    }

    const int part2 = height - part1;
    if (part2 <= 0) {
        return;
    }
    const float near2 = (from >= 0.0f) ? from : 0.0f;
    const float far2  = to;
    for (int row = part1; row < height; ++row) {
        const float t = (float(row - part1) + 0.5f) / float(part2);
        out[row] = std::fabs(near2 + (far2 - near2) * t);
    }
}


Stats alongTrack(int16_t* const* columns, int width, int height,
                 const float* rangeAt, const float* alongTrackM,
                 const float* pitchRad, float fallbackSpacingM,
                 const Params& params)
{
    Stats stats;
    if (!columns || width < 3 || height <= 0 || !rangeAt) {
        return stats;
    }
    if (!(params.angularResolutionDeg > 0.0f) || params.iterations <= 0) {
        return stats;
    }

    // Spatial mode needs a position for every column: a hole in the middle would
    // silently splice two stretches of track together as if they were adjacent.
    bool spatial = alongTrackM != nullptr;
    if (spatial) {
        for (int c = 0; c < width; ++c) {
            if (!std::isfinite(alongTrackM[c])) { spatial = false; break; }
        }
    }
    const bool usePitch = spatial && pitchRad != nullptr;
    stats.spatial = spatial;

    // Even-ping fallback, and the grid step when there is no navigation.
    float spacing = fallbackSpacingM;
    if (spatial) {
        std::vector<float> steps;
        steps.reserve(size_t(width));
        for (int c = 1; c < width; ++c) {
            const float d = alongTrackM[c] - alongTrackM[c - 1];
            if (std::isfinite(d) && d > 0.0f) { steps.push_back(d); }
        }
        if (int(steps.size()) * 2 >= width) {
            spacing = medianOf(std::move(steps));
        }
        else {
            // Positions exist but the platform is not advancing - station
            // keeping, or a recording with a frozen fix. Nothing to deconvolve
            // along a track that is not moving.
            spatial = false;
            stats.spatial = false;
        }
    }
    if (!(spacing > 0.0f)) {
        return stats;
    }

    // Each row is an independent 1D problem that writes only to its own row, so
    // ranges of rows can go to separate threads with no sharing beyond the
    // read-only inputs. Scratch is per range, not per row.
    int nThreads = params.threads > 0
                     ? params.threads
                     : int(std::thread::hardware_concurrency());
    if (nThreads < 1) { nThreads = 1; }
    nThreads = std::min(nThreads, height);
    // Below this the thread hand-off costs more than the work saved.
    if (height < 8) { nThreads = 1; }

    std::vector<Stats> partial(static_cast<size_t>(nThreads), Stats{});

    auto processRows = [&](int rowBegin, int rowStride, Stats& out) {
        const size_t nCols = static_cast<size_t>(width);
        std::vector<float> observed(nCols, 0.0f);
        std::vector<float> estimate(nCols, 0.0f);
        std::vector<float> blurred(nCols, 0.0f);
        std::vector<float> ratio(nCols, 0.0f);
        std::vector<float> correction(nCols, 0.0f);
        std::vector<float> kernel;
        std::vector<float> footprintX(nCols, 0.0f); // where the beam looked, metres
        std::vector<float> resampled(nCols, 0.0f);

        for (int row = rowBegin; row < height; row += rowStride) {
            const float range = rangeAt[row];

            // Where each ping's footprint actually sat along the track. Pitch
            // moves it by R*tan(pitch) whether or not the vessel moved, so this
            // is the axis the smear is really along - not the ping index, and
            // not the vessel's position either.
            // Positions always set the kernel width; they only reshape the row
            // when geometry compensation is asked for. See Params.
            bool rowSpatial = spatial && params.compensateGeometry;
            float step = spacing;
            if (rowSpatial) {
                for (int c = 0; c < width; ++c) {
                    float x = alongTrackM[c];
                    if (usePitch && std::isfinite(pitchRad[c])) {
                        x += range * std::tan(pitchRad[c]);
                    }
                    footprintX[size_t(c)] = x;
                }
                // Ordered is not enough. The pitch term scales with range, so at
                // long range it crowds neighbouring samples together, and an
                // interpolation through nearly coincident nodes is unstable no
                // matter how ordered they are - that is what smeared the far
                // half of the image while the near half looked fine.
                //
                // Demand real separation, not merely positive separation.
                const float span = footprintX[size_t(width - 1)] - footprintX[0];
                const float mean = span / float(width - 1);
                float minStep = footprintX[1] - footprintX[0];
                for (int c = 2; c < width; ++c) {
                    minStep = std::min(minStep, footprintX[size_t(c)] - footprintX[size_t(c - 1)]);
                }
                if (!(mean > 0.0f) || !(minStep > kMinStepFraction * mean)) {
                    rowSpatial = false;
                    ++out.rowsNonMonotonic;
                }
                else {
                    step = mean;
                }
            }
            if (!(step > 0.0f)) {
                ++out.rowsSkipped;
                continue;
            }

            const float sigma = footprintMetres(range, params.angularResolutionDeg)
                                * kFwhmToSigma / step;

            if (!(sigma > params.minSigmaColumns)) {
                ++out.rowsSkipped;
                continue;
            }

            int radius = int(std::ceil(3.0f * sigma));
            radius = std::min(radius, params.maxKernelRadius);
            radius = std::min(radius, (width - 1) / 2);
            if (radius < 1) {
                ++out.rowsSkipped;
                continue;
            }
            out.maxRadius = std::max(out.maxRadius, radius);

            kernel.assign(size_t(2 * radius + 1), 0.0f);
            float sum = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                const float x = float(k) / sigma;
                const float w = std::exp(-0.5f * x * x);
                kernel[size_t(k + radius)] = w;
                sum += w;
            }
            for (float& w : kernel) { w /= sum; }

            float rowMax = 0.0f;
            for (int c = 0; c < width; ++c) {
                const float v = float(columns[c][row]);
                observed[size_t(c)] = v > 0.0f ? v : 0.0f;
                estimate[size_t(c)] = observed[size_t(c)];
                rowMax = std::max(rowMax, observed[size_t(c)]);
            }

            // Water above the surface and everything past the bottom are flat
            // zero on a real echogram, and a blank row has nothing to sharpen.
            if (rowMax <= 0.0f) {
                ++out.rowsSkipped;
                continue;
            }

            // Onto an even grid, so the convolution the solver assumes matches
            // the one the water performed. Both sequences ascend, so one march
            // through each is enough.
            if (rowSpatial) {
                // Linear, on purpose. A cubic through unevenly spaced nodes is
                // sharper when the nodes are well conditioned and catastrophic
                // when they are not, and echogram geometry decides which without
                // telling us. Linear cannot overshoot; the value always lies
                // between its two neighbours.
                const float x0 = footprintX[0];
                int src = 0;
                for (int j = 0; j < width; ++j) {
                    const float gx = x0 + float(j) * step;
                    while (src + 2 < width && footprintX[size_t(src + 1)] < gx) { ++src; }
                    const float xa = footprintX[size_t(src)];
                    const float xb = footprintX[size_t(src + 1)];
                    const float t  = (xb > xa) ? std::clamp((gx - xa) / (xb - xa), 0.0f, 1.0f) : 0.0f;
                    resampled[size_t(j)] = observed[size_t(src)] * (1.0f - t)
                                         + observed[size_t(src + 1)] * t;
                }
                std::copy(resampled.begin(), resampled.end(), observed.begin());
                std::copy(resampled.begin(), resampled.end(), estimate.begin());
            }

            // Richardson-Lucy. Non-negative and flux-preserving by
            // construction, which is what makes it safe on intensity data: no
            // ringing, no negative lobes to clip into black bands.
            for (int it = 0; it < params.iterations; ++it) {
                convolve(estimate, blurred, kernel, radius);
                for (int c = 0; c < width; ++c) {
                    const float b = blurred[size_t(c)];
                    ratio[size_t(c)] = observed[size_t(c)] / (b > kEps ? b : kEps);
                }
                // The kernel is symmetric, so its transpose is itself and the
                // correction step is another convolution with the same weights.
                convolve(ratio, correction, kernel, radius);
                for (int c = 0; c < width; ++c) {
                    estimate[size_t(c)] *= correction[size_t(c)];
                }
            }

            // Back off the even grid onto the columns the screen actually has.
            if (rowSpatial) {
                const float x0 = footprintX[0];
                for (int c = 0; c < width; ++c) {
                    const float g = (footprintX[size_t(c)] - x0) / step;
                    const int   j = std::clamp(int(g), 0, width - 2);
                    const float t = std::clamp(g - float(j), 0.0f, 1.0f);
                    const float ym1 = estimate[size_t(std::max(j - 1, 0))];
                    const float y0  = estimate[size_t(j)];
                    const float y1  = estimate[size_t(j + 1)];
                    const float y2  = estimate[size_t(std::min(j + 2, width - 1))];
                    resampled[size_t(c)] = std::max(0.0f, catmullRom(ym1, y0, y1, y2, t));
                }
                std::copy(resampled.begin(), resampled.end(), estimate.begin());
            }

            for (int c = 0; c < width; ++c) {
                const float v = estimate[size_t(c)];
                columns[c][row] = int16_t(std::lround(std::clamp(v, 0.0f, kMaxValue)));
            }
            ++out.rowsProcessed;
        }
    };

    if (nThreads <= 1) {
        processRows(0, 1, partial[0]);
    }
    else {
        // Interleave rather than slice: kernel radius grows with range, so
        // contiguous blocks would hand the far-range worker several times the
        // work of the near-range one and the pass would wait on it. Scratch is
        // allocated once per worker, not once per row.
        std::vector<std::thread> workers;
        workers.reserve(size_t(nThreads - 1));
        for (int t = 1; t < nThreads; ++t) {
            workers.emplace_back([&, t] { processRows(t, nThreads, partial[size_t(t)]); });
        }
        processRows(0, nThreads, partial[0]);
        for (std::thread& w : workers) { w.join(); }
    }

    for (const Stats& s : partial) {
        stats.rowsProcessed += s.rowsProcessed;
        stats.rowsSkipped   += s.rowsSkipped;
        stats.maxRadius      = std::max(stats.maxRadius, s.maxRadius);
    }

    return stats;
}

} // namespace echo_deconv

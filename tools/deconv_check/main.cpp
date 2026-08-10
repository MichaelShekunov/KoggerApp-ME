// Does the deconvolution actually undo the beam's along-track smear?
//
// Builds a synthetic scene of point targets, smears it with the same footprint
// model the deconvolver assumes, then checks that running the deconvolution
// moves the image back towards the truth rather than merely changing it.
#include "echogram_deconvolution.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr float kSpacingM   = 0.5f;   // metres between pings
constexpr float kAngleDeg   = 5.0f;   // along-track angular resolution
constexpr int   kWidth      = 512;    // pings
constexpr int   kHeight     = 64;     // range bins
constexpr float kRangeNear  = 10.0f;
constexpr float kRangeFar   = 100.0f;
constexpr int   kTargetStep = 64;     // one point target every N pings
constexpr float kBackground = 4.0f;
constexpr float kTarget     = 220.0f;

float rangeOfRow(int row)
{
    return kRangeNear + (kRangeFar - kRangeNear) * float(row) / float(kHeight - 1);
}

// The same Gaussian the deconvolver builds, used here as the forward model.
std::vector<float> psfFor(float range, int& radiusOut)
{
    const float fwhm  = echo_deconv::footprintMetres(range, kAngleDeg);
    const float sigma = fwhm / 2.35482004503f / kSpacingM;
    int radius = int(std::ceil(3.0f * sigma));
    if (radius < 1) { radius = 1; }
    radiusOut = radius;

    std::vector<float> k(size_t(2 * radius + 1), 0.0f);
    float sum = 0.0f;
    for (int i = -radius; i <= radius; ++i) {
        const float x = float(i) / sigma;
        const float w = std::exp(-0.5f * x * x);
        k[size_t(i + radius)] = w;
        sum += w;
    }
    for (float& w : k) { w /= sum; }
    return k;
}

double rmse(const std::vector<float>& a, const std::vector<float>& b)
{
    double acc = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double d = double(a[i]) - double(b[i]);
        acc += d * d;
    }
    return std::sqrt(acc / double(a.size()));
}

} // namespace


// Row -> range for the side-scan column layout. Checked separately because a
// mistake here is invisible: the image still looks like an echogram, it is just
// sharpened with the wrong kernel.
bool checkRowRanges()
{
    bool ok = true;
    constexpr int h = 200;
    std::vector<float> r(size_t(h), 0.0f);

    auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

    // Single channel, 5 m to 60 m: monotonically increasing, endpoints in range.
    echo_deconv::rowRanges(5.0f, 60.0f, h, false, r.data());
    for (int i = 1; i < h; ++i) {
        if (r[size_t(i)] < r[size_t(i - 1)]) { ok = false; }
    }
    if (!near(r[0], 5.0f, 0.5f) || !near(r[size_t(h - 1)], 60.0f, 0.5f)) { ok = false; }
    std::printf("  single  [5..60]      row0=%.1f  rowN=%.1f  %s\n",
                r[0], r[size_t(h - 1)], ok ? "ok" : "BAD");

    // Side-scan, both sides visible: far port -> nadir -> far starboard, so the
    // range falls to a minimum near the seam and rises again.
    echo_deconv::rowRanges(-50.0f, 50.0f, h, true, r.data());
    int minAt = 0;
    for (int i = 1; i < h; ++i) {
        if (r[size_t(i)] < r[size_t(minAt)]) { minAt = i; }
    }
    const bool seamCentred = minAt > h / 2 - 5 && minAt < h / 2 + 5;
    const bool endsFar = near(r[0], 50.0f, 1.0f) && near(r[size_t(h - 1)], 50.0f, 1.0f);
    if (!seamCentred || !endsFar) { ok = false; }
    std::printf("  dual    [-50..50]    row0=%.1f  nadir at row %d of %d  rowN=%.1f  %s\n",
                r[0], minAt, h, r[size_t(h - 1)], (seamCentred && endsFar) ? "ok" : "BAD");

    // Side-scan scrolled entirely onto one side: no nadir in view. Channel 1
    // covers [-to, -from], so the range runs 40 -> 10 and never reaches zero.
    echo_deconv::rowRanges(-40.0f, -10.0f, h, true, r.data());
    const bool oneSided = near(r[0], 40.0f, 1.0f) && near(r[size_t(h - 1)], 10.0f, 1.0f);
    bool descends = true;
    for (int i = 1; i < h; ++i) {
        if (r[size_t(i)] > r[size_t(i - 1)] + 1.0e-3f) { descends = false; }
    }
    if (!oneSided || !descends) { ok = false; }
    std::printf("  dual    [-40..-10]   row0=%.1f  rowN=%.1f  monotone=%s  %s\n",
                r[0], r[size_t(h - 1)], descends ? "yes" : "no",
                (oneSided && descends) ? "ok" : "BAD");

    return ok;
}

int main()
{
    std::printf("row -> range mapping\n");
    const bool rangesOk = checkRowRanges();
    std::printf("\n");

    // --- truth, then the smeared observation -------------------------------
    std::vector<std::vector<float>> truth(size_t(kHeight),
                                          std::vector<float>(size_t(kWidth), kBackground));
    for (int row = 0; row < kHeight; ++row) {
        for (int c = kTargetStep / 2; c < kWidth; c += kTargetStep) {
            truth[size_t(row)][size_t(c)] = kTarget;
        }
    }

    std::vector<int16_t> field(size_t(kWidth) * size_t(kHeight), 0);
    std::vector<int16_t*> columns(size_t(kWidth), nullptr);
    for (int c = 0; c < kWidth; ++c) {
        columns[size_t(c)] = field.data() + size_t(c) * size_t(kHeight);
    }

    std::vector<std::vector<float>> observed{static_cast<size_t>(kHeight), std::vector<float>{}};
    std::vector<float> ranges(size_t(kHeight), 0.0f);
    std::vector<float> spacing(size_t(kWidth), kSpacingM);

    for (int row = 0; row < kHeight; ++row) {
        ranges[size_t(row)] = rangeOfRow(row);
        int radius = 0;
        const std::vector<float> psf = psfFor(ranges[size_t(row)], radius);

        std::vector<float> blurred(size_t(kWidth), 0.0f);
        for (int c = 0; c < kWidth; ++c) {
            float acc = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                int s = c + k;
                if (s < 0) { s = 0; }
                if (s >= kWidth) { s = kWidth - 1; }
                acc += truth[size_t(row)][size_t(s)] * psf[size_t(k + radius)];
            }
            blurred[size_t(c)] = acc;
        }
        observed[size_t(row)] = blurred;
        for (int c = 0; c < kWidth; ++c) {
            columns[size_t(c)][row] = int16_t(std::lround(blurred[size_t(c)]));
        }
    }

    // --- where does this actually work? -------------------------------------
    //
    // A Gaussian of width sigma suppresses spatial frequency f by
    // exp(-2*pi^2*sigma^2*f^2). Recovering a point target needs f near 0.5
    // cycles/column, where a sigma of 7 columns leaves nothing to recover from -
    // no iteration count fixes that, because the information is gone. So the
    // honest question is not "does it converge" but "up to what smear".
    std::printf("recovery of an isolated target, 20 iterations\n");
    std::printf("%-8s %-8s %-10s %-10s %-10s %s\n",
                "row", "range", "sigma", "blurred", "deconv", "recovered");
    {
        std::vector<int16_t> probe(size_t(kWidth) * size_t(kHeight), 0);
        std::vector<int16_t*> probeCols(size_t(kWidth), nullptr);
        for (int c = 0; c < kWidth; ++c) {
            probeCols[size_t(c)] = probe.data() + size_t(c) * size_t(kHeight);
            for (int r = 0; r < kHeight; ++r) {
                probeCols[size_t(c)][r] = int16_t(std::lround(observed[size_t(r)][size_t(c)]));
            }
        }
        echo_deconv::Params pp;
        pp.angularResolutionDeg = kAngleDeg;
        pp.iterations = 20;
        echo_deconv::alongTrack(probeCols.data(), kWidth, kHeight,
                                ranges.data(), spacing.data(), kSpacingM, pp);

        for (int row : {0, 4, 8, 16, 24, 32, 48, 63}) {
            const float range = rangeOfRow(row);
            const float sigma = echo_deconv::footprintMetres(range, kAngleDeg)
                                / 2.35482004503f / kSpacingM;
            const int c = kTargetStep / 2;
            const double blurred = observed[size_t(row)][size_t(c)];
            const double deconv  = double(probeCols[size_t(c)][row]);
            const double frac    = (deconv - kBackground) / (kTarget - kBackground);
            std::printf("%-8d %-8.0f %-10.2f %-10.1f %-10.1f %.0f %%\n",
                        row, range, sigma, blurred, deconv, 100.0 * frac);
        }
    }
    std::printf("\n");

    // --- deconvolve ---------------------------------------------------------
    echo_deconv::Params params;
    params.angularResolutionDeg = kAngleDeg;
    params.iterations = 8;

    const echo_deconv::Stats stats = echo_deconv::alongTrack(
        columns.data(), kWidth, kHeight, ranges.data(), spacing.data(), kSpacingM, params);

    std::printf("rows processed=%d skipped=%d  max kernel radius=%d columns\n",
                stats.rowsProcessed, stats.rowsSkipped, stats.maxRadius);

    // --- did it get closer to the truth? ------------------------------------
    int improved = 0, worsened = 0;
    double worstRatio = 0.0;
    double peakBlur = 0.0, peakDeconv = 0.0, peakTruth = 0.0;

    for (int row = 0; row < kHeight; ++row) {
        std::vector<float> result(size_t(kWidth), 0.0f);
        for (int c = 0; c < kWidth; ++c) {
            result[size_t(c)] = float(columns[size_t(c)][row]);
        }

        const double before = rmse(observed[size_t(row)], truth[size_t(row)]);
        const double after  = rmse(result, truth[size_t(row)]);
        if (before <= 1.0e-6) { continue; }

        const double ratio = after / before;
        if (after < before) { ++improved; } else { ++worsened; }
        if (ratio > worstRatio) { worstRatio = ratio; }

        // Peak recovery on the farthest row, where the smear is worst.
        if (row == kHeight - 1) {
            const int c = kTargetStep / 2;
            peakTruth  = truth[size_t(row)][size_t(c)];
            peakBlur   = observed[size_t(row)][size_t(c)];
            peakDeconv = result[size_t(c)];
        }
    }

    std::printf("rows closer to truth=%d, further=%d, worst per-row RMSE ratio=%.3f\n",
                improved, worsened, worstRatio);
    std::printf("farthest row peak: truth=%.0f blurred=%.1f deconvolved=%.1f\n",
                peakTruth, peakBlur, peakDeconv);

    // --- cost on a realistic window ----------------------------------------
    {
        const int w = 2000, h = 800;
        std::vector<int16_t> big(size_t(w) * size_t(h), 40);
        std::vector<int16_t*> bigCols(size_t(w), nullptr);
        for (int c = 0; c < w; ++c) { bigCols[size_t(c)] = big.data() + size_t(c) * size_t(h); }
        std::vector<float> bigRanges(size_t(h), 0.0f);
        for (int r = 0; r < h; ++r) {
            bigRanges[size_t(r)] = kRangeNear + (kRangeFar - kRangeNear) * float(r) / float(h - 1);
        }
        std::vector<float> bigSpacing(size_t(w), kSpacingM);

        echo_deconv::Params p3 = params;
        p3.iterations = 3;
        const auto t0 = std::chrono::steady_clock::now();
        echo_deconv::alongTrack(bigCols.data(), w, h, bigRanges.data(), bigSpacing.data(), kSpacingM, p3);
        const auto t1 = std::chrono::steady_clock::now();
        std::printf("cost: %dx%d, 3 iterations -> %.0f ms\n", w, h,
                    std::chrono::duration<double, std::milli>(t1 - t0).count());
    }

    // The guard is on the regime where deconvolution is supposed to work at all:
    // a smear of about one column. Asserting anything at sigma 7 would only
    // enshrine the fact that the information is gone.
    std::vector<int16_t> guard(size_t(kWidth) * size_t(kHeight), 0);
    std::vector<int16_t*> guardCols(size_t(kWidth), nullptr);
    for (int c = 0; c < kWidth; ++c) {
        guardCols[size_t(c)] = guard.data() + size_t(c) * size_t(kHeight);
        for (int r = 0; r < kHeight; ++r) {
            guardCols[size_t(c)][r] = int16_t(std::lround(observed[size_t(r)][size_t(c)]));
        }
    }
    echo_deconv::Params gp;
    gp.angularResolutionDeg = kAngleDeg;
    gp.iterations = 20;
    echo_deconv::alongTrack(guardCols.data(), kWidth, kHeight,
                            ranges.data(), spacing.data(), kSpacingM, gp);

    const double nearRecovered =
        (double(guardCols[size_t(kTargetStep / 2)][0]) - kBackground) / (kTarget - kBackground);
    std::printf("guard: sigma=%.2f columns recovers %.0f %% (needs >= 80 %%)\n",
                echo_deconv::footprintMetres(rangeOfRow(0), kAngleDeg) / 2.35482004503f / kSpacingM,
                100.0 * nearRecovered);

    const bool ok = rangesOk && stats.rowsProcessed > 0 && worsened == 0 && nearRecovered >= 0.80;
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

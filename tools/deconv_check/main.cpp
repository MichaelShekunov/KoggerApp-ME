// Does the deconvolution actually undo the beam's along-track smear?
//
// Builds a synthetic scene of point targets, smears it with the same footprint
// model the deconvolver assumes, then checks that running the deconvolution
// moves the image back towards the truth rather than merely changing it.
#include "echogram_deconvolution.h"

#include <chrono>
#include <string>
#define _USE_MATH_DEFINES
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

// Stage 2: does knowing the pitch actually buy anything?
//
// The vessel advances evenly, but pitch swings the beam fore and aft, so the
// footprint samples the bottom at uneven positions. Deconvolving as if the
// samples were evenly spaced convolves with a kernel that is in the wrong place
// for most of them. This blurs in the true geometry and then asks both paths to
// undo it.
bool checkPitchCompensation()
{
    constexpr int   w          = 512;
    constexpr int   h          = 16;
    constexpr float spacing    = 0.5f;   // metres per ping
    constexpr float angleDeg   = 2.0f;
    constexpr float range      = 40.0f;
    constexpr float pitchAmpDeg = 2.0f;
    constexpr float pitchPeriod = 40.0f; // pings
    constexpr int   targetStep = 64;
    constexpr float background = 4.0f;
    constexpr float target     = 220.0f;

    const float sigmaM = echo_deconv::footprintMetres(range, angleDeg) / 2.35482004503f;

    std::vector<float> alongM(size_t(w), 0.0f);
    std::vector<float> pitchRad(size_t(w), 0.0f);
    std::vector<float> footX(size_t(w), 0.0f);
    for (int c = 0; c < w; ++c) {
        alongM[size_t(c)]   = float(c) * spacing;
        pitchRad[size_t(c)] = float(pitchAmpDeg * M_PI / 180.0
                                    * std::sin(2.0 * M_PI * double(c) / pitchPeriod));
        footX[size_t(c)]    = alongM[size_t(c)] + range * std::tan(pitchRad[size_t(c)]);
    }

    // Truth is a set of point targets at fixed positions on the bottom. Each
    // ping sees them through the beam, centred wherever the beam was pointing.
    std::vector<float> targetsAt;
    for (int c = targetStep / 2; c < w; c += targetStep) {
        targetsAt.push_back(float(c) * spacing);
    }

    std::vector<int16_t> fieldA(size_t(w) * size_t(h), 0);
    std::vector<int16_t> fieldB(size_t(w) * size_t(h), 0);
    std::vector<int16_t*> colsA(size_t(w), nullptr), colsB(size_t(w), nullptr);
    for (int c = 0; c < w; ++c) {
        colsA[size_t(c)] = fieldA.data() + size_t(c) * size_t(h);
        colsB[size_t(c)] = fieldB.data() + size_t(c) * size_t(h);
        float acc = background;
        for (float tx : targetsAt) {
            const float d = (footX[size_t(c)] - tx) / sigmaM;
            acc += (target - background) * std::exp(-0.5f * d * d) / (sigmaM * 2.5066f) * spacing;
        }
        for (int r = 0; r < h; ++r) {
            colsA[size_t(c)][r] = int16_t(std::lround(std::min(acc, 255.0f)));
            colsB[size_t(c)][r] = colsA[size_t(c)][r];
        }
    }

    std::vector<float> ranges(size_t(h), range);

    echo_deconv::Params p;
    p.angularResolutionDeg = angleDeg;
    p.iterations = 20;

    // A: told nothing, assumes even pings. B: given position and pitch, and
    // asked to resample into that geometry.
    echo_deconv::alongTrack(colsA.data(), w, h, ranges.data(), nullptr, nullptr, spacing, p);
    echo_deconv::Params pb = p;
    pb.compensateGeometry = true;
    const echo_deconv::Stats sb =
        echo_deconv::alongTrack(colsB.data(), w, h, ranges.data(),
                                alongM.data(), pitchRad.data(), spacing, pb);

    // Score by how tightly each target's energy is gathered around where it
    // really is - not by peak height, which over-sharpening inflates on its own.
    // Fraction of the return landing within one sigma of the true position.
    auto concentration = [&](int16_t* const* cols, float tx) {
        double inner = 0.0, outer = 0.0;
        for (int c = 0; c < w; ++c) {
            const double d = std::fabs(double(footX[size_t(c)]) - double(tx));
            if (d > 4.0 * sigmaM) { continue; }
            const double v = std::max(0.0, double(cols[size_t(c)][h / 2]) - double(background));
            outer += v;
            if (d <= sigmaM) { inner += v; }
        }
        return outer > 0.0 ? inner / outer : 0.0;
    };

    double sumA = 0.0, sumB = 0.0;
    for (float tx : targetsAt) {
        sumA += concentration(colsA.data(), tx);
        sumB += concentration(colsB.data(), tx);
    }
    const double meanA = 100.0 * sumA / double(targetsAt.size());
    const double meanB = 100.0 * sumB / double(targetsAt.size());

    std::printf("  pitch +-%.1f deg at %.0f m walks the footprint +-%.2f m (%.1f columns)\n",
                pitchAmpDeg, range, range * std::tan(pitchAmpDeg * M_PI / 180.0),
                range * std::tan(pitchAmpDeg * M_PI / 180.0) / spacing);
    std::printf("  energy within one sigma of the truth: even-ping %.1f %%, position+pitch %.1f %%\n",
                meanA, meanB);
    std::printf("  spatial=%s  rows falling back to even sampling=%d\n",
                sb.spatial ? "yes" : "no", sb.rowsNonMonotonic);

    // Recorded, not asserted as a win. The measurement is why
    // Params::compensateGeometry defaults off: at echogram sampling density a
    // resampling round trip costs more resolution than correcting the kernel
    // width recovers. What is asserted is that the geometry was seen and that
    // the path stays sane - a regression here means broken, not just unhelpful.
    const bool ok = sb.spatial && sb.rowsNonMonotonic == 0 && meanB > 0.5 * meanA;
    std::printf("  %s (resampling %s here: %+.1f points)\n",
                ok ? "ok" : "BAD - spatial path is broken, not merely unhelpful",
                meanB > meanA ? "helps" : "does not pay", meanB - meanA);
    return ok;
}

int main()
{
    std::printf("row -> range mapping\n");
    const bool rangesOk = checkRowRanges();
    std::printf("\n");

    std::printf("pitch compensation\n");
    const bool pitchOk = checkPitchCompensation();
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
                                ranges.data(), nullptr, nullptr, kSpacingM, pp);

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
        columns.data(), kWidth, kHeight, ranges.data(), nullptr, nullptr, kSpacingM, params);

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

        std::printf("cost, %dx%d, %d iterations (the shipped default)\n", w, h, params.iterations);
        std::printf("%-10s %-10s %s\n", "threads", "ms", "speedup");
        double serial = 0.0;
        for (int t : {1, 2, 4, 8, 16, 0}) {
            echo_deconv::Params pc = params;
            pc.threads = t;
            const auto t0 = std::chrono::steady_clock::now();
            echo_deconv::alongTrack(bigCols.data(), w, h, bigRanges.data(),
                                    nullptr, nullptr, kSpacingM, pc);
            const auto t1 = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            if (t == 1) { serial = ms; }
            std::printf("%-10s %-10.0f %.1fx\n",
                        t == 0 ? "auto" : std::to_string(t).c_str(), ms,
                        serial > 0.0 ? serial / ms : 1.0);
        }
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
                            ranges.data(), nullptr, nullptr, kSpacingM, gp);

    const double nearRecovered =
        (double(guardCols[size_t(kTargetStep / 2)][0]) - kBackground) / (kTarget - kBackground);
    std::printf("guard: sigma=%.2f columns recovers %.0f %% (needs >= 80 %%)\n",
                echo_deconv::footprintMetres(rangeOfRow(0), kAngleDeg) / 2.35482004503f / kSpacingM,
                100.0 * nearRecovered);

    const bool ok = rangesOk && pitchOk && stats.rowsProcessed > 0 && worsened == 0 && nearRecovered >= 0.80;
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

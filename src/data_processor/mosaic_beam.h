#pragma once

// The brightness a side-scan sonar puts into its echogram besides the bottom itself, measured from
// the echograms and taken out of the mosaic (docs/mosaic-quality.md).
//
// A sample's level is, on average, the product of what depends on the angle it left the sonar at
// (the beam's vertical pattern, and how the bottom scatters at that angle) and what depends on its
// slant range (spreading, absorption, the device's own gain). With the bottom flat under the sonar,
// a sample at slant range r over altitude h left at angle acos(h / r) from straight down. A survey
// sees the same angle at many ranges, as the altitude changes, so the two parts can be told apart:
// log(mean level) = A(angle) + B(range), fitted by alternating least squares over a table of mean
// levels binned by angle and range, one table a side. The mosaic then multiplies each sample by
// exp(C - A - B), C chosen so that the survey keeps its overall brightness.
//
// Nothing here is a property of any device: the pattern is measured from the data at run time.
// No Qt, so tools/beam_check builds it alone.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace mosaic_beam {

constexpr int   kAngleBins     = 90;       // 1 degree each, 0 = straight down
constexpr float kRangeBinM     = 0.25f;
constexpr int   kMaxRangeBins  = 1200;     // 300 m
constexpr float kMinAltitudeM  = 1.0f;     // lower, the angles are all but unknown (and often a lost bottom)
constexpr int   kMinCellCount  = 30;       // samples a cell needs to count in the fit
constexpr int   kMinAngleCover = 30;       // a model's data must reach this far out (degrees): below, all
                                           // but its middle would be guessed. Not more: a survey starting
                                           // in deep water on a short range sees 37-40 degrees only
                                           // (00014 at 20 m on 25 m, 2026-10-18), and a young model is
                                           // measured again as the data widens (DataProcessor)
constexpr float kMaxCorrDb     = 12.0f;    // the correction never goes further either way
constexpr float kPi            = 3.14159265358979323846f;

/// Mean levels of one side, binned by angle from straight down and by slant range.
struct Table {
    int rangeBins = 0;
    std::vector<double> sum;        // [angle * kMaxRangeBins + range], of the level
    std::vector<uint32_t> count;
    uint32_t pings = 0;

    Table() : sum(size_t(kAngleBins) * kMaxRangeBins, 0.0), count(size_t(kAngleBins) * kMaxRangeBins, 0u) {}

    /// One ping: `level[i]` at slant range i * resolution, the bottom at `altitude` below the sonar.
    void add(const uint8_t* level, int n, float resolution, float altitude)
    {
        if (!level || n <= 0 || !(resolution > 0.0f) || !std::isfinite(altitude) || altitude < kMinAltitudeM) {
            return;
        }
        // Walked outward: the angle grows with the range, so the bin moves on when the range passes
        // altitude / cos(the bin's upper edge) -- no acos a sample.
        int angleBin = 0;
        float nextEdgeR = altitude / std::cos(1.0f * kPi / 180.0f);
        const int first = std::max(1, int(std::floor(altitude / resolution)) + 1);
        bool any = false;
        for (int i = first; i < n; ++i) {
            const float r = float(i) * resolution;
            if (r <= altitude) {
                continue;
            }
            while (r > nextEdgeR && angleBin < kAngleBins - 1) {
                ++angleBin;
                nextEdgeR = angleBin + 1 < kAngleBins ? altitude / std::cos(float(angleBin + 1) * kPi / 180.0f)
                                                      : std::numeric_limits<float>::max();
            }
            const int rangeBin = int(r / kRangeBinM);
            if (rangeBin >= kMaxRangeBins) {
                break;
            }
            const size_t c = size_t(angleBin) * kMaxRangeBins + size_t(rangeBin);
            sum[c] += level[i];
            count[c] += 1;
            rangeBins = std::max(rangeBins, rangeBin + 1);
            any = true;
        }
        if (any) {
            ++pings;
        }
    }
};

constexpr int kCosSteps = 4096;   // the tables below, over the cosine of the angle from straight down

/// acos in degrees by a table of kCosSteps steps of the cosine, linear between: within 0.03 degree but
/// in the last step to straight down, where acos is steep (0.35 there; tools/beam_check measures). For
/// the mosaic's per-sample work, where std::acos was a tenth of the painting (2026-10-18, a profile of
/// 00017).
inline float angleFromCos(float c)
{
    static const std::vector<float> table = [] {
        std::vector<float> t(kCosSteps + 1);
        for (int k = 0; k <= kCosSteps; ++k) {
            t[size_t(k)] = std::acos(float(k) / float(kCosSteps)) * (180.0f / kPi);
        }
        return t;
    }();
    const float x = std::clamp(c, 0.0f, 1.0f) * float(kCosSteps);
    const int i = std::min(int(x), kCosSteps - 1);
    return table[size_t(i)] + (x - float(i)) * (table[size_t(i) + 1] - table[size_t(i)]);
}

/// The fitted response of one side.
struct SideModel {
    bool valid = false;
    uint32_t pings = 0;
    std::array<float, kAngleBins> angleDb{};   // A in dB, 0 at its maximum over the bins with data
    std::vector<float> rangeDb;                // B in dB, per range bin
    float offsetDb = 0.0f;                     // C in dB
    int angleFrom = 0, angleTo = -1;           // the bins with data
    float residualDb = 0.0f;                   // rms of what A + B leaves, over the cells
    float spreadDb = 0.0f;                     // rms of the cells about their mean: what there was to explain

    // For gainFast (prepare): 10^(-A/20) by the cosine of the angle, 10^(-B/20) by range bin,
    // 10^(C/20), and the stops as factors -- the stop of the sum in dB is the stop of the product.
    std::vector<float> gainByCos;
    std::vector<float> gainByRange;
    float gainC = 1.0f;
    float gainMin = 1.0f, gainMax = 1.0f;

    /// The factor a level at this angle (degrees from straight down) and slant range is multiplied by.
    float gain(float angleDeg, float rangeM) const
    {
        if (!valid) {
            return 1.0f;
        }
        const float a = interp(angleDb.data(), kAngleBins, angleDeg - 0.5f);
        const float b = rangeDb.empty() ? 0.0f : interp(rangeDb.data(), int(rangeDb.size()), rangeM / kRangeBinM - 0.5f);
        const float db = std::clamp(offsetDb - a - b, -kMaxCorrDb, kMaxCorrDb);
        return std::pow(10.0f, db / 20.0f);
    }

    /// As gain, from tables (prepare): `cosA` is altitude / slant range. Within a few hundredths of a dB
    /// of gain but in the last step of the cosine to straight down (tools/beam_check).
    float gainFast(float cosA, float rangeM) const
    {
        if (!valid || gainByCos.empty()) {
            return 1.0f;
        }
        const float ga = interp(gainByCos.data(), int(gainByCos.size()), std::clamp(cosA, 0.0f, 1.0f) * float(kCosSteps));
        const float gb = gainByRange.empty() ? 1.0f
                                             : interp(gainByRange.data(), int(gainByRange.size()), rangeM / kRangeBinM - 0.5f);
        return std::clamp(gainC * ga * gb, gainMin, gainMax);
    }

    /// Builds the tables of gainFast from A, B and C.
    void prepare()
    {
        gainByCos.resize(size_t(kCosSteps) + 1);
        for (int k = 0; k <= kCosSteps; ++k) {
            const float angle = angleFromCos(float(k) / float(kCosSteps));
            gainByCos[size_t(k)] = std::pow(10.0f, -interp(angleDb.data(), kAngleBins, angle - 0.5f) / 20.0f);
        }
        gainByRange.resize(rangeDb.size());
        for (size_t r = 0; r < rangeDb.size(); ++r) {
            gainByRange[r] = std::pow(10.0f, -rangeDb[r] / 20.0f);
        }
        gainC = std::pow(10.0f, offsetDb / 20.0f);
        gainMin = std::pow(10.0f, -kMaxCorrDb / 20.0f);
        gainMax = std::pow(10.0f, kMaxCorrDb / 20.0f);
    }

    /// The largest angle at which A is still above `downDb` (negative) of its maximum; -1 when A has
    /// not fallen that far by the end of the data -- the end of the data is not the beam's edge.
    float edgeAngle(float downDb) const
    {
        if (angleTo < angleFrom || angleDb[size_t(std::clamp(angleTo, 0, kAngleBins - 1))] >= downDb) {
            return -1.0f;
        }
        int last = angleFrom;
        for (int k = angleFrom; k <= angleTo && k < kAngleBins; ++k) {
            if (angleDb[size_t(k)] >= downDb) {
                last = k;
            }
        }
        return float(last) + 0.5f;
    }

    float peakAngle() const
    {
        int best = angleFrom;
        for (int k = angleFrom; k <= angleTo && k < kAngleBins; ++k) {
            if (angleDb[size_t(k)] > angleDb[size_t(best)]) {
                best = k;
            }
        }
        return float(best) + 0.5f;
    }

    static float interp(const float* v, int n, float x)
    {
        if (x <= 0.0f) {
            return v[0];
        }
        if (x >= float(n - 1)) {
            return v[n - 1];
        }
        const int i = int(x);
        const float t = x - float(i);
        return v[i] + t * (v[i + 1] - v[i]);
    }
};

constexpr uint32_t kMinPings = 300;        // a model's pings a side, at least

/// Fits A and B to a side's table; invalid when it holds too little (minPings pings, minCells cells,
/// data short of kMinAngleCover).
inline SideModel fit(const Table& t, uint32_t minPings = kMinPings, int minCells = 200, int iterations = 60)
{
    SideModel m;
    m.pings = t.pings;
    const int nr = t.rangeBins;
    if (t.pings < minPings || nr <= 0) {
        return m;
    }

    const size_t na = kAngleBins;
    std::vector<double> L(na * size_t(nr), 0.0), W(na * size_t(nr), 0.0);
    int cells = 0;
    for (size_t a = 0; a < na; ++a) {
        for (int r = 0; r < nr; ++r) {
            const size_t src = a * kMaxRangeBins + size_t(r);
            const uint32_t c = t.count[src];
            if (c < uint32_t(kMinCellCount)) {
                continue;
            }
            const size_t dst = a * size_t(nr) + size_t(r);
            L[dst] = std::log(std::max(t.sum[src] / double(c), 0.5));
            W[dst] = std::min(double(c), 5000.0);
            ++cells;
        }
    }
    if (cells < minCells) {
        return m;
    }

    std::vector<double> A(na, 0.0), B(size_t(nr), 0.0);
    std::vector<char> hasA(na, 0), hasB(size_t(nr), 0);
    for (int it = 0; it < iterations; ++it) {
        for (size_t a = 0; a < na; ++a) {
            double s = 0.0, w = 0.0;
            for (int r = 0; r < nr; ++r) {
                const size_t i = a * size_t(nr) + size_t(r);
                s += W[i] * (L[i] - B[size_t(r)]);
                w += W[i];
            }
            hasA[a] = w > 0.0;
            A[a] = w > 0.0 ? s / w : 0.0;
        }
        std::vector<double> sorted;
        for (int r = 0; r < nr; ++r) {
            double s = 0.0, w = 0.0;
            for (size_t a = 0; a < na; ++a) {
                const size_t i = a * size_t(nr) + size_t(r);
                s += W[i] * (L[i] - A[a]);
                w += W[i];
            }
            hasB[size_t(r)] = w > 0.0;
            B[size_t(r)] = w > 0.0 ? s / w : 0.0;
            if (w > 0.0) {
                sorted.push_back(B[size_t(r)]);
            }
        }
        // A and B are each defined up to a constant: B's median is held at zero.
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        const double med = sorted.empty() ? 0.0 : sorted[sorted.size() / 2];
        for (int r = 0; r < nr; ++r) {
            if (hasB[size_t(r)]) {
                B[size_t(r)] -= med;
            }
        }
    }

    // What the fit leaves, and what there was: rms in dB over the cells, weighted alike.
    double sw = 0.0, sl = 0.0;
    for (size_t i = 0; i < L.size(); ++i) {
        sw += W[i];
        sl += W[i] * L[i];
    }
    const double meanL = sl / sw;
    double res2 = 0.0, spr2 = 0.0, levelSum = 0.0, flatSum = 0.0;
    for (size_t a = 0; a < na; ++a) {
        for (int r = 0; r < nr; ++r) {
            const size_t i = a * size_t(nr) + size_t(r);
            if (W[i] <= 0.0) {
                continue;
            }
            const double p = A[a] + B[size_t(r)];
            res2 += W[i] * (L[i] - p) * (L[i] - p);
            spr2 += W[i] * (L[i] - meanL) * (L[i] - meanL);
            const double c = double(t.count[a * kMaxRangeBins + size_t(r)]);
            levelSum += c * std::exp(L[i]);
            flatSum += c * std::exp(L[i] - p);
        }
    }
    constexpr double kToDb = 20.0 / 2.302585092994046;
    m.residualDb = float(std::sqrt(res2 / sw) * kToDb);
    m.spreadDb = float(std::sqrt(spr2 / sw) * kToDb);
    // C: the corrected samples' mean level is the samples' mean level, so the survey keeps its
    // brightness and the colour levels set for it still fit.
    const double c = std::log(levelSum / flatSum);

    // Bins without data hold their nearest neighbour's value; then a 3-bin running mean, which a
    // survey's 1-degree, 25-cm bins need against the speckle of a sparse cell.
    auto fill = [](std::vector<double>& v, const std::vector<char>& has, int& from, int& to) {
        from = -1;
        to = -1;
        for (int i = 0; i < int(v.size()); ++i) {
            if (has[size_t(i)]) {
                if (from < 0) {
                    from = i;
                }
                to = i;
            }
        }
        if (from < 0) {
            return false;
        }
        for (int i = 0; i < from; ++i) {
            v[size_t(i)] = v[size_t(from)];
        }
        for (int i = to + 1; i < int(v.size()); ++i) {
            v[size_t(i)] = v[size_t(to)];
        }
        for (int i = from + 1; i < to; ++i) {
            if (!has[size_t(i)]) {
                v[size_t(i)] = v[size_t(i - 1)];
            }
        }
        std::vector<double> s(v.size());
        for (int i = 0; i < int(v.size()); ++i) {
            const int lo = std::max(from, i - 1);
            const int hi = std::min(to, i + 1);
            double acc = 0.0;
            for (int k = lo; k <= hi; ++k) {
                acc += v[size_t(k)];
            }
            s[size_t(i)] = (i < from || i > to) ? v[size_t(i)] : acc / double(hi - lo + 1);
        }
        v.swap(s);
        return true;
    };
    int bFrom = 0, bTo = 0;
    if (!fill(A, hasA, m.angleFrom, m.angleTo) || !fill(B, hasB, bFrom, bTo) || m.angleTo < kMinAngleCover) {
        return m;
    }

    double aMax = -1e30;
    for (int k = m.angleFrom; k <= m.angleTo; ++k) {
        aMax = std::max(aMax, A[size_t(k)]);
    }
    for (size_t k = 0; k < na; ++k) {
        m.angleDb[k] = float((A[k] - aMax) * kToDb);
    }
    m.rangeDb.resize(size_t(nr));
    for (int r = 0; r < nr; ++r) {
        m.rangeDb[size_t(r)] = float((B[size_t(r)] + aMax) * kToDb);   // A + B kept
    }
    m.offsetDb = float(c * kToDb);
    m.valid = true;
    m.prepare();
    return m;
}

/// The largest difference, in dB, between the corrections of two models, over the angles the first
/// has data at and the ranges both have, where such a sample can exist (an altitude of at least
/// kMinAltitudeM); a new estimate within a dB or so of the old one is not worth repainting for.
inline float maxDifferenceDb(const SideModel& a, const SideModel& b)
{
    if (a.valid != b.valid) {
        return 1e9f;
    }
    if (!a.valid) {
        return 0.0f;
    }
    float worst = 0.0f;
    const int nr = int(std::min(a.rangeDb.size(), b.rangeDb.size()));
    for (int k = a.angleFrom; k <= a.angleTo; ++k) {
        const float angle = float(k) + 0.5f;
        const float cosA = std::cos(angle * kPi / 180.0f);
        for (int r = 0; r < nr; ++r) {
            const float range = (float(r) + 0.5f) * kRangeBinM;
            if (range * cosA < kMinAltitudeM) {
                continue;
            }
            const float da = 20.0f * std::log10(a.gain(angle, range));
            const float db = 20.0f * std::log10(b.gain(angle, range));
            worst = std::max(worst, std::fabs(da - db));
        }
    }
    return worst;
}

/// The angle from straight down, degrees, of a sample at slant range `rangeM` over `altitudeM`.
inline float angleDeg(float altitudeM, float rangeM)
{
    if (!(rangeM > altitudeM) || !(altitudeM > 0.0f)) {
        return 0.0f;
    }
    return std::acos(altitudeM / rangeM) * (180.0f / kPi);
}

/// Both sides: [0] the mosaic's first channel (the left), [1] its second (the right).
struct Model {
    std::array<SideModel, 2> side;
    uint32_t epochs = 0;   // the epochs it was measured on
    bool valid() const { return side[0].valid || side[1].valid; }
};

/// Which sample a pixel keeps where pings overlap.
enum class Overlap : uint8_t {
    Newest = 0,       // the newest, as the mosaic always did
    EdgesUnder = 1,   // the newest, but a sample from a ping's edge -- under the sonar or past its
                      // beam -- only where no other ping's middle is ("shine-through")
    Nearest = 2,      // the one seen nearest straight down ("nadir priority")
};

/// What the mosaic paints of a sample, and over what.
struct PaintRules {
    float nadirCropDeg = 0.0f;   // samples nearer straight down than this are not painted
    float farCropDeg = 90.0f;    // nor those further out than this
    Overlap overlap = Overlap::Newest;
    // EdgesUnder: a ping's edges -- nearer straight down than edgeNadirDeg, or past edgeFarDeg of its
    // side (the beam's -6 dB angle when measured) -- go under other pings' middles.
    float edgeNadirDeg = 12.0f;
    std::array<float, 2> edgeFarDeg{ { 75.0f, 75.0f } };

    // The angles above as cosines (prepare): the mosaic compares altitude / slant range with them
    // instead of taking an acos a sample. Not part of ==.
    float cosNadirCrop = 2.0f, cosFarCrop = -1.0f, cosEdgeNadir = 1.0f;
    std::array<float, 2> cosEdgeFar{ { 0.0f, 0.0f } };

    bool crops() const { return nadirCropDeg > 0.0f || farCropDeg < 90.0f; }
    bool usesQuality() const { return overlap != Overlap::Newest; }
    bool operator==(const PaintRules& o) const
    {
        return nadirCropDeg == o.nadirCropDeg && farCropDeg == o.farCropDeg && overlap == o.overlap
            && edgeNadirDeg == o.edgeNadirDeg && edgeFarDeg == o.edgeFarDeg;
    }
    bool operator!=(const PaintRules& o) const { return !(*this == o); }

    void prepare()
    {
        auto cosDeg = [](float a) { return std::cos(a * kPi / 180.0f); };
        cosNadirCrop = nadirCropDeg > 0.0f ? cosDeg(nadirCropDeg) : 2.0f;   // off: no cosine is above
        cosFarCrop = farCropDeg < 90.0f ? cosDeg(farCropDeg) : -1.0f;       // off: none below (cos 90 is not 0)
        cosEdgeNadir = cosDeg(edgeNadirDeg);
        for (size_t s = 0; s < 2; ++s) {
            cosEdgeFar[s] = cosDeg(edgeFarDeg[s]);
        }
    }

    /// The crop by the cosine of the angle from straight down (altitude / slant range): true leaves
    /// the sample out. prepare() first.
    bool cropped(float cosA) const { return cosA > cosNadirCrop || cosA < cosFarCrop; }

    /// A sample's quality for the overlap rule (0 is kept for "never painted"): a pixel keeps the
    /// best, the newest of equals. Nearest: 255 straight down, 1 at the horizon. EdgesUnder: 2 in a
    /// ping's middle, 1 at its edges. By the cosine, as cropped.
    uint8_t quality(int side, float cosA) const
    {
        if (overlap == Overlap::Nearest) {
            return uint8_t(1 + int(254.0f * (1.0f - angleFromCos(cosA) / 90.0f) + 0.5f));
        }
        return (cosA > cosEdgeNadir || cosA < cosEdgeFar[size_t(side & 1)]) ? 1 : 2;
    }
};

} // namespace mosaic_beam

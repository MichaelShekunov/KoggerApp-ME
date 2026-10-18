// Offline check of src/data_processor/mosaic_beam.h -- the sonar's angular and range response, measured
// from the echograms and taken out of the mosaic (docs/mosaic-quality.md).
//
//  1. Binning: the outward walk of Table::add puts a sample in the bin acos(h / r) gives, but for the
//     odd sample a float rounding away from a bin edge.
//  2. A survey made up with a known angular response A and range response B, speckled, over altitudes
//     from 2 to 15 m: the fitted A + B is the true one up to a constant (rms 0.5 dB, worst 1.5 dB over
//     the cells with data), and the corrected levels are flat by angle and by range (within 1 dB) while
//     they were not before (6 dB and more), with the survey's mean level kept (5 %). The made-up
//     response spans some 28 dB, more than the correction may (kMaxCorrDb either way): accuracy and
//     flatness are judged where it is not at its stop, and at most 10 % of the samples may be.
//     Control: a correction by angle alone -- the mean level of each angle over every range -- must
//     fail it and leave the range in, or the check proves nothing (and the range part is not needed).
//  3. Refusals: too few pings is no model (gain 1), and so is data short of kMinAngleCover; a ping under
//     kMinAltitudeM or with no altitude adds nothing; the correction stops at kMaxCorrDb either way.
//  4. maxDifferenceDb: 0 for a model against itself, the shift for a model with B shifted.
//  5. The per-sample paths of the mosaic: angleFromCos against acos, gainFast against gain, the crop and
//     the overlap qualities by the cosine against the angles they stand for.
//
//     beam_check            all checks, exit code 0 when they pass

#include "mosaic_beam.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

int g_fail = 0;
int g_pass = 0;

void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? g_pass : g_fail)++;
}

constexpr float kDeg = mosaic_beam::kPi / 180.0f;

// The made-up sonar: flat to 50 deg with a 3 dB bump at 20, then down 12 dB by 85; 0.45 dB a metre.
float trueAdB(float angleDeg)
{
    float a = 3.0f * std::exp(-0.5f * std::pow((angleDeg - 20.0f) / 6.0f, 2.0f));
    if (angleDeg > 50.0f) {
        a -= 12.0f * std::pow((angleDeg - 50.0f) / 35.0f, 1.5f);
    }
    return a;
}
float trueBdB(float rangeM) { return -0.45f * rangeM; }

struct Ping { std::vector<uint8_t> level; float resolution; float altitude; };

std::vector<Ping> makeSurvey(int n, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    std::vector<Ping> out;
    const float res = 0.02f;
    const int samples = 1500;   // 30 m
    for (int p = 0; p < n; ++p) {
        Ping pg;
        pg.resolution = res;
        pg.altitude = 2.0f + 13.0f * (0.5f + 0.5f * std::sin(float(p) * 0.01f) * (0.6f + 0.4f * u01(rng)));
        pg.level.resize(samples);
        for (int i = 0; i < samples; ++i) {
            const float r = float(i) * res;
            float v = 3.0f;   // the water column
            if (r > pg.altitude) {
                const float angle = std::acos(pg.altitude / r) / kDeg;
                const float db = trueAdB(angle) + trueBdB(r);
                // Rayleigh speckle, mean 1
                const float ray = std::sqrt(-2.0f * std::log(std::max(1e-6f, u01(rng)))) / 1.2533141f;
                v = 60.0f * std::pow(10.0f, db / 20.0f) * ray;
            }
            pg.level[size_t(i)] = uint8_t(std::clamp(std::lround(v), 0L, 255L));
        }
        out.push_back(std::move(pg));
    }
    return out;
}

void checkBinning()
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> alt(1.0f, 40.0f), resol(0.005f, 0.05f);
    long total = 0, same = 0;
    for (int p = 0; p < 300; ++p) {
        const float h = alt(rng), res = resol(rng);
        const int n = 4000;
        std::vector<uint8_t> lv(size_t(n), 1);
        mosaic_beam::Table t;
        t.add(lv.data(), n, res, h);
        // reference: each sample's bin from acos, counted the same way
        std::vector<uint32_t> ref(size_t(mosaic_beam::kAngleBins) * mosaic_beam::kMaxRangeBins, 0);
        for (int i = 0; i < n; ++i) {
            const float r = float(i) * res;
            if (r <= h) {
                continue;
            }
            const int rb = int(r / mosaic_beam::kRangeBinM);
            if (rb >= mosaic_beam::kMaxRangeBins) {
                break;
            }
            const int ab = std::min(mosaic_beam::kAngleBins - 1, int(std::acos(h / r) / kDeg));
            ref[size_t(ab) * mosaic_beam::kMaxRangeBins + size_t(rb)]++;
            ++total;
        }
        for (size_t c = 0; c < ref.size(); ++c) {
            same += std::min(ref[c], t.count[c]);
        }
    }
    const double share = double(same) / double(total);
    std::printf("      binning: %ld samples, %.5f in the acos bin\n", total, share);
    check(share > 0.999, "1. the outward walk bins as acos does (99.9 % of the samples)");
}

struct Recovery { double rmsDb; double maxDb; double flatAngleBefore, flatAngleAfter, flatRangeBefore, flatRangeAfter, meanRatio, clampedShare; };

// The control: a correction by angle alone, from the mean level of each angle over every range --
// what the first look at the data gives. Over a survey of changing altitude it carries the range part
// into the angles, and must fail where the fit of both passes.
mosaic_beam::SideModel angleOnly(const mosaic_beam::Table& t)
{
    mosaic_beam::SideModel m;
    m.valid = true;
    m.angleFrom = mosaic_beam::kAngleBins;
    m.angleTo = -1;
    std::array<double, mosaic_beam::kAngleBins> mean{};
    double all = 0.0, n = 0.0;
    for (int a = 0; a < mosaic_beam::kAngleBins; ++a) {
        double s = 0.0, c = 0.0;
        for (int r = 0; r < t.rangeBins; ++r) {
            s += t.sum[size_t(a) * mosaic_beam::kMaxRangeBins + size_t(r)];
            c += t.count[size_t(a) * mosaic_beam::kMaxRangeBins + size_t(r)];
        }
        mean[size_t(a)] = c > 0 ? s / c : 0.0;
        if (c > 0) {
            m.angleFrom = std::min(m.angleFrom, a);
            m.angleTo = a;
        }
        all += s;
        n += c;
    }
    for (int a = 0; a < mosaic_beam::kAngleBins; ++a) {
        const int k = std::clamp(a, m.angleFrom, m.angleTo);
        m.angleDb[size_t(a)] = float(20.0 * std::log10(std::max(mean[size_t(k)], 0.5)));
    }
    m.rangeDb.assign(size_t(t.rangeBins), 0.0f);
    m.offsetDb = float(20.0 * std::log10(all / n));
    return m;
}

Recovery recover(const std::vector<Ping>& survey, bool control)
{
    mosaic_beam::Table t;
    for (const Ping& p : survey) {
        t.add(p.level.data(), int(p.level.size()), p.resolution, p.altitude);
    }
    const mosaic_beam::SideModel m = control ? angleOnly(t) : mosaic_beam::fit(t);
    Recovery rc{};
    if (!m.valid) {
        rc.rmsDb = rc.maxDb = 1e9;
        return rc;
    }
    // fitted vs true correction over the populated cells, up to a constant
    std::vector<double> diff, w;
    for (int a = 0; a < mosaic_beam::kAngleBins; ++a) {
        for (int r = 0; r < t.rangeBins; ++r) {
            const uint32_t c = t.count[size_t(a) * mosaic_beam::kMaxRangeBins + size_t(r)];
            if (c < 200) {
                continue;
            }
            const float angle = float(a) + 0.5f, range = (float(r) + 0.5f) * mosaic_beam::kRangeBinM;
            const double fitted = -20.0 * std::log10(m.gain(angle, range));
            if (std::fabs(fitted) > mosaic_beam::kMaxCorrDb - 0.01) {
                continue;   // at the stop: checked apart (3.)
            }
            const double truth = trueAdB(angle) + trueBdB(range);
            diff.push_back(fitted - truth);
            w.push_back(c);
        }
    }
    double sw = 0, sd = 0;
    for (size_t i = 0; i < diff.size(); ++i) { sw += w[i]; sd += w[i] * diff[i]; }
    const double mean = sd / sw;
    double s2 = 0, mx = 0;
    for (size_t i = 0; i < diff.size(); ++i) {
        s2 += w[i] * (diff[i] - mean) * (diff[i] - mean);
        mx = std::max(mx, std::fabs(diff[i] - mean));
    }
    rc.rmsDb = std::sqrt(s2 / sw);
    rc.maxDb = mx;

    // Mean level by angle and by range before and after the correction: the spread in dB of the
    // bins with plenty of samples.
    std::vector<double> sa0(90), sa1(90), na(90), sr0(200), sr1(200), nr(200);
    double tot0 = 0, tot1 = 0, n = 0;
    for (const Ping& p : survey) {
        for (size_t i = 0; i < p.level.size(); ++i) {
            const float r = float(i) * p.resolution;
            if (r <= p.altitude || p.altitude < mosaic_beam::kMinAltitudeM) {
                continue;
            }
            const float angle = mosaic_beam::angleDeg(p.altitude, r);
            const double v0 = p.level[i];
            const float g = m.gain(angle, r);
            const double v1 = v0 * g;
            tot0 += v0; tot1 += v1; n += 1;
            if (std::fabs(20.0f * std::log10(g)) > mosaic_beam::kMaxCorrDb - 0.01f) {
                rc.clampedShare += 1;
                continue;
            }
            const int ab = std::min(89, int(angle));
            const int rb = std::min(199, int(r / 0.5f));
            sa0[size_t(ab)] += v0; sa1[size_t(ab)] += v1; na[size_t(ab)] += 1;
            sr0[size_t(rb)] += v0; sr1[size_t(rb)] += v1; nr[size_t(rb)] += 1;
        }
    }
    rc.clampedShare /= n;
    auto spread = [](const std::vector<double>& s, const std::vector<double>& c) {
        double lo = 1e30, hi = -1e30;
        for (size_t i = 0; i < s.size(); ++i) {
            if (c[i] < 20000) {
                continue;
            }
            const double db = 20.0 * std::log10(s[i] / c[i]);
            lo = std::min(lo, db);
            hi = std::max(hi, db);
        }
        return hi - lo;
    };
    rc.flatAngleBefore = spread(sa0, na);
    rc.flatAngleAfter = spread(sa1, na);
    rc.flatRangeBefore = spread(sr0, nr);
    rc.flatRangeAfter = spread(sr1, nr);
    rc.meanRatio = tot1 / tot0;
    return rc;
}

void checkRecovery()
{
    const auto survey = makeSurvey(3000, 11);
    const Recovery rc = recover(survey, false);
    std::printf("      fit: rms %.2f dB, worst %.2f dB; by angle %.1f -> %.1f dB, by range %.1f -> %.1f dB; mean x%.3f;"
                " %.1f %% of the samples at the stop\n",
                rc.rmsDb, rc.maxDb, rc.flatAngleBefore, rc.flatAngleAfter, rc.flatRangeBefore, rc.flatRangeAfter, rc.meanRatio,
                100.0 * rc.clampedShare);
    check(rc.rmsDb < 0.5 && rc.maxDb < 1.5, "2. the fitted A + B is the true one up to a constant (rms 0.5, worst 1.5 dB)");
    check(rc.flatAngleBefore > 6.0 && rc.flatAngleAfter < 1.0, "2. corrected levels are flat by angle (were 6 dB and more apart)");
    check(rc.flatRangeBefore > 6.0 && rc.flatRangeAfter < 1.0, "2. corrected levels are flat by range (were 6 dB and more apart)");
    check(std::fabs(rc.meanRatio - 1.0) < 0.05, "2. the survey keeps its mean level (5 %)");

    // The made-up beam falls 6 dB below its peak (+3 dB at 20 deg) at 63.9 deg; a model whose data ends
    // before any such fall has no edge (-1), not the end of its data.
    mosaic_beam::Table t;
    for (const Ping& p : survey) {
        t.add(p.level.data(), int(p.level.size()), p.resolution, p.altitude);
    }
    const auto fitted = mosaic_beam::fit(t);
    mosaic_beam::SideModel flat;
    flat.valid = true;
    flat.angleFrom = 0;
    flat.angleTo = 44;
    flat.angleDb.fill(0.0f);
    std::printf("      edge: -6 dB at %.1f deg (63.9 made up); a flat model to 45 deg: %.1f\n",
                double(fitted.edgeAngle(-6.0f)), double(flat.edgeAngle(-6.0f)));
    check(std::fabs(fitted.edgeAngle(-6.0f) - 63.9f) < 2.0f && flat.edgeAngle(-6.0f) < 0.0f,
          "2. the -6 dB edge is where the beam falls, and none where the data ends first");
    check(rc.clampedShare < 0.10, "2. at most 10 % of the samples at the correction's stop");

    const Recovery ctl = recover(survey, true);
    std::printf("      control, by angle alone: rms %.2f dB, worst %.2f dB; by range %.1f -> %.1f dB\n",
                ctl.rmsDb, ctl.maxDb, ctl.flatRangeBefore, ctl.flatRangeAfter);
    check(!(ctl.rmsDb < 0.5 && ctl.maxDb < 1.5) && ctl.flatRangeAfter > 3.0,
          "2. control: a correction by angle alone fails the recovery and leaves the range in");
}

void checkRefusals()
{
    const auto few = makeSurvey(50, 3);
    mosaic_beam::Table t;
    for (const Ping& p : few) {
        t.add(p.level.data(), int(p.level.size()), p.resolution, p.altitude);
    }
    const auto m = mosaic_beam::fit(t);
    check(!m.valid && m.gain(30.0f, 10.0f) == 1.0f, "3. 50 pings are no model, and no model is a gain of 1");

    // 2000 pings at 22 to 24 m over a 25 m range: plenty of cells, but angles to 28 deg at most -- the
    // rest would be guessed. The same with the range to 30 m reaches 42 deg and is a model (control).
    auto deepFit = [](int samples) {
        mosaic_beam::Table deep;
        std::mt19937 rng(9);
        std::uniform_real_distribution<float> u01(0.0f, 1.0f);
        std::vector<uint8_t> lvl(size_t(samples), 0);
        for (int p = 0; p < 2000; ++p) {
            for (auto& v : lvl) {
                v = uint8_t(20 + 40 * u01(rng));
            }
            deep.add(lvl.data(), samples, 0.02f, 22.0f + 2.0f * u01(rng));
        }
        return mosaic_beam::fit(deep);
    };
    const auto md = deepFit(1250);
    const auto mw = deepFit(1500);
    std::printf("      deep survey: 25 m range to %d deg, %s; 30 m range to %d deg, %s\n",
                md.angleTo, md.valid ? "a model" : "no model", mw.angleTo, mw.valid ? "a model" : "no model");
    check(!md.valid && md.angleTo >= 20 && mw.valid,
          "3. data short of kMinAngleCover (a deep survey on a short range) is no model; a longer range is");

    mosaic_beam::Table u;
    std::vector<uint8_t> lv(1000, 50);
    u.add(lv.data(), 1000, 0.02f, 0.5f);
    u.add(lv.data(), 1000, 0.02f, std::nanf(""));
    u.add(lv.data(), 1000, 0.0f, 5.0f);
    uint64_t c = 0;
    for (uint32_t k : u.count) {
        c += k;
    }
    check(u.pings == 0 && c == 0, "3. a ping under the minimum altitude, without one, or without a resolution adds nothing");

    mosaic_beam::SideModel s;
    s.valid = true;
    s.angleFrom = 0;
    s.angleTo = mosaic_beam::kAngleBins - 1;
    s.angleDb.fill(-40.0f);
    s.rangeDb.assign(10, 0.0f);
    const float up = 20.0f * std::log10(s.gain(45.0f, 1.0f));
    s.angleDb.fill(40.0f);
    const float down = 20.0f * std::log10(s.gain(45.0f, 1.0f));
    check(std::fabs(up - mosaic_beam::kMaxCorrDb) < 1e-3f && std::fabs(down + mosaic_beam::kMaxCorrDb) < 1e-3f,
          "3. the correction stops at kMaxCorrDb either way");
}

void checkDifference()
{
    const auto survey = makeSurvey(1500, 5);
    mosaic_beam::Table t;
    for (const Ping& p : survey) {
        t.add(p.level.data(), int(p.level.size()), p.resolution, p.altitude);
    }
    const auto a = mosaic_beam::fit(t);
    auto b = a;
    for (float& v : b.rangeDb) {
        v += 2.0f;
    }
    const float same = mosaic_beam::maxDifferenceDb(a, a);
    const float shifted = mosaic_beam::maxDifferenceDb(a, b);
    std::printf("      difference: itself %.3f dB, B shifted 2 dB: %.3f dB\n", same, shifted);
    check(same == 0.0f && std::fabs(shifted - 2.0f) < 0.01f, "4. maxDifferenceDb: 0 against itself, the shift against B shifted");
}

// 5. What the mosaic uses a sample (2026-10-18: an acos and a pow a sample were half the painting):
// angleFromCos against acos, gainFast against gain over the angles and ranges a survey has, and the
// crop and the overlap qualities by the cosine against the same by the angle.
void checkFastPaths()
{
    double worstAngle = 0.0, worstAngleOff = 0.0;
    for (int k = 0; k <= 100000; ++k) {
        const float c = float(k) / 100000.0f;
        const double d = std::fabs(mosaic_beam::angleFromCos(c) - std::acos(c) / kDeg);
        if (c < 1.0f - 1.0f / float(mosaic_beam::kCosSteps)) {
            worstAngle = std::max(worstAngle, d);
        }
        else {
            worstAngleOff = std::max(worstAngleOff, d);
        }
    }

    const auto survey = makeSurvey(2000, 21);
    mosaic_beam::Table t;
    for (const Ping& p : survey) {
        t.add(p.level.data(), int(p.level.size()), p.resolution, p.altitude);
    }
    const auto m = mosaic_beam::fit(t);
    double worstDb = 0.0;
    for (int a = 0; a < 90; ++a) {
        for (int r = 4; r < 120; ++r) {
            const float angle = float(a) + 0.5f, range = float(r) * 0.25f;
            const float c = std::cos(angle * kDeg);
            if (range * c < mosaic_beam::kMinAltitudeM || c > 1.0f - 1.0f / float(mosaic_beam::kCosSteps)) {
                continue;
            }
            worstDb = std::max(worstDb, std::fabs(20.0 * std::log10(m.gainFast(c, range) / m.gain(angle, range))));
        }
    }
    std::printf("      fast paths: angleFromCos within %.4f deg (%.2f in the last step to straight down);"
                " gainFast within %.3f dB of gain\n", worstAngle, worstAngleOff, worstDb);
    check(worstAngle < 0.03 && worstAngleOff < 0.35, "5. angleFromCos is acos within 0.03 deg (0.35 in its last step)");
    check(m.valid && worstDb < 0.05, "5. gainFast is gain within 0.05 dB");

    mosaic_beam::PaintRules off;
    off.prepare();
    mosaic_beam::PaintRules on;
    on.nadirCropDeg = 10.0f;
    on.farCropDeg = 70.0f;
    on.overlap = mosaic_beam::Overlap::EdgesUnder;
    on.edgeFarDeg = { { 60.0f, 80.0f } };
    on.prepare();
    bool cropOk = !off.cropped(0.0f) && !off.cropped(1.0f) && !off.cropped(0.5f);
    bool edgesOk = true;
    for (float a = 0.25f; a < 90.0f; a += 0.5f) {
        const float c = std::cos(a * kDeg);
        cropOk = cropOk && on.cropped(c) == (a < 10.0f || a > 70.0f);
        edgesOk = edgesOk && on.quality(0, c) == ((a < 12.0f || a > 60.0f) ? 1 : 2)
                          && on.quality(1, c) == ((a < 12.0f || a > 80.0f) ? 1 : 2);
    }
    check(cropOk, "5. the crop by the cosine: off at 0 and 90 (a 0 altitude too), on outside 10..70 deg");
    check(edgesOk, "5. edges under: a ping's middle 2, its edges 1, the far edge per side");

    mosaic_beam::PaintRules nearest;
    nearest.overlap = mosaic_beam::Overlap::Nearest;
    nearest.prepare();
    bool monotone = nearest.quality(0, 1.0f) == 255 && nearest.quality(0, 0.0f) == 1;
    int prev = 256;
    for (float a = 0.0f; a <= 90.0f; a += 0.1f) {
        const int q = nearest.quality(0, std::cos(a * kDeg));
        monotone = monotone && q <= prev && q >= 1;
        prev = q;
    }
    check(monotone, "5. nearest the track: 255 straight down, 1 at the horizon, never rising outward");
}

} // namespace

int main()
{
    checkBinning();
    checkRecovery();
    checkRefusals();
    checkDifference();
    checkFastPaths();
    std::printf("%d/%d\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}

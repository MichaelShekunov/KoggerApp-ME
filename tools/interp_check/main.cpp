// Offline check of TrackSmoother (src/track_smoother.*): pings between GNSS fixes on a simulated
// survey with the GODIO boat's float32 coordinates. The survey, its noise and its quantisation are
// Godio_autopilot/tools/interp_prototype.py ported line by line (same LCG, same numbers), with the
// GNSS noise at 0.30 m as calibrated there on the lake recording of 2026-10-07 (the prototype's
// default is 0.10) and fixes at 6.58 Hz as the boat sent them on the water on 2026-10-08. What is
// added is the order in which Dataset sees the data: a ping opens an epoch, a fix goes into the newest
// epoch that has no position, a heading into the newest epoch, and headings between two raw ones are
// filled pairwise (Dataset::interpolateAtt) before positions are placed.
//
// An epoch is "drawn" where Epoch::getTrackPosition() puts it, which is what the sonar position is
// built from: its track fix when TrackSmoother gave it one (an epoch with a raw fix), else its position.
//
// The window rule is worked out here a second time, from the epochs alone and with the numbers as
// decided (Godio_autopilot/docs/INTERP_WINDOW_TASK.md and that session's answers), not with the
// class's Params: a changed default shows up as a miss.
//
//     interp_check            all checks, exit code 0 when they pass
//     interp_check --table    also the error table per speed
//     interp_check --debug    the epochs around the first miss of each check

#include "track_smoother.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {

constexpr double kLat0 = 42.5182, kLon0 = 124.0200;   // where the boat was measured
constexpr double kMPerDeg = 111320.0;
constexpr double kTrackRot = 37.4;                     // the survey is not aligned with north
constexpr double kPi = 3.14159265358979323846;
constexpr double kBootS = 1000.0;                      // positions carry time_boot_ms: never zero here
const double kNaN = std::numeric_limits<double>::quiet_NaN();

double rad(double d) { return d * kPi / 180.0; }
double pymod(double a, double m) { return a - m * std::floor(a / m); }   // Python's %, for floats

float f32(double x) { return static_cast<float>(x); }

void toLla(double n, double e, double& lat, double& lon)
{
    lat = kLat0 + n / kMPerDeg;
    lon = kLon0 + e / (kMPerDeg * std::cos(rad(kLat0)));
}

void toNe(double lat, double lon, double& n, double& e)
{
    n = (lat - kLat0) * kMPerDeg;
    e = (lon - kLon0) * kMPerDeg * std::cos(rad(kLat0));
}

struct Truth { double n, e, h; };

// A survey leg with a turn: straight, 180 deg turn, straight back, rotated off north.
Truth truth(double t, double speed)
{
    const double leg = 40.0, radius = 6.0;
    const double tLeg = leg / speed;
    const double tTurn = kPi * radius / speed;
    double n, e, h;
    if (t < tLeg) {
        n = speed * t; e = 0.0; h = 0.0;
    } else if (t < tLeg + tTurn) {
        const double a = speed * (t - tLeg) / radius;
        n = leg + radius * std::sin(a); e = radius * (1.0 - std::cos(a)); h = a * 180.0 / kPi;
    } else {
        const double tt = t - tLeg - tTurn;
        n = leg - speed * tt; e = 2.0 * radius; h = 180.0;
    }
    const double r = rad(kTrackRot);
    return { n * std::cos(r) - e * std::sin(r), n * std::sin(r) + e * std::cos(r), pymod(h + kTrackRot, 360.0) };
}

double wrap180(double a) { return pymod(a + 180.0, 360.0) - 180.0; }

// The least-squares value at 0 held to the range of the samples, as TrackSmoother holds it - computed
// here on its own, so that a copy of the class without the hold is caught.
double fitInRange(const std::vector<double>& dt, const std::vector<double>& y)
{
    const double v = TrackSmoother::lineAtZero(dt.data(), y.data(), int(dt.size()));
    const auto [lo, hi] = std::minmax_element(y.begin(), y.end());
    return std::min(std::max(v, *lo), *hi);
}

// The rule as decided: INTERP_WINDOW_TASK.md, with the GODIO session's answers of 2026-10-08 (rate as
// fixes per second, at least 12 fixes in the window).
struct Rule {
    bool enabled = true;
    double target = 0.08;          // normal; responsive 0.12, smooth 0.06
    double noise = 0.48;
    double maxPath = 5.0;
    double minWindow = 1.0, maxWindow = 6.0;
    double rateSpan = 5.0, minSpeedSpan = 2.0;
    double minFixes = 12.0;
    qint64 yawHalfNs = 500000000LL;
    int posEachSide = 2, yawEachSide = 1;
};

// --- the epochs, as Dataset holds them ----------------------------------------------------------

enum class Kind : uint8_t { None, Raw, Interpolated };

struct Epoch {
    bool chart = false;
    double pingT = kNaN;           // when the ping really was
    Kind pos = Kind::None;
    double lat = kNaN, lon = kNaN, alt = kNaN, n = kNaN, e = kNaN, d = kNaN;
    bool track = false;            // a track fix beside the raw one (Store::setTrackFix)
    double tLat = kNaN, tLon = kNaN, tN = kNaN, tE = kNaN;
    qint64 tNs = 0;
    Kind yawKind = Kind::None;
    float yaw = std::numeric_limits<float>::quiet_NaN();
};

double drawnN(const Epoch& ep) { return ep.track ? ep.tN : ep.n; }
double drawnE(const Epoch& ep) { return ep.track ? ep.tE : ep.e; }

class Epochs final : public TrackSmoother::Store
{
public:
    std::vector<Epoch> v;

    int count() const override { return int(v.size()); }
    bool hasRawFix(int i) const override { return v[size_t(i)].pos == Kind::Raw; }
    TrackSmoother::Fix rawFix(int i) const override
    {
        const Epoch& ep = v[size_t(i)];
        TrackSmoother::Fix f;
        f.lat = ep.lat; f.lon = ep.lon; f.alt = ep.alt; f.n = ep.n; f.e = ep.e; f.d = ep.d;
        return f;
    }
    qint64 timeNs(int i) const override { return v[size_t(i)].tNs; }
    void setTimeNs(int i, qint64 ns) override { v[size_t(i)].tNs = ns; }
    void setPlacedFix(int i, const TrackSmoother::Fix& f) override
    {
        Epoch& ep = v[size_t(i)];
        misplaced += ep.pos == Kind::Raw ? 1 : 0;
        ep.lat = f.lat; ep.lon = f.lon; ep.alt = f.alt; ep.n = f.n; ep.e = f.e; ep.d = f.d;
        ep.pos = Kind::Interpolated;
    }
    void setTrackFix(int i, const TrackSmoother::Fix& f) override
    {
        Epoch& ep = v[size_t(i)];
        misplaced += ep.pos != Kind::Raw ? 1 : 0;
        ep.tLat = f.lat; ep.tLon = f.lon; ep.tN = f.n; ep.tE = f.e;
        ep.track = true;
    }
    bool hasRawYaw(int i) const override { return v[size_t(i)].yawKind == Kind::Raw && std::isfinite(v[size_t(i)].yaw); }
    bool hasInterpolatedYaw(int i) const override { return v[size_t(i)].yawKind == Kind::Interpolated && std::isfinite(v[size_t(i)].yaw); }
    float yawDeg(int i) const override { return v[size_t(i)].yaw; }
    void setInterpolatedYaw(int i, float deg) override { v[size_t(i)].yaw = deg; v[size_t(i)].yawKind = Kind::Interpolated; }

    int misplaced = 0;   // a raw fix placed over, or a track fix given to an epoch without a raw fix
};

// --- the window rule, worked out on its own -----------------------------------------------------

enum class State { Off, NoRate, TooFew, Active };   // the order of TrackSmoother::Status::State

struct Expect {
    State state = State::NoRate;
    double windowS = 0.0, fixes = 0.0;
};

// The window for epoch j: rate and speed from the raw fixes within 5 s before it (its own included).
Expect expectWindow(const std::vector<Epoch>& v, size_t j, const Rule& r)
{
    Expect x;
    if (!r.enabled) {
        x.state = State::Off;
        return x;
    }
    const qint64 tj = v[j].tNs;
    const qint64 spanNs = qint64(r.rateSpan * 1e9);
    std::vector<double> dt, yn, ye;
    qint64 newer = 0;
    for (size_t k = j + 1; k-- > 0;) {
        if (v[k].pos != Kind::Raw) {
            continue;
        }
        const qint64 tk = v[k].tNs;
        if (tk <= 0 || (newer > 0 && tk > newer) || tj - tk > spanNs) {
            break;
        }
        if (tk == newer) {
            continue;
        }
        newer = tk;
        dt.push_back(double(tk - tj) / 1e9);
        yn.push_back(v[k].n);
        ye.push_back(v[k].e);
    }
    const int n = int(dt.size());
    const double span = n >= 2 ? dt.front() - dt.back() : 0.0;
    if (span <= 0.0) {
        return x;
    }
    const double rate = double(n - 1) / span;
    const double k = r.noise / r.target;
    double w = k * k / rate;
    if (span >= r.minSpeedSpan) {
        const double speed = std::hypot(TrackSmoother::lineSlope(dt.data(), yn.data(), n),
                                        TrackSmoother::lineSlope(dt.data(), ye.data(), n));
        if (speed > 0.0) {
            w = std::min(w, r.maxPath / speed);
        }
    }
    x.windowS = std::clamp(w, r.minWindow, r.maxWindow);
    x.fixes = rate * x.windowS;
    x.state = x.fixes >= r.minFixes ? State::Active : State::TooFew;
    return x;
}

// --- the simulated survey -----------------------------------------------------------------------

struct Survey {
    double speed = 1.5, fixHz = 6.58, headHz = 2.5, pingHz = 10.0, noise = 0.30, headNoise = 0.3;
    double gapFrom = -1.0, gapLen = 0.0;   // no fixes in [gapFrom, gapFrom + gapLen)
    struct Fix { double t; float lat, lon; };
    struct Head { double t; double deg; };
    std::vector<Fix> fixes;
    std::vector<Head> heads;
    std::vector<double> pings;
    double dur = 0.0;

    void build()
    {
        dur = (40.0 / speed) * 2.0 + kPi * 6.0 / speed;
        uint64_t rng = 12345;
        auto rnd = [&rng]() { rng = (rng * 1103515245ULL + 12345ULL) & 0x7FFFFFFFULL; return double(rng) / double(0x7FFFFFFF); };
        auto gauss = [&]() { double s = 0.0; for (int k = 0; k < 12; ++k) { s += rnd(); } return (s - 6.0) * noise; };

        for (double t = 0.0; t < dur; t += 1.0 / fixHz) {
            const Truth tr = truth(t, speed);
            const double gn = gauss();
            const double ge = gauss();
            double lat, lon;
            toLla(tr.n + gn, tr.e + ge, lat, lon);
            if (t >= gapFrom && t < gapFrom + gapLen) {
                continue;   // the noise is drawn all the same, so the rest of the survey is unchanged
            }
            fixes.push_back({ t, f32(lat), f32(lon) });
        }
        for (double t = 0.0; t < dur; t += 1.0 / headHz) {
            double h = truth(t, speed).h;
            h += (rnd() - 0.5) * 2.0 * headNoise;   // gyro error, then the protocol's 1 deg rounding
            heads.push_back({ t, pymod(std::nearbyint(pymod(h, 360.0)), 360.0) });
        }
        const int np = int(dur * pingHz);
        for (int i = 0; i < np; ++i) {
            pings.push_back(i / pingHz);
        }
    }
};

enum class Method { Old, New };

struct Run {
    Epochs epochs;
    TrackSmoother smoother;                          // the app's defaults unless a test sets them
    Rule rule;                                       // what the checks expect
    // the check's own records
    std::vector<int> finalSeen;                      // finalCount after every advance
    std::vector<Epoch> snapshot;                     // each epoch as it was when it became final
    std::vector<std::pair<float, float>> rawWritten; // per epoch: the raw fix written, or NaN
    std::vector<float> rawYawWritten;                // per epoch: the raw heading written, or NaN
    int edgeWithoutPosition = 0;                     // epochs between two fixes found without one, live
    double worstWaitOver = -1e9;                     // live: (newest fix - newest final epoch) - half window
    int notAtOnce = 0;                               // smoothing off: advances after which not all is final
    bool zeroTimes = false;
    double timeJumpAt = -1.0;                        // a new session: times start again from 5 s

    void setRule(const Rule& r)
    {
        rule = r;
        TrackSmoother::Params p;
        p.enabled = r.enabled;
        p.targetAccuracyM = r.target;
        smoother.setParams(p);
    }
};

qint64 fixTimeNs(const Run& run, double t)
{
    if (run.zeroTimes) {
        return 0;
    }
    double s = t + kBootS;
    if (run.timeJumpAt >= 0.0 && t >= run.timeJumpAt) {
        s = 5.0 + (t - run.timeJumpAt);
    }
    return qint64(std::llround(s * 1000.0)) * 1000000LL;   // time_boot_ms resolution
}

void recordFinal(Run& run)
{
    const int fc = run.smoother.finalCount();
    while (int(run.snapshot.size()) < fc) {
        run.snapshot.push_back(run.epochs.v[run.snapshot.size()]);
    }
    run.finalSeen.push_back(fc);
}

// Dataset::interpolatePos before TrackSmoother: the epochs between the two newest raw fixes, along
// the straight line between them, by index.
void placeOldPairwise(Run& run)
{
    std::vector<Epoch>& v = run.epochs.v;
    int k1 = -1, k0 = -1;
    for (int j = int(v.size()) - 1; j >= 0; --j) {
        if (v[size_t(j)].pos == Kind::Raw) {
            if (k1 < 0) { k1 = j; } else { k0 = j; break; }
        }
    }
    if (k0 < 0) {
        return;
    }
    for (int j = k0 + 1; j < k1; ++j) {
        const double p = double(j - k0) / double(k1 - k0);
        Epoch& ep = v[size_t(j)];
        const Epoch& a = v[size_t(k0)];
        const Epoch& b = v[size_t(k1)];
        ep.lat = (1 - p) * a.lat + p * b.lat; ep.lon = (1 - p) * a.lon + p * b.lon;
        ep.n = (1 - p) * a.n + p * b.n; ep.e = (1 - p) * a.e + p * b.e;
        ep.pos = Kind::Interpolated;
    }
}

// Dataset::interpolateAtt: the epochs between the two newest raw headings, by index, the short way.
void fillYawPairwise(Run& run)
{
    std::vector<Epoch>& v = run.epochs.v;
    int k1 = -1, k0 = -1;
    for (int j = int(v.size()) - 1; j >= 0; --j) {
        if (v[size_t(j)].yawKind == Kind::Raw) {
            if (k1 < 0) { k1 = j; } else { k0 = j; break; }
        }
    }
    if (k0 < 0) {
        return;
    }
    for (int j = k0 + 1; j < k1; ++j) {
        const double p = double(j - k0) / double(k1 - k0);
        double y = v[size_t(k0)].yaw + p * wrap180(double(v[size_t(k1)].yaw) - v[size_t(k0)].yaw);
        v[size_t(j)].yaw = float(pymod(y, 360.0));
        v[size_t(j)].yawKind = Kind::Interpolated;
    }
}

// The live edge: every epoch between the first and the newest fix has a position already - final, or
// the pairwise one meanwhile. And the wait: the newest final epoch is no further behind the newest fix
// than half the window of the epoch that waits; with smoothing off, nothing waits.
void checkLive(Run& run, qint64 newestFixNs)
{
    const std::vector<Epoch>& v = run.epochs.v;
    int lastRaw = -1;
    for (size_t j = 0; j < v.size(); ++j) {
        lastRaw = v[j].pos == Kind::Raw ? int(j) : lastRaw;
    }
    bool afterFirst = false;
    for (int j = 0; j < lastRaw; ++j) {
        const Epoch& ep = v[size_t(j)];
        afterFirst = afterFirst || ep.pos == Kind::Raw;
        if (afterFirst && ep.pos != Kind::Raw && !std::isfinite(ep.n)) {
            ++run.edgeWithoutPosition;
        }
    }

    const int fc = run.smoother.finalCount();
    if (!run.rule.enabled) {
        run.notAtOnce += fc != int(v.size()) ? 1 : 0;
        return;
    }
    if (fc > 0 && fc < int(v.size()) && v[size_t(fc)].tNs > 0 && v[size_t(fc - 1)].tNs > 0) {
        const Expect x = expectWindow(v, size_t(fc), run.rule);
        if (x.state == State::Active) {
            const double behind = double(newestFixNs - v[size_t(fc - 1)].tNs) / 1e9;
            run.worstWaitOver = std::max(run.worstWaitOver, behind - x.windowS / 2.0);
        }
    }
}

void runSurvey(const Survey& s, Method method, Run& run)
{
    // The three streams in time order; at equal times a ping first, then a fix, then a heading.
    size_t ip = 0, ifx = 0, ih = 0;
    std::vector<Epoch>& v = run.epochs.v;
    while (ip < s.pings.size() || ifx < s.fixes.size() || ih < s.heads.size()) {
        const double tp = ip < s.pings.size() ? s.pings[ip] : 1e300;
        const double tf = ifx < s.fixes.size() ? s.fixes[ifx].t : 1e300;
        const double th = ih < s.heads.size() ? s.heads[ih].t : 1e300;

        if (tp <= tf && tp <= th) {                     // Dataset::addChart
            if (v.empty() || v.back().chart) {
                v.emplace_back();
            }
            v.back().chart = true;
            v.back().pingT = tp;
            ++ip;
        } else if (tf <= th) {                          // Dataset::addPosition
            if (v.empty() || std::isfinite(v.back().lat)) {
                v.emplace_back();
            }
            const Survey::Fix& f = s.fixes[ifx++];
            Epoch& ep = v.back();
            ep.lat = f.lat; ep.lon = f.lon;
            toNe(f.lat, f.lon, ep.n, ep.e);
            ep.tNs = fixTimeNs(run, f.t);
            ep.pos = Kind::Raw;
            run.rawWritten.resize(v.size(), { NAN, NAN });
            run.rawWritten[v.size() - 1] = { f.lat, f.lon };
            if (method == Method::Old) {
                placeOldPairwise(run);
            } else {
                run.smoother.advance(run.epochs);
                recordFinal(run);
                checkLive(run, ep.tNs);
            }
        } else {                                        // Dataset::addAtt
            if (v.empty()) {
                v.emplace_back();
            }
            v.back().yaw = float(s.heads[ih++].deg);
            v.back().yawKind = Kind::Raw;
            run.rawYawWritten.resize(v.size(), NAN);
            run.rawYawWritten[v.size() - 1] = v.back().yaw;
            fillYawPairwise(run);
        }
    }
    run.rawWritten.resize(v.size(), { NAN, NAN });
    run.rawYawWritten.resize(v.size(), NAN);
    if (method == Method::New) {
        run.smoother.flush(run.epochs);   // Core::onFileStopsOpening
        recordFinal(run);
    }
}

bool g_debug = false;

void describe(const char* what, const std::vector<Epoch>& v, size_t j)
{
    if (!g_debug) {
        return;
    }
    for (size_t k = (j > 3 ? j - 3 : 0); k < std::min(v.size(), j + 4); ++k) {
        const Epoch& ep = v[k];
        std::printf("      %s #%zu chart %d ping %.3f pos %d t %.3f n %.4f e %.4f track %d %.4f %.4f yaw %d %.2f\n",
                    k == j ? what : "  ", k, int(ep.chart), ep.pingT, int(ep.pos), double(ep.tNs) * 1e-9, ep.n, ep.e,
                    int(ep.track), ep.tN, ep.tE, int(ep.yawKind), double(ep.yaw));
    }
}

struct Errors {
    double posRms = 0, crossRms = 0, yawRms = 0;
    int stuck = 0, pings = 0, unplaced = 0;
};

Errors measure(const Survey& s, const Run& run, bool verbose = false)
{
    Errors r;
    double se = 0, sc = 0, sy = 0;
    int ny = 0;
    const std::vector<Epoch>& v = run.epochs.v;
    double pn = kNaN, pe = kNaN;
    for (size_t j = 0; j < v.size(); ++j) {
        const Epoch& ep = v[j];
        if (!ep.chart) {
            continue;
        }
        const double n = drawnN(ep), e = drawnE(ep);
        if (!std::isfinite(n) || !std::isfinite(e)) {
            ++r.unplaced;
            continue;
        }
        const Truth tr = truth(ep.pingT, s.speed);
        const double dn = n - tr.n, de = e - tr.e;
        se += dn * dn + de * de;
        const double hv = rad(tr.h);
        const double cross = -dn * std::sin(hv) + de * std::cos(hv);
        sc += cross * cross;
        if (std::isfinite(pn) && std::abs(n - pn) < 1e-9 && std::abs(e - pe) < 1e-9) {
            if (r.stuck == 0 && verbose) {
                describe("STUCK", v, j);
            }
            ++r.stuck;
        }
        pn = n; pe = e;
        if (std::isfinite(ep.yaw)) {
            const double dy = wrap180(double(ep.yaw) - tr.h);
            sy += dy * dy;
            ++ny;
        }
        ++r.pings;
    }
    r.posRms = std::sqrt(se / std::max(1, r.pings));
    r.crossRms = std::sqrt(sc / std::max(1, r.pings));
    r.yawRms = std::sqrt(sy / std::max(1, ny));
    return r;
}

// Where TrackSmoother should have drawn every epoch, worked out again from the final epochs alone:
// where the rule is active and the window (+-half of it) holds two fixes or more on each side, the
// line through those fixes held to their range (a track fix for an epoch with a raw fix); otherwise the
// pairwise position between the neighbouring fixes, or the raw fix itself. For surveys with monotonic
// times only. Returns the epochs that differ; `active` counts the epochs where the rule applied.
int drawnOffRule(const std::vector<Epoch>& v, const Rule& r, int& total, int& active)
{
    int off = 0;
    total = 0;
    active = 0;
    int prevRaw = -1;
    for (size_t j = 0; j < v.size(); ++j) {
        const Epoch& ep = v[j];
        if (ep.pos == Kind::Raw) {
            prevRaw = int(j);
        }
        if (ep.pos == Kind::None) {
            continue;   // before the first fix, or after the last
        }
        const Expect x = expectWindow(v, j, r);
        bool track = false, fitted = false;
        double wn = ep.n, we = ep.e;
        int before = 0, after = 0;
        if (x.state == State::Active) {
            ++active;
            const qint64 halfNs = qint64(x.windowS * 0.5 * 1e9);
            std::vector<double> dt, yn, ye;
            for (const Epoch& f : v) {
                if (f.pos == Kind::Raw && std::llabs(f.tNs - ep.tNs) <= halfNs) {
                    dt.push_back(double(f.tNs - ep.tNs) * 1e-9);
                    yn.push_back(f.n);
                    ye.push_back(f.e);
                    before += f.tNs < ep.tNs ? 1 : 0;
                    after += f.tNs > ep.tNs ? 1 : 0;
                }
            }
            if (before >= r.posEachSide && after >= r.posEachSide) {
                wn = fitInRange(dt, yn);
                we = fitInRange(dt, ye);
                track = ep.pos == Kind::Raw;
                fitted = true;
            }
        }
        if (!fitted && ep.pos == Kind::Interpolated) {
            int nextRaw = int(j) + 1;
            while (nextRaw < int(v.size()) && v[size_t(nextRaw)].pos != Kind::Raw) {
                ++nextRaw;
            }
            if (prevRaw < 0 || nextRaw >= int(v.size())) {
                wn = we = kNaN;   // can not be: an epoch is placed only between two fixes
            } else {
                const double p = double(int(j) - prevRaw) / double(nextRaw - prevRaw);
                const Epoch& a = v[size_t(prevRaw)];
                const Epoch& b = v[size_t(nextRaw)];
                wn = (1.0 - p) * a.n + p * b.n;
                we = (1.0 - p) * a.e + p * b.e;
            }
        }
        ++total;
        const bool ok = ep.track == track && std::abs(drawnN(ep) - wn) < 1e-6 && std::abs(drawnE(ep) - we) < 1e-6;
        if (!ok) {
            if (off == 0) {
                describe("OFF", v, j);
                if (g_debug) {
                    std::printf("      expected n %.4f e %.4f track %d (state %d, window %.3f s, %d before, %d after)\n",
                                wn, we, int(track), int(x.state), x.windowS, before, after);
                }
            }
            ++off;
        }
    }
    return off;
}

// The same for the headings: an epoch whose heading was interpolated when it became final, and where
// the window rule was active, carries - with a raw heading on each side within +-0.5 s - the line
// through the raw headings there (unwrapped) held to their range; every other one still the pairwise
// value between the neighbouring raw headings. The raw headings are found as the class finds them,
// walking the epochs while their times hold. Returns the epochs that differ.
int yawOffRule(const Run& run, int& total, int& kept)
{
    const std::vector<Epoch>& v = run.epochs.v;
    int off = 0;
    total = 0;
    kept = 0;
    for (size_t j = 0; j < run.snapshot.size(); ++j) {
        const Epoch& ep = v[j];
        if (run.snapshot[j].yawKind != Kind::Interpolated || ep.pos == Kind::None) {
            continue;
        }
        std::vector<double> dt, deg;
        std::vector<int> earlier;
        qint64 prev = ep.tNs;
        for (int k = int(j) - 1; k >= 0; --k) {
            const qint64 tk = v[size_t(k)].tNs;
            if (tk <= 0 || tk > prev || ep.tNs - tk > run.rule.yawHalfNs) {
                break;
            }
            prev = tk;
            if (v[size_t(k)].yawKind == Kind::Raw) {
                earlier.push_back(k);
            }
        }
        for (auto it = earlier.rbegin(); it != earlier.rend(); ++it) {
            dt.push_back(double(v[size_t(*it)].tNs - ep.tNs) * 1e-9);
            deg.push_back(v[size_t(*it)].yaw);
        }
        prev = ep.tNs;
        for (size_t k = j + 1; k < v.size(); ++k) {
            const qint64 tk = v[k].tNs;
            if (tk <= 0 || tk < prev || tk - ep.tNs > run.rule.yawHalfNs) {
                break;
            }
            prev = tk;
            if (v[k].yawKind == Kind::Raw) {
                dt.push_back(double(tk - ep.tNs) * 1e-9);
                deg.push_back(v[k].yaw);
            }
        }
        const bool active = expectWindow(v, j, run.rule).state == State::Active;
        const int laterCount = int(dt.size() - earlier.size());
        double want = kNaN;
        if (!active || int(earlier.size()) < run.rule.yawEachSide || laterCount < run.rule.yawEachSide) {
            // Not refined: the pairwise value stays (fillYawPairwise).
            int k0 = int(j) - 1, k1 = int(j) + 1;
            while (k0 >= 0 && v[size_t(k0)].yawKind != Kind::Raw) { --k0; }
            while (k1 < int(v.size()) && v[size_t(k1)].yawKind != Kind::Raw) { ++k1; }
            if (k0 >= 0 && k1 < int(v.size())) {
                const double p = double(int(j) - k0) / double(k1 - k0);
                want = pymod(v[size_t(k0)].yaw + p * wrap180(double(v[size_t(k1)].yaw) - v[size_t(k0)].yaw), 360.0);
            }
            ++kept;
        } else {
            std::vector<double> unwrapped(deg.size());
            double acc = deg[0];
            for (size_t k = 0; k < deg.size(); ++k) {
                acc = k == 0 ? acc : acc + wrap180(deg[k] - acc);
                unwrapped[k] = acc;
            }
            want = pymod(fitInRange(dt, unwrapped), 360.0);
        }
        ++total;
        if (!(std::abs(wrap180(double(ep.yaw) - want)) <= 1e-3)) {   // NaN counts as off
            if (off == 0) {
                describe("YAW", v, j);
                if (g_debug) {
                    std::printf("      expected %.4f deg (rule %s)\n", want, active ? "active" : "not active");
                }
            }
            ++off;
        }
    }
    return off;
}

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
    }
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
}

// What finalCount() hands on is the position, the track fix and the time. A heading is handed on by
// its own horizon (Dataset::attitudeAdded), so one not yet filled when the position became final may
// be filled later; one the smoother refined must stay.
bool sameEpoch(const Epoch& a, const Epoch& b)
{
    auto same = [](double x, double y) { return (std::isnan(x) && std::isnan(y)) || x == y; };
    const bool yawKept = a.yawKind != Kind::Interpolated || (same(a.yaw, b.yaw) && b.yawKind == a.yawKind);
    return same(a.lat, b.lat) && same(a.lon, b.lon) && same(a.n, b.n) && same(a.e, b.e) && a.tNs == b.tNs
        && a.pos == b.pos && a.track == b.track && same(a.tN, b.tN) && same(a.tE, b.tE)
        && same(a.tLat, b.tLat) && same(a.tLon, b.tLon) && yawKept;
}

bool rawKept(const Run& run)
{
    for (size_t j = 0; j < run.epochs.v.size(); ++j) {
        const auto& w = run.rawWritten[j];
        const Epoch& ep = run.epochs.v[j];
        if (std::isfinite(w.first) && (ep.pos != Kind::Raw || float(ep.lat) != w.first || float(ep.lon) != w.second)) {
            return false;
        }
        const float y = run.rawYawWritten[j];
        if (std::isfinite(y) && (ep.yawKind != Kind::Raw || ep.yaw != y)) {
            return false;
        }
    }
    return run.epochs.misplaced == 0;
}

bool handedOnKept(const Run& run)
{
    bool monotonic = true;
    for (size_t k = 1; k < run.finalSeen.size(); ++k) {
        monotonic = monotonic && run.finalSeen[k] >= run.finalSeen[k - 1];
    }
    bool unchanged = true;
    for (size_t j = 0; j < run.snapshot.size(); ++j) {
        if (!sameEpoch(run.snapshot[j], run.epochs.v[j])) {
            if (unchanged) {
                describe("WAS", run.snapshot, j);
                describe("NOW", run.epochs.v, j);
            }
            unchanged = false;
        }
    }
    return monotonic && unchanged;
}

// Positions, track fixes and headings exactly as the old pairwise run has them.
int differFromOld(const Run& oldRun, const Run& newRun, int& compared)
{
    compared = 0;
    int differ = 0;
    if (oldRun.epochs.v.size() != newRun.epochs.v.size()) {
        return -1;
    }
    for (size_t j = 0; j < newRun.epochs.v.size(); ++j) {
        const Epoch& a = oldRun.epochs.v[j];
        const Epoch& b = newRun.epochs.v[j];
        const bool yawSame = (std::isnan(a.yaw) && std::isnan(b.yaw)) || a.yaw == b.yaw;
        if (std::isnan(a.n) && std::isnan(drawnN(b))) {
            differ += yawSame ? 0 : 1;
            continue;
        }
        ++compared;
        differ += b.track || !(std::abs(a.n - drawnN(b)) < 1e-9 && std::abs(a.e - drawnE(b)) < 1e-9) || !yawSame ? 1 : 0;
    }
    return differ;
}

const char* stateName(TrackSmoother::Status::State s)
{
    switch (s) {
    case TrackSmoother::Status::State::Off: return "off";
    case TrackSmoother::Status::State::NoRate: return "no rate";
    case TrackSmoother::Status::State::TooFewFixes: return "too few fixes";
    case TrackSmoother::Status::State::Active: return "active";
    }
    return "?";
}

}   // namespace

int main(int argc, char** argv)
{
    bool table = false;
    for (int a = 1; a < argc; ++a) {
        table = table || std::strcmp(argv[a], "--table") == 0;
        g_debug = g_debug || std::strcmp(argv[a], "--debug") == 0;
    }

    std::printf("TrackSmoother - pings between float32 fixes (fixes 6.58 Hz, pings 10 Hz, headings 2.5 Hz)\n");
    char what[260];

    // --- the arithmetic
    {
        const double dt[] = { -1.0, 0.5, 2.0 };
        const double y[] = { 3.0 - 2.0, 3.0 + 1.0, 3.0 + 4.0 };   // y = 3 + 2 t
        check(std::abs(TrackSmoother::lineAtZero(dt, y, 3) - 3.0) < 1e-12 && std::abs(TrackSmoother::lineSlope(dt, y, 3) - 2.0) < 1e-12,
              "lineAtZero/lineSlope: a line through three points gives its value at 0 and its slope");
        const double same[] = { 0.2, 0.2 };
        const double ys[] = { 1.0, 2.0 };
        check(std::abs(TrackSmoother::lineAtZero(same, ys, 2) - 1.5) < 1e-12 && TrackSmoother::lineSlope(same, ys, 2) == 0.0,
              "lineAtZero/lineSlope: equal times give the mean and no slope");
        check(std::abs(TrackSmoother::wrap180(359.0 - 1.0) - (-2.0)) < 1e-12 && std::abs(TrackSmoother::wrap180(-190.0) - 170.0) < 1e-12,
              "wrap180 folds into [-180, 180)");
    }

    // --- heading across north: unwrapped, not averaged to the south
    {
        Run run;
        const double ts[] = { 0.0, 0.2, 0.4, 0.6, 0.8, 1.0 };
        const double fixN[] = { 0.0, 0.3, 0.6, 0.9, 1.2, 1.5 };
        for (int k = 0; k < 6; ++k) {
            Epoch ep;
            ep.pos = Kind::Raw;
            ep.n = fixN[k]; ep.e = 0.0; ep.lat = kLat0 + fixN[k] / kMPerDeg; ep.lon = kLon0;
            ep.tNs = qint64((kBootS + ts[k]) * 1e9);
            run.epochs.v.push_back(ep);
            if (k == 2) {   // one ping between the 3rd and 4th fix, heading filled pairwise
                Epoch ping;
                ping.chart = true;
                ping.yaw = 0.5f;
                ping.yawKind = Kind::Interpolated;
                run.epochs.v.push_back(ping);
            }
        }
        const float yaws[] = { 358.0f, 359.0f, 0.0f /* unused */, 1.0f, 2.0f, 3.0f };
        for (int k = 0, j = 0; k < int(run.epochs.v.size()); ++k) {
            if (run.epochs.v[size_t(k)].pos == Kind::Raw) {
                if (j != 2) {
                    run.epochs.v[size_t(k)].yaw = yaws[j];
                    run.epochs.v[size_t(k)].yawKind = Kind::Raw;
                }
                ++j;
            }
        }
        run.smoother.flush(run.epochs);
        const float got = run.epochs.v[3].yaw;
        check(run.epochs.v[3].yawKind == Kind::Interpolated && (got < 2.5f || got > 357.5f),
              "heading across 0/360 is fitted on the unwrapped angle (near north, not near 180)");
    }

    // --- a step of the float32 grid inside the window: the line is held to the fixes' range. The
    //     window is pinned to 2 s here, so that it holds exactly the five fixes below.
    {
        Run run;
        TrackSmoother::Params p;
        p.minWindowS = p.maxWindowS = 2.0;
        p.minFixesInWindow = 0;
        run.smoother.setParams(p);
        const double q = 0.42;   // one step of the latitude grid
        const double ts[] = { -0.9, -0.6, -0.05, 0.05, 0.07, 1.5 };
        const double ns[] = { 0.0, q, q, q, q, q };
        for (int k = 0; k < 6; ++k) {
            Epoch ep;
            ep.pos = Kind::Raw;
            ep.n = ns[k]; ep.e = 0.0; ep.lat = kLat0 + ns[k] / kMPerDeg; ep.lon = kLon0;
            ep.tNs = qint64(std::llround((kBootS + ts[k]) * 1e9));
            run.epochs.v.push_back(ep);
            if (k == 2) {   // one ping midway between -0.05 and +0.05: its time is 0
                Epoch ping;
                ping.chart = true;
                run.epochs.v.push_back(ping);
            }
        }
        run.smoother.flush(run.epochs);
        const double bare = TrackSmoother::lineAtZero(ts, ns, 5);   // the five fixes within 1 s
        const Epoch& pe = run.epochs.v[3];
        std::snprintf(what, sizeof what, "a grid step in the window: the ping stays on the step (%.4f m), the bare line overshoots to %.4f m",
                      pe.n, bare);
        check(pe.pos == Kind::Interpolated && std::abs(pe.n - q) < 1e-12 && bare > q * 1.02, what);
    }

    // --- the survey at four speeds, fixes at 6.58 Hz
    const double speeds[] = { 0.5, 1.0, 1.5, 2.5 };
    if (table) {
        std::printf("\n  speed | window, delay (last epoch) | position RMS pairwise -> now | stuck pings | heading RMS\n");
    }
    for (double speed : speeds) {
        Survey s;
        s.speed = speed;
        s.build();

        Run oldRun, newRun;
        runSurvey(s, Method::Old, oldRun);
        runSurvey(s, Method::New, newRun);
        const Errors eo = measure(s, oldRun);
        const Errors en = measure(s, newRun, true);
        const TrackSmoother::Status st = newRun.smoother.status();

        if (table) {
            std::printf("  %.1f m/s | %.1f s, %.1f s (%s, %.0f fixes) | %.3f -> %.3f m (%.2fx) | %d -> %d of %d | %.2f -> %.2f deg\n",
                        speed, st.windowS, st.windowS / 2.0, stateName(st.state), st.fixesInWindow,
                        eo.posRms, en.posRms, eo.posRms / en.posRms, eo.stuck, en.stuck, en.pings, eo.yawRms, en.yawRms);
        }

        std::snprintf(what, sizeof what, "%.1f m/s: pings drawn on the spot of the one before: %d -> %d of %d (at most 1%%, 20x fewer)",
                      speed, eo.stuck, en.stuck, en.pings);
        check(en.stuck * 100 <= en.pings && en.stuck * 20 <= eo.stuck, what);

        int total = 0, active = 0;
        const int off = drawnOffRule(newRun.epochs.v, newRun.rule, total, active);
        std::snprintf(what, sizeof what, "%.1f m/s: every epoch drawn by the rule (%d of %d, the window rule active for %d)",
                      speed, total - off, total, active);
        check(off == 0 && total > 0 && active * 10 >= total * 9, what);

        int yawTotal = 0, yawKept = 0;
        const int yawOff = yawOffRule(newRun, yawTotal, yawKept);
        std::snprintf(what, sizeof what, "%.1f m/s: every interpolated heading by the rule (%d of %d, %d left pairwise)",
                      speed, yawTotal - yawOff, yawTotal, yawKept);
        check(yawOff == 0 && yawTotal > 0, what);

        std::snprintf(what, sizeof what, "%.1f m/s: position RMS better than pairwise (%.3f -> %.3f m)", speed, eo.posRms, en.posRms);
        check(en.posRms < eo.posRms, what);
        std::snprintf(what, sizeof what, "%.1f m/s: never waits past half its window (worst %+.2f s over); every ping placed, live too (%d without)",
                      speed, newRun.worstWaitOver, newRun.edgeWithoutPosition);
        check(newRun.worstWaitOver <= 0.35 && newRun.worstWaitOver > -1e8 && en.unplaced == eo.unplaced && newRun.edgeWithoutPosition == 0, what);
        std::snprintf(what, sizeof what, "%.1f m/s: raw fixes and raw headings untouched; nothing handed on changes later", speed);
        check(rawKept(newRun) && handedOnKept(newRun), what);
    }

    // --- smoothing off: exactly as before this class, and nothing waits
    {
        Survey s;
        s.speed = 1.0;
        s.build();
        Run oldRun, newRun;
        Rule off;
        off.enabled = false;
        newRun.setRule(off);
        runSurvey(s, Method::Old, oldRun);
        runSurvey(s, Method::New, newRun);
        int compared = 0;
        const int differ = differFromOld(oldRun, newRun, compared);
        std::snprintf(what, sizeof what, "smoothing off: %d of %d epochs otherwise than before (headings too), %d times something waited, status %s",
                      differ, compared, newRun.notAtOnce, stateName(newRun.smoother.status().state));
        check(differ == 0 && compared > 0 && newRun.notAtOnce == 0
              && newRun.smoother.status().state == TrackSmoother::Status::State::Off, what);
    }

    // --- the responsive setting (T = 0.12 m): a shorter window, still by the rule
    {
        Survey s;
        s.speed = 0.5;
        s.build();
        Run run;
        Rule responsive;
        responsive.target = 0.12;
        run.setRule(responsive);
        runSurvey(s, Method::New, run);
        int total = 0, active = 0;
        const int off = drawnOffRule(run.epochs.v, run.rule, total, active);
        const TrackSmoother::Status st = run.smoother.status();
        std::snprintf(what, sizeof what, "responsive (0.12 m) at 0.5 m/s: by the rule (%d of %d, %d active), window %.2f s, never waits past half of it (%+.2f s)",
                      total - off, total, active, st.windowS, run.worstWaitOver);
        check(off == 0 && active * 10 >= total * 9 && std::abs(st.windowS - 2.4) < 0.3 && run.worstWaitOver <= 0.35, what);
    }

    // --- fixes at 20 Hz: the same accuracy in a shorter window, so a shorter wait
    {
        Survey s;
        s.speed = 0.7;
        s.fixHz = 20.0;
        s.build();
        Run oldRun, newRun;
        runSurvey(s, Method::Old, oldRun);
        runSurvey(s, Method::New, newRun);
        const Errors eo = measure(s, oldRun);
        const Errors en = measure(s, newRun);
        int total = 0, active = 0;
        const int off = drawnOffRule(newRun.epochs.v, newRun.rule, total, active);
        const TrackSmoother::Status st = newRun.smoother.status();
        std::snprintf(what, sizeof what, "fixes at 20 Hz, 0.7 m/s: window %.2f s, by the rule (%d of %d), waits at most %+.2f s past half of it, %.3f -> %.3f m",
                      st.windowS, total - off, total, newRun.worstWaitOver, eo.posRms, en.posRms);
        check(off == 0 && std::abs(st.windowS - 1.8) < 0.1 && newRun.worstWaitOver <= 0.35 && en.posRms < eo.posRms, what);
    }

    // --- fixes at 4.85 Hz, 0.5 m/s: 36 / 4.85 = 7.4 s, held at the 6 s ceiling
    {
        Survey s;
        s.speed = 0.5;
        s.fixHz = 4.85;
        s.build();
        Run run;
        runSurvey(s, Method::New, run);
        int total = 0, active = 0;
        const int off = drawnOffRule(run.epochs.v, run.rule, total, active);
        const TrackSmoother::Status st = run.smoother.status();
        std::snprintf(what, sizeof what, "fixes at 4.85 Hz, 0.5 m/s: window %.2f s (the ceiling), by the rule (%d of %d)", st.windowS, total - off, total);
        check(off == 0 && st.windowS == 6.0 && active * 10 >= total * 9, what);
    }

    // --- a boat standing still: every ping drawn at the fix, no spread
    {
        Survey s;
        for (double t = 0.0; t < 30.0; t += 1.0 / s.fixHz) {
            s.fixes.push_back({ t, f32(kLat0 + 3.0 / kMPerDeg), f32(kLon0) });
        }
        for (int i = 0; i < 300; ++i) {
            s.pings.push_back(i / s.pingHz);
        }
        Run run;
        runSurvey(s, Method::New, run);
        const Epoch& first = run.epochs.v.front();
        int drawn = 0, elsewhere = 0, tracked = 0;
        for (const Epoch& ep : run.epochs.v) {
            if (std::isfinite(drawnN(ep))) {
                ++drawn;
                tracked += ep.track ? 1 : 0;
                // 1 nm: the pairwise (1 - p) a + p a may round off a by an ulp
                elsewhere += std::abs(drawnN(ep) - first.n) > 1e-9 || std::abs(drawnE(ep) - first.e) > 1e-9 ? 1 : 0;
            }
        }
        std::snprintf(what, sizeof what, "a boat standing still: %d of %d epochs drawn off the fix (%d on a track fix)",
                      elsewhere, drawn, tracked);
        check(drawn > 0 && tracked > 0 && elsewhere == 0, what);
    }

    // --- a gap of 5 s without fixes: pairwise across it, the fit again after it
    {
        Survey s;
        s.speed = 1.5;
        s.gapFrom = 20.0;
        s.gapLen = 5.0;
        s.build();
        Run oldRun, newRun;
        runSurvey(s, Method::Old, oldRun);
        runSurvey(s, Method::New, newRun);
        const Errors eo = measure(s, oldRun);
        const Errors en = measure(s, newRun);
        int total = 0, active = 0;
        const int off = drawnOffRule(newRun.epochs.v, newRun.rule, total, active);
        std::snprintf(what, sizeof what, "a 5 s gap in the fixes: drawn by the rule (%d of %d), every ping placed (%d live without one), "
                      "better than pairwise (%.3f -> %.3f m)", total - off, total, newRun.edgeWithoutPosition, eo.posRms, en.posRms);
        check(off == 0 && en.unplaced == eo.unplaced && newRun.edgeWithoutPosition == 0 && en.posRms < eo.posRms
              && rawKept(newRun) && handedOnKept(newRun), what);
    }

    // --- pings at 2 Hz (a long range): most epochs carry a fix and nothing else, so epochs with a fix
    //     follow one another, each waiting for its own window
    {
        Survey s;
        s.speed = 1.5;
        s.pingHz = 2.0;
        s.build();
        Run oldRun, newRun;
        runSurvey(s, Method::Old, oldRun);
        runSurvey(s, Method::New, newRun);
        const Errors eo = measure(s, oldRun);
        const Errors en = measure(s, newRun);
        int total = 0, active = 0;
        const int off = drawnOffRule(newRun.epochs.v, newRun.rule, total, active);
        std::snprintf(what, sizeof what, "pings at 2 Hz: drawn by the rule (%d of %d), every ping placed (%d live without one), "
                      "better than pairwise (%.3f -> %.3f m)", total - off, total, newRun.edgeWithoutPosition, eo.posRms, en.posRms);
        check(off == 0 && total > 0 && en.unplaced == eo.unplaced && newRun.edgeWithoutPosition == 0 && en.posRms < eo.posRms
              && rawKept(newRun) && handedOnKept(newRun), what);
    }

    // --- headings at 1 Hz: many epochs see a raw heading on one side only within 0.5 s
    {
        Survey s;
        s.speed = 1.5;
        s.headHz = 1.0;
        s.build();
        Run run;
        runSurvey(s, Method::New, run);
        int yawTotal = 0, yawKept = 0;
        const int yawOff = yawOffRule(run, yawTotal, yawKept);
        std::snprintf(what, sizeof what, "headings at 1 Hz: by the rule (%d of %d), %d left pairwise",
                      yawTotal - yawOff, yawTotal, yawKept);
        check(yawOff == 0 && yawKept > 0 && yawKept < yawTotal, what);
    }

    // --- fixes at 0.99 Hz, as in the recordings made before the GODIO fix: fewer than 12 fixes in any
    //     window, so every epoch is drawn exactly as before, and handed on at once
    {
        Survey s;
        s.speed = 1.25;
        s.fixHz = 0.99;
        s.build();
        Run oldRun, newRun;
        runSurvey(s, Method::Old, oldRun);
        runSurvey(s, Method::New, newRun);
        int compared = 0;
        const int differ = differFromOld(oldRun, newRun, compared);
        const TrackSmoother::Status st = newRun.smoother.status();
        std::snprintf(what, sizeof what, "fixes at 0.99 Hz (recordings before the GODIO fix): %d of %d epochs otherwise than before; status %s, %.1f fixes in %.1f s",
                      differ, compared, stateName(st.state), st.fixesInWindow, st.windowS);
        check(differ == 0 && compared > 0 && st.state == TrackSmoother::Status::State::TooFewFixes && st.fixesInWindow < 12.0, what);
    }

    // --- no usable times: exactly the old placement
    {
        Survey s;
        s.speed = 1.5;
        s.build();
        Run oldRun, newRun;
        oldRun.zeroTimes = newRun.zeroTimes = true;
        runSurvey(s, Method::Old, oldRun);
        runSurvey(s, Method::New, newRun);
        int compared = 0;
        const int differ = differFromOld(oldRun, newRun, compared);
        std::snprintf(what, sizeof what, "fixes without times: placed exactly as before (%d of %d differ), status %s",
                      differ, compared, stateName(newRun.smoother.status().state));
        check(differ == 0 && compared > 0 && newRun.smoother.status().state == TrackSmoother::Status::State::NoRate, what);
    }

    // --- times stepping back mid-run (a new session): no wait for ever, nothing placed across it
    {
        Survey s;
        s.speed = 1.5;
        s.build();
        Run newRun;
        newRun.timeJumpAt = s.dur / 2.0;
        runSurvey(s, Method::New, newRun);
        const Errors en = measure(s, newRun);
        Run ref;
        runSurvey(s, Method::Old, ref);
        const Errors eo = measure(s, ref);
        if (g_debug) {
            std::printf("      time jump: unplaced %d (old %d), stuck %d, RMS %.3f (old %.3f)\n",
                        en.unplaced, eo.unplaced, en.stuck, en.posRms, eo.posRms);
        }
        check(en.unplaced == eo.unplaced && en.stuck * 20 <= eo.stuck && en.posRms < eo.posRms && handedOnKept(newRun),
              "times stepping back: every ping still placed, as few stuck, still better than pairwise");
    }

    std::printf("\n%d of %d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}

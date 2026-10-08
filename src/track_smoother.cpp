#include "track_smoother.h"

#include <algorithm>
#include <cmath>


namespace {

// Between two fixes without usable times the epochs get times 100 ms apart, as they always have.
constexpr qint64 kNoTimeStepNs = 100000000LL;
constexpr double kNsPerS = 1e9;

}


void TrackSmoother::clear()
{
    raw_.clear();
    scanned_ = 0;
    timedSegments_ = 0;
    provisional_ = 0;
    done_ = 0;
}

double TrackSmoother::wrap180(double deg)
{
    double r = std::fmod(deg + 180.0, 360.0);
    if (r < 0.0) {
        r += 360.0;
    }
    return r - 180.0;
}

double TrackSmoother::lineAtZero(const double* dt, const double* y, int n)
{
    if (n <= 0) {
        return NAN;
    }

    // Relative to the first value: coordinates are large numbers that differ in the sixth decimal.
    const double y0 = y[0];
    double st = 0.0, stt = 0.0, sy = 0.0, sty = 0.0;
    for (int k = 0; k < n; ++k) {
        const double v = y[k] - y0;
        st  += dt[k];
        stt += dt[k] * dt[k];
        sy  += v;
        sty += dt[k] * v;
    }

    const double det = n * stt - st * st;
    if (std::abs(det) < 1e-12) {
        return y0 + sy / n;
    }
    const double slope = (n * sty - st * sy) / det;
    return y0 + (sy - slope * st) / n;
}

double TrackSmoother::lineAtZeroInRange(const double* dt, const double* y, int n)
{
    const double v = lineAtZero(dt, y, n);
    if (!std::isfinite(v)) {
        return v;
    }
    const auto [lo, hi] = std::minmax_element(y, y + n);
    return std::min(std::max(v, *lo), *hi);
}

void TrackSmoother::collectRawFixes(Store& store)
{
    const int count = store.count();
    for (int i = scanned_; i < count; ++i) {
        if (store.hasRawFix(i) && (raw_.empty() || raw_.back().index < i)) {
            raw_.push_back({ i, store.timeNs(i) });
        }
    }
    // Dataset writes a fix into the newest epoch when that one has none, so the newest is looked
    // at again next time.
    scanned_ = std::max(scanned_, count - 1);
}

void TrackSmoother::setSegmentTimes(Store& store)
{
    for (; timedSegments_ + 1 < int(raw_.size()); ++timedSegments_) {
        const RawFix& a = raw_[size_t(timedSegments_)];
        const RawFix& b = raw_[size_t(timedSegments_) + 1];
        const int steps = b.index - a.index;
        if (steps < 2 || a.timeNs <= 0 || b.timeNs <= a.timeNs) {
            continue;   // nothing between them, or no usable times: run() gives those their own
        }
        // An epoch has no time of its own: evenly between the two fixes, by index, as before.
        const qint64 step = (b.timeNs - a.timeNs) / steps;
        for (int j = a.index + 1, c = 1; j < b.index; ++j, ++c) {
            store.setTimeNs(j, a.timeNs + step * c);
        }
    }
}

int TrackSmoother::run(Store& store, bool flushing)
{
    const int count = store.count();
    if (count < done_ || count < scanned_) {
        clear();   // the epochs were reset under us
    }

    collectRawFixes(store);
    setSegmentTimes(store);

    std::vector<int> window;   // indices into raw_

    while (done_ < count) {
        const int i = done_;
        // The first fix after the epoch (after its own, for one that carries a fix).
        const auto next = std::upper_bound(raw_.begin(), raw_.end(), i,
                                           [](int index, const RawFix& r) { return index < r.index; });
        const size_t m1 = size_t(next - raw_.begin());

        if (store.hasRawFix(i)) {
            const bool known = m1 > 0 && raw_[m1 - 1].index == i;
            const qint64 ti = known ? raw_[m1 - 1].timeNs : 0;
            if (ti <= 0) {
                ++done_;   // no time of its own: drawn at its fix, as before
                continue;
            }
            if (!collectWindow(m1, ti, flushing, window)) {
                break;
            }
            if (hasBothSides(window, ti)) {
                store.setTrackFix(i, fitLine(store, window, ti));
            }
            if (store.hasInterpolatedYaw(i)) {
                refineYaw(store, i, ti);
            }
            ++done_;
            continue;
        }

        if (m1 == 0) {
            ++done_;   // before the first fix: nothing to place it by
            continue;
        }
        if (m1 == raw_.size()) {
            break;     // after the last fix: it waits for the next one, at flush too, as before
        }

        const RawFix a = raw_[m1 - 1];
        const RawFix b = raw_[m1];

        if (a.timeNs <= 0 || b.timeNs <= a.timeNs) {
            store.setTimeNs(i, a.timeNs + kNoTimeStepNs * (i - a.index));
            placePairwise(store, i, a.index, b.index);
            ++done_;
            continue;
        }

        const qint64 ti = store.timeNs(i);
        if (!collectWindow(m1, ti, flushing, window)) {
            break;
        }
        if (hasBothSides(window, ti)) {
            store.setPlacedFix(i, fitLine(store, window, ti));
        }
        else {
            placePairwise(store, i, a.index, b.index);   // a line through one side would extrapolate
        }

        if (store.hasInterpolatedYaw(i)) {
            refineYaw(store, i, ti);
        }
        ++done_;
    }

    placeProvisionally(store);
    return done_;
}

bool TrackSmoother::collectWindow(size_t next, qint64 timeNs, bool flushing, std::vector<int>& window) const
{
    // The window is complete once a fix lies past its end, or the times step back (a new session) -
    // until then the epoch waits, so that it is placed only once. raw_[next - 1] is the fix before the
    // epoch, or its own.
    const qint64 halfNs = qint64(params_.posHalfWindowS * kNsPerS);
    window.clear();
    bool complete = flushing;
    for (size_t m = next; m < raw_.size(); ++m) {
        const qint64 tm = raw_[m].timeNs;
        if (tm <= 0 || tm < raw_[m - 1].timeNs || tm - timeNs > halfNs) {
            complete = true;
            break;
        }
        window.push_back(int(m));
        if (tm - timeNs == halfNs) {
            complete = true;
            break;
        }
    }
    if (!complete) {
        return false;
    }
    for (size_t m = next; m-- > 0;) {
        const qint64 tm = raw_[m].timeNs;
        if (tm <= 0 || (m + 1 < next && tm > raw_[m + 1].timeNs) || timeNs - tm > halfNs) {
            break;
        }
        window.push_back(int(m));
    }
    return true;
}

bool TrackSmoother::hasBothSides(const std::vector<int>& window, qint64 timeNs) const
{
    int before = 0, after = 0;
    for (int m : window) {
        before += raw_[size_t(m)].timeNs < timeNs ? 1 : 0;
        after  += raw_[size_t(m)].timeNs > timeNs ? 1 : 0;
    }
    return !window.empty() && before >= params_.posMinEachSide && after >= params_.posMinEachSide;
}

void TrackSmoother::placeProvisionally(Store& store)
{
    // The epochs waiting for their window (the live edge) carry the pairwise position meanwhile, as
    // they always did, for whatever reads an epoch directly; only finalCount() hands them on.
    if (raw_.size() < 2) {
        return;
    }
    const int lastRaw = raw_.back().index;
    for (int i = std::max(done_, provisional_); i < lastRaw; ++i) {
        if (store.hasRawFix(i)) {
            continue;
        }
        const auto next = std::upper_bound(raw_.begin(), raw_.end(), i,
                                           [](int index, const RawFix& r) { return index < r.index; });
        if (next == raw_.begin() || next == raw_.end()) {
            continue;
        }
        placePairwise(store, i, (next - 1)->index, next->index);
    }
    provisional_ = std::max(provisional_, lastRaw);
}

TrackSmoother::Fix TrackSmoother::fitLine(Store& store, const std::vector<int>& window, qint64 timeNs) const
{
    const int n = int(window.size());
    const size_t size = window.size();
    std::vector<double> dt(size), lat(size), lon(size), alt(size), north(size), east(size), down(size);
    for (int k = 0; k < n; ++k) {
        const RawFix& r = raw_[size_t(window[size_t(k)])];
        const Fix f = store.rawFix(r.index);
        dt[size_t(k)]    = double(r.timeNs - timeNs) / kNsPerS;
        lat[size_t(k)]   = f.lat;
        lon[size_t(k)]   = f.lon;
        alt[size_t(k)]   = f.alt;
        north[size_t(k)] = f.n;
        east[size_t(k)]  = f.e;
        down[size_t(k)]  = f.d;
    }

    Fix placed;
    placed.lat = lineAtZeroInRange(dt.data(), lat.data(), n);
    placed.lon = lineAtZeroInRange(dt.data(), lon.data(), n);
    placed.alt = lineAtZeroInRange(dt.data(), alt.data(), n);
    placed.n   = lineAtZeroInRange(dt.data(), north.data(), n);
    placed.e   = lineAtZeroInRange(dt.data(), east.data(), n);
    placed.d   = lineAtZeroInRange(dt.data(), down.data(), n);
    return placed;
}

void TrackSmoother::placePairwise(Store& store, int i, int k0, int k1) const
{
    // As before: on the straight line between the two neighbouring fixes, by index.
    const double p = double(i - k0) / double(k1 - k0);
    const Fix a = store.rawFix(k0);
    const Fix b = store.rawFix(k1);

    Fix placed;
    placed.lat = (1.0 - p) * a.lat + p * b.lat;
    placed.lon = (1.0 - p) * a.lon + p * b.lon;
    placed.alt = (1.0 - p) * a.alt + p * b.alt;
    placed.n   = (1.0 - p) * a.n   + p * b.n;
    placed.e   = (1.0 - p) * a.e   + p * b.e;
    placed.d   = (1.0 - p) * a.d   + p * b.d;
    store.setPlacedFix(i, placed);
}

void TrackSmoother::refineYaw(Store& store, int i, qint64 timeNs) const
{
    const qint64 halfNs = qint64(params_.yawHalfWindowS * kNsPerS);

    // The raw headings within the window, earliest first, so that unwrapping walks forward in time.
    std::vector<int> earlier;
    qint64 prev = timeNs;
    for (int j = i - 1; j >= 0; --j) {
        const qint64 tj = store.timeNs(j);
        if (tj <= 0 || tj > prev || timeNs - tj > halfNs) {
            break;
        }
        prev = tj;
        if (store.hasRawYaw(j)) {
            earlier.push_back(j);
        }
    }

    std::vector<double> dt, deg;
    for (auto it = earlier.rbegin(); it != earlier.rend(); ++it) {
        dt.push_back(double(store.timeNs(*it) - timeNs) / kNsPerS);
        deg.push_back(store.yawDeg(*it));
    }

    prev = timeNs;
    const int count = store.count();
    for (int j = i + 1; j < count; ++j) {
        const qint64 tj = store.timeNs(j);
        if (tj <= 0 || tj < prev || tj - timeNs > halfNs) {
            break;
        }
        prev = tj;
        if (store.hasRawYaw(j)) {
            dt.push_back(double(tj - timeNs) / kNsPerS);
            deg.push_back(store.yawDeg(j));
        }
    }

    const int n = int(dt.size());
    const int before = int(earlier.size());
    if (n == 0 || before < params_.yawMinEachSide || n - before < params_.yawMinEachSide) {
        return;   // the pairwise value stays
    }

    std::vector<double> unwrapped(dt.size());
    double acc = deg[0];
    unwrapped[0] = acc;
    for (int k = 1; k < n; ++k) {
        acc += wrap180(deg[size_t(k)] - acc);
        unwrapped[size_t(k)] = acc;
    }

    double v = std::fmod(lineAtZeroInRange(dt.data(), unwrapped.data(), n), 360.0);
    if (v < 0.0) {
        v += 360.0;
    }
    if (v >= 360.0) {   // fmod of a tiny negative number, rounded up
        v -= 360.0;
    }
    store.setInterpolatedYaw(i, float(v));
}

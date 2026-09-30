#pragma once

#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

/**
 * @brief How far a running mission still has to go, when it ends at the boat's speed, what the battery will show
 * then, and where the boat should be while its link is lost (owner 2026-09-30: mission progress, a "ghost" boat).
 *
 * Pure geometry and trends; MavVehicle feeds them. Distances in metres, speeds in m/s, times in seconds or ms.
 */
namespace mission_progress {

struct Geo {
    double lat = 0.0;
    double lon = 0.0;
};

inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// Below this the boat is not going anywhere (GPS jitter at rest reads 0.1-0.2 m/s).
inline constexpr double kMinSpeedMs = 0.3;

/** Great-circle distance, the formula of MavVehicle::distanceM. */
inline double distanceM(Geo a, Geo b)
{
    constexpr double r = 6371008.8;
    constexpr double deg = 3.14159265358979323846 / 180.0;
    const double p1 = a.lat * deg, p2 = b.lat * deg;
    const double dp = p2 - p1, dl = (b.lon - a.lon) * deg;
    const double h = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2.0 * r * std::atan2(std::sqrt(h), std::sqrt(1.0 - h));
}

/** The way still ahead: the boat, the plan's points from `target` on, then home when the mission returns there. */
inline QVector<Geo> routeAhead(Geo boat, const QVector<Geo>& plan, int target, bool returnHome, bool homeValid, Geo home)
{
    QVector<Geo> route;
    route.append(boat);
    for (int i = std::max(0, target); i < plan.size(); ++i)
        route.append(plan[i]);
    if (returnHome && homeValid)
        route.append(home);
    return route;
}

inline double lengthM(const QVector<Geo>& route)
{
    double sum = 0.0;
    for (int i = 1; i < route.size(); ++i)
        sum += distanceM(route[i - 1], route[i]);
    return sum;
}

/** Seconds to cover `metres` at `speedMs`, NaN when the speed is too low to tell. */
inline double etaS(double metres, double speedMs)
{
    return (std::isfinite(metres) && std::isfinite(speedMs) && speedMs >= kMinSpeedMs) ? metres / speedMs : kNaN;
}

/** Where a boat is after `seconds` at `speedMs` along `route` (its first point being where it was): on the legs,
 *  stopping at the last point. Legs are short, so a point on one is taken linearly in latitude and longitude. */
inline Geo alongRoute(const QVector<Geo>& route, double speedMs, double seconds, bool* ended = nullptr)
{
    if (ended)
        *ended = false;
    if (route.isEmpty())
        return Geo{};
    double left = (std::isfinite(speedMs) && speedMs > 0.0 && seconds > 0.0) ? speedMs * seconds : 0.0;
    for (int i = 1; i < route.size(); ++i) {
        const double leg = distanceM(route[i - 1], route[i]);
        if (left <= leg) {
            const double t = leg > 0.0 ? left / leg : 1.0;
            return Geo{ route[i - 1].lat + (route[i].lat - route[i - 1].lat) * t,
                        route[i - 1].lon + (route[i].lon - route[i - 1].lon) * t };
        }
        left -= leg;
    }
    if (ended)
        *ended = route.size() > 1;
    return route.last();
}

/** The boat's speed, smoothed (exponential, time constant kTauMs), so an ETA does not jump with every GPS fix. */
class SpeedSmoother
{
public:
    static constexpr int64_t kTauMs = 10000;

    void reset() { has_ = false; value_ = 0.0; lastMs_ = 0; }
    void add(double speedMs, int64_t nowMs)
    {
        if (!std::isfinite(speedMs))
            return;
        if (!has_) {
            value_ = speedMs;
            has_ = true;
        } else {
            const double dt = double(std::max<int64_t>(0, nowMs - lastMs_));
            const double a = 1.0 - std::exp(-dt / double(kTauMs));
            value_ += a * (speedMs - value_);
        }
        lastMs_ = nowMs;
    }
    bool valid() const { return has_; }
    double value() const { return has_ ? value_ : kNaN; }

private:
    bool has_ = false;
    double value_ = 0.0;
    int64_t lastMs_ = 0;
};

/** The battery's trend over a run: percent per second from its first sample to the latest. The autopilot reports
 *  whole percents (SYS_STATUS.battery_remaining, -1 without a calibrated monitor), so a trend needs time and a
 *  change: kMinSpanMs apart and kMinDropPct down. */
class BatteryTrend
{
public:
    static constexpr int64_t kMinSpanMs = 60000;
    static constexpr int kMinDropPct = 1;

    void reset() { has_ = false; }
    void add(int64_t nowMs, int percent)
    {
        if (percent < 0 || percent > 100)
            return;
        if (!has_) {
            firstMs_ = nowMs;
            firstPct_ = percent;
            has_ = true;
        }
        lastMs_ = nowMs;
        lastPct_ = percent;
    }
    /** Percent per second, negative while draining; NaN until the run is long enough and the charge has moved. */
    double ratePctPerS() const
    {
        if (!has_ || lastMs_ - firstMs_ < kMinSpanMs || firstPct_ - lastPct_ < kMinDropPct)
            return kNaN;
        return double(lastPct_ - firstPct_) / (double(lastMs_ - firstMs_) / 1000.0);
    }
    int lastPercent() const { return has_ ? lastPct_ : -1; }
    /** The charge at the end of a run `etaS` from now, 0..100; -1 when either is unknown. */
    int atEnd(double etaS) const
    {
        const double rate = ratePctPerS();
        if (!std::isfinite(rate) || !std::isfinite(etaS) || etaS < 0.0)
            return -1;
        const double pct = double(lastPct_) + rate * etaS;
        return int(std::lround(std::clamp(pct, 0.0, 100.0)));
    }

private:
    bool has_ = false;
    int64_t firstMs_ = 0, lastMs_ = 0;
    int firstPct_ = 0, lastPct_ = 0;
};

} // namespace mission_progress

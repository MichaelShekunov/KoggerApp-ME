#pragma once

#include <QtGlobal>

#include <vector>

// Places the epochs that have no position of their own (pings between two GNSS fixes) on a
// least-squares line through the raw fixes within a window around the epoch's time, and refines an
// interpolated heading the same way within +-yawHalfWindowS, the angle unwrapped first. It replaces
// pairwise interpolation between the two neighbouring fixes, which puts every ping between two equal
// fixes on one spot: the GODIO boat sends float32 coordinates, a grid of 0.42 m by 0.63 m at 42.5 N,
// and at survey speed consecutive fixes are often equal. Prototype and numbers:
// Godio_autopilot/docs/GPS_SNIFFER_FINDINGS.md and INTERP_WINDOW_TASK.md; docs/track-interpolation.md
// here.
//
// The window is chosen per epoch by how many fixes it holds: the fit's error is about
// noiseConstantM / sqrt(fixes in the window) (measured on the lake at 5 to 20 Hz), so reaching
// targetAccuracyM takes (noiseConstantM / targetAccuracyM)^2 / rate seconds, and the line may not span
// more than maxPathM of track, or it cuts manoeuvres: window = clamp(min(that, maxPathM / speed),
// minWindowS, maxWindowS). Rate and speed come from the raw fixes within rateSpanS before the epoch,
// so the window is known as soon as the epoch's time is, and the epoch waits for half of it.
//
// An epoch that carries a raw fix keeps it; the same line at the fix's time goes beside it
// (Store::setTrackFix), and that is where its sonar is drawn. About half of the pings carry a fix,
// and on the float32 grid they would stay stuck to it.
//
// Nothing handed on is changed later. An epoch is placed for good once a fix more than half its window
// after it exists (or at flush()), and finalCount() moves past it only then; meanwhile it carries the
// pairwise position, as it always did. Raw fixes and raw headings are never written.
//
// The fit is used only where the window has samples on both sides of the epoch (posMinEachSide,
// yawMinEachSide), and its value is held to the range of those samples: a line through samples on
// one side extrapolates (twice the heading error measured on the boat's own records), and a step of
// the float32 grid inside the window can pull it past the step. Elsewhere - the start of a track, a
// gap, the end of a file, fixes without usable times - an epoch is placed pairwise, and one with a
// fix is drawn at its fix, as before.
//
// Pairwise at once, exactly as before this class, also where smoothing is off (Params::enabled) and
// where the window would hold fewer than minFixesInWindow fixes: at the 1 Hz of the recordings made
// before the GODIO fix the line measured worse than pairwise (Godio_autopilot, lake 2026-10-07).
//
// Transport-free: epochs are reached through Store, so tools/interp_check runs it without Dataset.
class TrackSmoother
{
public:
    struct Fix {
        double lat = 0.0, lon = 0.0, alt = 0.0;
        double n = 0.0, e = 0.0, d = 0.0;
    };

    class Store
    {
    public:
        virtual ~Store() = default;
        virtual int    count() const = 0;
        virtual bool   hasRawFix(int i) const = 0;
        virtual Fix    rawFix(int i) const = 0;
        virtual qint64 timeNs(int i) const = 0;                // the epoch's position time, 0 = none
        virtual void   setTimeNs(int i, qint64 ns) = 0;
        virtual void   setPlacedFix(int i, const Fix& fix) = 0; // and marks the position interpolated
        virtual void   setTrackFix(int i, const Fix& fix) = 0;  // an epoch with a raw fix: where it is drawn
        virtual bool   hasRawYaw(int i) const = 0;
        virtual bool   hasInterpolatedYaw(int i) const = 0;
        virtual float  yawDeg(int i) const = 0;
        virtual void   setInterpolatedYaw(int i, float deg) = 0;
    };

    // Fewer fixes expected in the window: pairwise. 12 is conservative. The model (fit RMS =
    // 0.48 / sqrt(N)) promised 1.5x at 1 Hz with a 4 s window, and the only real record at a low rate
    // (1 Hz, lake 2026-10-07) gave 0.98x: with few fixes and a long window the model is optimistic.
    // It allows ~10 (2.4-2.6x promised at N = 10); lower this once real records at 2-5 Hz confirm it.
    static constexpr int kMinFixesInWindow = 12;

    // The numbers come from the boat's records (Godio_autopilot: GPS_SNIFFER_FINDINGS.md, lake
    // 2026-10-08; INTERP_WINDOW_TASK.md).
    struct Params {
        bool enabled = true;            // off: pairwise and handed on at once, as before this class
        double targetAccuracyM = 0.08;  // 0.12 responsive, 0.08 normal, 0.06 smooth
        double noiseConstantM = 0.48;   // fit RMS = this / sqrt(fixes in the window), 5% spread
        double maxPathM = 5.0;          // a line over more track than this cuts the turns
        double minWindowS = 1.0;
        double maxWindowS = 6.0;        // past ~4 s the track's own curvature shows
        double rateSpanS = 5.0;         // rate and speed from the fixes this far before the epoch
        double minSpeedSpanS = 2.0;     // a speed from less than this much track is noise: not used
        int minFixesInWindow = kMinFixesInWindow;
        double yawHalfWindowS = 0.5;    // a 1 s window: a longer one cuts the turns
        int posMinEachSide = 2;         // fixes before and after the epoch
        int yawMinEachSide = 1;         // the heading changes ~2.7 times a second: 2 and 2 in 1 s never come
    };

    TrackSmoother() = default;
    explicit TrackSmoother(const Params& params) : params_(params) {}

    // For the epochs not yet final; the ones handed on stay as they are.
    void setParams(const Params& params);
    const Params& params() const { return params_; }

    // Places every epoch that can be placed for good; returns finalCount().
    int advance(Store& store) { return run(store, false); }
    // No more fixes will come (a file has been read): places the rest with the fixes there are.
    int flush(Store& store) { return run(store, true); }
    // Epochs [0, finalCount()) are final: placed, a raw fix with or without its track fix, or before
    // the first fix.
    int finalCount() const { return done_; }
    // How the epoch placed last went, for the settings page: there the switch can be on while the
    // fixes come too slowly for the line to be trusted.
    struct Status {
        enum class State { Off, NoRate, TooFewFixes, Active };
        State state = State::NoRate;
        double windowS = 0.0;        // chosen (Active), or what it would be (TooFewFixes)
        double fixesInWindow = 0.0;  // rate x window
    };
    const Status& status() const { return status_; }
    void clear();

    // The value at dt = 0 of the least-squares line through (dt[k], y[k]); with all dt equal, the mean.
    static double lineAtZero(const double* dt, const double* y, int n);
    // The slope of that line; 0 with all dt equal.
    static double lineSlope(const double* dt, const double* y, int n);
    // An angle difference folded into [-180, 180).
    static double wrap180(double deg);

private:
    struct RawFix {
        int index;
        qint64 timeNs;   // as it came: Dataset::interpolateAtt used to overwrite epoch times
    };

    int  run(Store& store, bool flushing);
    void collectRawFixes(Store& store);
    void setSegmentTimes(Store& store);
    void placeProvisionally(Store& store);
    // The window for an epoch at timeNs whose fix before it (or its own) is raw_[next - 1]. Active
    // only where smoothing applies: not off, a rate known, minFixesInWindow fixes in it at least.
    Status chooseWindow(Store& store, size_t next, qint64 timeNs) const;
    static qint64 halfWindowNs(double windowS);
    // The fixes within +-halfNs of timeNs, raw_[next] being the first fix after the epoch.
    // False while a later fix can still join (none past the window's end yet, not flushing).
    bool collectWindow(size_t next, qint64 timeNs, qint64 halfNs, bool flushing, std::vector<int>& window) const;
    bool hasBothSides(const std::vector<int>& window, qint64 timeNs) const;
    Fix  fitLine(Store& store, const std::vector<int>& window, qint64 timeNs) const;
    void placePairwise(Store& store, int i, int k0, int k1) const;
    void refineYaw(Store& store, int i, qint64 timeNs) const;
    // lineAtZero held to [min, max] of y.
    static double lineAtZeroInRange(const double* dt, const double* y, int n);

    Params params_;
    std::vector<RawFix> raw_;   // the raw fixes seen, ascending by index
    int scanned_ = 0;           // epochs [0, scanned_) have been looked at for raw fixes
    int timedSegments_ = 0;     // epochs between raw_[m] and raw_[m + 1], m < this, have their times
    int provisional_ = 0;       // epochs [done_, provisional_) carry a pairwise position meanwhile
    int done_ = 0;
    Status status_;
};

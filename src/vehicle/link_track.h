#pragma once

#include <QVector>

#include "mission_progress.h"

#include <climits>
#include <cstdint>

/**
 * @brief The boat's track with the quality of the link on each stretch (owner 2026-09-30: where the link was
 * strong or weak, so a mission is planned within reach).
 *
 * A sample comes with each position the autopilot reports, which it can only do over a working link; its quality
 * is the Wi-Fi signal of the head unit's module then. A stretch between the last position before a loss and the
 * first after it is marked Lost. Samples are thinned: a new one only kMinStepM from the last, when the quality
 * changes, or after a loss; at most kMaxSamples, the oldest dropped.
 */
class LinkTrack
{
public:
    enum Quality { Good = 0, Fair = 1, Weak = 2, Bad = 3, Unknown = 4, Lost = 5 };

    static constexpr double kMinStepM = 2.0;
    static constexpr int kMaxSamples = 3000;
    static constexpr int kNoRssi = INT_MIN;

    struct Sample {
        double lat = 0.0;
        double lon = 0.0;
        int quality = Unknown;   // of the stretch from the previous sample to this one
    };

    /** dBm to a quality: -60 and above good, to -70 fair, to -80 weak, below bad; 0 or kNoRssi unknown (a module
     *  reports 0 when it is not connected). */
    static int qualityOf(int rssiDbm)
    {
        if (rssiDbm == kNoRssi || rssiDbm >= 0)
            return Unknown;
        if (rssiDbm >= -60) return Good;
        if (rssiDbm >= -70) return Fair;
        if (rssiDbm >= -80) return Weak;
        return Bad;
    }

    /** A position; afterLoss: the first one since the link was lost. True when a sample was added. */
    bool add(double lat, double lon, int rssiDbm, bool afterLoss)
    {
        if (!std::isfinite(lat) || !std::isfinite(lon) || (lat == 0.0 && lon == 0.0))
            return false;
        const int q = afterLoss ? Lost : qualityOf(rssiDbm);
        if (!samples_.isEmpty() && !afterLoss) {
            const Sample& last = samples_.last();
            const double step = mission_progress::distanceM({ last.lat, last.lon }, { lat, lon });
            if (step < kMinStepM && q == last.quality)
                return false;
        }
        samples_.append(Sample{ lat, lon, q });
        if (samples_.size() > kMaxSamples)
            samples_.remove(0, samples_.size() - kMaxSamples);
        ++version_;
        return true;
    }

    void clear()
    {
        if (samples_.isEmpty())
            return;
        samples_.clear();
        ++version_;
    }

    const QVector<Sample>& samples() const { return samples_; }
    /** Moves with every change, for "did it change since I looked". */
    int version() const { return version_; }

private:
    QVector<Sample> samples_;
    int version_ = 0;
};

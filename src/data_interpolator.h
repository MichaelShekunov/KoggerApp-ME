#pragma once

#include <math.h>
#include <time.h>

#include "dataset_defs.h"
#include "track_smoother.h"


class Dataset;
class DataInterpolator {
public:
    explicit DataInterpolator(Dataset* datasetPtr);

    // Places the epochs without a fix of their own (TrackSmoother) and announces through
    // Dataset::positionAdded how far positions are final.
    void interpolatePos(bool fromStart);
    // No more fixes will come (a file has been read): places the rest with the fixes there are.
    void flushPos();
    // Track smoothing on or off, and the accuracy aimed at (TrackSmoother::Params); for the epochs
    // not yet final.
    void setSmoothing(bool enabled, double targetAccuracyM);
    const TrackSmoother::Status& smoothingStatus() const { return smoother_.status(); }
    void interpolateAtt(bool fromStart);
    void interpolateArtificalAtt(bool fromStart);
    void clear();

private:
    void announcePositions(int finalBefore);
    float interpAttParam(float start, float end, float progress) const;
    float interpDist(float start, float end, float progress) const;
    qint64 calcTimeDiffInNanoSecs(time_t startSecs, int startNanoSecs, time_t endSecs, int endNanoSecs) const;
    qint64 convertToNanosecs(time_t secs, int nanoSecs) const;
    std::pair<time_t, int> convertFromNanosecs(qint64 totalNanoSecs) const; // first - secs, second - nanosecs

private:
    const quint64 oneSecInNsecs = 1000000000ULL;
    const quint64 oneHundredNsecs = 100000000ULL;
    const quint64 zeroNsecs = 0ULL;

    Dataset* datasetPtr_;
    TrackSmoother smoother_;
    int lastAttInterpIndx_;
    int lastArtificalAttInterpIndx_;
};

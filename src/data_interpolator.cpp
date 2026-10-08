#include "data_interpolator.h"

#include "dataset.h"


namespace {

// The dataset's epochs as TrackSmoother sees them.
class DatasetEpochs final : public TrackSmoother::Store
{
public:
    explicit DatasetEpochs(Dataset* dataset) : dataset_(dataset) {}

    int count() const override { return dataset_->size(); }

    bool hasRawFix(int i) const override
    {
        Epoch* ep = at(i);
        return ep && ep->getPositionDataType() == DataType::kRaw && ep->getPositionGNSS().lla.isCoordinatesValid();
    }

    TrackSmoother::Fix rawFix(int i) const override
    {
        const Position p = at(i)->getPositionGNSS();
        TrackSmoother::Fix f;
        f.lat = p.lla.latitude;
        f.lon = p.lla.longitude;
        f.alt = p.lla.altitude;
        f.n = p.ned.n;
        f.e = p.ned.e;
        f.d = p.ned.d;
        return f;
    }

    qint64 timeNs(int i) const override
    {
        const Position p = at(i)->getPositionGNSS();
        return qint64(p.time.sec) * kNsPerS + p.time.nanoSec;
    }

    void setTimeNs(int i, qint64 ns) override
    {
        Epoch* ep = at(i);
        ep->setGNSSSec(time_t(ns / kNsPerS));
        ep->setGNSSNanoSec(int(ns % kNsPerS));
    }

    void setPlacedFix(int i, const TrackSmoother::Fix& f) override
    {
        Epoch* ep = at(i);
        ep->setPositionLLA(toLla(f));
        ep->setPositionNED(toNed(f));
        ep->setPositionDataType(DataType::kInterpolated);
    }

    void setTrackFix(int i, const TrackSmoother::Fix& f) override
    {
        at(i)->setTrackPosition(toLla(f), toNed(f));
    }

    bool hasRawYaw(int i) const override
    {
        Epoch* ep = at(i);
        return ep && ep->getAttDataType() == DataType::kRaw && std::isfinite(ep->yaw());
    }

    bool hasInterpolatedYaw(int i) const override
    {
        Epoch* ep = at(i);
        return ep && ep->getAttDataType() == DataType::kInterpolated && std::isfinite(ep->yaw());
    }

    float yawDeg(int i) const override { return at(i)->yaw(); }

    void setInterpolatedYaw(int i, float deg) override
    {
        Epoch* ep = at(i);
        ep->setAtt(deg, ep->pitch(), ep->roll(), DataType::kInterpolated);
    }

private:
    static constexpr qint64 kNsPerS = 1000000000LL;

    // Dataset::fromIndex() answers an index past the end with the last epoch, not nullptr.
    Epoch* at(int i) const { return (i >= 0 && i < dataset_->size()) ? dataset_->fromIndex(i) : nullptr; }

    static LLA toLla(const TrackSmoother::Fix& f) { return LLA(f.lat, f.lon, f.alt); }
    static NED toNed(const TrackSmoother::Fix& f) { return NED(f.n, f.e, f.d); }

    Dataset* dataset_;
};

}


DataInterpolator::DataInterpolator(Dataset *datasetPtr) :
    datasetPtr_(datasetPtr),
    lastAttInterpIndx_(0),
    lastArtificalAttInterpIndx_(0)
{}

void DataInterpolator::interpolatePos(bool fromStart)
{
    if (!datasetPtr_) {
        return;
    }
    if (fromStart) {
        smoother_.clear();
    }

    const int finalBefore = smoother_.finalCount();
    DatasetEpochs epochs(datasetPtr_);
    smoother_.advance(epochs);
    announcePositions(finalBefore);
}

void DataInterpolator::flushPos()
{
    if (!datasetPtr_) {
        return;
    }

    const int finalBefore = smoother_.finalCount();
    DatasetEpochs epochs(datasetPtr_);
    smoother_.flush(epochs);
    announcePositions(finalBefore);
}

void DataInterpolator::announcePositions(int finalBefore)
{
    // Downstream (DataHorizon: the sonar position, the mosaic, the boat track) takes every epoch up
    // to the index it is given as final, and takes an index below the last one for a new session.
    const int finalNow = smoother_.finalCount();
    if (finalNow > finalBefore && finalNow > 0) {
        emit datasetPtr_->positionAdded(uint64_t(finalNow - 1));
    }
}

void DataInterpolator::interpolateAtt(bool fromStart)
{
    if (!datasetPtr_) {
        return;
    }

    int startEpochIndx = fromStart ? 0 : lastAttInterpIndx_;
    int endEpochIndx = datasetPtr_->size();

    if ((endEpochIndx - startEpochIndx) < 1) {
        return;
    }

    int firstValidIndex = startEpochIndx;
    bool beenInterp = false;

    while (firstValidIndex < endEpochIndx) {
        while (firstValidIndex < endEpochIndx) {
            if (auto* ep = datasetPtr_->fromIndex(firstValidIndex); ep) {
                if (ep->getAttDataType() == DataType::kRaw) {
                    break;
                }
                ++firstValidIndex;
            }
        }

        int secondValidIndex = firstValidIndex + 1;
        while (secondValidIndex < endEpochIndx) {
            if (auto* ep = datasetPtr_->fromIndex(secondValidIndex); ep) {
                if (ep->getAttDataType() == DataType::kRaw) {
                    break;
                }
                ++secondValidIndex;
            }
        }

        if (secondValidIndex > endEpochIndx) {
            break;
        }

        int numInterpIndx = secondValidIndex - firstValidIndex;
        if (numInterpIndx < 1) {
            firstValidIndex = secondValidIndex;
            continue;
        }

        int fromIndx = firstValidIndex + 1;
        int toIndx = secondValidIndex;

        // boundaries epochs
        auto* startEpoch = datasetPtr_->fromIndex(firstValidIndex);
        auto* endEpoch = datasetPtr_->fromIndex(secondValidIndex);
        if (!startEpoch || !endEpoch) {
            continue;
        }

        auto startYaw = startEpoch->yaw();
        auto endYaw = endEpoch->yaw();
        auto startPitch = startEpoch->pitch();
        auto endPitch = endEpoch->pitch();
        auto startRoll = startEpoch->roll();
        auto endRoll = endEpoch->roll();


        const int numSteps = secondValidIndex - firstValidIndex;

        const quint64 startTimeNs = convertToNanosecs(startEpoch->getPositionGNSS().time.sec,
                                                      startEpoch->getPositionGNSS().time.nanoSec);
        const quint64 endTimeNs   = convertToNanosecs(endEpoch->getPositionGNSS().time.sec,
                                                    endEpoch->getPositionGNSS().time.nanoSec);
        const bool haveTimes = (startTimeNs != 0 || endTimeNs != 0) && (endTimeNs > startTimeNs);
        const quint64 timeDiffNs = haveTimes ? (endTimeNs - startTimeNs) : zeroNsecs;

        const quint64 stepNs = haveTimes ? (timeDiffNs / numSteps) : oneHundredNsecs;

        int cnt = 1;
        for (int j = fromIndx; j < toIndx; ++j, ++cnt) {
            if (auto* ep = datasetPtr_->fromIndex(j); ep) {

                quint64 currentNs = haveTimes ? convertToNanosecs(ep->getPositionGNSS().time.sec, ep->getPositionGNSS().time.nanoSec) : (startTimeNs + stepNs * cnt);

                // Only an epoch with no time yet: one with a fix, or placed between two (TrackSmoother),
                // has its own, and the track is fitted against the fixes' times.
                if (!haveTimes && ep->getPositionGNSS().time.sec == 0 && ep->getPositionGNSS().time.nanoSec == 0) {
                    const auto t = convertFromNanosecs(currentNs);
                    ep->setGNSSSec(t.first);
                    ep->setGNSSNanoSec(t.second);
                }

                double progress = haveTimes
                                      ? double(currentNs - startTimeNs) / double(timeDiffNs)
                                      : double(cnt) / double(numSteps);
                if (!haveTimes || currentNs <= startTimeNs || currentNs >= endTimeNs) {
                    progress = double(cnt) / double(numSteps);
                }
                progress = qBound(0.0, progress, 1.0);

                float resYaw   = interpAttParam(startYaw,   endYaw,   float(progress));
                float resPitch = interpAttParam(startPitch, endPitch, float(progress));
                float resRoll  = interpAttParam(startRoll,  endRoll,  float(progress));

                ep->setAtt(resYaw, resPitch, resRoll, DataType::kInterpolated);
                beenInterp = true;
                lastAttInterpIndx_ = j;
            }
        }

        firstValidIndex = secondValidIndex;
    }

    if (beenInterp) {
    //    emit datasetPtr_->interpYaw(endEpochIndx);
    }
    else {
        lastAttInterpIndx_ = firstValidIndex - 1;
    }
}

void DataInterpolator::interpolateArtificalAtt(bool fromStart)
{
    if (!datasetPtr_) {
        return;
    }

    int startEpochIndx = fromStart ? 0 : lastArtificalAttInterpIndx_;
    int endEpochIndx = datasetPtr_->size();

    if ((endEpochIndx - startEpochIndx) < 1) {
        return;
    }

    int firstValidIndex = startEpochIndx;
    bool beenInterp = false;

    while (firstValidIndex < endEpochIndx) {
        while (firstValidIndex < endEpochIndx) {
            if (auto* ep = datasetPtr_->fromIndex(firstValidIndex); ep) {
                if (ep->getArtificalAttDataType() == DataType::kRaw) {
                    break;
                }
                ++firstValidIndex;
            }
        }

        int secondValidIndex = firstValidIndex + 1;
        while (secondValidIndex < endEpochIndx) {
            if (auto* ep = datasetPtr_->fromIndex(secondValidIndex); ep) {
                if (ep->getArtificalAttDataType() == DataType::kRaw) {
                    break;
                }
                ++secondValidIndex;
            }
        }

        if (secondValidIndex > endEpochIndx) {
            break;
        }

        int numInterpIndx = secondValidIndex - firstValidIndex;
        if (numInterpIndx < 1) {
            firstValidIndex = secondValidIndex;
            continue;
        }

        int fromIndx = firstValidIndex + 1;
        int toIndx = secondValidIndex;

        // boundaries epochs
        auto* startEpoch = datasetPtr_->fromIndex(firstValidIndex);
        auto* endEpoch = datasetPtr_->fromIndex(secondValidIndex);
        if (!startEpoch || !endEpoch) {
            continue;
        }

        auto startYaw = startEpoch->artificalYaw();
        auto endYaw = endEpoch->artificalYaw();
        auto startPitch = startEpoch->artificalPitch();
        auto endPitch = endEpoch->artificalPitch();
        auto startRoll = startEpoch->artificalRoll();
        auto endRoll = endEpoch->artificalRoll();


        const int numSteps = secondValidIndex - firstValidIndex;

        const quint64 startTimeNs = convertToNanosecs(startEpoch->getPositionGNSS().time.sec,
                                                      startEpoch->getPositionGNSS().time.nanoSec);
        const quint64 endTimeNs   = convertToNanosecs(endEpoch->getPositionGNSS().time.sec,
                                                    endEpoch->getPositionGNSS().time.nanoSec);
        const bool haveTimes = (startTimeNs != 0 || endTimeNs != 0) && (endTimeNs > startTimeNs);
        const quint64 timeDiffNs = haveTimes ? (endTimeNs - startTimeNs) : zeroNsecs;

        const quint64 stepNs = haveTimes ? (timeDiffNs / numSteps) : oneHundredNsecs;

        int cnt = 1;
        for (int j = fromIndx; j < toIndx; ++j, ++cnt) {
            if (auto* ep = datasetPtr_->fromIndex(j); ep) {

                quint64 currentNs = haveTimes ? convertToNanosecs(ep->getPositionGNSS().time.sec, ep->getPositionGNSS().time.nanoSec) : (startTimeNs + stepNs * cnt);

                // As in interpolateAtt: never over a time an epoch already has.
                if (!haveTimes && ep->getPositionGNSS().time.sec == 0 && ep->getPositionGNSS().time.nanoSec == 0) {
                    const auto t = convertFromNanosecs(currentNs);
                    ep->setGNSSSec(t.first);
                    ep->setGNSSNanoSec(t.second);
                }

                double progress = haveTimes
                                      ? double(currentNs - startTimeNs) / double(timeDiffNs)
                                      : double(cnt) / double(numSteps);
                if (!haveTimes || currentNs <= startTimeNs || currentNs >= endTimeNs) {
                    progress = double(cnt) / double(numSteps);
                }
                progress = qBound(0.0, progress, 1.0);

                float resYaw   = interpAttParam(startYaw,   endYaw,   float(progress));
                float resPitch = interpAttParam(startPitch, endPitch, float(progress));
                float resRoll  = interpAttParam(startRoll,  endRoll,  float(progress));

                ep->setArtificalAtt(resYaw, resPitch, resRoll, DataType::kInterpolated);
                beenInterp = true;
                lastArtificalAttInterpIndx_ = j;
            }
        }

        firstValidIndex = secondValidIndex;
    }

    if (beenInterp) {
        //    emit datasetPtr_->interpYaw(endEpochIndx);
    }
    else {
        lastArtificalAttInterpIndx_ = firstValidIndex - 1;
    }
}

void DataInterpolator::clear()
{
    lastAttInterpIndx_= 0;
    lastArtificalAttInterpIndx_ = 0;
    smoother_.clear();
}

float DataInterpolator::interpAttParam(float start, float end, float progress) const
{
    float delta = end - start;
    if (delta > 180.0f) {
        delta -= 360.0f;
    }
    else if (delta < -180.0f) {
        delta += 360.0f;
    }

    float interpolated = start + progress * delta;
    if (interpolated < 0.0f) {
        interpolated += 360.0f;
    }
    else if (interpolated >= 360.0f) {
        interpolated -= 360.0f;
    }

    return interpolated;
}

float DataInterpolator::interpDist(float start, float end, float progress) const
{
    return (1.0 - progress) * start + progress * end;
}

qint64 DataInterpolator::calcTimeDiffInNanoSecs(time_t startSecs, int startNanoSecs, time_t endSecs, int endNanoSecs) const
{
    qint64 stTime = static_cast<qint64>(startSecs) * oneSecInNsecs + startNanoSecs;
    qint64 enTime = static_cast<qint64>(endSecs)   * oneSecInNsecs + endNanoSecs;

    return enTime - stTime;
}

qint64 DataInterpolator::convertToNanosecs(time_t secs, int nanoSecs) const
{
    return static_cast<qint64>(secs) * oneSecInNsecs + nanoSecs;
}

std::pair<time_t, int> DataInterpolator::convertFromNanosecs(qint64 totalNanoSecs) const
{
    time_t seconds  = static_cast<time_t>(totalNanoSecs / oneSecInNsecs);
    int nanoseconds = static_cast<int>(totalNanoSecs % oneSecInNsecs);

    return { seconds, nanoseconds };
}

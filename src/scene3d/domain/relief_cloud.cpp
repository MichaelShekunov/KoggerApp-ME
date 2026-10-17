#include "relief_cloud.h"

#include <QDebug>
#include <QOpenGLFunctions>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <vector>

#include "scene3d_view.h"
#include "echogram_relief.h"


ReliefCloud::ReliefCloudRenderImplementation::ReliefCloudRenderImplementation()
{
    m_primitiveType = GL_POINTS;
    m_color = QColor(120, 220, 255);
    m_width = 1.0f;
}

void ReliefCloud::ReliefCloudRenderImplementation::render(
    QOpenGLFunctions* ctx,
    const QMatrix4x4& mvp,
    const QMap<QString, std::shared_ptr<QOpenGLShaderProgram>>& shaderProgramMap) const
{
    // Nothing of its own to say: the base draws m_data with m_primitiveType
    // through the flat-colour shader, which is exactly a point cloud.
    SceneObject::RenderImplementation::render(ctx, mvp, shaderProgramMap);
}


ReliefCloud::ReliefCloud(GraphicsScene3dView* view, QObject* parent) :
    SceneObject(new ReliefCloudRenderImplementation, view, parent)
{
    setPrimitiveType(GL_POINTS);

    tail_.setSingleShot(true);
    tail_.setInterval(kMinRebuildMs);
    connect(&tail_, &QTimer::timeout, this, [this]() {
        const qint64 quiet = lastGrowth_.isValid() ? lastGrowth_.elapsed() : kMinRebuildMs;
        if (quiet < kMinRebuildMs) {   // still growing: once the growth pauses
            tail_.start(int(kMinRebuildMs - quiet));
            return;
        }
        if (isVisible() && datasetPtr_ && datasetPtr_->size() > builtAtEpochCount_) {
            rebuild();
        }
    });

    // Off until the layer is switched on, as the layer store has it. A scene
    // object starts visible, and the store's "off" arrives before the 3D view
    // exists, so the cloud was built on every file load with nothing to show it.
    setVisible(false);
}

ReliefCloud::~ReliefCloud() = default;

SceneObject::SceneObjectType ReliefCloud::type() const
{
    // No enumerator of its own: the type list is shared with hit-testing and
    // menus, and adding one would put this estimate into places that assume
    // measured geometry. Unknown keeps it drawable and otherwise inert.
    return SceneObject::SceneObjectType::Unknown;
}

void ReliefCloud::setDatasetPtr(Dataset* datasetPtr)
{
    datasetPtr_ = datasetPtr;
}

namespace {

// Altitude from the echogram itself, for recordings with no computed bottom
// track. The first strong return along the beam is the bottom under the boat;
// this is not a bottom tracker and does not pretend to be, but it gives the
// datum the relief needs and it works on data as recorded.
float altitudeFromProfile(Epoch* epoch, const ChannelId& channelId,
                          float maxRangeM, int16_t* buf, int n)
{
    if (!epoch || n < 8 || !(maxRangeM > 0.0f)) {
        return NAN;
    }
    if (!epoch->chartTo(channelId, 0, 0.0f, maxRangeM, buf, n, 0)) {
        return NAN;
    }

    int peak = 0;
    for (int i = 0; i < n; ++i) {
        peak = std::max<int>(peak, buf[i]);
    }
    if (peak < 24) {
        return NAN; // nothing that could be a bottom
    }

    const int threshold = peak / 2;
    for (int i = 1; i < n; ++i) {
        if (buf[i] >= threshold) {
            return maxRangeM * (float(i) + 0.5f) / float(n);
        }
    }
    return NAN;
}

} // namespace

void ReliefCloud::onDatasetGrew()
{
    if (!isVisible() || !datasetPtr_) {
        return;
    }
    // Rebuilt as the file loads, but not on every update: the builder walks
    // every ping, so it waits until the dataset has grown enough to be worth
    // another pass, and until the last pass is kMinRebuildMs old.
    const int now = datasetPtr_->size();
    if (now < builtAtEpochCount_) {
        builtAtEpochCount_ = 0;   // another dataset: what was built is not its own
    }
    if (now <= builtAtEpochCount_) {
        return;
    }
    // Whatever is left unbuilt, once the growth pauses. Restarted on every epoch, the timer cost two
    // window-system calls an epoch: a tenth of the GUI thread while a file loaded (2026-10-17).
    lastGrowth_.restart();
    if (!tail_.isActive()) {
        tail_.start(kMinRebuildMs);
    }
    if (now < builtAtEpochCount_ + kRebuildGrowth) {
        return;
    }
    if (lastBuild_.isValid() && lastBuild_.elapsed() < kMinRebuildMs) {
        return;
    }
    rebuild();
}

bool ReliefCloud::hasPoints() const
{
    return !cdata().isEmpty();
}

void ReliefCloud::setPingStride(int stride)
{
    pingStride_ = qBound(1, stride, 512);
}

void ReliefCloud::setSampleStride(int stride)
{
    sampleStride_ = qBound(1, stride, 64);
}

void ReliefCloud::setAltitudeLimits(float minAltitudeM, float maxAltitudeM)
{
    minAltitudeM_ = minAltitudeM;
    maxAltitudeM_ = maxAltitudeM;
}

void ReliefCloud::clearData()
{
    builtAtEpochCount_ = 0;
    tail_.stop();
    SceneObject::clearData();
}

void ReliefCloud::rebuild()
{
    if (!datasetPtr_) {
        qInfo("ReliefCloud: no dataset");
        SceneObject::clearData();
        return;
    }

    const int epochCount = datasetPtr_->size();
    if (epochCount <= 0) {
        // Says so rather than returning in silence: switching the layer on
        // before a file has loaded used to do nothing and report nothing.
        qInfo("ReliefCloud: dataset is empty, nothing to build yet");
        SceneObject::clearData();
        return;
    }
    builtAtEpochCount_ = epochCount;

    constexpr int kProfileSamples = 512;

    QVector<QVector3D> points;
    points.reserve((epochCount / qMax(1, pingStride_)) * 64);

    std::vector<int16_t> profile(size_t(kProfileSamples), 0);
    std::vector<float>   slant(size_t(kProfileSamples), 0.0f);
    std::vector<float>   gx(size_t(kProfileSamples), 0.0f);
    std::vector<float>   gz(size_t(kProfileSamples), 0.0f);

    echo_relief::ReliefParams params;

    // Counted rather than guessed at: a cloud that comes back empty should say
    // which requirement the data failed, not leave it to be inferred from an
    // empty scene.
    int considered = 0, noEpoch = 0, noAltitude = 0, noPosition = 0, noHeading = 0, fromProfile = 0;
    int noChannels = 0, noProfile = 0, noRelief = 0;

    for (int i = 0; i < epochCount; i += pingStride_) {
        ++considered;
        Epoch* epoch = datasetPtr_->fromIndex(i);
        if (!epoch) {
            ++noEpoch;
            continue;
        }

        // Bottom track first, because it is the better number when it exists.
        double altitude = epoch->distProccesing();
        if (!std::isfinite(altitude)) {
            const QList<ChannelId> firstChannels = epoch->chartChannels();
            if (!firstChannels.isEmpty()) {
                altitude = double(altitudeFromProfile(epoch, firstChannels.first(),
                                                      maxAltitudeM_, profile.data(), kProfileSamples));
                if (std::isfinite(altitude)) { ++fromProfile; }
            }
        }
        if (!std::isfinite(altitude) || altitude < double(minAltitudeM_) || altitude > double(maxAltitudeM_)) {
            ++noAltitude;
            continue; // no bottom under the boat means no datum to hang relief from
        }

        const Position pos = epoch->getTrackPosition(); // a ping with a raw fix: on the fitted track
        if (!std::isfinite(pos.ned.n) || !std::isfinite(pos.ned.e)) {
            ++noPosition;
            continue;
        }

        // Heading decides which way "across track" points. Without it the cloud
        // would smear along whatever direction the last valid ping happened to
        // have, which looks like relief and is not.
        const float yawDeg = epoch->tryRetValidYaw();
        if (!std::isfinite(yawDeg)) {
            ++noHeading;
            continue;
        }
        const double yawRad = qDegreesToRadians(double(yawDeg));
        // Across-track unit vector in NED: heading rotated by 90 degrees.
        const double acrossN = -std::sin(yawRad);
        const double acrossE =  std::cos(yawRad);

        // Each channel of the epoch is one side of the swath.
        const QList<ChannelId> channels = epoch->chartChannels();
        if (channels.isEmpty()) { ++noChannels; }
        int side = 0;
        for (const ChannelId& channelId : channels) {
            if (side >= 2) { break; }
            ++side;
            const float from = float(altitude);
            const float to   = float(altitude) * 8.0f;

            if (!epoch->chartTo(channelId, 0, from, to, profile.data(), kProfileSamples, 0)) {
                ++noProfile;
                continue;
            }
            for (int s = 0; s < kProfileSamples; ++s) {
                slant[size_t(s)] = from + (to - from) * (float(s) + 0.5f) / float(kProfileSamples);
            }

            const int n = echo_relief::acrossTrackRelief(profile.data(), slant.data(), kProfileSamples,
                                                          float(altitude), params,
                                                          gx.data(), gz.data(), kProfileSamples);
            if (n <= 0) {
                ++noRelief;
                continue;
            }

            // Thinned across track as well as along it. The base renderer hands
            // the whole array to GL from client memory every frame, so the point
            // count is a frame-rate budget, not just memory: 2.5 million from
            // this recording was more than the scene could carry.
            const double sign = (side == 1) ? -1.0 : 1.0;
            for (int s = 0; s < n; s += sampleStride_) {
                const double across = sign * double(gx[size_t(s)]);
                const double north  = pos.ned.n + acrossN * across;
                const double east   = pos.ned.e + acrossE * across;
                // Scene convention, taken from BoatTrack and BottomTrack rather
                // than assumed: QVector3D(north, east, depth), with depth
                // POSITIVE DOWN - boat_track.cpp writes
                // QVector3D(posNed.n, posNed.e, 0) and the bottom track writes
                // the bottom distance straight into z.
                //
                // Getting this wrong is silent: the cloud simply lands somewhere
                // else, mirrored about the diagonal and above the water.
                const double depth = altitude - double(gz[size_t(s)]);
                points.append(QVector3D(float(north), float(east), float(depth)));
            }
        }
    }

    qInfo("ReliefCloud: %d points from %d pings of %d (stride %d). Rejected -"
          " no epoch %d, no altitude %d, no position %d, no heading %d,"
          " no channels %d, no profile %d, no relief %d; altitude from echogram %d",
          int(points.size()), considered - noEpoch - noAltitude - noPosition - noHeading,
          epochCount, pingStride_,
          noEpoch, noAltitude, noPosition, noHeading, noChannels, noProfile, noRelief, fromProfile);

    setData(points, GL_POINTS);
    lastBuild_.restart();   // the interval counts from the end of a pass
    tail_.stop();
}

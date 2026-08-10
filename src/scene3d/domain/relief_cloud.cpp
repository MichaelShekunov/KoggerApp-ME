#include "relief_cloud.h"

#include <QDebug>
#include <QOpenGLFunctions>
#include <QtMath>

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

void ReliefCloud::onDatasetGrew()
{
    if (!isVisible() || !datasetPtr_) {
        return;
    }
    // Rebuilt as the file loads, but not on every update: the builder walks
    // every ping, so it waits until the dataset has grown enough to be worth
    // another pass.
    const int now = datasetPtr_->size();
    if (now <= 0 || now < builtAtEpochCount_ + kRebuildGrowth) {
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

void ReliefCloud::setAltitudeLimits(float minAltitudeM, float maxAltitudeM)
{
    minAltitudeM_ = minAltitudeM;
    maxAltitudeM_ = maxAltitudeM;
}

void ReliefCloud::clearData()
{
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
    int considered = 0, noEpoch = 0, noAltitude = 0, noPosition = 0, noHeading = 0;
    int noChannels = 0, noProfile = 0, noRelief = 0;

    for (int i = 0; i < epochCount; i += pingStride_) {
        ++considered;
        Epoch* epoch = datasetPtr_->fromIndex(i);
        if (!epoch) {
            ++noEpoch;
            continue;
        }

        const double altitude = epoch->distProccesing();
        if (!std::isfinite(altitude) || altitude < double(minAltitudeM_) || altitude > double(maxAltitudeM_)) {
            ++noAltitude;
            continue; // no bottom under the boat means no datum to hang relief from
        }

        const Position pos = epoch->getPositionGNSS();
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

            const double sign = (side == 1) ? -1.0 : 1.0;
            for (int s = 0; s < n; ++s) {
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
          " no channels %d, no profile %d, no relief %d",
          int(points.size()), considered - noEpoch - noAltitude - noPosition - noHeading,
          epochCount, pingStride_,
          noEpoch, noAltitude, noPosition, noHeading, noChannels, noProfile, noRelief);

    setData(points, GL_POINTS);
}

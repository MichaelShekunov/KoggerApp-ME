#pragma once

#include "scene_object.h"
#include "dataset.h"

class GraphicsScene3dView;

// Point cloud of seabed relief inferred from side-scan brightness.
//
// Deliberately its own object rather than points added to the surface: this is
// an estimate, the surface is measurement, and once they share a mesh nothing on
// screen tells them apart. Backlog item 9, and the reason the 2/3D pane exists.
//
// One point per usable across-track sample: position from the ping's navigation
// plus the sample's ground range across the heading, and depth from the bottom
// track adjusted by the brightness-derived elevation.
class ReliefCloud : public SceneObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(ReliefCloud)

public:
    class ReliefCloudRenderImplementation : public SceneObject::RenderImplementation
    {
    public:
        ReliefCloudRenderImplementation();
        void render(QOpenGLFunctions* ctx,
                    const QMatrix4x4& mvp,
                    const QMap<QString, std::shared_ptr<QOpenGLShaderProgram>>& shaderProgramMap) const override;
    };

    explicit ReliefCloud(GraphicsScene3dView* view = nullptr, QObject* parent = nullptr);
    ~ReliefCloud() override;

    SceneObjectType type() const override;

    void setDatasetPtr(Dataset* datasetPtr);

    // How many pings to skip between profiles. A survey holds far more pings
    // than a point cloud needs, and every one of them costs samples.
    void setPingStride(int stride);
    int  pingStride() const { return pingStride_; }

    // The sonar's along-track beamwidth, as the deconvolution asks for it. Only
    // used to reject profiles whose geometry cannot support an estimate.
    void setAltitudeLimits(float minAltitudeM, float maxAltitudeM);

public Q_SLOTS:
    // Walks the dataset and rebuilds the whole cloud. Cheap enough to call on
    // demand, far too expensive to call per ping.
    void rebuild();
    void clearData() override;

private:
    Dataset* datasetPtr_ = nullptr;
    int   pingStride_    = 4;
    float minAltitudeM_  = 1.0f;
    float maxAltitudeM_  = 300.0f;
};

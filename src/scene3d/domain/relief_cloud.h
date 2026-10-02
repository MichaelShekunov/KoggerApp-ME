#pragma once

#include "scene_object.h"
#include "dataset.h"

#include <QElapsedTimer>
#include <QTimer>

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
    bool hasPoints() const;

    // How many pings to skip between profiles. A survey holds far more pings
    // than a point cloud needs, and every one of them costs samples.
    void setPingStride(int stride);
    int  pingStride() const { return pingStride_; }

    // Same idea across the swath. Both strides exist because the point count is
    // a frame-rate budget: the base renderer uploads the whole array each frame.
    void setSampleStride(int stride);

    // The sonar's along-track beamwidth, as the deconvolution asks for it. Only
    // used to reject profiles whose geometry cannot support an estimate.
    void setAltitudeLimits(float minAltitudeM, float maxAltitudeM);

public Q_SLOTS:
    // Called as data arrives. Rebuilds only when the dataset has grown by
    // kRebuildGrowth epochs, and not more often than every kMinRebuildMs: a full
    // pass is not free, and a file loading grows the dataset by thousands of
    // epochs a second. What a skipped call leaves unbuilt, a timer builds once
    // the growth pauses.
    void onDatasetGrew();

    // Walks the dataset and rebuilds the whole cloud. Cheap enough to call on
    // demand, far too expensive to call per ping.
    void rebuild();
    void clearData() override;

private:
    static constexpr int kRebuildGrowth = 500;
    // A pass over 20 000 pings took 60 ms on a tablet; every 500 of them while a
    // file loaded stopped the GUI thread for 3.4 s (Xiaomi Pad 7, 2026-10-02).
    static constexpr int kMinRebuildMs = 1000;

    QElapsedTimer lastBuild_;
    QTimer tail_;

    Dataset* datasetPtr_ = nullptr;
    int   builtAtEpochCount_ = 0;
    int   pingStride_    = 8;
    int   sampleStride_  = 4;
    float minAltitudeM_  = 1.0f;
    float maxAltitudeM_  = 300.0f;
};

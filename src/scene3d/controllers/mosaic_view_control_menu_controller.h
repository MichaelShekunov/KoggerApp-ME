#pragma once

#include <QList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include "qml_component_controller.h"


class DataProcessor;
class GraphicsScene3dView;
class MosaicViewControlMenuController : public QmlComponentController
{
    Q_OBJECT

    Q_PROPERTY(QVariantMap pipelineStatus READ pipelineStatus NOTIFY pipelineStatusChanged)
    Q_PROPERTY(bool statusMonitorEnabled READ statusMonitorEnabled WRITE setStatusMonitorEnabled NOTIFY statusMonitorEnabledChanged)
    Q_PROPERTY(bool statusDetailedPolling READ statusDetailedPolling WRITE setStatusDetailedPolling NOTIFY statusDetailedPollingChanged)
    // The brightness model measured from the echograms (DataProcessor::mosaicBeamState).
    Q_PROPERTY(QVariantMap beamState READ beamState NOTIFY beamStateChanged)

public:
    explicit MosaicViewControlMenuController(QObject *parent = nullptr);

    void setGraphicsSceneView(GraphicsScene3dView* sceneView);
    void setDataProcessorPtr(DataProcessor* dataProcessorPtr);

    Q_INVOKABLE void onVisibilityChanged(bool state);
    Q_INVOKABLE void onUseFilterChanged(bool state);
    Q_INVOKABLE void onGridVisibleChanged(bool state);
    Q_INVOKABLE void onMeasLineVisibleChanged(bool state);
    Q_INVOKABLE void onClearClicked();
    Q_INVOKABLE void onUpdateStateChanged(bool state);
    Q_INVOKABLE void onThemeChanged(int val);
    // Colormap gradient stops [{pos, color}] for the theme swatch (QML picker).
    Q_INVOKABLE QVariantList themeStops(int index) const;
    Q_INVOKABLE void onLevelChanged(float lowLevel, float highLevel);
    Q_INVOKABLE void onUpdateClicked();
    Q_INVOKABLE void onSetLAngleOffset(float val);
    Q_INVOKABLE void onSetRAngleOffset(float val);
    Q_INVOKABLE void onSetResolution(float val);
    /// Mosaic quality (docs/mosaic-quality.md): the brightness correction, the crop by angle from
    /// straight down, and the overlap rule (0 newest, 1 newest with edges under, 2 nearest the track).
    /// Repaints the mosaic.
    Q_INVOKABLE void setQuality(bool correction, double nadirCropDeg, double farCropDeg, int overlap);

    QVariantMap pipelineStatus() const;
    QVariantMap beamState() const { return beamState_; }
    bool statusMonitorEnabled() const;
    void setStatusMonitorEnabled(bool state);
    bool statusDetailedPolling() const;
    void setStatusDetailedPolling(bool state);
    Q_INVOKABLE void refreshPipelineStatus();

    /// The colour theme (mosaic::PlotColorTable's id) and the levels as set in the tool; the loaded
    /// maps' mosaics follow them too.
    int themeId() const { return themeId_; }
    float lowLevel() const { return lowLevel_; }
    float highLevel() const { return highLevel_; }

signals:
    void pipelineStatusChanged();
    void statusMonitorEnabledChanged();
    void statusDetailedPollingChanged();
    void beamStateChanged();
    void styleChanged();   // themeId or the levels

protected:
    void findComponent() override;

private slots:
    void onMosaicStats(const QVariantMap& stats);

private:
    void tryInitPendingLambda();

    /*data*/
    GraphicsScene3dView* graphicsSceneViewPtr_;
    DataProcessor* dataProcessorPtr_;
    std::function<void()> pendingLambda_;
    bool visibility_;
    bool usingFilter_;
    bool gridVisible_;
    bool measLineVisible_;
    float resolution_;
    bool updateState_;
    int themeId_;
    float lowLevel_;
    float highLevel_;
    float lAngleOffset_;
    float rAngleOffset_;
    QTimer statusTimer_;
    QVariantMap pipelineStatus_;
    bool statusMonitorEnabled_;
    bool statusRequestPending_;
    bool statusDetailedPolling_;
    QVariantMap beamState_;
    // setQuality before the data processor is there: sent when it comes
    bool qualitySet_ = false;
    bool qualityCorrection_ = false;
    float qualityNadirCrop_ = 0.0f;
    float qualityFarCrop_ = 90.0f;
    int qualityOverlap_ = 0;
    void sendQuality();
};

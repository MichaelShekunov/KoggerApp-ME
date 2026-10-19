#pragma once

#include <QObject>
#include <QVector>
#include <QPair>
#include <QSet>
#include "surface_processor.h"
#include "surface_tile.h"
#include "isobaths_processor.h"
#include "mosaic_processor.h"


struct WorkBundle
{
    QVector<QPair<char,int>> surfaceVec;
    QVector<int>             mosaicVec;
    bool                     doIsobaths{false};
    bool                     batchMosaicEmit{false};
    int                      mosaicEraseBelow{-1}; // "Clear old data": the pixels of older pairs go (MosaicProcessor::eraseOwnersBelow)

    void clear() {
        surfaceVec.clear();
        surfaceVec.shrink_to_fit();
        mosaicVec.clear();
        mosaicVec.shrink_to_fit();
        doIsobaths = false;
        batchMosaicEmit = false;
        mosaicEraseBelow = -1;
    }
};
Q_DECLARE_METATYPE(WorkBundle) // NOLINT(performance-enum-size)

class Dataset;
class DataProcessor;
class SurfaceMesh;

class ComputeWorker : public QObject
{
    Q_OBJECT

public:
    explicit ComputeWorker(DataProcessor* ownerDp,
                           Dataset* dataset,
                           QObject* parent = nullptr);
    ~ComputeWorker() override;

    const QSet<TileKey>& getVisibleTileKeysCPtr();

public slots:
    // service
    void clearAll();
    void clearSurfaceMosaicContext();
    void clearSurface();
    void clearMosaic();
    void clearIsobaths();
    // An echogram edit took points out (DataProcessor::rebuildAfterEdit): the surface, the mosaic and
    // the isobaths start again from nothing, the mesh keeps its tiles blank and marked as updated, so
    // that a tile nothing is painted into now still reaches the view, blank.
    void clearForEditRebuild();

    // settings
    void setDatasetPtr(Dataset* ds);
    void setBottomTrackPtr(BottomTrack* bt);
    void setSurfaceThemeId(int id);
    void setSurfaceEdgeLimit(float v);
    void reapplySurfaceEdgeLimit();
    void setSurfaceExtraWidth(int v);
    void setSurfaceIsobathsStepSize(float v);
    void setIsobathsLabelStepSize(float v);
    void setMosaicChannels(const ChannelId& ch1, uint8_t sub1, const ChannelId& ch2, uint8_t sub2);
    void setMosaicLAngleOffset(float val);
    void setMosaicRAngleOffset(float val);
    void setMosaicTileResolution(float res);
    void setMosaicSource(int source);
    /// The brightness correction and the crop / overlap rules (mosaic_beam.h); called in this thread.
    void setMosaicBeam(std::shared_ptr<const mosaic_beam::Model> model, const mosaic_beam::PaintRules& rules);
    void setMosaicTrackOwners(bool on);
    // Threads painting the view's mosaic: 0 the default (MosaicProcessor::defaultPaintThreads), 1 in
    // place; applied between jobs (the slots come through the queue). KOGGER_MOSAIC_THREADS wins.
    void setMosaicPaintThreads(int n);
    void applySurfaceZoomChange(const TileMap& cached, bool fullCoverage);
    void setMinZ(float v);
    void setMaxZ(float v);

    // tasks
    void processBundle(const WorkBundle& wb); // выполнить пачку задач последовательно
    void setVisibleTileKeys(const QSet<TileKey>& val);

    // diagnostics
    void reportPipelineStats();
    void reportMosaicStats(int probeWindow);

signals:
    void jobFinished(); // для dataProcessor (нормально, отмена)

private:
    inline bool isCanceled() const noexcept;

private:
    DataProcessor*       dp_;
    Dataset*             dataset_;
    SurfaceMesh          surfaceMesh_;

    SurfaceProcessor     surface_;
    IsobathsProcessor    isobaths_;
    MosaicProcessor      mosaic_;

    QSet<TileKey> visibleTileKeys_;
};

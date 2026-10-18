#pragma once

#include <atomic>

#include <QObject>
#include <QSet>
#include <QVector>

#include "mosaic_processor.h"
#include "surface_mesh.h"

class DataProcessor;
class Dataset;

/// Paints the mosaic at a zoom level the view is not on -- the next finer and coarser ones of what is
/// on screen -- on a thread of its own, at low priority, so a zoom finds its tiles in the hot cache
/// (DataProcessor::maybeStartPrecompute schedules it, its precompute slots take the results). A run
/// starts from an empty mesh and takes the tiles it paints into from the cache, so it never keeps a
/// copy older than the one the view's own worker posted. A run stops, and what it sends is dropped,
/// once the data processor has moved to a newer generation (a zoom change, new settings, a reset).
class MosaicPrecomputer : public QObject
{
    Q_OBJECT

public:
    explicit MosaicPrecomputer(DataProcessor* dp, QObject* parent = nullptr);

    /// From any thread: the runs of older generations stop at their next chunk.
    void setLatestGeneration(quint64 generation) { latest_.store(generation); }

public slots:
    void setDatasetPtr(Dataset* ds);
    void setChannels(const ChannelId& ch1, uint8_t sub1, const ChannelId& ch2, uint8_t sub2);
    void setLAngleOffset(float val);
    void setRAngleOffset(float val);
    void setSource(int source);
    /// As ComputeWorker::setMosaicBeam: the same model and rules as the view's.
    void setBeam(std::shared_ptr<const mosaic_beam::Model> model, const mosaic_beam::PaintRules& rules);
    void setPaintThreads(int n);   // from the next run on (a run is one slot)
    /// Paints `epochs` into `tiles` (all of zoom level `zoom`), then calls the data processor's
    /// onPrecomputeFinished(generation) whether or not it ran to the end.
    void run(int zoom, const QSet<TileKey>& tiles, const QVector<int>& epochs, quint64 generation);
    void clear();

private:
    DataProcessor* dp_;
    SurfaceMesh mesh_;
    MosaicProcessor mosaic_;
    QSet<TileKey> tiles_;
    std::atomic<quint64> latest_{0};
    bool hasDataset_ = false;
};

#include "mosaic_precomputer.h"

#include "data_processor.h"
#include "data_processor_defs.h"
#include "surface_tile.h"

#include <iterator>

namespace {
constexpr int kPrecomputeMeshHighWM = 512;
constexpr int kPrecomputeMeshLowWM = 256;
constexpr int kPrecomputePaintThreads = 2;   // beside the view's worker and its own pool
} // namespace

MosaicPrecomputer::MosaicPrecomputer(DataProcessor* dp, QObject* parent) :
    QObject(parent),
    dp_(dp),
    mesh_(defaultTileSidePixelSize, defaultTileHeightMatrixRatio, defaultTileResolution),
    mosaic_(dp, nullptr)
{
    mesh_.setLRUWatermarks(kPrecomputeMeshHighWM, kPrecomputeMeshLowWM);
    mosaic_.setSurfaceMeshPtr(&mesh_);
    mosaic_.setPaintThreads(kPrecomputePaintThreads);
    mosaic_.setBackground(&tiles_, &latest_, 0);
}

void MosaicPrecomputer::setDatasetPtr(Dataset* ds)
{
    mosaic_.setDatasetPtr(ds);
    hasDataset_ = ds != nullptr;
}

void MosaicPrecomputer::setChannels(const ChannelId& ch1, uint8_t sub1, const ChannelId& ch2, uint8_t sub2)
{
    mosaic_.setChannels(ch1, sub1, ch2, sub2);
}

void MosaicPrecomputer::setLAngleOffset(float val)
{
    mosaic_.setLAngleOffset(val);
}

void MosaicPrecomputer::setRAngleOffset(float val)
{
    mosaic_.setRAngleOffset(val);
}

void MosaicPrecomputer::setSource(int source)
{
    mosaic_.setSource(static_cast<MosaicProcessor::Source>(source));
}

void MosaicPrecomputer::run(int zoom, const QSet<TileKey>& tiles, const QVector<int>& epochs, quint64 generation)
{
    if (latest_.load() == generation && hasDataset_ && zoom >= 1 && zoom <= int(std::size(ZL)) && !epochs.isEmpty()) {
        const float res = 1.0f / ZL[zoom - 1].pxPerMeter;
        mosaic_.clear();
        mesh_.reinit(defaultTileSidePixelSize, defaultTileHeightMatrixRatio, res);
        mosaic_.setTileResolution(res);
        tiles_ = tiles;
        mosaic_.setBackground(&tiles_, &latest_, generation);
        mosaic_.updateDataWrapper(epochs, false);
        clear();   // the tiles are the cache's now; the next run starts empty anyway
    }
    QMetaObject::invokeMethod(dp_, "onPrecomputeFinished", Qt::QueuedConnection, Q_ARG(quint64, generation));
}

void MosaicPrecomputer::clear()
{
    mosaic_.clear();
    mesh_.clear();
    tiles_.clear();
}
